"""Server-side route resolution.

The point of moving this off the device is that a /v1/aircraft request must
never wait on adsbdb, so that is tested explicitly rather than assumed.
adsbdb itself is stubbed - these tests must not depend on a third party
being up, or on a real callsign still flying.
"""

import asyncio
import time

import httpx
import pytest

from app import routes


def _payload(origin="CFU", destination="EDI"):
    return {
        "response": {
            "flightroute": {
                "callsign": "EZY51NR",
                "airline": {"name": "easyJet", "icao": "EZY"},
                "origin": {
                    "iata_code": origin,
                    "icao_code": "LGKR",
                    "name": "Ioannis Kapodistrias International Airport",
                    "municipality": "Corfu",
                },
                "destination": {
                    "iata_code": destination,
                    "icao_code": "EGPH",
                    "name": "Edinburgh Airport",
                    "municipality": "Edinburgh",
                },
            }
        }
    }


def _resolver(handler) -> routes.RouteResolver:
    resolver = routes.RouteResolver()
    resolver._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    resolver._workers = [
        asyncio.create_task(resolver._worker()) for _ in range(routes.MAX_CONCURRENT_LOOKUPS)
    ]
    return resolver


async def _settle(resolver):
    await asyncio.wait_for(resolver._queue.join(), timeout=5)


@pytest.mark.asyncio
async def test_first_lookup_returns_nothing_then_resolves():
    resolver = _resolver(lambda request: httpx.Response(200, json=_payload()))
    try:
        # Nothing cached yet, so the caller gets no route and is not blocked.
        assert resolver.lookup("EZY51NR") is None
        await _settle(resolver)
        route = resolver.lookup("EZY51NR")
        assert route["origin"] == "CFU"
        assert route["destination"] == "EDI"
        assert route["origin_city"] == "Corfu"
        # Comes from the same adsbdb response as the route, so the device
        # never needs its own airline table.
        assert route["airline"] == "easyJet"
        assert route["destination_name"] == "Edinburgh Airport"
    finally:
        await resolver.stop()


@pytest.mark.asyncio
async def test_lookup_never_blocks_on_a_slow_upstream():
    """The whole reason this moved off the ESP32 - a slow adsbdb must not
    become a slow /v1/aircraft."""

    async def slow(request):
        await asyncio.sleep(3)
        return httpx.Response(200, json=_payload())

    resolver = _resolver(slow)
    try:
        started = time.monotonic()
        for _ in range(50):
            resolver.lookup("EZY51NR")
        assert time.monotonic() - started < 0.1
    finally:
        await resolver.stop()


@pytest.mark.asyncio
async def test_one_upstream_request_per_callsign_however_many_callers():
    calls = []

    def handler(request):
        calls.append(str(request.url))
        return httpx.Response(200, json=_payload())

    resolver = _resolver(handler)
    try:
        # Every device in range sees the same flight on the same poll.
        for _ in range(25):
            resolver.lookup("EZY51NR")
        await _settle(resolver)
        for _ in range(25):
            resolver.lookup("EZY51NR")
        await _settle(resolver)
        assert len(calls) == 1, calls
    finally:
        await resolver.stop()


@pytest.mark.asyncio
async def test_404_is_remembered_as_no_route():
    calls = []

    def handler(request):
        calls.append(str(request.url))
        return httpx.Response(404, json={"response": "unknown callsign"})

    resolver = _resolver(handler)
    try:
        resolver.lookup("ZZZZZZ")
        await _settle(resolver)
        assert resolver.lookup("ZZZZZZ") is None
        await _settle(resolver)
        # Cached as a miss, so it is not re-asked on every single poll.
        assert len(calls) == 1, calls
    finally:
        await resolver.stop()


@pytest.mark.asyncio
async def test_transport_failure_is_not_cached_as_no_route():
    """A network blip must not hide a real route for the whole miss TTL."""
    attempts = []

    def handler(request):
        attempts.append(1)
        if len(attempts) == 1:
            raise httpx.ConnectError("boom")
        return httpx.Response(200, json=_payload())

    resolver = _resolver(handler)
    try:
        resolver.lookup("EZY51NR")
        await _settle(resolver)
        assert resolver.lookup("EZY51NR") is None  # still unknown, and re-queued
        await _settle(resolver)
        assert resolver.lookup("EZY51NR")["origin"] == "CFU"
    finally:
        await resolver.stop()


@pytest.mark.asyncio
async def test_stale_but_known_route_is_still_served_while_refreshing():
    resolver = _resolver(lambda request: httpx.Response(200, json=_payload()))
    try:
        resolver.lookup("EZY51NR")
        await _settle(resolver)
        resolver._cache["EZY51NR"].resolved_at = time.time() - routes.ROUTE_TTL_SECONDS - 1
        # Expired, but showing yesterday's airport pair beats showing nothing.
        assert resolver.lookup("EZY51NR")["origin"] == "CFU"
    finally:
        await resolver.stop()


@pytest.mark.asyncio
async def test_padded_and_lowercase_callsigns_hit_the_same_entry():
    calls = []

    def handler(request):
        calls.append(str(request.url))
        return httpx.Response(200, json=_payload())

    resolver = _resolver(handler)
    try:
        resolver.lookup("EZY51NR")
        await _settle(resolver)
        # The feeds pad to eight characters and case varies by provider.
        assert resolver.lookup("ezy51nr")["origin"] == "CFU"
        assert resolver.lookup("EZY51NR ")["origin"] == "CFU"
        await _settle(resolver)
        assert len(calls) == 1, calls
    finally:
        await resolver.stop()


@pytest.mark.asyncio
async def test_short_or_missing_callsigns_are_ignored():
    resolver = _resolver(lambda request: httpx.Response(200, json=_payload()))
    try:
        assert resolver.lookup(None) is None
        assert resolver.lookup("") is None
        assert resolver.lookup("AB") is None
        assert resolver._queue.qsize() == 0
    finally:
        await resolver.stop()
