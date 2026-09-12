"""Keeps usage_log from growing without bound.

models.UsageLog has claimed since it was written that it is "pruned
periodically - see prune_usage_log()". There was no such function. One row
is written per /v1/aircraft request and nothing ever deleted one, so the
table grew forever:

    one device  = 1 poll / 30s = 2,880 rows/day
    measured    = 95 bytes/row including both indexes
    300 devices = 864,000 rows/day, 82 MB/day, ~2.5 GB/month

That is the constraint that would have taken the box down first - well
before CPU, RAM or bandwidth - and on a VPS the symptom is a full disk,
which stops writes for feeders and device polls alike.

Two things this deliberately does NOT do:

* It does not VACUUM. Vacuuming needs an exclusive lock on the whole
  database and rewrites the entire file, which on a multi-gigabyte table
  would park every device poll and every feeder write for as long as it
  takes. Freed pages are reused by later inserts instead, so the file
  stops growing rather than shrinking - which is the actual goal. Shrinking
  the file is a one-off maintenance job for a human, not something to do
  behind a running service.
* It does not delete in one statement. A single DELETE covering a month of
  backlog holds SQLite's one writer lock for the whole run, and every
  concurrent writer here waits on that lock for at most busy_timeout (3s)
  before failing outright. So it deletes in bounded batches and yields
  between them.
"""

import asyncio
import datetime
import logging
import os
import time

from sqlalchemy import delete, func, select, tuple_

from . import models

logger = logging.getLogger("retention")

# How much history to keep. Usage rows exist for quotas, abuse
# investigation and "is this receiver actually working" - all questions
# about recent behaviour. A month is generous for all three.
RETENTION_DAYS = int(os.environ.get("USAGE_LOG_RETENTION_DAYS", "30"))
# Rows per DELETE. Small enough that the writer lock is held for
# milliseconds, large enough to clear a day's backlog from a big fleet in a
# handful of batches.
BATCH_ROWS = int(os.environ.get("USAGE_LOG_PRUNE_BATCH", "2000"))
# Pause between batches, so a backlog clear stays in the background instead
# of monopolising the one thread that also serves every device poll.
BATCH_PAUSE_SECONDS = 0.05
# A prune run is cheap and there is no value in a tight schedule; this is
# about a slow leak, not a spike.
INTERVAL_SECONDS = int(os.environ.get("USAGE_LOG_PRUNE_INTERVAL_SECONDS", str(6 * 3600)))
# Safety valve: never delete more than this in one run, so a first prune
# against years of backlog is spread over several runs rather than one long
# grind. 30 devices' worth of a month, roughly.
MAX_ROWS_PER_RUN = int(os.environ.get("USAGE_LOG_PRUNE_MAX_PER_RUN", "200000"))


def _latest_row_per_device(db) -> list[int]:
    """The one row per device that must survive whatever its age.

    The admin dashboard's "Last result" column reads each device's most
    recent UsageLog row. Pruning purely by age would blank that for any
    receiver quiet for longer than the retention window - exactly the
    device an operator most wants to look at. So the newest row for each
    device is always kept, however old it is.

    "Newest" means the largest `at`, not the largest id. They agree for
    rows written by device polls, but not for backfilled or imported rows,
    and keeping the wrong one would leave the dashboard reporting an older
    poll as the last result.
    """
    latest = db.execute(
        select(models.UsageLog.device_id, func.max(models.UsageLog.at))
        .group_by(models.UsageLog.device_id)
    ).all()
    if not latest:
        return []
    # One more query rather than one per device: a deployment can have
    # hundreds of receivers and this is housekeeping, not a request path.
    rows = db.execute(
        select(models.UsageLog.id, models.UsageLog.device_id)
        .where(tuple_(models.UsageLog.device_id, models.UsageLog.at).in_(latest))
    ).all()
    # Two rows can share a device's newest timestamp; keeping one of them
    # is enough to answer "what did its last poll return".
    keep: dict[int, int] = {}
    for row_id, device_id in rows:
        keep.setdefault(device_id, row_id)
    return list(keep.values())


def prune_usage_log(db, retention_days: int = RETENTION_DAYS, batch_rows: int = BATCH_ROWS,
                    max_rows: int = MAX_ROWS_PER_RUN, on_batch=None) -> int:
    """Deletes usage rows older than the retention window. Returns the count.

    Synchronous and batched: intended to be called from a worker thread, and
    `on_batch` is invoked after each committed batch so an async caller can
    yield the loop between them.
    """
    cutoff = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=retention_days)
    keep = _latest_row_per_device(db)
    removed = 0
    while removed < max_rows:
        doomed = db.execute(
            select(models.UsageLog.id)
            .where(models.UsageLog.at < cutoff, models.UsageLog.id.notin_(keep))
            .limit(min(batch_rows, max_rows - removed))
        ).scalars().all()
        if not doomed:
            break
        db.execute(delete(models.UsageLog).where(models.UsageLog.id.in_(doomed)))
        db.commit()
        removed += len(doomed)
        if on_batch is not None:
            on_batch(len(doomed))
        if len(doomed) < batch_rows:
            break
    return removed


class UsageLogPruner:
    """Background task that runs prune_usage_log on a schedule."""

    def __init__(self, session_factory):
        self._session_factory = session_factory
        self._task: asyncio.Task | None = None
        self.last_removed = 0
        self.total_removed = 0
        self.last_run_at: datetime.datetime | None = None

    def start(self):
        self._task = asyncio.create_task(self._loop())

    async def stop(self):
        if self._task:
            self._task.cancel()
            self._task = None

    async def run_once(self) -> int:
        def work() -> int:
            db = self._session_factory()
            try:
                # Sleeping in the worker thread releases the GIL and, more
                # importantly, releases SQLite's writer lock between
                # batches, so device polls interleave with a backlog clear
                # instead of queueing behind it.
                return prune_usage_log(db, on_batch=lambda _n: time.sleep(BATCH_PAUSE_SECONDS))
            finally:
                db.close()

        # to_thread, not inline: every statement in here is a blocking
        # SQLAlchemy call, and this process serves device polls on the same
        # event loop thread.
        removed = await asyncio.to_thread(work)
        self.last_removed = removed
        self.total_removed += removed
        self.last_run_at = datetime.datetime.now(datetime.timezone.utc)
        if removed:
            logger.info("pruned %d usage_log rows older than %d days", removed, RETENTION_DAYS)
        return removed

    async def _loop(self):
        while True:
            try:
                await self.run_once()
            except asyncio.CancelledError:
                raise
            except Exception:  # noqa: BLE001
                # A failed prune must not end the task: the whole point is
                # that it keeps running for the life of the deployment.
                logger.exception("usage_log prune failed")
            await asyncio.sleep(INTERVAL_SECONDS)
