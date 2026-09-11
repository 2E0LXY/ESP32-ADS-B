"""Server-side callsign -> route resolution.

The ESP32 used to query adsbdb itself, once per callsign, over its own TLS
connection. On that hardware each lookup cost about 2.2 seconds of blocked
network task and needed ~32 KB of contiguous internal RAM for the handshake
against roughly 31.7 KB actually free, so lookups were throttled to two per
refresh and still failed intermittently with mbedTLS allocation errors
reported as certificate failures.

Doing it here removes all of that. The device gets origin and destination
already attached to each aircraft in the /v1/aircraft response it was
fetching anyway - no extra connection, no extra handshake, no throttle.

It is also strictly less load on adsbdb than before: one resolution is
shared by every customer who can see that flight, rather than each device
asking separately for the same callsign.

Nothing here ever blocks a device's request. A callsign that is not already
resolved is queued and returned without a route; the device shows "looking
up route" and picks it up on a later poll, which is what it already does.
"""

import asyncio
import logging
import time
from dataclasses import dataclass

import httpx

logger = logging.getLogger("routes")

ADSBDB_URL = "https://api.adsbdb.com/v0/callsign/{callsign}"
USER_AGENT = "2E0LXY-ADSB-Aggregator/1.0 (+https://github.com/2E0LXY/ESP32-ADS-B)"

# A scheduled route does not change during a day of flying, and the same
# callsigns recur daily around a fixed receiver.
ROUTE_TTL_SECONDS = 6 * 60 * 60
# Negative results are cached too, and for much less time: "no route on file"
# is the permanent answer for GA traffic, but re-asking every callsign that
# ever missed would be a slow leak of requests at adsbdb for no benefit.
MISS_TTL_SECONDS = 30 * 60
# adsbdb is a free service run by one person. Resolve at a deliberate walking
# pace rather than firing a burst every poll cycle.
MAX_CONCURRENT_LOOKUPS = 2
LOOKUP_TIMEOUT_SECONDS = 10
# Bounded so a flood of unresolvable callsigns cannot grow without limit.
MAX_QUEUE = 500
MAX_CACHE = 5000


@dataclass
class Route:
    origin: str = ""
    destination: str = ""
    origin_name: str = ""
    destination_name: str = ""
    origin_city: str = ""
    destination_city: str = ""
    found: bool = False
    resolved_at: float = 0.0

    def expired(self, now: float) -> bool:
        ttl = ROUTE_TTL_SECONDS if self.found else MISS_TTL_SECONDS
        return now - self.resolved_at > ttl

    def as_dict(self) -> dict:
        return {
            "origin": self.origin,
            "destination": self.destination,
            "origin_name": self.origin_name,
            "destination_name": self.destination_name,
            "origin_city": self.origin_city,
            "destination_city": self.destination_city,
        }


def normalise(callsign: str | None) -> str:
    """Matches the firmware's normalizeCallsign(): upper case, no padding.

    adsbdb is case sensitive and the feeds pad callsigns to eight characters,
    so without this the same flight is looked up repeatedly under names that
    never match the cache.
    """
    return (callsign or "").strip().upper()


class RouteResolver:
    def __init__(self):
        self._cache: dict[str, Route] = {}
        self._queue: asyncio.Queue[str] = asyncio.Queue(maxsize=MAX_QUEUE)
        self._queued: set[str] = set()
        self._workers: list[asyncio.Task] = []
        self._client: httpx.AsyncClient | None = None
        self.lookups = 0
        self.hits = 0
        self.misses = 0

    def start(self):
        self._client = httpx.AsyncClient(
            timeout=LOOKUP_TIMEOUT_SECONDS, headers={"User-Agent": USER_AGENT}
        )
        self._workers = [asyncio.create_task(self._worker()) for _ in range(MAX_CONCURRENT_LOOKUPS)]

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

    def lookup(self, callsign: str | None) -> dict | None:
        """The route for a callsign if known, queueing it if not.

        Deliberately synchronous and non-blocking: it is called once per
        aircraft while building a response, and a device waiting on adsbdb
        here would just move the stall from the ESP32 to the server.
        """
        name = normalise(callsign)
        if len(name) < 3:
            return None
        now = time.time()
        entry = self._cache.get(name)
        if entry and not entry.expired(now):
            self.hits += 1
            return entry.as_dict() if entry.found else None
        self.misses += 1
        self._enqueue(name)
        # A stale-but-found entry is still served while the refresh runs. A
        # six-hour-old airport pair is far better than showing nothing.
        if entry and entry.found:
            return entry.as_dict()
        return None

    def _enqueue(self, name: str):
        if name in self._queued:
            return
        try:
            self._queue.put_nowait(name)
        except asyncio.QueueFull:
            return
        self._queued.add(name)

    async def _worker(self):
        while True:
            name = await self._queue.get()
            try:
                await self._resolve(name)
            except asyncio.CancelledError:
                raise
            except Exception:
                logger.exception("route lookup failed for %s", name)
            finally:
                self._queued.discard(name)
                self._queue.task_done()

    async def _resolve(self, name: str):
        if self._client is None:
            return
        self.lookups += 1
        route = Route(resolved_at=time.time())
        try:
            response = await self._client.get(ADSBDB_URL.format(callsign=name))
        except httpx.HTTPError as exc:
            # Do not cache a transport failure as "no route" - that would
            # hide a real route behind a network blip for half an hour.
            logger.debug("route %s: %s", name, exc)
            return
        if response.status_code == 404:
            # adsbdb's genuine "we have never heard of this callsign".
            self._store(name, route)
            return
        if response.status_code != 200:
            logger.debug("route %s: HTTP %s", name, response.status_code)
            return
        try:
            payload = response.json()
        except ValueError:
            return
        flight = (payload.get("response") or {}).get("flightroute") or {}
        origin = flight.get("origin") or {}
        destination = flight.get("destination") or {}
        route.origin = _code(origin)
        route.destination = _code(destination)
        route.origin_name = (origin.get("name") or "")[:64]
        route.destination_name = (destination.get("name") or "")[:64]
        route.origin_city = (origin.get("municipality") or "")[:48]
        route.destination_city = (destination.get("municipality") or "")[:48]
        route.found = bool(route.origin and route.destination)
        self._store(name, route)
        if route.found:
            logger.info("route %s %s>%s", name, route.origin, route.destination)

    def _store(self, name: str, route: Route):
        if len(self._cache) >= MAX_CACHE:
            # Drop the oldest quarter rather than one entry at a time, so
            # this eviction scan runs rarely instead of on every insert.
            for old in sorted(self._cache, key=lambda k: self._cache[k].resolved_at)[: MAX_CACHE // 4]:
                del self._cache[old]
        self._cache[name] = route

    def stats(self) -> dict:
        return {
            "cached": len(self._cache),
            "queued": self._queue.qsize(),
            "lookups": self.lookups,
            "hits": self.hits,
            "misses": self.misses,
        }


def _code(airport: dict) -> str:
    """IATA where available, ICAO otherwise - the firmware's own preference,
    since three letters fit the panel where four often do not."""
    return (airport.get("iata_code") or airport.get("icao_code") or "")[:4]
