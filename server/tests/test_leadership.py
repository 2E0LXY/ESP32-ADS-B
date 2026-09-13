"""Only one worker may poll upstream or bind the feeder ports.

Sharing the cache is what allows several uvicorn workers at all; this is
what stops them treading on each other once there are. Two failures it has
to prevent outright: asking each community API for the same sky once per
worker, and several processes racing to bind one device's feeder port.
"""

import asyncio

import pytest

from app.leader import Leadership


@pytest.fixture()
async def redis_client():
    fakeredis = pytest.importorskip("fakeredis")
    pytest.importorskip("lupa")  # fakeredis needs it to run the lease scripts
    client = fakeredis.aioredis.FakeRedis(decode_responses=True)
    yield client
    await client.flushall()
    await client.aclose()


async def test_exactly_one_of_several_workers_holds_the_role(redis_client):
    workers = [Leadership(redis_client, identity=f"worker-{n}") for n in range(4)]

    held = [await worker.acquire() for worker in workers]

    assert held.count(True) == 1
    assert [w.is_leader for w in workers].count(True) == 1


async def test_the_holder_keeps_it_across_renewals(redis_client):
    first = Leadership(redis_client, identity="first")
    second = Leadership(redis_client, identity="second")
    await first.acquire()

    for _ in range(3):
        assert await first.acquire() is True
        assert await second.acquire() is False

    assert first.is_leader and not second.is_leader


async def test_a_dead_leader_is_replaced_once_its_lease_lapses(redis_client):
    """Nothing tells us a worker has died - it is the lease expiring that
    hands the job over, with no intervention."""
    dead = Leadership(redis_client, identity="dead", lease_seconds=1)
    spare = Leadership(redis_client, identity="spare", lease_seconds=1)
    await dead.acquire()
    assert await spare.acquire() is False

    # The lease, not the process, is what expires.
    await redis_client.delete("sky:leader")

    assert await spare.acquire() is True
    # And the old leader does not simply take it back on its next renewal.
    assert await dead.acquire() is False
    assert dead.is_leader is False


async def test_a_clean_shutdown_hands_over_immediately(redis_client):
    """Otherwise the whole fleet goes unpolled until the lease expires."""
    leaving = Leadership(redis_client, identity="leaving")
    staying = Leadership(redis_client, identity="staying")
    await leaving.acquire()

    await leaving.stop()

    assert leaving.is_leader is False
    assert await staying.acquire() is True


async def test_shutting_down_never_releases_someone_elses_lease(redis_client):
    """A worker shutting down slowly must not delete the lease its
    successor has already taken - that would give two leaders at once."""
    slow = Leadership(redis_client, identity="slow")
    await slow.acquire()
    await redis_client.delete("sky:leader")
    successor = Leadership(redis_client, identity="successor")
    await successor.acquire()

    await slow.stop()

    assert await redis_client.get("sky:leader") == "successor"
    assert await successor.acquire() is True


async def test_gaining_and_losing_the_role_starts_and_stops_the_work(redis_client):
    """The feeder listeners follow the lease, not startup: a worker that
    takes over from a dead leader has to open them itself, or those
    customers' receivers stay unaccepted until someone restarts the
    service."""
    events = []
    holder = Leadership(redis_client, identity="holder")
    other = Leadership(redis_client, identity="other")
    other.on_change(events.append)

    await holder.acquire()
    await other.acquire()
    assert events == []  # never had it, so nothing to stop

    await redis_client.delete("sky:leader")
    await other.acquire()
    assert events == [True]

    # Somebody else takes it while this worker was busy.
    await redis_client.set("sky:leader", "holder")
    await other.acquire()
    assert events == [True, False]


async def test_a_callback_that_fails_does_not_cost_the_lease(redis_client):
    """A subscriber failing to bind its ports is its own problem; dropping
    the role over it would leave the deployment with no leader."""
    async def explode(_leader):
        raise RuntimeError("could not bind")

    worker = Leadership(redis_client, identity="worker")
    worker.on_change(explode)

    assert await worker.acquire() is True
    assert worker.is_leader is True


async def test_redis_going_away_gives_up_the_role_rather_than_assuming_it(redis_client):
    """A worker that cannot reach Redis cannot know it still holds the
    lease. Carrying on polling would mean two leaders as soon as somebody
    else takes over."""
    worker = Leadership(redis_client, identity="worker", renew_seconds=1)
    events = []
    worker.on_change(events.append)
    await worker.acquire()
    assert worker.is_leader is True

    async def broken(*_args, **_kwargs):
        raise ConnectionError("redis is gone")

    worker._acquire_script = broken
    worker.start()
    try:
        await asyncio.sleep(0.05)
    finally:
        await worker.stop()

    assert worker.is_leader is False
    assert events[-1] is False


async def test_the_renewal_loop_survives_a_failure_and_recovers(redis_client):
    """Redis being briefly unreachable must not end the loop, or there is
    no leader until somebody restarts the service."""
    worker = Leadership(redis_client, identity="worker", renew_seconds=1)
    calls = []
    real = worker._acquire_script

    async def flaky(*args, **kwargs):
        calls.append(1)
        if len(calls) == 1:
            raise ConnectionError("transient")
        return await real(*args, **kwargs)

    worker._acquire_script = flaky
    worker.start()
    try:
        for _ in range(60):
            await asyncio.sleep(0.05)
            if worker.is_leader:
                break
    finally:
        await worker.stop()

    assert len(calls) >= 2
    assert worker.is_leader is False  # stop() released it
    assert await redis_client.get("sky:leader") is None


def test_with_no_redis_the_single_process_is_always_the_leader():
    """The deployment's default. Redis must not become a requirement for
    anyone who has not asked for more than one worker."""
    worker = Leadership(None)
    assert worker.is_leader is True
    assert worker.shared is False


async def test_with_no_redis_there_is_nothing_to_renew_or_release():
    worker = Leadership(None)
    worker.start()
    assert await worker.acquire() is True
    await worker.stop()
    # Still the leader: there is no one to hand over to.
    assert worker.is_leader is True


# --- what the aggregator does with it ----------------------------------

class _FakeLeadership:
    def __init__(self, leader):
        self.is_leader = leader


async def test_a_follower_worker_does_not_poll_upstream(monkeypatch):
    """The whole point: N workers must not ask each community API for the
    same sky N times every cycle."""
    from app.aggregator import Aggregator

    polled = []

    async def record(self, client):
        polled.append(1)

    monkeypatch.setattr(Aggregator, "_poll_all", record)
    monkeypatch.setattr("app.aggregator.POLL_INTERVAL_SECONDS", 0.01)

    follower = Aggregator(53.73, -1.57, 50, None, leadership=_FakeLeadership(False))
    follower.start()
    await asyncio.sleep(0.05)
    await follower.stop()

    assert polled == []
    # But it still serves device polls from the shared cache, which is the
    # only reason a follower exists.
    assert follower.cache is not None


async def test_the_leader_does_poll_upstream(monkeypatch):
    from app.aggregator import Aggregator

    polled = []

    async def record(self, client):
        polled.append(1)

    monkeypatch.setattr(Aggregator, "_poll_all", record)
    monkeypatch.setattr("app.aggregator.POLL_INTERVAL_SECONDS", 0.01)

    leader = Aggregator(53.73, -1.57, 50, None, leadership=_FakeLeadership(True))
    leader.start()
    await asyncio.sleep(0.05)
    await leader.stop()

    assert polled
    assert leader.polls_upstream() is True


async def test_with_no_leadership_at_all_it_polls_as_it_always_did():
    """A single-process deployment passes no leadership object, and must
    behave exactly as before."""
    from app.aggregator import Aggregator

    assert Aggregator(53.73, -1.57, 50, None).polls_upstream() is True
