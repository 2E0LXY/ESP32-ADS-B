"""The aircraft worth looking up for.

What these mostly guard is restraint. An alert stream that fires eighty
times for one aircraft squawking 7700 for twenty minutes is worse than no
alert stream, because the one that matters is buried in it.
"""

import time

from app.alerts import EMERGENCY_SQUAWKS, MAX_ALERTS, REPEAT_AFTER_SECONDS, AlertWatcher
from app.runtime_settings import DEFINITIONS, SettingsStore


class _Reference:
    """Stands in for the reference lists, which map a callsign prefix to a
    military operator - the same table the display uses to name them."""
    military = {"RRR": "Royal Air Force", "RCH": "United States Air Force"}


def _settings(**overrides):
    store = SettingsStore()
    store._values.update(overrides)
    return store


def _watcher(**overrides):
    values = {"alerts_enabled": True, "alert_emergency": True, "alert_military": True}
    values.update(overrides)
    return AlertWatcher(settings=_settings(**values), reference=_Reference())


def _aircraft(**fields):
    base = {"hex": "4ca2d6", "flight": "EZY51NR", "lat": 53.7, "lon": -1.5}
    base.update(fields)
    return base


# --- what counts as notable ---------------------------------------------

def test_the_three_emergency_squawks_raise_an_alert():
    for code, meaning in EMERGENCY_SQUAWKS.items():
        watcher = _watcher()
        watcher.observe([_aircraft(squawk=code)], now=1000.0)

        alert, = watcher.recent(now=1000.0)
        assert alert["kind"] == "emergency"
        assert alert["label"] == meaning
        assert alert["detail"] == f"Squawk {code}"
        assert alert["flight"] == "EZY51NR"


def test_an_ordinary_squawk_raises_nothing():
    watcher = _watcher()
    watcher.observe([_aircraft(squawk="7000"), _aircraft(hex="abc123", squawk="")],
                    now=1000.0)

    assert watcher.recent() == []


def test_a_declared_emergency_without_the_squawk_still_counts():
    """Some feeds carry an explicit field; 'none' is how most of them say
    there isn't one."""
    watcher = _watcher()
    watcher.observe([_aircraft(emergency="lifeguard")], now=1000.0)
    watcher.observe([_aircraft(hex="abc123", emergency="none")], now=1000.0)

    alerts = watcher.recent()
    assert len(alerts) == 1
    assert alerts[0]["detail"] == "lifeguard"


def test_a_7700_with_an_emergency_field_is_one_alert_not_two():
    watcher = _watcher()
    watcher.observe([_aircraft(squawk="7700", emergency="general")], now=1000.0)

    assert len(watcher.recent()) == 1


def test_military_is_matched_from_the_reference_prefixes():
    watcher = _watcher()
    watcher.observe([_aircraft(flight="RRR7241", t="A400")], now=1000.0)

    alert, = watcher.recent()
    assert alert["kind"] == "military"
    assert alert["label"] == "Royal Air Force"
    assert alert["detail"] == "A400"


def test_an_airliner_is_not_military():
    watcher = _watcher()
    watcher.observe([_aircraft(flight="RYR2BH")], now=1000.0)

    assert watcher.recent() == []


def test_one_aircraft_can_be_notable_twice():
    """A military aircraft squawking 7700 is both things, and saying only
    one of them would lose the half that matters more."""
    watcher = _watcher()
    watcher.observe([_aircraft(flight="RRR7241", squawk="7700")], now=1000.0)

    kinds = {alert["kind"] for alert in watcher.recent()}
    assert kinds == {"emergency", "military"}


# --- restraint ----------------------------------------------------------

def test_a_continuing_emergency_is_one_alert_not_one_per_poll():
    """The whole point. Twenty minutes of 7700 at a fifteen-second poll is
    eighty cycles and one event."""
    watcher = _watcher()
    for cycle in range(80):
        watcher.observe([_aircraft(squawk="7700")], now=1000.0 + cycle * 15)

    assert len(watcher.recent(limit=100, now=1000.0 + 80 * 15)) == 1


def test_the_same_aircraft_can_raise_again_much_later():
    watcher = _watcher()
    watcher.observe([_aircraft(squawk="7700")], now=1000.0)
    later = 1000.0 + REPEAT_AFTER_SECONDS + 60
    watcher.observe([_aircraft(squawk="7700")], now=later)

    assert len(watcher.recent(limit=100, now=later)) == 2


def test_alerts_expire():
    watcher = _watcher(alert_retention_minutes=5)
    watcher.observe([_aircraft(squawk="7700")], now=1000.0)

    watcher.prune(now=1000.0 + 4 * 60)
    assert len(watcher.recent()) == 1

    watcher.prune(now=1000.0 + 6 * 60)
    assert watcher.recent() == []


def test_the_list_cannot_grow_without_bound():
    watcher = _watcher(alert_retention_minutes=1440)
    for i in range(MAX_ALERTS + 50):
        watcher.observe([_aircraft(hex=f"{i:06x}", squawk="7700")], now=1000.0 + i)

    assert len(watcher._alerts) == MAX_ALERTS


def test_newest_first():
    watcher = _watcher()
    watcher.observe([_aircraft(hex="aaa111", squawk="7600")], now=1000.0)
    watcher.observe([_aircraft(hex="bbb222", squawk="7700")], now=2000.0)

    alerts = watcher.recent(now=2000.0)
    assert [a["hex"] for a in alerts] == ["bbb222", "aaa111"]
    assert alerts[0]["age"] == 0.0 and alerts[1]["age"] == 1000.0


# --- switches -----------------------------------------------------------

def test_each_kind_can_be_switched_off_on_its_own():
    no_military = _watcher(alert_military=False)
    no_military.observe([_aircraft(flight="RRR7241", squawk="7700")], now=1000.0)
    assert {a["kind"] for a in no_military.recent()} == {"emergency"}

    no_emergency = _watcher(alert_emergency=False)
    no_emergency.observe([_aircraft(flight="RRR7241", squawk="7700")], now=1000.0)
    assert {a["kind"] for a in no_emergency.recent()} == {"military"}


def test_switched_off_raises_nothing_at_all():
    watcher = _watcher(alerts_enabled=False)
    watcher.observe([_aircraft(squawk="7700")], now=1000.0)

    assert watcher.recent() == []
    assert watcher.stats()["enabled"] is False


def test_a_watcher_with_no_settings_does_nothing():
    watcher = AlertWatcher()
    watcher.observe([_aircraft(squawk="7700")])

    assert watcher.recent() == []


def test_an_aircraft_with_no_hex_is_skipped():
    watcher = _watcher()
    watcher.observe([{"squawk": "7700", "flight": "GHOST"}], now=1000.0)

    assert watcher.recent() == []


def test_the_settings_page_offers_the_alert_controls():
    names = {d.name for d in DEFINITIONS if d.group == "Alerts"}

    assert names == {"alerts_enabled", "alert_emergency", "alert_military",
                     "alert_first_sighting", "alert_retention_minutes"}


# --- the endpoint -------------------------------------------------------

def _issue_key(client) -> str:
    client.post("/signup", data={"email": "alerts@example.com", "password": "correct-horse"},
                follow_redirects=False)
    client.post("/devices", data={"name": "Alert test"}, follow_redirects=False)

    from app import models, security
    from app.database import SessionLocal

    db = SessionLocal()
    try:
        device_id = db.query(models.Device).first().id
    finally:
        db.close()
    client.post(f"/devices/{device_id}/reissue-key", follow_redirects=False)
    return security.read_flash_token(client.cookies.get("flash_key"))["key"]


def test_the_endpoint_serves_what_has_been_raised(client):
    from app.main import app

    key = _issue_key(client)
    app.state.alerts._settings = _settings(alerts_enabled=True, alert_emergency=True,
                                           alert_military=True)
    app.state.alerts.observe([_aircraft(squawk="7700")], now=time.time())

    response = client.get("/v1/alerts", headers={"Authorization": f"Bearer {key}"})

    body = response.json()
    assert body["enabled"] is True and body["count"] >= 1
    assert body["alerts"][0]["label"] == "General emergency"


def test_switched_off_says_so_rather_than_reporting_a_quiet_sky(client):
    from app.main import app

    key = _issue_key(client)
    app.state.alerts._settings = _settings(alerts_enabled=False)

    response = client.get("/v1/alerts", headers={"Authorization": f"Bearer {key}"})

    assert response.json() == {"enabled": False, "alerts": [], "count": 0}


def test_alerts_need_a_device_key(client):
    assert client.get("/v1/alerts").status_code in (401, 403)
