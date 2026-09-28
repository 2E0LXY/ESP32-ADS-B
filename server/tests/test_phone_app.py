"""The phone and tablet app.

Served from the aggregator rather than from the receiver for one concrete
reason, which the first test pins: a page served over HTTPS cannot talk to
a device at http://192.168.1.228, because browsers block that as mixed
content. So the app never addresses the panel directly - it goes through
the command relay - and works identically at home and away.
"""

import json

from app import models
from app.database import SessionLocal


def _account(client, email="phone@example.com", device_name="Phone test"):
    client.post("/signup", data={"email": email, "password": "correct-horse"},
                follow_redirects=False)
    client.post("/devices", data={"name": device_name}, follow_redirects=False)
    db = SessionLocal()
    try:
        return db.query(models.Device).filter(models.Device.name == device_name).one().id
    finally:
        db.close()


# --- the shell -----------------------------------------------------------

def test_the_app_renders_for_a_device_you_own(client):
    device_id = _account(client)

    page = client.get(f"/app/{device_id}")

    assert page.status_code == 200
    assert "Phone test" in page.text
    # Installable, and sized for a phone rather than zoomed out.
    assert 'rel="manifest"' in page.text
    assert "viewport-fit=cover" in page.text


def test_the_app_never_addresses_the_panel_directly(client):
    """The architectural constraint, pinned. If a LAN address ever appears
    in this page, the app has stopped working away from home and will fail
    silently on mixed content when it is at home too."""
    device_id = _account(client, "phone2@example.com", "Phone test 2")

    page = client.get(f"/app/{device_id}").text

    assert "192.168." not in page
    assert "http://" not in page.replace("http://www.w3.org/2000/svg", "")
    # It drives the panel through the relay instead.
    assert f"const DEVICE = {device_id};" in page
    assert "/devices/${DEVICE}/command" in page


def test_the_app_needs_a_session(client):
    response = client.get("/app", follow_redirects=False)

    assert response.status_code in (303, 401, 403)


def test_an_account_with_no_devices_is_sent_to_set_one_up(client):
    client.post("/signup", data={"email": "empty@example.com", "password": "correct-horse"},
                follow_redirects=False)

    response = client.get("/app", follow_redirects=False)

    assert response.status_code == 303
    assert response.headers["location"] == "/account"


def test_another_accounts_device_falls_back_to_your_own(client):
    """Not an error - the id is a convenience, and showing somebody else's
    receiver because they guessed an id would be the actual problem."""
    theirs = _account(client, "owner-x@example.com", "Theirs X")
    client.post("/logout", follow_redirects=False)
    mine = _account(client, "mine-x@example.com", "Mine X")

    page = client.get(f"/app/{theirs}")

    assert page.status_code == 200
    assert "Mine X" in page.text
    assert "Theirs X" not in page.text
    assert f"const DEVICE = {mine};" in page.text


# --- installability ------------------------------------------------------

def test_the_manifest_is_valid_and_scoped_to_the_app(client):
    _account(client, "phone3@example.com", "Phone test 3")

    response = client.get("/app/manifest.webmanifest")

    assert response.status_code == 200
    manifest = json.loads(response.content)
    assert manifest["start_url"] == "/app"
    # Scoped, so installing the app does not capture the admin pages or the
    # account dashboard on the same domain.
    assert manifest["scope"] == "/app"
    assert manifest["display"] == "standalone"
    assert manifest["icons"]


def test_the_service_worker_caches_nothing(client):
    """This is a live view of the sky. A worker serving yesterday's
    aircraft from a cache would be worse than no app at all."""
    response = client.get("/app/sw.js")

    assert response.status_code == 200
    assert "addEventListener('fetch'" not in response.text
    assert "caches" not in response.text


def test_the_icon_is_served(client):
    response = client.get("/app/icon.svg")

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("image/svg")


# --- the data it polls ---------------------------------------------------

def _seed_sky():
    import asyncio

    from app.main import app

    asyncio.get_event_loop().run_until_complete(
        app.state.aggregator.cache.merge("test", [
            {"hex": "4ca2d6", "flight": "EZY51NR", "lat": 53.73, "lon": -1.57,
             "alt_baro": 37000, "t": "A320"},
        ]))


def test_one_request_returns_everything_the_app_shows(client):
    """A phone on mobile data should make one request per refresh, not one
    per panel of the screen."""
    device_id = _account(client, "phone4@example.com", "Phone test 4")
    _seed_sky()

    body = client.get(f"/account/devices/{device_id}/view").json()

    assert {"device", "aircraft", "alerts", "balloons", "logbook",
            "known_airframes", "total"} <= set(body)
    assert body["device"]["name"] == "Phone test 4"


def test_the_view_is_not_readable_by_another_account(client):
    theirs = _account(client, "owner-y@example.com", "Theirs Y")
    client.post("/logout", follow_redirects=False)
    _account(client, "mine-y@example.com", "Mine Y")

    response = client.get(f"/account/devices/{theirs}/view")

    assert response.status_code == 404


def test_the_view_needs_a_session(client):
    response = client.get("/account/devices/1/view", follow_redirects=False)

    assert response.status_code in (303, 401, 403)


def test_a_trail_is_gated_on_owning_the_device(client):
    theirs = _account(client, "owner-z@example.com", "Theirs Z")
    client.post("/logout", follow_redirects=False)
    _account(client, "mine-z@example.com", "Mine Z")

    response = client.get(f"/account/devices/{theirs}/view/track?hex=4ca2d6")

    assert response.status_code == 404
    assert response.json()["points"] == []


def test_the_app_never_carries_a_device_api_key(client):
    """A browser must not be given a receiver's key: anything in a page can
    be read by anything else that ends up in that page."""
    device_id = _account(client, "phone5@example.com", "Phone test 5")
    client.post(f"/devices/{device_id}/reissue-key", follow_redirects=False)

    from app import security

    key = security.read_flash_token(client.cookies.get("flash_key"))["key"]
    page = client.get(f"/app/{device_id}").text
    view = client.get(f"/account/devices/{device_id}/view").text

    assert key not in page
    assert key not in view
    assert "Authorization" not in page
