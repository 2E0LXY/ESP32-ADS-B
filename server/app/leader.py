"""Which worker process does the work that must only happen once.

Sharing the aircraft cache (see app/cache.py) lets several uvicorn workers
serve device polls from one view of the sky. It does not make everything
else safe to run several times over, and two of them would break outright:

* Upstream polling. Each worker running its own poll loop multiplies the
  requests this deployment makes to free community APIs by the worker
  count. One of them already answers 403 to us; the rest deserve better
  than being asked for the same sky four times every fifteen seconds.
* Feeder ingestion. Each feeder-enabled device gets a dedicated TCP port
  (see app/feed_ingest.py). Only one process can bind a given port, so
  with several workers all but one would fail to listen and those
  customers' receivers would simply stop being accepted.

So one worker holds a short lease in Redis and does that work; the others
serve requests only. The lease expires on its own, which means a worker
that is killed, hangs or loses its network hands the job over without
anybody intervening - the successor just has to outlive the TTL.

With no Redis there is one process by definition, so it is always the
leader and none of this is on the path. That is the behaviour this
deployment has always had, and it stays the default.
"""

import asyncio
import inspect
import logging
import os
import socket
import uuid

logger = logging.getLogger("leader")

# The lease. Long enough that a normally busy worker never loses it to a
# slow renewal, short enough that a dead leader is replaced promptly.
LEASE_SECONDS = int(os.environ.get("LEADER_LEASE_SECONDS", "30"))
# Renewed at a third of the lease, so two consecutive failures - a blocked
# loop, a dropped packet - still leave time to recover before it lapses.
RENEW_SECONDS = max(1, LEASE_SECONDS // 3)

# Take the lease if it is free, extend it if we already hold it, and do
# nothing if somebody else has it. As one script so that checking and
# taking cannot be separated: with GET then SET, two workers can both read
# "free" and both believe they are the leader, which is exactly the
# double-polling this exists to prevent.
_ACQUIRE_LUA = """
local holder = redis.call('GET', KEYS[1])
if (not holder) or holder == ARGV[1] then
  redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2])
  return 1
end
return 0
"""

# Only ever release a lease we still hold - otherwise a worker shutting
# down slowly could delete its successor's.
_RELEASE_LUA = """
if redis.call('GET', KEYS[1]) == ARGV[1] then
  return redis.call('DEL', KEYS[1])
end
return 0
"""


class Leadership:
    """A renewable lease on the single-instance work.

    Callbacks registered with on_change are awaited on every transition, so
    a subscriber can start listeners when it gains the role and close them
    when it loses it.
    """

    def __init__(self, redis=None, key: str = "sky:leader", identity: str | None = None,
                 lease_seconds: int = LEASE_SECONDS, renew_seconds: int = RENEW_SECONDS):
        self._redis = redis
        self._key = key
        # Host and pid, so the admin panel and the logs can say which
        # process is actually doing the polling.
        self.identity = identity or f"{socket.gethostname()}:{os.getpid()}:{uuid.uuid4().hex[:6]}"
        self._lease = lease_seconds
        self._renew = renew_seconds
        self._acquire_script = redis.register_script(_ACQUIRE_LUA) if redis is not None else None
        self._release_script = redis.register_script(_RELEASE_LUA) if redis is not None else None
        # No Redis means one process, which is therefore the leader. Never
        # a state this has to transition into, so no callback fires for it.
        self.is_leader = redis is None
        self.shared = redis is not None
        self._task: asyncio.Task | None = None
        self._callbacks: list = []

    def on_change(self, callback):
        self._callbacks.append(callback)

    async def _announce(self, leader: bool):
        for callback in self._callbacks:
            try:
                # Sync callbacks are allowed. Awaiting the None a plain
                # function returns raises TypeError, which the handler
                # below would swallow - so the subscriber would look like
                # it worked while logging an exception every transition.
                result = callback(leader)
                if inspect.isawaitable(result):
                    await result
            except Exception:  # noqa: BLE001
                # A subscriber that fails to start its listeners must not
                # cost us the lease or stop the other subscribers.
                logger.exception("leadership callback failed")

    async def acquire(self) -> bool:
        """Takes or renews the lease. Returns whether this process holds it."""
        if self._redis is None:
            return True
        held = bool(await self._acquire_script(keys=[self._key],
                                              args=[self.identity, self._lease]))
        if held != self.is_leader:
            self.is_leader = held
            logger.info("%s the single-instance role as %s",
                        "took" if held else "lost", self.identity)
            await self._announce(held)
        return held

    def start(self):
        if self._redis is None:
            return
        self._task = asyncio.create_task(self._loop())

    async def stop(self):
        if self._task:
            self._task.cancel()
            self._task = None
        if self._redis is not None and self.is_leader:
            # Hand over immediately on a clean shutdown rather than leaving
            # the fleet unpolled until the lease expires.
            await self._release_script(keys=[self._key], args=[self.identity])
            self.is_leader = False

    async def _loop(self):
        while True:
            try:
                await self.acquire()
            except asyncio.CancelledError:
                raise
            except Exception:  # noqa: BLE001
                # Redis being briefly unreachable must not end this loop, or
                # the deployment would have no leader until a restart. The
                # lease lapses, somebody takes over, and this worker rejoins
                # the contest on the next tick.
                logger.exception("leader lease renewal failed")
                if self.is_leader:
                    self.is_leader = False
                    await self._announce(False)
            await asyncio.sleep(self._renew)
