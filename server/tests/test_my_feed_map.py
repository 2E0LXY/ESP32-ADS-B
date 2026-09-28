"""The customer's own feed map, which had no test at all.

That absence is the point of this file. The endpoint below polls every
three seconds from the browser and returned a guaranteed NameError - it
referenced a variable that only existed in a different function - and the
whole suite stayed green through it, because nothing here had ever asked
the page for anything.
"""

import asyncio

import pytest


def _account_with_device(client, email="feed@example.com"):
    client.post("/signup", data={"email": email, "password": "correct-horse"},
                follow_redirects=False)
    client.post("/devices", data={"name": "Shed receiver"}, follow_redirects=False)

    from app import models
    from app.database import SessionLocal

    db = SessionLocal()
    try:
        return db.query(models.Device).filter(models.Device.name == "Shed receiver").one().id
    finally:
        db.close()


def _seed_feeder_aircraft(device_id, count=2):
    from app.main import app

    aircraft = [{"hex": f"4ca2{i:02x}", "flight": f"TEST{i}", "lat": 53.73 + i * 0.01,
                 "lon": -1.57, "alt_baro": 30000} for i in range(count)]
    asyncio.get_event_loop().run_until_complete(
        app.state.aggregator.cache.merge(f"feeder:{device_id}", aircraft))


def test_the_feed_map_page_renders(client):
    device_id = _account_with_device(client)

    page = client.get(f"/account/my-feed/{device_id}")

    assert page.status_code == 200
    assert "OpenStreetMap" in page.text or "leaflet" in page.text.lower()


def test_the_feed_aircraft_endpoint_answers(client):
    """The regression: this returned a NameError 500 on every poll."""
    device_id = _account_with_device(client)
    _seed_feeder_aircraft(device_id, count=2)

    response = client.get(f"/account/my-feed/{device_id}/aircraft")

    assert response.status_code == 200, response.text
    body = response.json()
    assert len(body["ac"]) == 2
    # Uncapped here - a browser, not a device with 320 KB of internal RAM -
    # so these agree, and the page reads both whichever endpoint it polls.
    assert body["total"] == body["returned"] == 2


def test_another_account_cannot_read_your_feed(client):
    device_id = _account_with_device(client)
    _seed_feeder_aircraft(device_id)
    client.post("/logout", follow_redirects=False)
    client.post("/signup", data={"email": "someone-else@example.com",
                                 "password": "correct-horse"}, follow_redirects=False)

    response = client.get(f"/account/my-feed/{device_id}/aircraft")

    # Not someone else's aircraft, whatever else it does.
    assert response.status_code != 200 or response.json()["ac"] == []


def test_the_feed_map_needs_a_session(client):
    response = client.get("/account/my-feed/1/aircraft", follow_redirects=False)

    assert response.status_code in (303, 401, 403)


# --- Trails on the map --------------------------------------------------

def _seed_track(hex_id="4ca200"):
    from app.main import app

    app.state.tracks.observe([{"hex": hex_id, "lat": 53.70, "lon": -1.50,
                               "alt_baro": 35000}], now=1000.0)
    app.state.tracks.observe([{"hex": hex_id, "lat": 53.75, "lon": -1.55,
                               "alt_baro": 34000}], now=1015.0)


def test_the_map_page_is_told_where_to_fetch_trails(client):
    device_id = _account_with_device(client)

    page = client.get(f"/account/my-feed/{device_id}")

    assert f"/account/my-feed/{device_id}/track" in page.text
    # And it only asks for the one that was clicked, rather than pulling a
    # trail for every aircraft on every three-second poll.
    assert "popupopen" in page.text


def test_an_owner_can_read_a_trail(client):
    device_id = _account_with_device(client)
    _seed_track()

    response = client.get(f"/account/my-feed/{device_id}/track?hex=4CA200")

    assert response.status_code == 200
    body = response.json()
    assert body["hex"] == "4ca200"
    assert body["count"] == 2
    assert body["points"][0][:2] == [53.7, -1.5]


def test_a_trail_is_not_readable_without_the_device(client):
    device_id = _account_with_device(client)
    _seed_track()
    client.post("/logout", follow_redirects=False)
    client.post("/signup", data={"email": "nosy@example.com", "password": "correct-horse"},
                follow_redirects=False)

    response = client.get(f"/account/my-feed/{device_id}/track?hex=4ca200")

    assert response.status_code != 200 or response.json()["points"] == []


def test_a_shared_link_serves_the_map_and_its_trails(client):
    device_id = _account_with_device(client)
    _seed_track()
    client.post(f"/devices/{device_id}/share", follow_redirects=False)

    from app import models
    from app.database import SessionLocal

    db = SessionLocal()
    try:
        token = db.query(models.Device).filter(models.Device.id == device_id).one().share_token
    finally:
        db.close()

    page = client.get(f"/share/{token}")
    trail = client.get(f"/share/{token}/track?hex=4ca200")

    assert f"/share/{token}/track" in page.text
    assert trail.status_code == 200
    assert trail.json()["count"] == 2


def test_a_revoked_share_cannot_read_trails(client):
    device_id = _account_with_device(client)
    _seed_track()
    client.post(f"/devices/{device_id}/share", follow_redirects=False)

    from app import models
    from app.database import SessionLocal

    db = SessionLocal()
    try:
        token = db.query(models.Device).filter(models.Device.id == device_id).one().share_token
    finally:
        db.close()
    client.post(f"/devices/{device_id}/share/revoke", follow_redirects=False)

    assert client.get(f"/share/{token}/track?hex=4ca200").status_code == 404
