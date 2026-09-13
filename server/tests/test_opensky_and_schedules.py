"""Two APIs the operator supplies keys for: OpenSky and AirLabs.

Both are off until a key is entered, both are polled or queued
server-side so no receiver ever waits on them, and neither may leak its
credential into a log, an audit entry or a rendered page.
"""

import time

import httpx
import pytest

from app import opensky
from app.runtime_settings import DEFINITIONS, SettingsStore, from_form
from app.schedules import Schedule, ScheduleResolver, extract, normalise


# --- OpenSky: SI units, which is where this goes wrong -----------------

def _state(icao="4ca2d6", callsign="RYR2BH  ", metres=11277.6, mps=231.5,
           vertical_mps=0.0, on_ground=False, last_contact=1700000095):
    """One state vector, in the fixed order OpenSky documents."""
    return [icao, callsign, "Ireland", 1700000090, last_contact, -1.57, 53.73,
            metres, on_ground, mps, 84.9, vertical_mps, None,
            metres + 305, "7000", False, 0, "A3"]


def test_metres_and_metres_per_second_become_feet_and_knots():
    """OpenSky reports SI; every other source here, the firmware and the
    display work in feet and knots. Merged unconverted, an airliner would
    sit at "11,000 ft" against adsb.fi's 36,000 and the two records would
    fight over the same hex on every poll."""
    entry, = opensky.normalise({"time": 1700000100, "states": [_state()]})

    assert entry["alt_baro"] == 37000        # 11,277.6 m
    assert entry["alt_geom"] == 38001        # +305 m, which is 1,000.7 ft
    assert entry["gs"] == 450.0              # 231.5 m/s
    assert entry["hex"] == "4ca2d6"
    assert entry["flight"] == "RYR2BH"       # the padding OpenSky sends is gone


def test_a_vertical_rate_becomes_feet_per_minute():
    """baro_rate means feet per minute everywhere else here. -5.08 m/s is a
    standard 1,000 fpm descent, and left in m/s it would read as -5."""
    entry, = opensky.normalise({"time": 1700000100, "states": [_state(vertical_mps=-5.08)]})

    assert entry["baro_rate"] == -1000


def test_an_aircraft_on_the_ground_says_ground():
    """The string every other source uses, and what the firmware tests for
    rather than a number."""
    entry, = opensky.normalise({"time": 1700000100,
                                "states": [_state(on_ground=True, metres=0.0)]})

    assert entry["alt_baro"] == "ground"


def test_seen_is_seconds_ago_not_a_timestamp():
    """The cache's freshness comparison is built on seconds-ago, and a
    source handing it a Unix timestamp would win every comparison for ever."""
    entry, = opensky.normalise({"time": 1700000100,
                                "states": [_state(last_contact=1700000080)]})

    assert entry["seen"] == 20


@pytest.mark.parametrize("payload", [
    {}, {"states": None}, {"states": []}, [], "nonsense", None,
    {"time": 1, "states": [["too", "short"]]},
    {"time": 1, "states": ["not a vector"]},
])
def test_a_malformed_response_yields_nothing_rather_than_raising(payload):
    """One bad vector in a response of hundreds should cost that aircraft,
    not the whole poll - this runs inside the shared poll loop."""
    assert opensky.normalise(payload) == []


def test_an_aircraft_with_no_position_is_dropped():
    state = _state()
    state[opensky.LATITUDE] = None
    assert opensky.normalise({"time": 1, "states": [state]}) == []


async def test_the_bounding_box_widens_with_latitude():
    """A degree of longitude is 60 nm at the equator and 35 at 54 north. A
    box square in degrees would be far narrower than asked for on the
    ground, so the requested area would quietly shrink the further north
    the receiver is."""
    seen = {}

    async def handler(request):
        seen.update(dict(request.url.params))
        return httpx.Response(200, json={"time": 1, "states": []})

    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    store = opensky.OpenSkyClient()
    store._token = "cached"
    store._token_expires_at = time.time() + 600

    await store.fetch_region(client, "id", "secret", 53.73, -1.57, 60.0)
    await client.aclose()

    latitude_span = float(seen["lamax"]) - float(seen["lamin"])
    longitude_span = float(seen["lomax"]) - float(seen["lomin"])
    assert round(latitude_span, 3) == 2.0          # 60 nm each way
    assert longitude_span > latitude_span * 1.5    # widened by the cosine


async def test_a_rejected_token_is_discarded_so_the_next_cycle_refetches():
    """Retrying a string the server has already refused just spends another
    request to be refused again."""
    async def handler(request):
        return httpx.Response(401, json={})

    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    store = opensky.OpenSkyClient()
    store._token = "stale"
    store._token_expires_at = time.time() + 600

    with pytest.raises(httpx.HTTPStatusError):
        await store.fetch_region(client, "id", "secret", 53.73, -1.57, 50.0)
    await client.aclose()

    assert store._token == ""


async def test_a_token_is_fetched_once_and_reused():
    calls = []

    async def handler(request):
        calls.append(str(request.url))
        if "token" in str(request.url):
            return httpx.Response(200, json={"access_token": "abc", "expires_in": 1800})
        return httpx.Response(200, json={"time": 1, "states": []})

    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    store = opensky.OpenSkyClient()
    for _ in range(3):
        await store.fetch_region(client, "id", "secret", 53.73, -1.57, 50.0)
    await client.aclose()

    assert sum(1 for url in calls if "token" in url) == 1, "a token per poll"


def test_opensky_needs_both_halves_of_the_credential():
    store = opensky.OpenSkyClient()
    assert store.configured("id", "secret") is True
    assert store.configured("id", "") is False
    assert store.configured("", "secret") is False


# --- AirLabs: which callsigns are worth a lookup -----------------------

@pytest.mark.parametrize("callsign,expected", [
    ("RYR2BH", "RYR2BH"),
    ("ryr2bh", "RYR2BH"),
    ("  BAW123  ", "BAW123"),
    ("UAL935", "UAL935"),
    ("G-CEMY", ""),      # a registration flown as a callsign
    ("GCEMY", ""),       # ...and without its hyphen
    ("N790AN", ""),
    ("", ""),
    (None, ""),
    ("X", ""),
])
def test_only_callsigns_that_look_like_flights_are_looked_up(callsign, expected):
    """Every receiver sees registrations, military and gliders flying as
    callsigns. Each would cost a lookup off a metered allowance and always
    come back empty."""
    assert normalise(callsign) == expected


# --- AirLabs: the envelope nobody can agree on -------------------------

FLIGHT = {"dep_iata": "DUB", "arr_iata": "LBA", "dep_gate": "B32",
          "arr_terminal": "1", "arr_baggage": "4", "status": "en-route",
          "dep_delayed": 15, "arr_delayed": 0, "flight_icao": "RYR2BH"}


@pytest.mark.parametrize("payload", [
    FLIGHT,                      # a bare object, as the endpoint docs show
    {"response": FLIGHT},        # wrapped, as AirLabs does elsewhere
    {"response": [FLIGHT]},      # a one-element list
    [FLIGHT],
])
def test_every_envelope_shape_is_accepted(payload):
    """The endpoint documentation shows a bare object while AirLabs wraps
    payloads in "response" elsewhere, and a single-flight query can
    reasonably answer with a one-element list. This cannot be tried against
    the real API without a key, so all three are accepted rather than
    betting on one."""
    schedule = extract(payload)

    assert schedule["dep_iata"] == "DUB"
    assert schedule["arr_iata"] == "LBA"
    assert schedule["dep_gate"] == "B32"
    assert schedule["status"] == "en-route"


def test_a_zero_delay_is_kept_but_an_empty_field_is_not():
    """"On time" is worth showing; a field the airline left blank is not."""
    schedule = extract({"dep_delayed": 0, "arr_gate": "", "dep_gate": None,
                        "dep_iata": "DUB"})

    assert schedule["dep_delayed"] == 0
    assert "arr_gate" not in schedule
    assert "dep_gate" not in schedule


@pytest.mark.parametrize("payload", [
    {"error": {"message": "Unknown flight"}}, {}, None, "nonsense", [], [None],
])
def test_nothing_usable_is_no_schedule_rather_than_an_empty_one(payload):
    assert extract(payload) is None


def test_fields_the_panel_cannot_use_are_dropped():
    """The response carries a position and a velocity too, which this
    pipeline already has from the aircraft itself and better."""
    schedule = extract({**FLIGHT, "lat": 53.7, "lng": -1.5, "alt": 11000,
                        "speed": 450, "hex": "4ca2d6"})

    for unwanted in ("lat", "lng", "alt", "speed", "hex"):
        assert unwanted not in schedule


# --- AirLabs: off until configured, and never blocking -----------------

def _resolver(enabled=True, key="test-key", minutes=30):
    settings = SettingsStore()
    settings._values["airlabs_schedules"] = enabled
    settings._values["airlabs_api_key"] = key
    settings._values["airlabs_cache_minutes"] = minutes
    return ScheduleResolver(settings)


def test_nothing_happens_until_a_key_is_entered():
    assert _resolver(enabled=True, key="").enabled() is False
    assert _resolver(enabled=False, key="test-key").enabled() is False
    assert _resolver().enabled() is True
    # And with no settings store at all - what a bare Aggregator gets.
    assert ScheduleResolver().enabled() is False


def test_a_lookup_never_waits_and_queues_instead():
    """Called once per aircraft while building a response. Waiting here
    would move the stall off the ESP32 and onto the server, where every
    device would feel it at once."""
    resolver = _resolver()

    assert resolver.lookup("RYR2BH") is None
    assert resolver._queue.qsize() == 1


def test_the_same_flight_is_only_queued_once():
    resolver = _resolver()
    for _ in range(5):
        resolver.lookup("RYR2BH")
    assert resolver._queue.qsize() == 1


def test_a_cached_schedule_comes_back_without_another_lookup():
    resolver = _resolver()
    resolver._cache["RYR2BH"] = Schedule(FLIGHT, time.time())

    assert resolver.lookup("RYR2BH")["dep_gate"] == "B32"
    assert resolver._queue.qsize() == 0, "a fresh cache entry was re-queued"
    assert resolver.hits == 1


def test_a_stale_schedule_is_still_served_while_it_refreshes():
    """Blanking a gate mid-flight because the cache window lapsed is worse
    than showing one a few minutes old."""

    resolver = _resolver(minutes=5)
    resolver._cache["RYR2BH"] = Schedule(FLIGHT, time.time() - 600)

    assert resolver.lookup("RYR2BH")["dep_gate"] == "B32"
    assert resolver._queue.qsize() == 1, "a stale entry should queue a refresh"


def test_a_flight_with_no_schedule_is_not_asked_about_again_immediately():
    """Most of what a receiver sees has no schedule at all, and re-asking
    for those every poll would spend a metered allowance on answers that
    never change."""

    resolver = _resolver()
    resolver._cache["RYR2BH"] = Schedule(None, time.time())

    assert resolver.lookup("RYR2BH") is None
    assert resolver._queue.qsize() == 0


async def test_a_non_200_is_recorded_as_a_miss_rather_than_retried_forever():
    """A rate limit or an outage would otherwise become one lookup per
    aircraft per poll for as long as it lasts."""
    resolver = _resolver()

    async def handler(request):
        return httpx.Response(429, json={"error": "too many requests"})

    resolver._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    await resolver._fetch("RYR2BH")
    await resolver._client.aclose()

    assert resolver._cache["RYR2BH"].data is None
    assert resolver.lookup("RYR2BH") is None
    assert resolver._queue.qsize() == 0


async def test_a_successful_lookup_is_cached_and_served():
    resolver = _resolver()

    async def handler(request):
        assert request.url.params["flight_icao"] == "RYR2BH"
        assert request.url.params["api_key"] == "test-key"
        return httpx.Response(200, json={"response": FLIGHT})

    resolver._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    await resolver._fetch("RYR2BH")
    await resolver._client.aclose()

    schedule = resolver.lookup("RYR2BH")
    assert schedule["arr_baggage"] == "4"
    assert resolver.lookups == 1


def test_a_full_queue_drops_rather_than_blocks():
    resolver = _resolver()
    for n in range(resolver._queue.maxsize + 20):
        resolver.lookup(f"RYR{n:04d}")

    assert resolver._queue.qsize() == resolver._queue.maxsize
    assert resolver.dropped == 20


# --- the keys themselves -----------------------------------------------

def test_a_key_is_stored_as_a_secret_kind():
    secrets = {d.name for d in DEFINITIONS if d.secret}
    assert secrets == {"opensky_client_id", "opensky_client_secret", "airlabs_api_key"}


def test_saving_the_form_with_a_blank_key_box_keeps_the_stored_key():
    """The form never renders a key back, so its field arrives empty on
    every save. Treating that as "clear it" would wipe every API key the
    first time somebody changed the poll interval."""
    submitted = from_form({"poll_interval_seconds": "15", "airlabs_api_key": ""})

    assert "airlabs_api_key" not in submitted
    assert "opensky_client_secret" not in submitted


def test_clearing_a_key_takes_its_own_checkbox():
    submitted = from_form({"airlabs_api_key": "", "airlabs_api_key__clear": "on"})
    assert submitted["airlabs_api_key"] == ""


def test_a_pasted_key_loses_its_stray_whitespace():
    """A key that differs from the real one by one trailing newline fails in
    a way nobody enjoys debugging."""
    definition = next(d for d in DEFINITIONS if d.name == "airlabs_api_key")
    assert definition.parse("  abc123\n ") == "abc123"


def test_an_absurdly_long_key_is_refused():
    definition = next(d for d in DEFINITIONS if d.name == "airlabs_api_key")
    with pytest.raises(ValueError):
        definition.parse("x" * 500)
