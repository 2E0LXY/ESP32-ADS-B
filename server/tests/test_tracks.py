"""Recent position history, for the trail drawn behind a selected aircraft.

The thing these are really guarding is memory: the store is the only part
of this service that grows with BOTH the number of aircraft and time, on a
VPS with 1 GB of RAM, so the caps and the pruning matter more than the
shape of the output.
"""

import math

import pytest

from app.tracks import MAX_POINTS, VALUES_PER_POINT, TrackStore, _altitude_feet


def _at(lat, lon, alt=35000, hex_id="4ca2d6"):
    return {"hex": hex_id, "lat": lat, "lon": lon, "alt_baro": alt}


def test_a_track_records_where_an_aircraft_has_been():
    store = TrackStore()
    store.observe([_at(53.70, -1.50)], now=1000.0)
    store.observe([_at(53.75, -1.55)], now=1015.0)

    points = store.get("4ca2d6", now=1030.0)

    assert len(points) == 2
    assert points[0][:3] == [53.7, -1.5, 35000]
    # Oldest first, and ages counted back from now.
    assert points[0][3] == 30.0
    assert points[1][3] == 15.0


def test_one_point_per_cycle_not_one_per_source():
    """An aircraft reported by adsb.fi, adsb.lol and OpenSky in the same
    cycle is one aircraft in one place. The store is fed from the merged
    cache once a cycle, so this is a property of where it is called from -
    but a second identical observation must not add a point either."""
    store = TrackStore()
    store.observe([_at(53.70, -1.50)], now=1000.0)
    store.observe([_at(53.70, -1.50)], now=1001.0)

    assert len(store.get("4ca2d6")) == 1


def test_an_aircraft_that_has_not_moved_does_not_accumulate_points():
    """Parked, or a position jittering on its last decimal. Without this a
    stationary aircraft would fill its whole track with one location."""
    store = TrackStore()
    for i in range(20):
        store.observe([_at(53.70, -1.50 + i * 0.00001)], now=1000.0 + i * 15)

    assert len(store.get("4ca2d6")) == 1


def test_a_stationary_aircraft_still_gets_an_occasional_point():
    """So a trail is not one long line with a two-hour gap in the middle."""
    store = TrackStore()
    store.observe([_at(53.70, -1.50)], now=1000.0)
    store.observe([_at(53.70, -1.50)], now=1000.0 + 130)

    assert len(store.get("4ca2d6")) == 2


def test_a_track_is_capped_and_drops_its_oldest_point():
    store = TrackStore()
    for i in range(MAX_POINTS + 50):
        store.observe([_at(53.70 + i * 0.01, -1.50)], now=1000.0 + i * 15)

    points = store.get("4ca2d6")
    assert len(points) == MAX_POINTS
    # The first 50 fell off the front, so the oldest kept is the 51st.
    assert points[0][0] == pytest.approx(53.70 + 50 * 0.01, abs=1e-4)


def test_the_store_is_bounded_by_aircraft_too():
    """A busy sky must not be able to grow this without limit."""
    store = TrackStore(max_aircraft=3)
    store.observe([_at(53.70, -1.50, hex_id=f"aaaa{i:02x}") for i in range(10)],
                  now=1000.0)

    assert store.stats()["aircraft"] == 3
    assert store.stats()["dropped"] == 7


def test_points_cost_sixteen_bytes_each():
    """The whole reason this uses array('f') rather than a list of tuples:
    a list of tuples is about 170 bytes a point, which at a few thousand
    aircraft is tens of megabytes on a 1 GB VPS."""
    store = TrackStore()
    for i in range(10):
        store.observe([_at(53.70 + i * 0.01, -1.50)], now=1000.0 + i * 15)

    stats = store.stats()
    assert stats["points"] == 10
    assert stats["bytes"] == 10 * VALUES_PER_POINT * 4 == 160


def test_a_track_is_dropped_once_nothing_has_been_added_for_long_enough():
    store = TrackStore()
    store.observe([_at(53.70, -1.50)], now=1000.0)

    assert store.prune(now=1000.0 + 60) == 0
    assert store.prune(now=1000.0 + 16 * 60) == 1
    assert store.get("4ca2d6") == []


def test_an_aircraft_with_no_position_is_ignored():
    store = TrackStore()
    store.observe([{"hex": "4ca2d6"}, {"lat": 53.7, "lon": -1.5}, {}], now=1000.0)

    assert store.stats()["aircraft"] == 0


def test_altitude_survives_ground_and_missing_readings():
    """'ground' is a string every source here uses, and a position-only
    report carries no altitude at all. Both have to fit a float array."""
    assert _altitude_feet("ground") == 0.0
    assert _altitude_feet(37000) == 37000.0
    assert math.isnan(_altitude_feet(None))
    assert math.isnan(_altitude_feet("cruising"))

    store = TrackStore()
    store.observe([_at(53.70, -1.50, alt=None)], now=1000.0)
    # Null rather than NaN, because JSON has no NaN and a caller has to be
    # able to tell "no reading" from "sea level".
    assert store.get("4ca2d6")[0][2] is None


def _issue_key(client) -> str:
    """A registered device's API key, the way the dashboard issues one."""
    client.post("/signup", data={"email": "track@example.com", "password": "correct-horse"},
                follow_redirects=False)
    client.post("/devices", data={"name": "Track test"}, follow_redirects=False)

    from app import models, security
    from app.database import SessionLocal

    db = SessionLocal()
    try:
        device_id = db.query(models.Device).first().id
    finally:
        db.close()
    client.post(f"/devices/{device_id}/reissue-key", follow_redirects=False)
    return security.read_flash_token(client.cookies.get("flash_key"))["key"]


def test_the_endpoint_serves_a_selected_aircrafts_trail(client):
    from app.main import app

    device_key = _issue_key(client)

    app.state.tracks.observe([_at(53.70, -1.50)], now=1000.0)
    app.state.tracks.observe([_at(53.75, -1.55)], now=1015.0)

    response = client.get("/v1/track/4CA2D6",
                          headers={"Authorization": f"Bearer {device_key}"})

    assert response.status_code == 200
    body = response.json()
    assert body["hex"] == "4ca2d6"
    assert body["count"] == 2
    assert body["points"][0][:2] == [53.7, -1.5]


def test_a_trail_needs_a_device_key_like_everything_else_on_v1(client):
    assert client.get("/v1/track/4ca2d6").status_code in (401, 403)
