"""What this deployment has seen before.

These are mostly about the database NOT being touched. The table is small -
bounded by distinct airframes - but two hundred aircraft on a fifteen-second
poll is 576,000 row touches a day on the same SQLite file the feeder
listeners and every device poll share, and that would be far heavier than
the traffic it records.
"""

import datetime
import time

import pytest

from app import models
from app.database import SessionLocal
from app.logbook import VISIT_GAP_SECONDS, Logbook
from app.runtime_settings import DEFINITIONS, SettingsStore


def _settings(**overrides):
    store = SettingsStore()
    store._values.update(overrides)
    return store


def _logbook(**overrides):
    values = {"logbook_enabled": True}
    values.update(overrides)
    book = Logbook(session_factory=SessionLocal, settings=_settings(**values))
    book.load()
    return book


def _aircraft(hex_id="4ca2d6", **fields):
    base = {"hex": hex_id, "flight": "EZY51NR", "r": "G-EZTK", "t": "A320",
            "ownOp": "easyJet"}
    base.update(fields)
    return base


@pytest.fixture(autouse=True)
def _clean_sightings(client):
    """The suite shares one database file, so start from an empty logbook."""
    db = SessionLocal()
    try:
        db.query(models.Sighting).delete()
        db.commit()
    finally:
        db.close()
    yield


def _rows():
    db = SessionLocal()
    try:
        return {row.hex: row for row in db.query(models.Sighting).all()}
    finally:
        db.close()


# --- what it remembers ---------------------------------------------------

async def test_an_aircraft_never_seen_before_is_reported_as_new():
    book = _logbook()

    firsts = book.observe([_aircraft()], now=1000.0)

    assert len(firsts) == 1
    assert firsts[0].hex == "4ca2d6"
    assert firsts[0].registration == "G-EZTK"
    assert firsts[0].operator == "easyJet"


async def test_the_same_aircraft_is_new_only_once():
    book = _logbook()

    book.observe([_aircraft()], now=1000.0)
    again = book.observe([_aircraft()], now=1015.0)

    assert again == []
    assert book.stats()["firsts_since_restart"] == 1


async def test_what_was_seen_survives_a_restart():
    book = _logbook()
    book.observe([_aircraft()], now=1000.0)
    await book.flush(now=1000.0)

    # A fresh Logbook is what a restarted process gets.
    after_restart = _logbook()

    assert after_restart.observe([_aircraft()], now=2000.0) == []
    assert after_restart.stats()["known"] == 1


async def test_a_flush_writes_the_identity_it_had():
    book = _logbook()
    book.observe([_aircraft()], now=1000.0)

    await book.flush(now=1000.0)

    row = _rows()["4ca2d6"]
    assert (row.registration, row.type_code, row.operator) == ("G-EZTK", "A320", "easyJet")
    assert row.visits == 1


async def test_a_later_report_without_a_registration_does_not_erase_one():
    """A position-only message carries none of the identity fields, and
    letting it blank them would lose what an earlier message knew."""
    book = _logbook()
    book.observe([_aircraft()], now=1000.0)
    await book.flush(now=1000.0)

    book.observe([{"hex": "4ca2d6"}], now=2000.0)
    await book.flush(now=2000.0)

    row = _rows()["4ca2d6"]
    assert row.registration == "G-EZTK" and row.operator == "easyJet"


# --- visits, not polls ---------------------------------------------------

async def test_ten_minutes_overhead_is_one_visit():
    book = _logbook()
    start = time.time()
    for cycle in range(40):           # ten minutes at a fifteen-second poll
        book.observe([_aircraft()], now=start + cycle * 15)
        await book.flush(now=start + cycle * 15)

    assert _rows()["4ca2d6"].visits == 1


async def test_coming_back_later_is_a_second_visit():
    book = _logbook()
    start = time.time()
    book.observe([_aircraft()], now=start)
    await book.flush(now=start)

    later = start + VISIT_GAP_SECONDS + 60
    book.observe([_aircraft()], now=later)
    await book.flush(now=later)

    assert _rows()["4ca2d6"].visits == 2


# --- not touching the database ------------------------------------------

def test_observing_writes_nothing_by_itself():
    """The whole design: a poll costs a set lookup, not a query."""
    book = _logbook()

    book.observe([_aircraft(hex_id=f"{i:06x}") for i in range(200)], now=1000.0)

    assert _rows() == {}
    assert book.stats()["pending"] == 200


async def test_the_first_batch_is_written_at_the_first_opportunity():
    """Not made to wait out the interval. The batching is there to stop a
    write per aircraft per poll, and the first cycle after a restart is all
    first sightings - worth persisting promptly, because a crash before the
    first sweep would announce every one of them again."""
    book = _logbook()
    book.observe([_aircraft()], now=1000.0)

    assert book.due_for_flush(now=1000.0) is True
    await book.flush(now=1000.0)

    # And then it waits, which is the part that matters for the write rate.
    book.observe([_aircraft(hex_id="abc123")], now=1030.0)
    assert book.due_for_flush(now=1030.0) is False
    assert book.due_for_flush(now=1000.0 + 6 * 60) is True


def test_nothing_pending_is_never_due():
    book = _logbook()

    assert book.due_for_flush(now=1000.0) is False


async def test_nothing_pending_means_nothing_written():
    book = _logbook()

    assert await book.flush() == 0
    assert book.stats()["flushes"] == 0


async def test_switched_off_records_nothing():
    book = _logbook(logbook_enabled=False)

    assert book.observe([_aircraft()], now=1000.0) == []
    assert await book.flush() == 0
    assert _rows() == {}


def test_a_logbook_with_no_database_does_nothing():
    book = Logbook(settings=_settings(logbook_enabled=True))

    assert book.enabled() is False
    assert book.observe([_aircraft()]) == []


def test_an_aircraft_with_no_hex_is_skipped():
    book = _logbook()

    assert book.observe([{"flight": "GHOST"}], now=1000.0) == []


def test_the_settings_page_offers_the_logbook_control():
    names = {d.name for d in DEFINITIONS if d.group == "Logbook"}

    assert names == {"logbook_enabled"}


# --- and the alert it feeds ---------------------------------------------

def test_a_first_sighting_becomes_an_alert():
    from app.alerts import AlertWatcher

    watcher = AlertWatcher(settings=_settings(
        alerts_enabled=True, alert_first_sighting=True))
    book = _logbook()
    firsts = book.observe([_aircraft()], now=1000.0)

    watcher.note_first_sightings(firsts, now=1000.0)

    alert, = watcher.recent(now=1000.0)
    assert alert["kind"] == "first"
    assert alert["label"] == "easyJet"
    assert alert["detail"] == "A320 G-EZTK"


def test_first_sighting_alerts_can_be_switched_off_on_their_own():
    from app.alerts import AlertWatcher

    watcher = AlertWatcher(settings=_settings(
        alerts_enabled=True, alert_first_sighting=False))
    book = _logbook()

    watcher.note_first_sightings(book.observe([_aircraft()], now=1000.0), now=1000.0)

    assert watcher.recent() == []
