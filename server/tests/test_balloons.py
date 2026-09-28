"""Balloons: radiosondes, amateur flights, and lighter-than-air on ADS-B.

The conversions are what these mostly guard. SondeHub reports altitude in
METRES and velocities in metres per second, and a balloon shown at
"30,000 ft" when it is at 30,000 m looks entirely plausible - which is
exactly why it would never be noticed.
"""

import time

import httpx
import pytest

from app import balloons
from app.balloons import BalloonTracker, parse_sondehub
from app.runtime_settings import DEFINITIONS, SettingsStore

# One real radiosonde reply, field for field as api.v2.sondehub.org
# returned it, so the parser is tested against the actual shape rather
# than against my idea of it.
SONDE_REPLY = {
    "310-2-03744": {
        "software_name": "radiosonde_auto_rx", "uploader_callsign": "KernowMan",
        "time_received": "2026-09-27T14:18:40.691022Z",
        "datetime": "2026-09-27T14:18:58.000000Z",
        "manufacturer": "Meteomodem", "type": "M20", "serial": "310-2-03744",
        "lat": 49.4912, "lon": -2.86412, "alt": 888.84,
        "temp": 12.8, "humidity": 81.6, "pressure": 921.31,
        "vel_v": -3.41, "vel_h": 11.64162, "heading": 92.5601,
        "frequency": 404.002625,
    }
}

AMATEUR_REPLY = {
    "SP0LND-4": {
        "software_name": "SondeHub APRS-IS Gateway", "uploader_callsign": "SQ2CPA-4",
        "payload_callsign": "SP0LND-4",
        "datetime": "2026-09-27T13:23:51.000000Z",
        "lat": 53.344166666666666, "lon": 17.643833333333333, "alt": 154.8384,
        "modulation": "APRS",
    }
}


def _fresh(reply: dict) -> dict:
    """The same captured reply, stamped now.

    The fixtures keep the real timestamps so the parser is tested against
    what SondeHub actually sent, but a poll prunes anything older than its
    stale window - so a test about polling has to use telemetry that is
    current, or it would pass or fail depending on the time of day.
    """
    stamp = time.strftime("%Y-%m-%dT%H:%M:%S.000000Z", time.gmtime())
    return {k: dict(v, datetime=stamp) for k, v in reply.items()}


def _settings(**overrides):
    store = SettingsStore()
    store._values.update(overrides)
    return store


def _tracker(**overrides):
    values = {"balloon_tracking": True, "balloon_sondes": True,
              "balloon_amateur": True, "balloon_adsb": False}
    values.update(overrides)
    return BalloonTracker(settings=_settings(**values))


# --- the units, which is where this would go quietly wrong --------------

def test_metres_become_feet_and_metres_per_second_become_knots():
    balloon, = parse_sondehub(SONDE_REPLY, "sonde")

    assert balloon.altitude_ft == 2916      # 888.84 m
    assert balloon.ground_kt == 22.6        # 11.64 m/s
    assert balloon.climb_fpm == -671        # -3.41 m/s, descending under canopy
    assert balloon.heading == 92.5601
    assert balloon.lat == 49.4912


def test_a_balloon_at_burst_altitude_is_not_confused_with_an_airliner():
    """The failure this is really about: 30,000 m read as 30,000 ft would
    put a radiosonde at airliner height and look entirely reasonable."""
    reply = {"x": dict(SONDE_REPLY["310-2-03744"], alt=30000.0)}

    balloon, = parse_sondehub(reply, "sonde")

    assert balloon.altitude_ft == 98425


def test_the_sonde_model_and_the_amateur_modulation_both_survive():
    sonde, = parse_sondehub(SONDE_REPLY, "sonde")
    amateur, = parse_sondehub(AMATEUR_REPLY, "amateur")

    assert (sonde.kind, sonde.detail, sonde.label) == ("sonde", "M20", "310-2-03744")
    assert (amateur.kind, amateur.detail, amateur.label) == \
        ("amateur", "APRS", "SP0LND-4")
    # Namespaced, because a radiosonde serial and an amateur callsign share
    # one store and could otherwise collide.
    assert sonde.id == "sonde:310-2-03744"
    assert amateur.id == "amateur:SP0LND-4"


def test_a_reply_without_a_position_is_skipped_not_raised_on():
    reply = {"a": {"alt": 1000.0}, "b": {"lat": 1.0}, "c": "not a dict"}

    assert parse_sondehub(reply, "sonde") == []
    assert parse_sondehub("not a dict", "sonde") == []


def test_an_unreadable_timestamp_counts_as_now():
    """Zero would prune the balloon immediately and hide a real flight."""
    reply = {"x": dict(SONDE_REPLY["310-2-03744"], datetime="not a date",
                       time_received=None)}

    balloon, = parse_sondehub(reply, "sonde")

    assert abs(balloon.reported_at - time.time()) < 5


# --- what a receiver is given -------------------------------------------

def test_only_the_fields_a_balloon_actually_reported_are_sent():
    """An amateur payload usually has no thermometer and often no speed;
    sending nulls for the difference costs the ESP32 parsing time for
    fields it cannot show."""
    amateur, = parse_sondehub(AMATEUR_REPLY, "amateur")

    sent = amateur.as_dict(time.time())

    assert "gs" not in sent and "climb" not in sent
    assert sent["alt"] == 508 and sent["kind"] == "amateur"


async def test_balloons_are_filtered_by_distance_from_the_receiver():
    tracker = _tracker()
    tracker._balloons = {b.id: b for b in parse_sondehub(SONDE_REPLY, "sonde")}

    near = tracker.query(49.5, -2.9, 50)
    far = tracker.query(53.73, -1.57, 50)

    assert len(near) == 1 and near[0]["id"] == "sonde:310-2-03744"
    assert far == []


async def test_a_balloon_nobody_has_heard_from_is_dropped():
    tracker = _tracker()
    balloon, = parse_sondehub(SONDE_REPLY, "sonde")
    balloon.reported_at = time.time() - balloons.STALE_AFTER_SECONDS - 60
    tracker._balloons = {balloon.id: balloon}

    assert tracker.prune() == 1
    assert tracker.stats()["tracked"] == 0


# --- polling ------------------------------------------------------------

async def test_both_sondehub_endpoints_are_asked_with_a_metre_radius():
    asked = []

    def handler(request: httpx.Request) -> httpx.Response:
        asked.append(request.url)
        body = _fresh(SONDE_REPLY if "amateur" not in str(request.url) else AMATEUR_REPLY)
        return httpx.Response(200, json=body)

    tracker = _tracker(balloon_radius_nm=100.0, balloon_predictions=False)
    tracker._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    tracker._regions = lambda: [(53.73, -1.57, 50.0)]

    await tracker.poll_once()

    assert len(asked) == 2
    # SondeHub takes metres; 100 nm is 185,200 of them.
    assert asked[0].params["distance"] == "185200"
    assert tracker.stats()["tracked"] == 2
    assert tracker.stats()["kinds"] == {"sonde": 1, "amateur": 1}


async def test_a_source_switched_off_is_not_asked_for():
    asked = []

    def handler(request: httpx.Request) -> httpx.Response:
        asked.append(str(request.url))
        return httpx.Response(200, json={})

    tracker = _tracker(balloon_amateur=False)
    tracker._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    tracker._regions = lambda: [(53.73, -1.57, 50.0)]

    await tracker.poll_once()

    assert len(asked) == 1 and "amateur" not in asked[0]


async def test_an_outage_is_recorded_rather_than_raised():
    """One bad cycle must not end the only thing filling this."""
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(503)

    tracker = _tracker()
    tracker._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    tracker._regions = lambda: [(53.73, -1.57, 50.0)]

    await tracker.poll_once()

    assert tracker.stats()["errors"] == 2
    assert "HTTPStatusError" in tracker.stats()["last_error"]


async def test_lighter_than_air_comes_from_the_aircraft_cache_for_free():
    """Airships and tethered balloons that DO carry a transponder are
    already in the aircraft poll - category B2 - so picking them out costs
    no extra request to anybody."""
    class _Cache:
        async def snapshot(self):
            return [
                {"hex": "4ca2d6", "lat": 53.7, "lon": -1.5, "category": "A3",
                 "alt_baro": 37000, "flight": "EZY51NR"},
                {"hex": "406b1a", "lat": 53.6, "lon": -1.6, "category": "B2",
                 "alt_baro": 1200, "flight": "GOODYEAR", "seen": 4},
            ]

    tracker = _tracker(balloon_adsb=True, balloon_sondes=False, balloon_amateur=False)
    tracker._cache = _Cache()
    tracker._regions = lambda: [(53.73, -1.57, 50.0)]

    await tracker.poll_once()

    tracked = tracker.query(53.73, -1.57, 100)
    assert len(tracked) == 1
    assert tracked[0]["id"] == "adsb:406b1a"
    assert tracked[0]["kind"] == "airship"
    assert tracked[0]["name"] == "GOODYEAR"


# --- the setting and the endpoint ---------------------------------------

def test_balloon_tracking_is_off_until_switched_on():
    assert BalloonTracker(settings=_settings()).enabled() is False
    assert _tracker().enabled() is True
    # And a tracker with no settings at all does nothing rather than
    # polling a free service on somebody's behalf uninvited.
    assert BalloonTracker().enabled() is False


def test_the_settings_page_offers_the_balloon_controls():
    names = {d.name for d in DEFINITIONS if d.group == "Balloons"}

    assert names == {"balloon_tracking", "balloon_sondes", "balloon_amateur",
                     "balloon_adsb", "balloon_predictions", "balloon_radius_nm",
                     "balloon_poll_seconds"}
    # No API key among them: SondeHub's GET endpoints are open, and the one
    # obvious keyed alternative cannot do geographic queries at all.
    assert not any(d.secret for d in DEFINITIONS if d.group == "Balloons")


def test_switched_off_says_so_rather_than_reporting_an_empty_sky(client):
    from app.main import app

    key = _issue_key(client)
    app.state.balloons._settings = _settings(balloon_tracking=False)

    response = client.get("/v1/balloons?lat=53.73&lon=-1.57&radius=250",
                          headers={"Authorization": f"Bearer {key}"})

    assert response.status_code == 200
    # "Off" and "nothing in range" are different things and must not look
    # identical to a receiver.
    assert response.json() == {"enabled": False, "balloons": [], "count": 0}


def test_the_endpoint_serves_what_is_flying(client):
    from app.main import app

    key = _issue_key(client)
    app.state.balloons._settings = _settings(balloon_tracking=True)
    app.state.balloons._balloons = {
        b.id: b for b in parse_sondehub(SONDE_REPLY, "sonde")}

    response = client.get("/v1/balloons?lat=49.5&lon=-2.9&radius=100",
                          headers={"Authorization": f"Bearer {key}"})

    body = response.json()
    assert body["enabled"] is True and body["count"] == 1
    assert body["balloons"][0]["alt"] == 2916


def test_balloons_need_a_device_key(client):
    assert client.get("/v1/balloons?lat=1&lon=2&radius=10").status_code in (401, 403)


def _issue_key(client) -> str:
    client.post("/signup", data={"email": "balloon@example.com", "password": "correct-horse"},
                follow_redirects=False)
    client.post("/devices", data={"name": "Balloon test"}, follow_redirects=False)

    from app import models, security
    from app.database import SessionLocal

    db = SessionLocal()
    try:
        device_id = db.query(models.Device).first().id
    finally:
        db.close()
    client.post(f"/devices/{device_id}/reissue-key", follow_redirects=False)
    return security.read_flash_token(client.cookies.get("flash_key"))["key"]


# --- landing predictions -------------------------------------------------
#
# The point of the whole balloon page for a sonde chaser: a radiosonde is
# free to recover, and "lands 12 miles away in 40 minutes" is actionable in
# a way that a dot on a map is not.

# One real record, as api.v2.sondehub.org/predictions returned it, with the
# forecast path cut to three points. Note `data` is a JSON STRING, not
# nested JSON - unusual enough that the parser is tested against it.
PREDICTION_REPLY = [{
    "vehicle": "Y1952527",
    "time": "2026-09-08T23:36:00Z",
    "latitude": 52.89886997547001, "longitude": -1.0164800193160772,
    "altitude": 5516.22, "ascent_rate": 4.51, "descent_rate": 3.7,
    "burst_altitude": 28700.0, "descending": 0, "landed": 0,
    "data": ('[{"time": 1788910560, "lat": 52.8988, "lon": -1.0164, "alt": 5516.22},'
             ' {"time": 1788910620, "lat": 52.8924, "lon": -0.9993, "alt": 5786.82},'
             ' {"time": 1788913000, "lat": 52.7100, "lon": -0.8100, "alt": 120.0}]'),
}]


def test_a_prediction_yields_the_end_of_the_forecast_path():
    from app.balloons import parse_predictions

    landings = parse_predictions(PREDICTION_REPLY)

    assert set(landings) == {"Y1952527"}
    landing = landings["Y1952527"]
    # The LAST point of the path - where it comes down - not the first.
    assert (landing["lat"], landing["lon"]) == (52.71, -0.81)
    assert landing["at"] == 1788913000
    # Burst altitude is metres here like everything else in this API.
    assert landing["burst_ft"] == 94160          # 28,700 m
    assert landing["descending"] is False


def test_a_malformed_prediction_is_skipped_not_raised_on():
    from app.balloons import parse_predictions

    assert parse_predictions("not a list") == {}
    assert parse_predictions([{"vehicle": "X", "data": "not json"}]) == {}
    assert parse_predictions([{"vehicle": "X", "data": "[]"}]) == {}
    assert parse_predictions([{"data": PREDICTION_REPLY[0]["data"]}]) == {}
    # A path whose last point has no position is no landing.
    assert parse_predictions([{"vehicle": "X", "data": '[{"time": 1}]'}]) == {}


async def test_a_landing_is_attached_to_its_sonde_and_sent_on():
    def handler(request: httpx.Request) -> httpx.Response:
        if "predictions" in str(request.url):
            return httpx.Response(200, json=PREDICTION_REPLY)
        if "amateur" in str(request.url):
            return httpx.Response(200, json={})
        return httpx.Response(200, json=_fresh(
            {"Y1952527": dict(SONDE_REPLY["310-2-03744"], serial="Y1952527")}))

    tracker = _tracker(balloon_predictions=True)
    tracker._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    tracker._regions = lambda: [(53.73, -1.57, 50.0)]

    await tracker.poll_once()

    balloon, = tracker.query(49.5, -2.9, 500)
    assert balloon["land"]["lat"] == 52.71
    assert balloon["burst"] == 94160
    assert balloon["descending"] is False
    # Seconds from now, not an absolute time: the receiver has no clock it
    # trusts, and "lands in 34 minutes" is the useful form.
    assert "in" in balloon["land"]


async def test_the_prediction_radius_is_clamped_to_what_the_api_accepts():
    """It refuses anything over 100 km, and the balloon radius defaults to
    250 nm - so passing it straight through would get every request
    rejected."""
    asked = []

    def handler(request: httpx.Request) -> httpx.Response:
        asked.append(request.url)
        if "predictions" in str(request.url):
            return httpx.Response(200, json=[])
        return httpx.Response(200, json=_fresh(SONDE_REPLY))

    tracker = _tracker(balloon_predictions=True, balloon_amateur=False,
                       balloon_radius_nm=250.0)
    tracker._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    tracker._regions = lambda: [(53.73, -1.57, 50.0)]

    await tracker.poll_once()

    prediction_call = [u for u in asked if "predictions" in str(u)][0]
    assert prediction_call.params["distance"] == "100000"


async def test_predictions_failing_does_not_lose_the_balloons():
    """A missing forecast costs a nicety, not the sonde."""
    def handler(request: httpx.Request) -> httpx.Response:
        if "predictions" in str(request.url):
            return httpx.Response(500)
        if "amateur" in str(request.url):
            return httpx.Response(200, json={})
        return httpx.Response(200, json=_fresh(SONDE_REPLY))

    tracker = _tracker(balloon_predictions=True)
    tracker._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    tracker._regions = lambda: [(53.73, -1.57, 50.0)]

    await tracker.poll_once()

    assert tracker.stats()["tracked"] == 1
    # Not counted against the poll: the sondes arrived fine.
    assert tracker.stats()["errors"] == 0


async def test_predictions_can_be_switched_off():
    asked = []

    def handler(request: httpx.Request) -> httpx.Response:
        asked.append(str(request.url))
        return httpx.Response(200, json=_fresh(SONDE_REPLY))

    tracker = _tracker(balloon_predictions=False, balloon_amateur=False)
    tracker._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    tracker._regions = lambda: [(53.73, -1.57, 50.0)]

    await tracker.poll_once()

    assert not any("predictions" in url for url in asked)
