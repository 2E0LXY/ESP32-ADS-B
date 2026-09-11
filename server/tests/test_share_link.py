"""Public "anyone with the link" view of one receiver's feed.

The token in the URL is the whole credential, so these tests care as much
about what a holder of the link *cannot* reach as about the map working.
"""

import time

from app import models
from app.aggregator import CachedAircraft
from app.database import SessionLocal
from app.main import app


def _account_with_device(client, email):
    assert client.post(
        "/signup", data={"email": email, "password": "correct-horse"}, follow_redirects=False
    ).status_code == 303
    client.post("/devices", data={"name": "Shed receiver"}, follow_redirects=False)
    db = SessionLocal()
    try:
        return db.query(models.Device).filter(models.Device.name == "Shed receiver").first().id
    finally:
        db.close()


def _token(device_id: int) -> str | None:
    db = SessionLocal()
    try:
        return db.query(models.Device).filter(models.Device.id == device_id).first().share_token
    finally:
        db.close()


def _seed_feed(device_id: int):
    """One aircraft attributed to this device's feeder, as feed_ingest would."""
    now = time.time()
    app.state.aggregator.cache._by_hex["4ca2d5"] = CachedAircraft(
        hex="4ca2d5",
        data={"hex": "4ca2d5", "flight": "RYR2BH", "lat": 53.72, "lon": -1.57, "alt_baro": 35000},
        seen_at=now,
        sources={f"feeder:{device_id}": now},
    )


def test_a_created_link_serves_the_map_and_the_aircraft(client):
    device_id = _account_with_device(client, "share1@example.com")
    assert client.post(f"/devices/{device_id}/share", follow_redirects=False).status_code == 303

    token = _token(device_id)
    assert token and len(token) >= 16
    # The dashboard shows the owner the whole URL to copy.
    assert f"/share/{token}" in client.get("/account").text

    _seed_feed(device_id)
    # No cookies at all: this is a stranger with the link.
    page = client.get(f"/share/{token}", headers={"Cookie": ""})
    assert page.status_code == 200
    assert "Shed receiver" in page.text
    assert page.headers["X-Robots-Tag"].startswith("noindex")

    data = client.get(f"/share/{token}/aircraft", headers={"Cookie": ""}).json()
    assert [a["flight"] for a in data["ac"]] == ["RYR2BH"]


def test_the_link_exposes_nothing_but_the_map(client):
    device_id = _account_with_device(client, "share2@example.com")
    client.post(f"/devices/{device_id}/share", follow_redirects=False)
    token = _token(device_id)

    body = client.get(f"/share/{token}").text
    # Whoever holds the link must not learn who owns it, nor be handed the
    # owner's account area.
    assert "share2@example.com" not in body
    assert "/account" not in body
    assert "Reissue" not in body


def test_a_revoked_link_stops_working(client):
    device_id = _account_with_device(client, "share3@example.com")
    client.post(f"/devices/{device_id}/share", follow_redirects=False)
    token = _token(device_id)
    assert client.get(f"/share/{token}").status_code == 200

    assert client.post(f"/devices/{device_id}/share/revoke", follow_redirects=False).status_code == 303
    assert _token(device_id) is None
    assert client.get(f"/share/{token}").status_code == 404
    assert client.get(f"/share/{token}/aircraft").status_code == 404


def test_a_new_link_replaces_the_old_one(client):
    device_id = _account_with_device(client, "share4@example.com")
    client.post(f"/devices/{device_id}/share", follow_redirects=False)
    first = _token(device_id)
    client.post(f"/devices/{device_id}/share", follow_redirects=False)
    second = _token(device_id)

    assert first != second
    assert client.get(f"/share/{first}").status_code == 404
    assert client.get(f"/share/{second}").status_code == 200


def test_a_junk_token_does_not_match_an_unshared_device(client):
    """A device that has never been shared has a null token. A short or
    empty token must not be allowed to collapse into matching it."""
    _account_with_device(client, "share5@example.com")
    for junk in ("x", "none", "null", "0" * 15):
        assert client.get(f"/share/{junk}").status_code == 404


def test_another_account_cannot_mint_a_link_for_your_device(client):
    device_id = _account_with_device(client, "owner@example.com")
    client.get("/logout")
    assert client.post(
        "/signup", data={"email": "intruder@example.com", "password": "correct-horse"},
        follow_redirects=False,
    ).status_code == 303

    assert client.post(f"/devices/{device_id}/share", follow_redirects=False).status_code == 303
    assert _token(device_id) is None  # silently did nothing, as the other owner-scoped routes do
