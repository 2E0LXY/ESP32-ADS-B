"""The event loop must stay free while the database is busy.

A customer's receiver feeds this backend over a plain TCP connection that
one asyncio event loop accepts and reads. Everything else the service does
runs on that same loop, so any synchronous database call made directly from
a coroutine stops reading every live feed for as long as SQLite takes to
answer - and on a rollback-journal database with several writers that is
long enough for a feeder to give up and reconnect.

These tests hold the database busy on purpose and check the loop keeps
running anyway.
"""

import asyncio
import socket
import time
import types

import pytest

from app import models
from app.aggregator import Aggregator
from app.database import Base, SessionLocal, engine
from app.feed_ingest import FeedIngestManager
from app.routers import public

# How long the stand-in database call is made to take, and how much loop
# latency is tolerated while it runs. The gap between them is wide because
# CI timing is noisy; the failure this guards against is a full stall for
# the whole SLOW_DB_SECONDS, which is nowhere near MAX_LOOP_STALL_SECONDS.
SLOW_DB_SECONDS = 0.5
MAX_LOOP_STALL_SECONDS = 0.15


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class _Heartbeat:
    """Records the longest gap between consecutive loop iterations."""

    def __init__(self):
        self.worst = 0.0
        self._task = None

    async def __aenter__(self):
        self._task = asyncio.ensure_future(self._run())
        await asyncio.sleep(0.02)  # let it establish a baseline
        return self

    async def _run(self):
        previous = time.monotonic()
        while True:
            await asyncio.sleep(0.005)
            now = time.monotonic()
            self.worst = max(self.worst, now - previous)
            previous = now

    async def __aexit__(self, *_exc):
        # Let it run once more before cancelling. A stall is only visible to
        # this task when it finally gets the loop back, so cancelling
        # straight away would throw away the very measurement being taken.
        await asyncio.sleep(0.02)
        self._task.cancel()


class _StubResolver:
    def lookup(self, _callsign):
        return None


async def test_device_poll_does_not_stall_the_loop(monkeypatch):
    """A /v1/aircraft request writes to the database on every call.

    Before this was moved off the loop, a device polling every few seconds
    parked the whole service - feeder listeners included - inside SQLite.
    """
    aggregator = Aggregator(0.0, 0.0, 50, SessionLocal)
    request = types.SimpleNamespace(
        app=types.SimpleNamespace(
            state=types.SimpleNamespace(aggregator=aggregator, routes=_StubResolver())
        ),
        client=types.SimpleNamespace(host="127.0.0.1"),
    )

    def slow_write(*_args, **_kwargs):
        time.sleep(SLOW_DB_SECONDS)

    monkeypatch.setattr(public, "_record_poll", slow_write)

    async with _Heartbeat() as heartbeat:
        result = await public.get_aircraft(
            lat=53.7, lon=-1.5, radius=50, request=request,
            device=models.Device(id=1, account_id=1, name="test"), db=None,
        )

    assert result["total"] == 0
    assert heartbeat.worst < MAX_LOOP_STALL_SECONDS, (
        f"event loop stalled for {heartbeat.worst:.3f}s during a device poll"
    )


async def test_feeder_is_still_accepted_while_the_database_is_busy():
    """The property that actually matters to a feeding receiver."""
    aggregator = Aggregator(0.0, 0.0, 50, SessionLocal)
    manager = FeedIngestManager(aggregator, SessionLocal)
    port = _free_port()
    await manager.start_for_device(1, port)
    try:

        # The real helper every feeder database write goes through, given a
        # slow call to make. Run inline it would hold the loop and this
        # connection would not be accepted until it finished.
        work = asyncio.ensure_future(manager._in_thread(time.sleep, SLOW_DB_SECONDS))

        # Queued before the connection attempt and not awaited first, so the
        # loop reaches it the moment anything yields. If it holds the loop
        # instead of handing the work to a thread, this connection waits
        # behind it.
        started = time.monotonic()
        _reader, writer = await asyncio.wait_for(
            asyncio.open_connection("127.0.0.1", port), timeout=5.0
        )
        elapsed = time.monotonic() - started
        writer.close()
        await work

        assert elapsed < MAX_LOOP_STALL_SECONDS, (
            f"feeder connection took {elapsed:.3f}s to be accepted while the "
            "database was busy"
        )
    finally:
        await manager.stop_for_device(port)


async def test_a_failing_database_write_does_not_end_the_feed():
    """A merge loop that died on an exception left the connection up and
    silently contributing nothing, which looks exactly like a dead feed."""
    manager = FeedIngestManager(Aggregator(0.0, 0.0, 50, SessionLocal), SessionLocal)
    calls = []

    def explode(_device_id):
        calls.append(1)
        raise RuntimeError("database is locked")

    await manager._in_thread(explode, 1)
    await manager._in_thread(explode, 1)
    assert len(calls) == 2  # the first failure did not stop the second


async def test_poll_regions_is_read_once_per_cycle():
    """Three upstreams asked the device table separately for the same
    answer, three synchronous queries deep in the poll cycle."""
    Base.metadata.create_all(bind=engine)  # this test does not use the client fixture
    aggregator = Aggregator(0.0, 0.0, 50, SessionLocal)
    calls = []
    real = aggregator.poll_regions

    def counted():
        calls.append(1)
        return real()

    aggregator.poll_regions = counted

    class _NoopClient:
        async def get(self, _url):
            raise RuntimeError("upstream disabled for this test")

    await aggregator._poll_all(_NoopClient())
    assert len(calls) == 1
