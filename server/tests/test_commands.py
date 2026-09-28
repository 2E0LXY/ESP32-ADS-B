"""Instructions an owner queues for their own receiver.

This is the one feature here where the server tells a device on somebody's
home network what to do, so most of these are about the boundary rather
than the happy path: who may queue, what may be queued, and what must never
be queueable however the request is dressed up.
"""

import pytest

from app import commands, models
from app.database import SessionLocal


def _account(client, email, device_name):
    client.post("/signup", data={"email": email, "password": "correct-horse"},
                follow_redirects=False)
    client.post("/devices", data={"name": device_name}, follow_redirects=False)
    db = SessionLocal()
    try:
        return db.query(models.Device).filter(
            models.Device.name == device_name).one().id
    finally:
        db.close()


def _device_key(client, device_id) -> str:
    from app import security

    client.post(f"/devices/{device_id}/reissue-key", follow_redirects=False)
    return security.read_flash_token(client.cookies.get("flash_key"))["key"]


# --- the allowlist, which is the security boundary ----------------------

def test_every_allowed_action_validates_its_value():
    assert commands.clean("page", "radar") == ("page", "radar")
    assert commands.clean("brightness", "60") == ("brightness", "60")
    assert commands.clean("screensaver", "on") == ("screensaver", "1")
    assert commands.clean("screensaver", "off") == ("screensaver", "0")
    assert commands.clean("radius", "100") == ("radius", "100")
    assert commands.clean("refresh", "") == ("refresh", None)


def test_an_unknown_action_is_refused():
    for action in ("reboot", "firmware", "wifi", "provider", "aggregatorApiKey",
                   "", None, "PAGE; DROP TABLE devices"):
        with pytest.raises(ValueError):
            commands.clean(action, "anything")


def test_nothing_that_moves_data_or_credentials_is_on_the_list():
    """What is absent is the point. A command can change what the panel
    shows and how bright it is - what somebody standing in front of it
    could change by touching it - and nothing that could turn a stolen
    account into a foothold on a home network."""
    assert set(commands.ACTIONS) == {"page", "brightness", "screensaver",
                                     "radius", "refresh"}


def test_a_bad_value_is_refused_even_for_a_good_action():
    for action, value in (("page", "settings"), ("page", "../../etc/passwd"),
                          ("brightness", "0"), ("brightness", "101"),
                          ("brightness", "abc"), ("radius", "7"),
                          ("radius", "1000"), ("screensaver", "maybe")):
        with pytest.raises(ValueError):
            commands.clean(action, value)


def test_brightness_cannot_be_taken_to_zero():
    """A panel that can be blacked out from the internet is a panel
    somebody will think is broken."""
    with pytest.raises(ValueError):
        commands.clean("brightness", "0")
    assert commands.clean("brightness", "10") == ("brightness", "10")


# --- who may queue -------------------------------------------------------

def test_an_owner_can_queue_for_their_own_device(client):
    device_id = _account(client, "owner@example.com", "Mine")

    response = client.post(f"/devices/{device_id}/command",
                           data={"action": "page", "value": "radar"})

    assert response.status_code == 200
    assert response.json()["queued"]["action"] == "page"


def test_another_account_cannot_queue_for_your_device(client):
    """The whole feature in one test: if this fails, anybody with an
    account can drive somebody else's panel."""
    device_id = _account(client, "owner2@example.com", "Theirs")
    client.post("/logout", follow_redirects=False)
    _account(client, "stranger@example.com", "Other")

    response = client.post(f"/devices/{device_id}/command",
                           data={"action": "page", "value": "radar"})

    assert response.status_code == 404
    db = SessionLocal()
    try:
        assert db.query(models.DeviceCommand).filter(
            models.DeviceCommand.device_id == device_id).count() == 0
    finally:
        db.close()


def test_queueing_needs_a_session(client):
    response = client.post("/devices/1/command",
                           data={"action": "page", "value": "radar"},
                           follow_redirects=False)

    assert response.status_code in (303, 401, 403)


def test_a_rejected_action_is_a_400_not_a_500(client):
    device_id = _account(client, "owner3@example.com", "Mine3")

    response = client.post(f"/devices/{device_id}/command",
                           data={"action": "reboot", "value": "1"})

    assert response.status_code == 400
    assert "Unknown action" in response.json()["error"]


# --- what the device collects -------------------------------------------

def test_a_device_collects_only_its_own_commands(client):
    mine = _account(client, "owner4@example.com", "Mine4")
    key = _device_key(client, mine)
    client.post(f"/devices/{mine}/command", data={"action": "page", "value": "map"})

    client.post("/logout", follow_redirects=False)
    theirs = _account(client, "other4@example.com", "Theirs4")
    client.post(f"/devices/{theirs}/command", data={"action": "page", "value": "radar"})

    collected = client.get("/v1/commands", headers={"Authorization": f"Bearer {key}"})

    body = collected.json()["commands"]
    assert [c["action"] for c in body] == ["page"]
    assert body[0]["value"] == "map"


def test_a_command_is_handed_out_once(client):
    """Marked on collection rather than on an acknowledgement: a receiver
    that takes one and reboots loses it, which beats a panel replaying
    stale instructions after an outage."""
    device_id = _account(client, "owner5@example.com", "Mine5")
    key = _device_key(client, device_id)
    client.post(f"/devices/{device_id}/command", data={"action": "page", "value": "map"})

    first = client.get("/v1/commands", headers={"Authorization": f"Bearer {key}"})
    second = client.get("/v1/commands", headers={"Authorization": f"Bearer {key}"})

    assert len(first.json()["commands"]) == 1
    assert second.json()["commands"] == []


def test_collecting_needs_a_device_key(client):
    assert client.get("/v1/commands").status_code in (401, 403)


def test_commands_come_back_in_the_order_they_were_queued(client):
    device_id = _account(client, "owner6@example.com", "Mine6")
    key = _device_key(client, device_id)
    for value in ("map", "radar", "table"):
        client.post(f"/devices/{device_id}/command",
                    data={"action": "page", "value": value})

    collected = client.get("/v1/commands", headers={"Authorization": f"Bearer {key}"})

    assert [c["value"] for c in collected.json()["commands"]] == ["map", "radar", "table"]


# --- keeping the table in its place -------------------------------------

def test_the_queue_is_capped(client):
    """A runaway client must not be able to fill the table."""
    device_id = _account(client, "owner7@example.com", "Mine7")
    for _ in range(commands.MAX_PENDING):
        assert client.post(f"/devices/{device_id}/command",
                           data={"action": "page", "value": "map"}).status_code == 200

    refused = client.post(f"/devices/{device_id}/command",
                          data={"action": "page", "value": "map"})

    assert refused.status_code == 400
    assert "already waiting" in refused.json()["error"]


def test_collecting_frees_the_queue_again(client):
    device_id = _account(client, "owner8@example.com", "Mine8")
    key = _device_key(client, device_id)
    for _ in range(commands.MAX_PENDING):
        client.post(f"/devices/{device_id}/command", data={"action": "page", "value": "map"})

    client.get("/v1/commands", headers={"Authorization": f"Bearer {key}"})

    assert client.post(f"/devices/{device_id}/command",
                       data={"action": "page", "value": "map"}).status_code == 200


def test_delivered_commands_are_pruned():
    """History, not state. This table must not become the next usage_log."""
    import datetime

    db = SessionLocal()
    try:
        old = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=30)
        db.add(models.DeviceCommand(device_id=1, action="page", value="map",
                                    created_at=old, delivered_at=old))
        db.add(models.DeviceCommand(device_id=1, action="page", value="radar"))
        db.commit()

        removed = commands.prune(db, older_than_days=7)

        assert removed == 1
        # The undelivered one stays: it has not happened yet.
        assert db.query(models.DeviceCommand).filter(
            models.DeviceCommand.delivered_at.is_(None)).count() >= 1
    finally:
        db.close()


def test_who_asked_is_recorded(client):
    device_id = _account(client, "owner9@example.com", "Mine9")
    client.post(f"/devices/{device_id}/command", data={"action": "page", "value": "map"})

    db = SessionLocal()
    try:
        row = db.query(models.DeviceCommand).filter(
            models.DeviceCommand.device_id == device_id).one()
        assert row.requested_by == "owner9@example.com"
    finally:
        db.close()
