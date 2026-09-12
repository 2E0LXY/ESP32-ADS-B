"""Watching the service, and acting on one device, without SSH.

Reading this service's logs meant `docker compose logs -f` on the VPS, and
the only lever over a misbehaving receiver was suspending its owner's
entire account.
"""

import datetime
import logging
import os

import pytest

from app import models
from app.database import SessionLocal
from app.log_buffer import RingBufferHandler


@pytest.fixture()
def admin(client):
    client.post("/admin/login",
                data={"email": os.environ["ADMIN_BOOTSTRAP_EMAIL"],
                      "password": os.environ["ADMIN_BOOTSTRAP_PASSWORD"]},
                follow_redirects=False)
    return client


def _record(name="poller", level=logging.INFO, message="hello %s", args=("world",),
            exc_info=None):
    return logging.LogRecord(name, level, "app/x.py", 1, message, args, exc_info)


# --- the buffer --------------------------------------------------------

def test_the_newest_lines_are_kept_and_the_oldest_dropped():
    handler = RingBufferHandler(capacity=3)
    for n in range(5):
        handler.emit(_record(message="line %d", args=(n,)))

    assert [entry.message for entry in handler.entries()] == ["line 4", "line 3", "line 2"]
    assert handler.dropped == 2
    assert handler.counts()["total"] == 3


def test_a_traceback_is_kept_not_just_the_message():
    """logger.exception() is used throughout this codebase and the
    traceback is the useful half - a page showing only "poll cycle failed"
    would send you back to SSH anyway."""
    handler = RingBufferHandler()
    try:
        raise ValueError("upstream returned nonsense")
    except ValueError:
        import sys
        handler.emit(_record(level=logging.ERROR, message="poll cycle failed", args=(),
                             exc_info=sys.exc_info()))

    entry, = handler.entries()
    assert "poll cycle failed" in entry.message
    assert "ValueError: upstream returned nonsense" in entry.message
    assert "Traceback" in entry.message
    assert entry.css_class == "bad"


def test_the_message_is_built_when_it_is_logged_not_when_it_is_read():
    """The arguments are only valid at emit time - a mutable object logged
    with %s could have changed by the time somebody opens the page."""
    handler = RingBufferHandler()
    changing = ["before"]
    handler.emit(_record(message="value is %s", args=(list(changing),)))
    changing[0] = "after"

    assert "before" in handler.entries()[0].message


def test_lines_can_be_filtered_by_level_and_by_text():
    handler = RingBufferHandler()
    handler.emit(_record(name="poller", level=logging.INFO, message="polled adsb.fi", args=()))
    handler.emit(_record(name="feeder", level=logging.WARNING, message="feed dropped", args=()))
    handler.emit(_record(name="feeder", level=logging.ERROR, message="bind failed", args=()))

    assert len(handler.entries(minimum_level=logging.WARNING)) == 2
    assert len(handler.entries(minimum_level=logging.ERROR)) == 1
    assert [e.message for e in handler.entries(contains="dropped")] == ["feed dropped"]
    # The logger name is searchable too, which is how you isolate one
    # subsystem without knowing what it says - so "feeder" matches both of
    # that logger's lines, including the one whose message never says it.
    assert len(handler.entries(contains="feeder")) == 2
    assert handler.counts() == {"total": 3, "warnings": 1, "errors": 1}


def test_a_handler_that_cannot_format_does_not_break_the_caller():
    """A logging handler that raises would break whatever called the
    logger, which is never worth it for a convenience buffer."""
    handler = RingBufferHandler()
    handler.emit(_record(message="needs %d args", args=("not a number",)))

    # Nothing recorded, nothing raised.
    assert handler.entries() == []


# --- the page ----------------------------------------------------------

def test_the_logs_page_shows_recent_lines(admin):
    logging.getLogger("test-subsystem").warning("a distinctive warning line")

    page = admin.get("/admin/logs")

    assert page.status_code == 200, page.text
    assert "a distinctive warning line" in page.text
    assert "test-subsystem" in page.text
    # And says what it is, so nobody mistakes it for the full history.
    assert "docker compose logs" in page.text


def test_the_logs_page_filters(admin):
    logging.getLogger("test-subsystem").info("an ordinary info line")
    logging.getLogger("test-subsystem").error("a specific failure")

    errors_only = admin.get("/admin/logs?level=error")
    assert "a specific failure" in errors_only.text
    assert "an ordinary info line" not in errors_only.text

    searched = admin.get("/admin/logs?q=ordinary")
    assert "an ordinary info line" in searched.text
    assert "a specific failure" not in searched.text


def test_a_nonsense_level_shows_everything_rather_than_failing(admin):
    logging.getLogger("test-subsystem").info("still here")
    page = admin.get("/admin/logs?level=banana")
    assert page.status_code == 200
    assert "still here" in page.text


def test_the_plain_text_view_reads_oldest_first(admin):
    logging.getLogger("test-subsystem").warning("first line")
    logging.getLogger("test-subsystem").warning("second line")

    text = admin.get("/admin/logs.txt?q=line").text

    assert text.index("first line") < text.index("second line"), "should read like a log file"
    assert "WARNING" in text


def test_the_logs_are_not_public(client):
    for path in ("/admin/logs", "/admin/logs.txt", "/admin/devices"):
        response = client.get(path, follow_redirects=False)
        assert response.status_code == 303, path
        assert response.headers["location"] == "/admin/login"


# --- devices -----------------------------------------------------------

def _account_with_device(client, email, name):
    client.post("/signup", data={"email": email, "password": "correct-horse"},
                follow_redirects=False)
    client.post("/devices", data={"name": name}, follow_redirects=False)
    db = SessionLocal()
    try:
        device = db.query(models.Device).filter(models.Device.name == name).first()
        return device.id
    finally:
        db.close()


def _issue_key(client, device_id):
    client.post(f"/devices/{device_id}/reissue-key", follow_redirects=False)
    from app import security
    return security.read_flash_token(client.cookies.get("flash_key"))["key"]


def test_devices_can_be_found_by_account_email_or_name(admin):
    leeds = _account_with_device(admin, "leeds@example.com", "Loft receiver")
    _account_with_device(admin, "truro@example.com", "Shed receiver")
    admin.post("/admin/login",
               data={"email": os.environ["ADMIN_BOOTSTRAP_EMAIL"],
                     "password": os.environ["ADMIN_BOOTSTRAP_PASSWORD"]},
               follow_redirects=False)

    everything = admin.get("/admin/devices")
    assert "Loft receiver" in everything.text and "Shed receiver" in everything.text

    by_name = admin.get("/admin/devices?q=Loft")
    assert "Loft receiver" in by_name.text
    assert "Shed receiver" not in by_name.text

    by_email = admin.get("/admin/devices?q=truro@")
    assert "Shed receiver" in by_email.text
    assert "Loft receiver" not in by_email.text
    assert leeds  # the id is what the action routes use


def test_revoking_one_devices_keys_leaves_the_account_alone(admin):
    """The only lever before was suspending the owner's whole account,
    which cuts off every receiver they have."""
    device_id = _account_with_device(admin, "rev@example.com", "Noisy receiver")
    key = _issue_key(admin, device_id)
    headers = {"Authorization": f"Bearer {key}"}
    assert admin.get("/v1/aircraft?lat=53.7&lon=-1.5&radius=25", headers=headers).status_code == 200

    admin.post("/admin/login",
               data={"email": os.environ["ADMIN_BOOTSTRAP_EMAIL"],
                     "password": os.environ["ADMIN_BOOTSTRAP_PASSWORD"]},
               follow_redirects=False)
    response = admin.post(f"/admin/devices/{device_id}/revoke-keys", follow_redirects=False)
    assert response.status_code == 303

    assert admin.get("/v1/aircraft?lat=53.7&lon=-1.5&radius=25", headers=headers).status_code == 401
    db = SessionLocal()
    try:
        assert db.query(models.Account).filter(
            models.Account.email == "rev@example.com").first().is_active is True
        assert db.query(models.AuditLog).filter(
            models.AuditLog.action == "revoke_device_keys").count() == 1
    finally:
        db.close()


def test_revoking_keys_for_a_device_that_does_not_exist_says_so(admin):
    response = admin.post("/admin/devices/4242/revoke-keys", follow_redirects=False)
    assert response.status_code == 303
    assert "flash_error=1" in response.headers["location"]


def test_stopping_a_feed_closes_its_listener_not_just_the_flag(admin):
    """Feeder ingestion has no authentication beyond knowing the port, so a
    feed injecting nonsense has to be stoppable now - leaving the listener
    bound until the next restart would keep accepting it."""
    from app.main import app

    device_id = _account_with_device(admin, "feed@example.com", "Feeding receiver")
    db = SessionLocal()
    try:
        device = db.query(models.Device).filter(models.Device.id == device_id).first()
        device.feeder_enabled = True
        device.feeder_port = 30199
        device.feeder_last_message_at = datetime.datetime.now(datetime.timezone.utc)
        db.commit()
    finally:
        db.close()

    stopped = []

    async def record_stop(port):
        stopped.append(port)

    app.state.feed_ingest.stop_for_device = record_stop
    admin.post("/admin/login",
               data={"email": os.environ["ADMIN_BOOTSTRAP_EMAIL"],
                     "password": os.environ["ADMIN_BOOTSTRAP_PASSWORD"]},
               follow_redirects=False)

    response = admin.post(f"/admin/devices/{device_id}/disable-feeder", follow_redirects=False)

    assert response.status_code == 303
    assert stopped == [30199], "the listener must be closed, not left bound"
    db = SessionLocal()
    try:
        assert db.query(models.Device).filter(
            models.Device.id == device_id).first().feeder_enabled is False
        assert db.query(models.AuditLog).filter(
            models.AuditLog.action == "disable_device_feeder").count() == 1
    finally:
        db.close()


def test_stopping_a_feed_that_was_not_running_is_refused_cleanly(admin):
    device_id = _account_with_device(admin, "nofeed@example.com", "Quiet receiver")
    admin.post("/admin/login",
               data={"email": os.environ["ADMIN_BOOTSTRAP_EMAIL"],
                     "password": os.environ["ADMIN_BOOTSTRAP_PASSWORD"]},
               follow_redirects=False)

    response = admin.post(f"/admin/devices/{device_id}/disable-feeder", follow_redirects=False)

    assert response.status_code == 303
    assert "flash_error=1" in response.headers["location"]
