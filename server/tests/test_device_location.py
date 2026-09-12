"""Per-device location.

The point of this is that a customer somewhere other than the operator's
own address gets their own sky, so that is what these assert: two devices
with different registered locations get different aircraft, and the
aggregator polls upstream for both areas rather than one global point.
"""

import asyncio

import pytest

from app import models
from app.aggregator import MAX_POLL_REGIONS, Aggregator
from app.database import SessionLocal

LEEDS = (53.7326, -1.4579)
TRURO = (50.2632, -5.0510)


def _account_with_device(client, email, name):
    client.post("/signup", data={"email": email, "password": "correct-horse"}, follow_redirects=False)
    client.post("/devices", data={"name": name}, follow_redirects=False)
    from app import security

    client.post(f"/devices/{_last_device_id()}/reissue-key", follow_redirects=False)
    payload = security.read_flash_token(client.cookies.get("flash_key"))
    client.get("/logout")
    return _last_device_id(), payload["key"]


def _last_device_id():
    db = SessionLocal()
    try:
        return db.query(models.Device).order_by(models.Device.id.desc()).first().id
    finally:
        db.close()


def _seed(hexid, flight, lat, lon):
    from app.main import app

    asyncio.get_event_loop().run_until_complete(
        app.state.aggregator.cache.merge(
            "test", [{"hex": hexid, "flight": flight, "lat": lat, "lon": lon, "alt_baro": 30000}]
        )
    )


def test_two_devices_in_different_places_get_different_aircraft(client):
    _, leeds_key = _account_with_device(client, "leeds@example.com", "Leeds receiver")
    _, truro_key = _account_with_device(client, "truro@example.com", "Truro receiver")

    _seed("aaa001", "NORTH1", LEEDS[0] + 0.1, LEEDS[1])
    _seed("bbb002", "SOUTH1", TRURO[0] + 0.1, TRURO[1])

    leeds = client.get(
        "/v1/aircraft",
        params={"lat": LEEDS[0], "lon": LEEDS[1], "radius": 50},
        headers={"Authorization": f"Bearer {leeds_key}"},
    ).json()
    truro = client.get(
        "/v1/aircraft",
        params={"lat": TRURO[0], "lon": TRURO[1], "radius": 50},
        headers={"Authorization": f"Bearer {truro_key}"},
    ).json()

    assert [a["flight"] for a in leeds["ac"]] == ["NORTH1"]
    assert [a["flight"] for a in truro["ac"]] == ["SOUTH1"]


def test_a_request_records_the_devices_location(client):
    device_id, key = _account_with_device(client, "moving@example.com", "Hotel receiver")
    client.get(
        "/v1/aircraft",
        params={"lat": TRURO[0], "lon": TRURO[1], "radius": 40},
        headers={"Authorization": f"Bearer {key}"},
    )
    db = SessionLocal()
    try:
        device = db.get(models.Device, device_id)
        # Recorded from the request itself, so a receiver that moves follows
        # itself with no configuration.
        assert device.reported_lat == pytest.approx(TRURO[0])
        assert device.reported_radius_nm == pytest.approx(40)
        assert device.location()[0] == pytest.approx(TRURO[0])
    finally:
        db.close()


def test_reported_location_wins_over_the_dashboard_value():
    device = models.Device(name="x", manual_lat=1.0, manual_lon=2.0, manual_radius_nm=30)
    assert device.location() == (1.0, 2.0, 30)
    device.reported_lat, device.reported_lon, device.reported_radius_nm = 10.0, 20.0, 60
    # A receiver that says where it is beats a remembered form field, or a
    # moved receiver would appear stuck at its old address.
    assert device.location() == (10.0, 20.0, 60)


def test_no_location_falls_back_to_the_deployment_default(client):
    device = models.Device(name="x")
    assert device.location() is None

    aggregator = Aggregator(53.73, -1.57, 50, SessionLocal)
    # No located devices at all, so the configured home is still what gets
    # polled - the env vars stay a fallback, not dead weight.
    assert aggregator.poll_regions() == [(53.73, -1.57, 50)]


def test_polling_covers_every_device_area(client):
    _account_with_device(client, "a@example.com", "A")
    _account_with_device(client, "b@example.com", "B")
    db = SessionLocal()
    try:
        devices = db.query(models.Device).order_by(models.Device.id).all()
        devices[0].reported_lat, devices[0].reported_lon, devices[0].reported_radius_nm = (*LEEDS, 40)
        devices[1].reported_lat, devices[1].reported_lon, devices[1].reported_radius_nm = (*TRURO, 40)
        db.commit()
    finally:
        db.close()

    regions = Aggregator(0.0, 0.0, 50, SessionLocal).poll_regions()
    assert len(regions) == 2
    from app.aggregator import _distance_nm

    # Both areas polled, neither collapsed into the other or onto the
    # unrelated configured home.
    assert min(_distance_nm(LEEDS[0], LEEDS[1], r[0], r[1]) for r in regions) < 1
    assert min(_distance_nm(TRURO[0], TRURO[1], r[0], r[1]) for r in regions) < 1


def test_nearby_devices_are_polled_as_one_area(client):
    _account_with_device(client, "n1@example.com", "N1")
    _account_with_device(client, "n2@example.com", "N2")
    db = SessionLocal()
    try:
        devices = db.query(models.Device).order_by(models.Device.id).all()
        devices[0].reported_lat, devices[0].reported_lon, devices[0].reported_radius_nm = (*LEEDS, 50)
        # Five miles away - the same sky to an upstream API.
        devices[1].reported_lat, devices[1].reported_lon, devices[1].reported_radius_nm = (
            LEEDS[0] + 0.08, LEEDS[1], 50,
        )
        db.commit()
    finally:
        db.close()

    # One request per upstream instead of two for the same aircraft.
    assert len(Aggregator(0.0, 0.0, 50, SessionLocal).poll_regions()) == 1


def test_region_count_is_capped(client):
    for index in range(MAX_POLL_REGIONS + 3):
        _account_with_device(client, f"many{index}@example.com", f"D{index}")
    db = SessionLocal()
    try:
        for index, device in enumerate(db.query(models.Device).order_by(models.Device.id).all()):
            # Spread far enough apart that none of them merge.
            device.reported_lat = 10.0 + index * 8
            device.reported_lon = 0.0
            device.reported_radius_nm = 50
        db.commit()
    finally:
        db.close()

    aggregator = Aggregator(0.0, 0.0, 50, SessionLocal)
    first = aggregator.poll_regions()
    assert len(first) == MAX_POLL_REGIONS
    # The extras are not dropped, they come round on a later cycle.
    second = aggregator.poll_regions()
    assert first != second


def test_a_tiny_requested_radius_still_fills_a_useful_area(client):
    _account_with_device(client, "tiny@example.com", "Tiny")
    db = SessionLocal()
    try:
        device = db.query(models.Device).order_by(models.Device.id.desc()).first()
        device.reported_lat, device.reported_lon, device.reported_radius_nm = (*LEEDS, 5)
        db.commit()
    finally:
        db.close()
    # Polling only 5 nm would cache an aircraft just as it arrives overhead.
    assert Aggregator(0.0, 0.0, 50, SessionLocal).poll_regions()[0][2] >= 25
