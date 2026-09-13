"""Airline schedules from AirLabs, attached to the aircraft already on the way.

Everything else here comes off the aircraft itself: position, altitude,
track, its registration and type. None of that says when the flight was
meant to leave, which gate it is going to, which belt the bags come out
on, or how late it is - that lives with the airline, and AirLabs is where
this deployment buys it.

Built to the same rule as the route lookups in app/routes.py, for the same
reason: a device must never wait on it. lookup() answers from cache or
returns None and queues the callsign, and the schedule arrives attached to
the aircraft on a later poll. A receiver polls every 30 seconds and an
aircraft is in range for minutes, so "later" is soon enough to be
invisible.

The free tier has a monthly cap, so the cache is the point rather than an
optimisation: one lookup per flight per cache window serves every receiver
watching that flight, and a flight nobody is watching is never looked up
at all.
"""

import asyncio
import logging
import re
import time

import httpx

logger = logging.getLogger("schedules")

BASE_URL = "https://airlabs.co/api/v9/flight"
LOOKUP_TIMEOUT_SECONDS = 12.0
MAX_CONCURRENT_LOOKUPS = 1
MAX_QUEUE = 200
# A flight that came back with nothing is not asked about again for this
# long. Most of what a receiver sees has no schedule at all - private
# aircraft, military, anything squawking a callsign that is not a flight
# number - and re-asking for those every cycle would spend the allowance on
# answers that are always the same.
MISS_TTL_SECONDS = 60 * 60
# Callsigns worth asking about: an airline prefix and a flight number.
# "G-CEMY" (a registration used as a callsign) and "N790AN" are not flights
# and asking about them wastes a lookup each.
CALLSIGN_PATTERN = re.compile(r"^([A-Z]{2,3})([0-9]{1,4}[A-Z]{0,2})$")

# What is kept out of a response. Everything here is something the panel or
# the web map can show and nothing else in the pipeline carries.
FIELDS = (
    "dep_iata", "dep_icao", "dep_terminal", "dep_gate",
    "dep_time", "dep_time_utc", "dep_estimated", "dep_estimated_utc", "dep_delayed",
    "arr_iata", "arr_icao", "arr_terminal", "arr_gate", "arr_baggage",
    "arr_time", "arr_time_utc", "arr_estimated", "arr_estimated_utc", "arr_delayed",
    "status", "duration", "airline_iata", "airline_icao", "flight_iata", "flight_icao",
)


class Schedule:
    __slots__ = ("data", "fetched_at")

    def __init__(self, data: dict | None, fetched_at: float):
        self.data = data
        self.fetched_at = fetched_at


class ScheduleResolver:
    """Looks schedules up in the background and caches what comes back."""

    def __init__(self, settings=None):
        self._settings = settings
        self._cache: dict[str, Schedule] = {}
        self._queue: asyncio.Queue[str] = asyncio.Queue(maxsize=MAX_QUEUE)
        self._queued: set[str] = set()
        self._workers: list[asyncio.Task] = []
        self._client: httpx.AsyncClient | None = None
        self.lookups = 0
        self.hits = 0
        self.misses = 0
        self.errors = 0
        self.dropped = 0

    # --- configuration, read live from the admin panel -------------------
    def enabled(self) -> bool:
        if self._settings is None:
            return False
        return bool(self._settings.get("airlabs_schedules")) and bool(self._api_key())

    def _api_key(self) -> str:
        return (self._settings.get("airlabs_api_key") or "") if self._settings else ""

    def _cache_seconds(self) -> float:
        minutes = self._settings.get("airlabs_cache_minutes") if self._settings else 30
        return float(minutes) * 60.0

    # --- lifecycle -------------------------------------------------------
    def start(self):
        # Kept if one is already set - see PhotoStore.start().
        if self._client is None:
            self._client = httpx.AsyncClient(timeout=LOOKUP_TIMEOUT_SECONDS)
        self._workers = [asyncio.create_task(self._worker())
                         for _ in range(MAX_CONCURRENT_LOOKUPS)]

    async def stop(self):
        for task in self._workers:
            task.cancel()
        for task in self._workers:
            try:
                await task
            except asyncio.CancelledError:
                pass
        self._workers.clear()
        if self._client:
            await self._client.aclose()
            self._client = None

    # --- the request path ------------------------------------------------
    def lookup(self, callsign: str | None) -> dict | None:
        """The schedule for a callsign if known, queueing it if not.

        Synchronous and non-blocking on purpose: it is called once per
        aircraft while building a response, and waiting on AirLabs here
        would move the stall from the ESP32 to the server, which is where
        every device would then feel it at once.
        """
        if not self.enabled():
            return None
        name = normalise(callsign)
        if not name:
            return None

        entry = self._cache.get(name)
        if entry is not None:
            ttl = MISS_TTL_SECONDS if entry.data is None else self._cache_seconds()
            if time.time() - entry.fetched_at < ttl:
                if entry.data is not None:
                    self.hits += 1
                return entry.data
            # Stale: fall through and queue a refresh, but keep serving what
            # is known meanwhile rather than blanking a gate mid-flight.
        self._enqueue(name)
        return entry.data if entry is not None else None

    def _enqueue(self, name: str):
        if name in self._queued:
            return
        try:
            self._queue.put_nowait(name)
        except asyncio.QueueFull:
            # A full queue means the sky is busier than the allowance can
            # keep up with. Dropping is right: the flight comes round again
            # on the next poll and the ones already queued are no less
            # deserving than this one.
            self.dropped += 1
            return
        self._queued.add(name)

    async def _worker(self):
        while True:
            name = await self._queue.get()
            try:
                await self._fetch(name)
            except asyncio.CancelledError:
                self._queued.discard(name)
                self._queue.task_done()
                raise
            except Exception:  # noqa: BLE001
                # One bad lookup must not end the worker: it is the only
                # thing draining the queue, and a dead worker would leave
                # every schedule permanently unresolved with nothing said.
                self.errors += 1
                logger.exception("schedule lookup failed for %s", name)
            finally:
                self._queued.discard(name)
                self._queue.task_done()

    async def _fetch(self, name: str):
        if self._client is None or not self.enabled():
            return
        self.lookups += 1
        response = await self._client.get(
            BASE_URL, params={"flight_icao": name, "api_key": self._api_key()})
        if response.status_code != 200:
            # Recorded as a miss so a rate limit or an outage does not turn
            # into one lookup per aircraft per poll for as long as it lasts.
            self._cache[name] = Schedule(None, time.time())
            logger.warning("AirLabs answered %d for %s", response.status_code, name)
            return
        schedule = extract(response.json())
        self._cache[name] = Schedule(schedule, time.time())
        if schedule is None:
            self.misses += 1
        else:
            logger.info("schedule for %s: %s-%s %s", name,
                        schedule.get("dep_iata") or "?",
                        schedule.get("arr_iata") or "?",
                        schedule.get("status") or "")

    def stats(self) -> dict:
        return {
            "enabled": self.enabled(),
            "configured": bool(self._api_key()),
            "cached": len(self._cache),
            "lookups": self.lookups,
            "hits": self.hits,
            "misses": self.misses,
            "errors": self.errors,
            "dropped": self.dropped,
            "queued": len(self._queued),
        }


def normalise(callsign: str | None) -> str:
    """The callsign as AirLabs wants it, or "" if it is not a flight.

    A receiver sees plenty of callsigns that are not flight numbers at all -
    a registration flown as a callsign, military, gliders. Each one would
    cost a lookup and always come back empty, so they are rejected here
    rather than by the API.
    """
    name = (callsign or "").strip().upper()
    if not name or not CALLSIGN_PATTERN.match(name):
        return ""
    return name


def extract(payload) -> dict | None:
    """The fields worth keeping out of an AirLabs reply.

    Tolerant of the envelope on purpose. The endpoint documentation shows a
    bare object, while AirLabs elsewhere wraps payloads in "response", and a
    single-flight query can reasonably come back as a one-element list.
    Accepting all three costs three lines and removes a whole class of "it
    worked in the docs" failure - which matters because this cannot be
    tried against the real API without a key.
    """
    if isinstance(payload, dict):
        if "error" in payload:
            return None
        body = payload.get("response", payload)
    else:
        body = payload
    if isinstance(body, list):
        body = body[0] if body else None
    if not isinstance(body, dict):
        return None

    schedule = {}
    for field in FIELDS:
        value = body.get(field)
        # Zero is meaningful for a delay; empty strings and None are not.
        if value is None or value == "":
            continue
        schedule[field] = value
    return schedule or None
