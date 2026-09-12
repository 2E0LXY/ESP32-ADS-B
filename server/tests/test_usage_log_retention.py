"""usage_log retention.

models.UsageLog promised it was "pruned periodically - see
prune_usage_log()" and no such function existed, so the table grew by 2,880
rows per device per day forever. These cover what the prune must and must
not delete, and that a big backlog clear does not lock out device polls.
"""

import datetime
import time

import pytest

from app import models
from app.database import SessionLocal
from app.retention import UsageLogPruner, prune_usage_log


def _account_with_devices(db, count=1):
    account = models.Account(email="r@example.com", password_hash="x")
    db.add(account)
    db.commit()
    devices = [models.Device(account_id=account.id, name=f"rx{i}") for i in range(count)]
    db.add_all(devices)
    db.commit()
    return [d.id for d in devices]


def _log(db, device_id, days_ago, returned=7):
    db.add(models.UsageLog(
        device_id=device_id,
        at=datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=days_ago),
        ip="203.0.113.7",
        aircraft_returned=returned,
    ))


def _ages(db, device_id):
    return sorted(
        round((datetime.datetime.now(datetime.timezone.utc)
               - row.at.replace(tzinfo=datetime.timezone.utc)).days)
        for row in db.query(models.UsageLog).filter(models.UsageLog.device_id == device_id)
    )


@pytest.fixture()
def db(client):
    session = SessionLocal()
    yield session
    session.close()


def test_rows_past_the_window_go_and_recent_ones_stay(db):
    device_id, = _account_with_devices(db)
    for age in (0, 1, 29, 31, 200):
        _log(db, device_id, age)
    db.commit()

    removed = prune_usage_log(db, retention_days=30)

    assert removed == 2
    assert _ages(db, device_id) == [0, 1, 29]


def test_the_newest_row_survives_however_old_it_is(db):
    """Otherwise the admin dashboard's "Last result" column blanks for any
    receiver quiet for longer than the window - the very device an operator
    is most likely to be looking at."""
    device_id, = _account_with_devices(db)
    for age in (400, 500, 600):
        _log(db, device_id, age)
    db.commit()

    removed = prune_usage_log(db, retention_days=30)

    assert removed == 2
    assert _ages(db, device_id) == [400]
    assert db.query(models.UsageLog).filter(
        models.UsageLog.device_id == device_id).first().aircraft_returned == 7


def test_every_device_keeps_its_own_newest_row(db):
    """The keep-set is per device, not one row globally."""
    first, second, third = _account_with_devices(db, 3)
    for device_id in (first, second, third):
        for age in (300, 301):
            _log(db, device_id, age)
    db.commit()

    prune_usage_log(db, retention_days=30)

    for device_id in (first, second, third):
        assert _ages(db, device_id) == [300], device_id


def test_a_prune_with_nothing_to_do_writes_nothing(db):
    device_id, = _account_with_devices(db)
    for age in (0, 2):
        _log(db, device_id, age)
    db.commit()

    assert prune_usage_log(db, retention_days=30) == 0
    assert len(_ages(db, device_id)) == 2


def test_a_backlog_is_deleted_in_bounded_batches(db):
    """One DELETE covering a month of backlog holds SQLite's single writer
    lock for its whole run, and every other writer here waits on that lock
    for at most busy_timeout before failing. So the batch size has to be
    honoured, not just the total."""
    device_id, = _account_with_devices(db)
    for _ in range(250):
        _log(db, device_id, 60)
    db.commit()

    batches = []
    removed = prune_usage_log(db, retention_days=30, batch_rows=100,
                              on_batch=batches.append)

    assert removed == 249  # 250 less the one newest row, which is always kept
    assert batches == [100, 100, 49]


def test_a_run_stops_at_the_per_run_ceiling(db):
    """A first prune against years of backlog is spread across runs rather
    than one long grind."""
    device_id, = _account_with_devices(db)
    for _ in range(60):
        _log(db, device_id, 90)
    db.commit()

    assert prune_usage_log(db, retention_days=30, batch_rows=10, max_rows=25) == 25
    assert len(_ages(db, device_id)) == 35


@pytest.mark.asyncio
async def test_the_prune_does_not_block_the_event_loop(db):
    """It runs on the same single thread that serves every device poll, so
    it has to go to a worker thread. Inline, a backlog clear would stall
    polls for the duration."""
    import asyncio

    device_id, = _account_with_devices(db)
    for _ in range(400):
        _log(db, device_id, 60)
    db.commit()

    pruner = UsageLogPruner(SessionLocal)
    ticks = 0

    async def heartbeat():
        nonlocal ticks
        while True:
            await asyncio.sleep(0.01)
            ticks += 1

    beat = asyncio.create_task(heartbeat())
    try:
        removed = await pruner.run_once()
    finally:
        beat.cancel()

    assert removed == 399
    # A loop held by a blocking prune would get few or no ticks; with the
    # work on a thread and a pause between batches it keeps running.
    assert ticks >= 3, ticks
    assert pruner.total_removed == 399
    assert pruner.last_run_at is not None
