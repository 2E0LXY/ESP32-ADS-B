"""Aircraft type photographs, restricted to licences that need no attribution.

The candidate filter is the substance here. Searching by model name returns
engine close-ups, cabins and museum pieces alongside aircraft, so these tests
feed it the kinds of result the live API actually returns.
"""

import io
import json
import struct

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

    async def get(self, url, params=None, timeout=None):
        request = httpx.Request("GET", url)
        if "openverse" in url:
            self.searches += 1
            return httpx.Response(200, json={"results": self.results}, request=request)
        self.downloads.append(url)
        return httpx.Response(200, content=self.image, request=request)

    async def aclose(self):
        pass


def _store(tmp_path, upstream):
    store = PhotoStore(cache_dir=str(tmp_path))
    store._client = upstream
    return store


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

    first = await store.photo("B738", "Boeing 737-800", 240)
    second = await store.photo("B738", "Boeing 737-800", 240)

    assert first == second and first.startswith(b"\x89PNG")
    assert upstream.searches == 1, "the cache did not save the API call"
    assert store.hits == 1 and store.fetches == 1


async def test_provenance_is_recorded_even_though_attribution_is_not_required(tmp_path):
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = _store(tmp_path, upstream)
    await store.photo("B738", "Boeing 737-800", 240)

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

    assert await store.photo("B738", "Boeing 737-800", 240) is not None
    assert upstream.downloads == ["https://example.invalid/good.jpg"]
    assert store.rejected == 2


async def test_no_usable_candidate_records_a_miss(tmp_path):
    upstream = _FakeOpenverse([_result("Boeing 737-800 engine")])
    store = _store(tmp_path, upstream)

    assert await store.photo("B738", "Boeing 737-800", 240) is None
    assert (tmp_path / "B738-240.miss").exists()
    assert await store.photo("B738", "Boeing 737-800", 240) is None
    assert upstream.searches == 1, "a known-missing photo was searched for twice"


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
    assert await store.photo("B738", "Boeing 737-800", 240) is None
    assert await store.photo("B738", "Boeing 737-800", 240) is None
    assert store._client.calls == 2
    assert not (tmp_path / "B738-240.miss").exists()


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
    await store.photo("B738", "Boeing 737-800", 240)
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
    assert await store.photo("b738", "Boeing 737-800", 240) is not None
    assert [p.name for p in tmp_path.glob("*.png")] == ["B738-240.png"]


async def test_an_odd_width_falls_back_to_the_default(tmp_path):
    """Otherwise a caller could fill the disk asking for every pixel width."""
    upstream = _FakeOpenverse([_result("Boeing 737-800")])
    store = _store(tmp_path, upstream)
    await store.photo("B738", "Boeing 737-800", 999)
    assert [p.name for p in tmp_path.glob("*.png")] == ["B738-240.png"]
