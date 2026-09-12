"""Settings an operator can change from the admin panel.

Everything tunable used to be an environment variable read once at import,
so changing the poll interval or turning a dead upstream source off meant
editing .env over SSH and restarting the container - which drops every
feeder connection and loses a poll cycle.

These cover that the pages render, that a change actually reaches the code
that reads it, and that a bad value cannot half-apply.
"""

import datetime
import os

import pytest

from app import models
from app.database import SessionLocal
from app.runtime_settings import DEFINITIONS, SettingsStore, from_form


@pytest.fixture()
def admin(client):
    client.post("/admin/login",
                data={"email": os.environ["ADMIN_BOOTSTRAP_EMAIL"],
                      "password": os.environ["ADMIN_BOOTSTRAP_PASSWORD"]},
                follow_redirects=False)
    return client


def _form(**overrides):
    """A complete submission, as the browser would send it."""
    form = {}
    for definition in DEFINITIONS:
        if definition.kind == "bool":
            if definition.default:
                form[definition.name] = "on"
        else:
            form[definition.name] = str(definition.default)
    for name, value in overrides.items():
        if value is None:
            form.pop(name, None)  # an unticked checkbox is simply absent
        else:
            form[name] = value
    return form


# --- the pages ---------------------------------------------------------

def test_the_settings_page_renders_every_setting(admin):
    page = admin.get("/admin/settings")
    assert page.status_code == 200, page.text
    for definition in DEFINITIONS:
        assert definition.label in page.text, definition.name
    # Grouped, not one undifferentiated column of numbers.
    for group in ("Polling", "Upstream sources", "Housekeeping"):
        assert group in page.text


def test_the_system_page_renders(admin):
    page = admin.get("/admin/system")
    assert page.status_code == 200, page.text
    assert "Scaling" in page.text
    # The honest part: it says why the shared cache is not a button here.
    assert "cannot be switched on from here" in page.text
    assert "Needs a restart to change" in page.text


def test_the_admin_pages_are_not_public(client):
    for path in ("/admin/settings", "/admin/system", "/admin/password"):
        response = client.get(path, follow_redirects=False)
        assert response.status_code == 303, path
        assert response.headers["location"] == "/admin/login"


# --- saving ------------------------------------------------------------

def test_saving_a_setting_changes_what_the_code_reads(admin):
    from app.main import app

    assert app.state.aggregator.settings.get("poll_interval_seconds") != 42

    response = admin.post("/admin/settings", data=_form(poll_interval_seconds="42"),
                          follow_redirects=False)
    assert response.status_code == 303

    # Not just stored - the running aggregator reads the same store, so the
    # next poll cycle uses it without a restart.
    assert app.state.aggregator.settings.get("poll_interval_seconds") == 42
    db = SessionLocal()
    try:
        row = db.query(models.Setting).filter(
            models.Setting.key == "poll_interval_seconds").first()
        assert row.value == "42"
        assert row.updated_by == os.environ["ADMIN_BOOTSTRAP_EMAIL"]
    finally:
        db.close()


def test_turning_a_source_off_stops_it_being_polled(admin):
    """adsb.fi is on by default; unticking it must actually stop the poll
    loop asking for it."""
    from app.main import app

    assert app.state.aggregator.source_enabled("adsbfi") is True

    admin.post("/admin/settings", data=_form(source_adsbfi=None), follow_redirects=False)

    assert app.state.aggregator.source_enabled("adsbfi") is False


def test_turning_a_source_on_is_possible_from_the_panel(admin):
    """airplanes.live is off by default. Before this it could only be
    re-enabled by editing DISABLED_SOURCES in .env and restarting."""
    from app.main import app

    assert app.state.aggregator.source_enabled("airplaneslive") is False

    admin.post("/admin/settings", data=_form(source_airplaneslive="on"),
               follow_redirects=False)

    assert app.state.aggregator.source_enabled("airplaneslive") is True


def test_every_change_is_recorded_in_the_audit_log(admin):
    admin.post("/admin/settings", data=_form(max_poll_regions="9"), follow_redirects=False)

    page = admin.get("/admin/audit-log")
    assert "change_setting" in page.text
    assert "max_poll_regions" in page.text
    assert "6 -&gt; 9" in page.text or "6 -> 9" in page.text


def test_saving_nothing_changed_writes_no_row(admin):
    admin.post("/admin/settings", data=_form(), follow_redirects=False)

    db = SessionLocal()
    try:
        assert db.query(models.Setting).count() == 0
        assert db.query(models.AuditLog).filter(
            models.AuditLog.action == "change_setting").count() == 0
    finally:
        db.close()


# --- validation --------------------------------------------------------

def test_a_value_out_of_range_is_refused_and_nothing_is_saved(admin):
    """A poll interval of zero would hammer three free public APIs in a
    tight loop. Rejecting the whole submission also means the operator's
    other edits in the same form are not half-applied."""
    from app.main import app

    response = admin.post("/admin/settings",
                          data=_form(poll_interval_seconds="0", max_poll_regions="9"),
                          follow_redirects=False)

    assert response.status_code == 303
    assert "flash_error=1" in response.headers["location"]
    assert app.state.aggregator.settings.get("poll_interval_seconds") != 0
    assert app.state.aggregator.settings.get("max_poll_regions") != 9
    db = SessionLocal()
    try:
        assert db.query(models.Setting).count() == 0
    finally:
        db.close()


def test_text_where_a_number_belongs_is_refused(admin):
    response = admin.post("/admin/settings", data=_form(max_poll_regions="lots"),
                          follow_redirects=False)
    assert "flash_error=1" in response.headers["location"]


def test_the_store_rejects_a_bad_value_before_touching_the_database():
    store = SettingsStore(SessionLocal)
    with pytest.raises(ValueError) as excess:
        store.set_many({"poll_interval_seconds": 9999}, "admin@example.com")
    # The message names the setting and the limit, not just "invalid".
    assert "Upstream poll interval" in str(excess.value)
    assert "300" in str(excess.value)


def test_an_unticked_checkbox_means_off_not_unmentioned():
    """An unticked checkbox is simply absent from a POST body, so a form
    read naively treats "turned off" as "not mentioned" and leaves it on."""
    submitted = from_form({"poll_interval_seconds": "15"})
    assert submitted["source_adsbfi"] is False
    assert submitted["aircraft_photos"] is False
    assert from_form({"source_adsbfi": "on"})["source_adsbfi"] is True


def test_an_unreadable_stored_value_falls_back_to_the_default(client):
    """A row hand-edited in the database, or left by an older version, must
    not take the service down on boot."""
    db = SessionLocal()
    try:
        db.add(models.Setting(key="poll_interval_seconds", value="not-a-number"))
        db.add(models.Setting(key="a_setting_from_the_future", value="1"))
        db.commit()
    finally:
        db.close()

    store = SettingsStore(SessionLocal)
    values = store.reload()

    assert values["poll_interval_seconds"] == 15
    assert "a_setting_from_the_future" not in values


def test_a_store_with_no_database_serves_the_environment_defaults():
    """What the aggregator gets in tests, and anything constructed with
    nothing configured."""
    store = SettingsStore()
    assert store.get("poll_interval_seconds") == 15
    assert store.source_enabled("airplaneslive") is False
    with pytest.raises(RuntimeError):
        store.set_many({"poll_interval_seconds": 20}, "admin@example.com")


# --- prune now ---------------------------------------------------------

def test_pruning_on_demand_reports_what_it_deleted(admin):
    db = SessionLocal()
    try:
        account = models.Account(email="p@example.com", password_hash="x")
        db.add(account)
        db.commit()
        device = models.Device(account_id=account.id, name="rx")
        db.add(device)
        db.commit()
        old = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=90)
        for _ in range(4):
            db.add(models.UsageLog(device_id=device.id, at=old, aircraft_returned=3))
        db.commit()
    finally:
        db.close()

    response = admin.post("/admin/settings/prune-now", follow_redirects=False)

    assert response.status_code == 303
    # Three of the four: the newest row for a device is always kept.
    assert "Pruned%203" in response.headers["location"]
    db = SessionLocal()
    try:
        assert db.query(models.UsageLog).count() == 1
        assert db.query(models.AuditLog).filter(
            models.AuditLog.action == "prune_usage_log").count() == 1
    finally:
        db.close()


# --- the admin's own password ------------------------------------------

def _login(client, password):
    return client.post("/admin/login",
                       data={"email": os.environ["ADMIN_BOOTSTRAP_EMAIL"], "password": password},
                       follow_redirects=False)


def test_the_admin_can_change_their_own_password(admin):
    """Until now the only admin password was ADMIN_BOOTSTRAP_PASSWORD, which
    means it is still sitting in .env on the server - and changing it meant
    editing that file and restarting."""
    response = admin.post("/admin/password",
                          data={"current": os.environ["ADMIN_BOOTSTRAP_PASSWORD"],
                                "replacement": "a-much-longer-secret",
                                "confirmation": "a-much-longer-secret"},
                          follow_redirects=False)
    assert response.status_code == 303
    assert "flash_error" not in response.headers["location"]

    admin.get("/admin/logout")
    assert _login(admin, os.environ["ADMIN_BOOTSTRAP_PASSWORD"]).status_code == 200  # re-shows the form
    assert _login(admin, "a-much-longer-secret").status_code == 303
    db = SessionLocal()
    try:
        assert db.query(models.AuditLog).filter(
            models.AuditLog.action == "change_admin_password").count() == 1
    finally:
        db.close()


def test_the_current_password_is_required(admin):
    """The session already proves who this is; this check stops a borrowed,
    unlocked browser being enough to lock the real operator out."""
    response = admin.post("/admin/password",
                          data={"current": "not-the-password",
                                "replacement": "a-much-longer-secret",
                                "confirmation": "a-much-longer-secret"},
                          follow_redirects=False)
    assert "flash_error=1" in response.headers["location"]

    admin.get("/admin/logout")
    assert _login(admin, os.environ["ADMIN_BOOTSTRAP_PASSWORD"]).status_code == 303


def test_a_mistyped_confirmation_changes_nothing(admin):
    response = admin.post("/admin/password",
                          data={"current": os.environ["ADMIN_BOOTSTRAP_PASSWORD"],
                                "replacement": "a-much-longer-secret",
                                "confirmation": "a-much-longer-secrat"},
                          follow_redirects=False)
    assert "flash_error=1" in response.headers["location"]

    admin.get("/admin/logout")
    assert _login(admin, os.environ["ADMIN_BOOTSTRAP_PASSWORD"]).status_code == 303


def test_a_short_password_is_refused(admin):
    response = admin.post("/admin/password",
                          data={"current": os.environ["ADMIN_BOOTSTRAP_PASSWORD"],
                                "replacement": "short", "confirmation": "short"},
                          follow_redirects=False)
    assert "flash_error=1" in response.headers["location"]


def test_the_system_page_reports_a_shared_cache_and_checks_it_is_reachable(admin):
    """The page is how an operator finds out the shared cache is broken, so
    an unreachable Redis has to be reported rather than raising."""
    fakeredis = pytest.importorskip("fakeredis")
    pytest.importorskip("lupa")
    from app.cache import RedisAircraftCache
    from app.main import app

    redis_client = fakeredis.aioredis.FakeRedis(decode_responses=True)
    original = app.state.aggregator.cache
    app.state.aggregator.cache = RedisAircraftCache("redis://fake", client=redis_client)
    try:
        page = admin.get("/admin/system")
        assert page.status_code == 200, page.text
        assert "Shared (Redis)" in page.text
        assert "reachable at redis://fake" in page.text
        # The restart-only guidance is for the single-worker case; with the
        # shared cache on, it is replaced by the live state.
        assert "cannot be switched on from here" not in page.text

        # And when Redis has gone away.
        async def broken():
            raise ConnectionError("connection refused")

        app.state.aggregator.cache._redis.ping = broken
        page = admin.get("/admin/system")
        assert page.status_code == 200, page.text
        assert "Unreachable" in page.text
        assert "connection refused" in page.text
    finally:
        app.state.aggregator.cache = original
        client_close = getattr(redis_client, "aclose", None)
        if client_close:
            admin.portal.call(client_close)
