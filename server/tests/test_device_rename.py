"""End-to-end checks for renaming a device from the account dashboard.

Runs the real app against a throwaway SQLite file via TestClient, the same
way the aggregator and feed-ingest features were tested: no mocking of the
routes, the ORM or the session cookie, so a break in any of them fails here.
"""

from app import models
from app.database import SessionLocal


def _signup(client, email="rename@example.com"):
    response = client.post(
        "/signup", data={"email": email, "password": "correct-horse"}, follow_redirects=False
    )
    assert response.status_code == 303, response.text
    return response


def _device_ids(client):
    db = SessionLocal()
    try:
        return [d.id for d in db.query(models.Device).all()]
    finally:
        db.close()


def test_rename_changes_the_displayed_name(client):
    _signup(client)
    client.post("/devices", data={"name": "Shed receiver"}, follow_redirects=False)
    device_id = _device_ids(client)[0]

    assert "Shed receiver" in client.get("/account").text

    response = client.post(
        f"/devices/{device_id}/rename", data={"name": "Loft receiver"}, follow_redirects=False
    )
    assert response.status_code == 303
    body = client.get("/account").text
    assert "Loft receiver" in body
    assert "Shed receiver" not in body


def test_blank_name_falls_back_rather_than_erasing_it(client):
    _signup(client)
    client.post("/devices", data={"name": "Shed receiver"}, follow_redirects=False)
    device_id = _device_ids(client)[0]

    client.post(f"/devices/{device_id}/rename", data={"name": "   "}, follow_redirects=False)
    # Same rule add_device() applies, so a device can never hold a name it
    # could not have been created with.
    assert "My receiver" in client.get("/account").text


def test_name_is_truncated_not_rejected(client):
    _signup(client)
    client.post("/devices", data={"name": "Shed"}, follow_redirects=False)
    device_id = _device_ids(client)[0]

    client.post(f"/devices/{device_id}/rename", data={"name": "x" * 500}, follow_redirects=False)
    db = SessionLocal()
    try:
        assert len(db.get(models.Device, device_id).name) == 120
    finally:
        db.close()


def test_cannot_rename_another_accounts_device(client):
    _signup(client, "owner@example.com")
    client.post("/devices", data={"name": "Owner receiver"}, follow_redirects=False)
    device_id = _device_ids(client)[0]

    client.get("/logout")
    _signup(client, "stranger@example.com")
    response = client.post(
        f"/devices/{device_id}/rename", data={"name": "Stolen"}, follow_redirects=False
    )
    # Redirects rather than 403 - same shape as the other ownership checks in
    # this router - but the name must be untouched.
    assert response.status_code == 303
    db = SessionLocal()
    try:
        assert db.get(models.Device, device_id).name == "Owner receiver"
    finally:
        db.close()
