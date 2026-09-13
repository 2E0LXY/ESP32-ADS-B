"""Aircraft type photographs, restricted to licences that need no attribution.

The candidate filter is the substance here. Searching by model name returns
engine close-ups, cabins and museum pieces alongside aircraft, so these tests
feed it the kinds of result the live API actually returns.
"""

import io
import json
import struct
import pathlib

import httpx
import pytest

from app.photos import PhotoStore, _is_plausible_photo, _to_panel_png


def _jpeg(width=1024, height=683):
    from PIL import Image
    out = io.BytesIO()
    Image.new("RGB", (width, height), (30, 90, 160)).save(out, format="JPEG")
    return out.getvalue()


def _result(title, width=1024, height=683, licence="cc0", url="https://example.invalid/a.jpg"):
    return {"title": title, "width": width, "height": height, "license": licence,
            "url": url, "source": "flickr", "creator": "Someone",
            "foreign_landing_url": "https://example.invalid/page"}


class _FakeOpenverse:
    """Stands in for the search API and the image host."""

    def __init__(self, results, image=None):
        self.results = results
        self.image = image if image is not None else _jpeg()
        self.searches = 0
        self.downloads = []
        # What was actually asked for, so a test can tell an operator search
        # from a plain type search.
        self.queries = []

    async def get(self, url, params=None, timeout=None):
        request = httpx.Request("GET", url)
        if "openverse" in url:
            self.searches += 1
            self.queries.append((params or {}).get("q"))
            return httpx.Response(200, json={"results": self.results}, request=request)
        self.downloads.append(url)
        return httpx.Response(200, content=self.image, request=request)

    async def aclose(self):
        pass


def _store(tmp_path, upstream):
    """A store whose worker is not running: the tests drive _fetch directly
    where they are testing the fetch, and photo() where they are testing the
    queueing contract."""
    store = PhotoStore(cache_dir=str(tmp_path))
    store._client = upstream
    return store


async def _fetch_now(store, code, model, width=240):
    """What the background worker does, run inline so a test can assert on
    the result rather than waiting on a task."""
    return await store._fetch(code, model, width)


# --- the candidate filter ------------------------------------------------

@pytest.mark.parametrize("title", [
    "Boeing 737-800 Engine Nacelle and Wing",
    "Engine of an Airbus A320 - Volotea",
    "Airbus A320 cockpit",
    "Boeing 737 cabin interior",
    "Cessna 172 landing gear detail",
    "Airbus A320 model diecast",
    "Boeing 737-800 in a museum",
    "A320 flight simulator",
])
def test_details_and_non_aircraft_are_rejected(title):
    """These are all real shapes of title the live search returns."""
    model = title.split()[0] + " " + title.split()[1]
    ok, why = _is_plausible_photo(_result(title), model)
    assert not ok, f"accepted {title!r}"
    assert why


@pytest.mark.parametrize("title", [
    "Boeing 737-800",
    "Boeing 737-800 JA81AN",
    "VH-VFQ Jetstar Airways Airbus A320-232",
    "F-HFBC - Cessna 172 - CAPAM",
])
def test_whole_aircraft_photos_are_kept(title):
    model = "Boeing 737-800" if "737" in title else ("Airbus A320" if "A320" in title else "Cessna 172")
    ok, why = _is_plausible_photo(_result(title), model)
    assert ok, f"rejected {title!r}: {why}"


def test_portrait_and_tiny_frames_are_rejected():
    """A portrait frame is usually a tail, a detail, or a person standing by
    an aeroplane rather than the aircraft in view."""
    assert not _is_plausible_photo(_result("Boeing 737-800", 600, 900), "Boeing 737-800")[0]
    assert not _is_plausible_photo(_result("Boeing 737-800", 320, 213), "Boeing 737-800")[0]


def test_a_title_that_never_mentions_the_model_is_rejected():
    """Full-text search is generous: an airport article mentioning a 737
    matches a query for one."""
    ok, why = _is_plausible_photo(_result("Sunset over the apron"), "Boeing 737-800")
    assert not ok and "model" in why


# --- conversion ----------------------------------------------------------

def test_conversion_produces_a_palette_png_of_the_right_shape():
    data = _to_panel_png(_jpeg(1600, 900), 240)
    assert data.startswith(b"\x89PNG")
    width, height = struct.unpack(">II", data[16:24])
    assert (width, height) == (240, 144)          # the panel band, 5:3
    assert data[25] == 3, "not a palette PNG, which PNGdec handles best"
    # A truecolour frame of this size came out at 61 KB, which is wasteful for
    # a 16-bit panel that cannot show the extra colours.
    assert len(data) < 45_000


def test_a_portrait_source_is_cropped_not_squashed():
    data = _to_panel_png(_jpeg(600, 1200), 240)
    width, height = struct.unpack(">II", data[16:24])
    assert (width, height) == (240, 144)


def test_rubbish_bytes_convert_to_nothing_rather_than_raising():
    assert _to_panel_png(b"not an image at all", 240) is None


# --- the store -----------------------------------------------------------

async def test_a_photo_is_fetched_once_then_served_from_disk(tmp_path):
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = _store(tmp_path, upstream)

    first = await _fetch_now(store, "B738", "Boeing 737-800")
    # Once cached, photo() answers from disk without touching the upstream.
    second = await store.photo("B738", "Boeing 737-800", 240)

    assert first == second and first.startswith(b"\x89PNG")
    assert upstream.searches == 1, "the cache did not save the API call"
    assert store.hits == 1 and store.fetches == 1


async def test_provenance_is_recorded_even_though_attribution_is_not_required(tmp_path):
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = _store(tmp_path, upstream)
    await _fetch_now(store, "B738", "Boeing 737-800")

    recorded = json.loads((tmp_path / "B738-240.json").read_text())
    assert recorded["licence"] == "cc0"
    assert recorded["source_url"].startswith("https://")
    assert recorded["creator"] == "Someone"
    assert store.credits()[0]["designator"] == "B738"


async def test_the_first_acceptable_candidate_wins(tmp_path):
    """Two unusable results ahead of a good one must not stop the fetch."""
    upstream = _FakeOpenverse([
        _result("Boeing 737-800 Engine Nacelle"),
        _result("Boeing 737 cockpit"),
        _result("Boeing 737-800", url="https://example.invalid/good.jpg"),
    ])
    store = _store(tmp_path, upstream)

    assert await _fetch_now(store, "B738", "Boeing 737-800") is not None
    assert upstream.downloads == ["https://example.invalid/good.jpg"]
    assert store.rejected == 2


async def test_no_usable_candidate_records_a_miss(tmp_path):
    upstream = _FakeOpenverse([_result("Boeing 737-800 engine")])
    store = _store(tmp_path, upstream)

    assert await _fetch_now(store, "B738", "Boeing 737-800") is None
    assert (tmp_path / "B738-240.miss").exists()
    # photo() now answers from the marker without queueing anything.
    assert await store.photo("B738", "Boeing 737-800", 240) is None
    assert upstream.searches == 1, "a known-missing photo was searched for twice"
    assert store.misses == 1


async def test_a_search_failure_is_not_cached_as_a_miss(tmp_path):
    """A failed search is not a type without a photograph, and caching it as
    one would hide a real photo for a month."""

    class _Broken:
        def __init__(self):
            self.calls = 0

        async def get(self, url, params=None, timeout=None):
            self.calls += 1
            raise httpx.ConnectError("no route to host")

    store = _store(tmp_path, _Broken())
    assert await _fetch_now(store, "B738", "Boeing 737-800") is None
    assert await _fetch_now(store, "B738", "Boeing 737-800") is None
    assert store._client.calls == 2
    assert not (tmp_path / "B738-240.miss").exists()
    # And it says so, rather than only at DEBUG where a permanent failure
    # would have been invisible.
    assert store.search_failures == 2


async def test_only_attribution_free_licences_are_ever_requested(tmp_path):
    """The entire reason this uses Openverse rather than Commons. If this
    ever sends by or by-sa it creates an obligation the panel cannot honour."""
    captured = {}

    class _Capturing(_FakeOpenverse):
        async def get(self, url, params=None, timeout=None):
            if params:
                captured.update(params)
            return await super().get(url, params, timeout)

    store = _store(tmp_path, _Capturing([_result("Boeing 737-800")]))
    await _fetch_now(store, "B738", "Boeing 737-800")
    assert captured["license"] == "cc0,pdm"


async def test_disabled_means_nothing_is_requested(tmp_path):
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = PhotoStore(cache_dir=str(tmp_path), enabled=False)
    store._client = upstream
    assert await store.photo("B738", "Boeing 737-800", 240) is None
    assert upstream.searches == 0
    assert store.configured() is False


async def test_a_malformed_designator_is_never_looked_up(tmp_path):
    """So the cache path cannot be steered by a request."""
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = _store(tmp_path, upstream)
    for code in ("", "../etc/passwd", "TOOLONG", "A", "B-38", "b 38"):
        assert await store.photo(code, "Boeing 737-800", 240) is None
    assert upstream.searches == 0
    assert not list(tmp_path.iterdir())


async def test_a_lowercase_designator_is_normalised(tmp_path):
    """Leniently, the same way the logo endpoints treat a callsign."""
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = _store(tmp_path, upstream)
    assert await _fetch_now(store, "b738".upper(), "Boeing 737-800") is not None
    assert [p.name for p in tmp_path.glob("*.png")] == ["B738-240.png"]
    # And photo() normalises before looking in the cache.
    assert await store.photo("b738", "Boeing 737-800", 240) is not None


async def test_an_odd_width_falls_back_to_the_default(tmp_path):
    """Otherwise a caller could fill the disk asking for every pixel width."""
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = _store(tmp_path, upstream)
    store.start()
    try:
        await store.photo("B738", "Boeing 737-800", 999)
        await store._queue.join()
    finally:
        await store.stop()
    assert [p.name for p in tmp_path.glob("*.png")] == ["B738-240.png"]


# --- the queueing contract ----------------------------------------------
#
# The workers are stopped in these tests before anything is queued. With them
# running the fake upstream answers instantly, so the queue drains before the
# assertion can read it and the test measures scheduling luck rather than
# behaviour.


async def _queue_without_workers(store):
    store.start()
    for task in store._workers:
        task.cancel()
    store._workers.clear()


async def test_a_caller_is_never_made_to_wait(tmp_path):
    """The first version searched and downloaded while the device held the
    connection open, which exceeded the ESP32's fifteen-second read timeout
    and failed with HTTPC_ERROR_READ_TIMEOUT. Now it queues and answers
    immediately, and the picture arrives on a later request."""
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = _store(tmp_path, upstream)
    await _queue_without_workers(store)
    try:
        assert await store.photo("B738", "Boeing 737-800", 240) is None
        assert upstream.searches == 0, "the caller did the fetch itself"
        assert store.stats()["queued"] == 1

        # What the worker would have done, and the result is then cached.
        await _fetch_now(store, "B738", "Boeing 737-800")
        assert (await store.photo("B738", "Boeing 737-800", 240)).startswith(b"\x89PNG")
    finally:
        await store.stop()


async def test_the_same_type_is_only_queued_once(tmp_path):
    """A map full of one type must not queue twenty identical searches."""
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = _store(tmp_path, upstream)
    await _queue_without_workers(store)
    try:
        for _ in range(20):
            await store.photo("B738", "Boeing 737-800", 240)
        assert store.stats()["queued"] == 1
        assert store._queue.qsize() == 1
    finally:
        await store.stop()


# --- what the endpoint tells the device --------------------------------

def _photo_client(client, monkeypatch, state):
    """Points the running app's photo store at a canned resolve()."""
    from app.main import app

    async def resolve(designator, model, width=240, airline=None, airline_name=None):
        if state != "ready":
            return None, state, "type"
        return b"\x89PNG fake", "ready", "airline" if airline else "type"

    monkeypatch.setattr(app.state.photos, "resolve", resolve)
    monkeypatch.setattr(app.state.photos, "configured", lambda: True)
    return client


def test_a_queued_photo_is_retry_later_not_not_found(client, monkeypatch):
    """The firmware treats 404 as final - it writes a marker and never asks
    again. The first request for any type is necessarily "queued, not
    fetched yet", so answering that with 404 blacklisted every type on the
    receiver the first time it asked, and the picture the queue fetched
    seconds later was never requested. This is the whole bug."""
    _photo_client(client, monkeypatch, "pending")

    response = client.get("/aircraft-photo/B738.png")

    assert response.status_code == 503
    assert response.headers["Retry-After"] == "30"


def test_a_type_with_no_free_photograph_is_not_found(client, monkeypatch):
    """The one case a caller may remember."""
    _photo_client(client, monkeypatch, "missing")

    response = client.get("/aircraft-photo/B738.png")

    assert response.status_code == 404
    # Says whether the search is working at all, because a systemic failure
    # looks identical from outside.
    assert "X-Photo-Search-Failures" in response.headers


def test_photographs_switched_off_are_retry_later_with_a_long_wait(client, monkeypatch):
    _photo_client(client, monkeypatch, "off")

    response = client.get("/aircraft-photo/B738.png")

    assert response.status_code == 503
    assert response.headers["Retry-After"] == "3600"


def test_a_cached_photograph_is_served(client, monkeypatch):
    _photo_client(client, monkeypatch, "ready")

    response = client.get("/aircraft-photo/B738.png")

    assert response.status_code == 200
    assert response.content.startswith(b"\x89PNG")


async def test_resolve_names_each_case(tmp_path):
    """The three states, from the store rather than the endpoint."""
    store = PhotoStore(cache_dir=str(tmp_path), enabled=True)
    store._client = object()  # not None, so queueing is reachable

    data, state, match = await store.resolve("B738", "Boeing 737-800", 240)
    assert (data, state, match) == (None, "pending", "type"), \
        "first ask must be pending, not missing"

    image_path, _meta, miss_path = store._paths("B738", 240)
    pathlib.Path(miss_path).write_text("")
    assert await store.resolve("B738", "Boeing 737-800", 240) == (None, "missing", "type")

    pathlib.Path(miss_path).unlink()
    pathlib.Path(image_path).write_bytes(b"\x89PNG cached")
    data, state, match = await store.resolve("B738", "Boeing 737-800", 240)
    assert (state, match) == ("ready", "type") and data == b"\x89PNG cached"

    disabled = PhotoStore(cache_dir=str(tmp_path), enabled=False)
    assert await disabled.resolve("B738", "Boeing 737-800", 240) == (None, "off", "type")


# --- the operator's own aircraft, not just the right type ----------------
#
# One photograph per type meant a Jet2 737-800 and a Ryanair 737-800 shared
# a picture, so a Jet2 flight was shown a Ryanair aeroplane. These cover the
# operator search, the fallback, and the title check that stops the search
# handing back the very fault it exists to fix.

def test_an_airline_name_becomes_useful_search_words():
    from app.photos import operator_terms

    assert operator_terms("Jet2.com") == ["jet2"]
    assert operator_terms("easyJet UK") == ["easyjet"]
    # Not trimmed harder than that: "Air France" must not become "France",
    # nor "British Airways" become "british", or the search matches the
    # wrong carrier's aeroplane.
    assert operator_terms("British Airways") == ["british", "airways"]
    assert operator_terms("Air France") == ["air", "france"]
    assert operator_terms("Ryanair (Malta Air)") == ["ryanair"]
    assert operator_terms(None) == []


def test_the_airline_must_be_named_in_the_title():
    """Searching "Jet2 Boeing 737-800" happily returns a Ryanair 737 -
    Openverse full-text search is generous - and caching that as Jet2's
    picture would reproduce the original fault having asked for the right
    thing."""
    from app.photos import _is_plausible_photo

    ryanair = _result("Boeing 737-800")  # title mentions the model only
    ok, reason = _is_plausible_photo(ryanair, "Boeing 737-800", ("jet2",))
    assert not ok and "jet2" in reason

    jet2 = _result("Jet2 Boeing 737-800 at Leeds Bradford")
    ok, _reason = _is_plausible_photo(jet2, "Boeing 737-800", ("jet2",))
    assert ok


async def test_the_operators_own_photograph_wins(tmp_path):
    store = _store(tmp_path, _FakeOpenverse([]))
    operator_image, _meta, _miss = store._paths("B738", 240, "EXS")
    pathlib.Path(operator_image).write_bytes(b"\x89PNG jet2")
    generic_image, _m, _s = store._paths("B738", 240)
    pathlib.Path(generic_image).write_bytes(b"\x89PNG someone else")

    data, state, match = await store.resolve(
        "B738", "Boeing 737-800", 240, "EXS", "Jet2.com")

    assert (state, match) == ("ready", "airline")
    assert data == b"\x89PNG jet2"
    assert store.operator_hits == 1


async def test_the_type_photograph_is_served_while_the_operators_is_queued(tmp_path):
    """A picture of the right type now beats no picture at all, and the
    caller is told it is only a type match so it can say so."""
    store = _store(tmp_path, _FakeOpenverse([]))
    await _queue_without_workers(store)
    try:
        generic_image, _m, _s = store._paths("B738", 240)
        pathlib.Path(generic_image).write_bytes(b"\x89PNG generic")

        data, state, match = await store.resolve(
            "B738", "Boeing 737-800", 240, "EXS", "Jet2.com")

        assert (state, match) == ("ready", "type")
        assert data == b"\x89PNG generic"
        assert store._queue.qsize() == 1, "the operator's own was not queued"
    finally:
        await store.stop()


async def test_an_operator_with_no_photograph_is_not_searched_again(tmp_path):
    """Most airline-and-type pairs have no attribution-free photograph, so
    re-searching every poll would spend the whole budget on answers that
    never change."""
    store = _store(tmp_path, _FakeOpenverse([]))
    await _queue_without_workers(store)
    try:
        _img, _meta, operator_miss = store._paths("B738", 240, "EXS")
        pathlib.Path(operator_miss).write_text("")
        generic_image, _m, _s = store._paths("B738", 240)
        pathlib.Path(generic_image).write_bytes(b"\x89PNG generic")

        data, state, match = await store.resolve(
            "B738", "Boeing 737-800", 240, "EXS", "Jet2.com")

        assert (data, state, match) == (b"\x89PNG generic", "ready", "type")
        assert store._queue.qsize() == 0, "a known-empty operator search was repeated"
    finally:
        await store.stop()


async def test_an_operator_search_asks_for_the_airline_and_checks_the_title(tmp_path):
    upstream = _FakeOpenverse([_result("Jet2 Boeing 737-800 at Leeds Bradford")])
    store = _store(tmp_path, upstream)

    data = await store._fetch("B738", "Boeing 737-800", 240, "EXS", "Jet2.com")

    assert data is not None and data.startswith(b"\x89PNG")
    assert upstream.queries[-1] == "jet2 Boeing 737-800"
    # Cached as the operator's, leaving the generic entry alone.
    operator_image, _meta, _miss = store._paths("B738", 240, "EXS")
    assert pathlib.Path(operator_image).exists()
    generic_image, _m, _s = store._paths("B738", 240)
    assert not pathlib.Path(generic_image).exists()


async def test_a_generic_result_is_rejected_for_an_operator_search(tmp_path):
    """The failure mode that matters: the search comes back with someone
    else's 737 and it must not be cached as this operator's."""
    upstream = _FakeOpenverse([_result("Boeing 737-800 on approach")])
    store = _store(tmp_path, upstream)

    data = await store._fetch("B738", "Boeing 737-800", 240, "EXS", "Jet2.com")

    assert data is None
    _img, _meta, operator_miss = store._paths("B738", 240, "EXS")
    assert pathlib.Path(operator_miss).exists(), "the empty search was not remembered"
    # And the request for the type itself still works, unaffected.
    assert await store._fetch("B738", "Boeing 737-800", 240) is not None


def test_the_endpoint_says_which_photograph_it_served(client, monkeypatch):
    _photo_client(client, monkeypatch, "ready")

    generic = client.get("/aircraft-photo/B738.png")
    operators = client.get("/aircraft-photo/B738.png?airline=EXS")

    assert generic.headers["X-Photo-Match"] == "type"
    assert operators.headers["X-Photo-Match"] == "airline"
