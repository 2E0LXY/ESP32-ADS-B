"""Airline logos are fetched once for the whole deployment, then served locally.

The point of the cache is saved API calls, so most of these count how many
times the upstream was actually asked rather than only checking that an image
comes back.
"""

import asyncio

import httpx
import pytest

from app import logos as logos_module
from app.logos import AIRLINE_DOMAINS, LogoStore, code_for_callsign

PNG = b"\x89PNG\r\n\x1a\n" + b"padding" * 8


class _FakeUpstream:
    """Stands in for logo.dev, counting requests and serving what it is told."""

    def __init__(self, responses):
        self.responses = responses          # domain -> status code
        self.requests: list[str] = []

    async def aclose(self):
        """The app's shutdown closes whatever client the store holds."""

    async def get(self, url, params=None):
        domain = url.rsplit("/", 1)[-1]
        self.requests.append(domain)
        status = self.responses.get(domain, 404)
        request = httpx.Request("GET", url)
        if status != 200:
            return httpx.Response(status, json={"error": "not found"}, request=request)
        return httpx.Response(200, content=PNG, headers={"content-type": "image/png"},
                              request=request)


def _store(tmp_path, upstream, token="pk_test"):
    store = LogoStore(cache_dir=str(tmp_path), token=token)
    store._client = upstream
    return store


async def test_a_logo_is_fetched_once_and_then_served_from_disk(tmp_path):
    upstream = _FakeUpstream({"ryanair.com": 200})
    store = _store(tmp_path, upstream)

    first = await store.logo("RYR")
    second = await store.logo("RYR")
    third = await store.logo("RYR")

    assert first == second == third == PNG
    assert upstream.requests == ["ryanair.com"], "the cache did not save the API calls"
    assert store.hits == 2 and store.fetches == 1


async def test_concurrent_requests_for_one_logo_make_one_call(tmp_path):
    """A map opening with twenty Ryanair aircraft on it must not start twenty
    identical fetches."""
    upstream = _FakeUpstream({"ryanair.com": 200})
    store = _store(tmp_path, upstream)

    results = await asyncio.gather(*(store.logo("RYR") for _ in range(20)))

    assert all(r == PNG for r in results)
    assert len(upstream.requests) == 1


async def test_a_missing_logo_is_remembered_and_not_re_asked(tmp_path):
    upstream = _FakeUpstream({})  # everything 404s
    store = _store(tmp_path, upstream)

    assert await store.logo("RYR") is None
    assert await store.logo("RYR") is None
    assert len(upstream.requests) == 1, "a known-missing logo was asked for twice"


async def test_a_stale_miss_is_retried(tmp_path, monkeypatch):
    """A logo added upstream must appear without anyone clearing the cache."""
    upstream = _FakeUpstream({})
    store = _store(tmp_path, upstream)
    assert await store.logo("RYR") is None

    monkeypatch.setattr(logos_module, "MISS_TTL_SECONDS", -1)
    upstream.responses["ryanair.com"] = 200
    assert await store.logo("RYR") == PNG


async def test_a_transport_error_is_not_cached_as_a_miss(tmp_path):
    """A network blip is not "this airline has no logo" - recording it as one
    would hide a real logo for a week."""

    class _Broken:
        def __init__(self):
            self.calls = 0

        async def get(self, url, params=None):
            self.calls += 1
            raise httpx.ConnectError("no route to host")

    store = _store(tmp_path, _Broken())
    assert await store.logo("RYR") is None
    assert await store.logo("RYR") is None
    assert store._client.calls == 2, "a connection failure was cached as a miss"


async def test_a_non_png_response_is_never_cached(tmp_path):
    """Whatever an unexpected 200 body is, it must not be served to viewers."""

    class _Html:
        async def get(self, url, params=None):
            return httpx.Response(200, content=b"<html>rate limited</html>",
                                  request=httpx.Request("GET", url))

    store = _store(tmp_path, _Html())
    assert await store.logo("RYR") is None
    assert not list(tmp_path.glob("*.png"))


async def test_no_token_means_no_upstream_call_at_all(tmp_path):
    upstream = _FakeUpstream({"ryanair.com": 200})
    store = _store(tmp_path, upstream, token="")

    assert await store.logo("RYR") is None
    assert upstream.requests == []
    assert store.configured() is False


async def test_an_unknown_airline_is_never_looked_up(tmp_path):
    upstream = _FakeUpstream({})
    store = _store(tmp_path, upstream)

    for code in ("ZZZ", "", "ryr", "../etc/passwd", "A", "TOOLONG"):
        assert await store.logo(code) is None
    assert upstream.requests == []


async def test_an_odd_size_falls_back_to_the_default(tmp_path):
    """Otherwise a caller could fill the disk asking for every pixel width."""
    upstream = _FakeUpstream({"ryanair.com": 200})
    store = _store(tmp_path, upstream)

    assert await store.logo("RYR", size=999) == PNG
    assert [p.name for p in tmp_path.glob("*.png")] == ["RYR-128.png"]


def test_callsign_prefixes_resolve_to_airlines():
    assert code_for_callsign("RYR2BH") == "RYR"
    assert code_for_callsign("  ryr2bh  ") == "RYR"
    assert code_for_callsign("EXS31YT") == "EXS"
    # A bare code is not a flight, and an unknown operator is not ours.
    assert code_for_callsign("RYR") is None
    assert code_for_callsign("ZZZ1234") is None
    assert code_for_callsign("") is None
    assert code_for_callsign(None) is None


def test_every_mapped_domain_looks_like_one():
    """Guards against a typo turning into a permanent silent miss."""
    for code, domain in AIRLINE_DOMAINS.items():
        assert len(code) == 3 and code.isalpha() and code.isupper(), code
        assert "." in domain and " " not in domain and "/" not in domain, (code, domain)


def test_the_endpoint_serves_and_404s(client, tmp_path):
    """404 is a normal answer here: the page's onerror draws its own badge."""
    from app.main import app

    # Swapped wholesale rather than reaching into the live store: pointing it
    # at tmp_path keeps the test from writing into the deployment's real
    # cache directory, and restoring it afterwards keeps the app's own
    # shutdown working for the rest of the session.
    original = app.state.logos
    store = LogoStore(cache_dir=str(tmp_path), token="pk_test")
    store._client = _FakeUpstream({"ryanair.com": 200})
    app.state.logos = store
    try:
        _assert_endpoint_behaviour(client, tmp_path)
    finally:
        app.state.logos = original


def _assert_endpoint_behaviour(client, tmp_path):
    ok = client.get("/logo/callsign/RYR2BH.png")
    assert ok.status_code == 200
    assert ok.headers["content-type"] == "image/png"
    assert "max-age" in ok.headers["cache-control"]

    assert client.get("/logo/callsign/ZZZ1234.png").status_code == 404
    assert client.get("/logo/airline/RYR.png").status_code == 200
    assert client.get("/logo/airline/ZZZ.png").status_code == 404
    # Cached inside the temporary directory, not the real one.
    assert [p.name for p in tmp_path.glob("*.png")] == ["RYR-128.png"]
