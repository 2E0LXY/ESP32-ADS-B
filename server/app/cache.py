"""The shared aircraft cache.

There are two implementations behind one interface:

* AircraftCache - an in-process dict. Correct and fast for exactly one
  worker process, which is how this has always run.
* RedisAircraftCache - the same thing in Redis, so several worker
  processes can serve device polls from one view of the sky.

The in-process cache was the single hard ceiling on this deployment. Not a
hardware one: the box is nowhere near saturated (a device poll measures
under 7 ms, and the process holds ~94 MB of the VPS's 1 GB), but a second
uvicorn worker would have had its own empty dict and served half the
receivers nothing. So buying more vCPU bought nothing at all. This is what
turns "one thread" back into a hardware question.

Choosing between them is one environment variable, REDIS_URL. Unset, the
deployment behaves exactly as before, and nothing here needs Redis
installed to run or to be tested.

Why the geospatial index rather than scanning: every device poll asks "what
is within N nm of here", and scanning every cached aircraft per request is
the one operation that would get worse as the fleet grows. Redis answers
that query directly with GEOSEARCH, so a poll costs one index lookup plus
one MGET however many aircraft are cached.
"""

import asyncio
import json
import logging
import math
import os
import time
from dataclasses import dataclass, field

logger = logging.getLogger("cache")

# How long an aircraft stays cached after the last record that won the
# freshness comparison for it. Matches the ESP32 firmware's own MLAT/route
# staleness window.
STALE_AFTER_SECONDS = 5 * 60
# How recently a source must have reported an aircraft for it to count as
# that source's own - used by the "my feed" map.
SOURCE_ATTRIBUTION_SECONDS = 60

NM_TO_KM = 1.852


@dataclass
class CachedAircraft:
    hex: str
    data: dict
    seen_at: float
    # Every source that has recently reported this aircraft, and when, kept
    # independently of whose record currently wins the freshness comparison
    # in merge(). Attribution used to be a single field on the winning
    # record, which meant an aircraft a customer's own receiver was tracking
    # vanished from their "my feed" map the moment an upstream API reported
    # it a fraction of a second fresher - so the aircraft nearest the
    # receiver, the ones the aggregator also polls for, were exactly the
    # ones that disappeared.
    sources: dict = field(default_factory=dict)


def distance_nm(lat1, lon1, lat2, lon2) -> float:
    r_nm = 3440.065
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlambda / 2) ** 2
    return 2 * r_nm * math.asin(math.sqrt(a))


class AircraftCache:
    """In-process cache. One worker only - see RedisAircraftCache."""

    def __init__(self):
        self._by_hex: dict[str, CachedAircraft] = {}
        self._lock = asyncio.Lock()

    async def merge(self, source: str, aircraft: list[dict]):
        now = time.time()
        async with self._lock:
            for ac in aircraft:
                hex_id = (ac.get("hex") or "").lower()
                if not hex_id:
                    continue
                existing = self._by_hex.get(hex_id)
                # Prefer the record with the most recent "seen" (seconds-ago
                # from the upstream API, smaller is fresher) rather than
                # simply "last source polled wins" - two sources can both
                # report the same aircraft at different staleness.
                # Record that this source saw it whatever happens next: who
                # reported it and whose values are freshest are two different
                # questions, and conflating them lost aircraft.
                sources = existing.sources if existing else {}
                sources[source] = now
                if existing is None or ac.get("seen", 1e9) <= existing.data.get("seen", 1e9):
                    ac = dict(ac)
                    ac["_source"] = source
                    self._by_hex[hex_id] = CachedAircraft(hex_id, ac, now, sources)
                else:
                    existing.sources = sources

    async def prune(self):
        cutoff = time.time() - STALE_AFTER_SECONDS
        async with self._lock:
            stale = [h for h, entry in self._by_hex.items() if entry.seen_at < cutoff]
            for h in stale:
                del self._by_hex[h]

    async def query(self, lat: float, lon: float, radius_nm: float) -> list[dict]:
        async with self._lock:
            snapshot = list(self._by_hex.values())
        result = []
        for entry in snapshot:
            ac_lat, ac_lon = entry.data.get("lat"), entry.data.get("lon")
            if ac_lat is None or ac_lon is None:
                continue
            if distance_nm(lat, lon, ac_lat, ac_lon) <= radius_nm:
                result.append(entry.data)
        return result

    async def query_by_source(self, source: str) -> list[dict]:
        """Used by the "my feed" account page - aircraft this source has
        reported recently, so a feeder sees what its own receiver actually
        contributed rather than the whole shared cache.

        Asks "has this source reported it lately", not "did this source win
        the last merge". The latter hid an aircraft from its own feeder
        whenever an upstream API happened to report it fractionally fresher,
        which is most likely for the traffic closest to the receiver.
        """
        cutoff = time.time() - SOURCE_ATTRIBUTION_SECONDS
        async with self._lock:
            return [
                entry.data
                for entry in self._by_hex.values()
                if entry.sources.get(source, 0) >= cutoff
            ]

    def size(self) -> int:
        return len(self._by_hex)

    async def refresh_size(self) -> int:
        return len(self._by_hex)

    async def close(self):
        pass


# One aircraft, merged atomically. Read-compare-write from several worker
# processes at once is a lost update waiting to happen: two sources
# reporting the same aircraft in the same instant could each read "no
# record" and both write, and the staler one could land last. A script runs
# on the Redis server with nothing interleaved, so the comparison and the
# write cannot be separated.
#
# KEYS: 1 record, 2 written-at zset, 3 geo index, 4 this source's zset,
#       5 freshness zset
# ARGV: 1 hex, 2 record JSON, 3 upstream "seen", 4 now, 5 ttl,
#       6 lon or "", 7 lat or ""
_MERGE_LUA = """
local previous = redis.call('ZSCORE', KEYS[5], ARGV[1])
if (not previous) or tonumber(ARGV[3]) <= tonumber(previous) then
  redis.call('SET', KEYS[1], ARGV[2], 'EX', ARGV[5])
  redis.call('ZADD', KEYS[2], ARGV[4], ARGV[1])
  redis.call('ZADD', KEYS[5], ARGV[3], ARGV[1])
  if ARGV[6] ~= '' then
    redis.call('GEOADD', KEYS[3], ARGV[6], ARGV[7], ARGV[1])
  end
end
redis.call('ZADD', KEYS[4], ARGV[4], ARGV[1])
return 1
"""


class RedisAircraftCache:
    """The same cache, shared by every worker process.

    Deliberately mirrors AircraftCache's behaviour rather than improving on
    it, so that switching REDIS_URL on cannot change what a device sees.
    The parity tests assert that by running the same scenarios through both.
    """

    def __init__(self, url: str, prefix: str = "sky", client=None):
        self.url = url
        self._prefix = prefix
        self._size = 0
        if client is not None:
            self._redis = client
        else:
            import redis.asyncio as redis_asyncio

            # decode_responses: everything stored here is JSON text or a
            # hex id, so there is no reason to hand bytes to every caller.
            self._redis = redis_asyncio.from_url(url, decode_responses=True)
        self._merge = self._redis.register_script(_MERGE_LUA)

    # --- key layout ------------------------------------------------------
    def _record(self, hex_id: str) -> str:
        return f"{self._prefix}:ac:{hex_id}"

    @property
    def _written(self) -> str:
        """hex -> when the winning record for it was written. Drives prune,
        and deliberately is NOT refreshed by an update that lost the
        freshness comparison, so staleness means the same thing it does in
        the in-process cache."""
        return f"{self._prefix}:written"

    @property
    def _fresh(self) -> str:
        """hex -> the upstream "seen" of the winning record, so the merge
        comparison needs no read-back of the record itself."""
        return f"{self._prefix}:fresh"

    @property
    def _geo(self) -> str:
        return f"{self._prefix}:geo"

    def _source_key(self, source: str) -> str:
        return f"{self._prefix}:src:{source}"

    # --- interface -------------------------------------------------------
    async def merge(self, source: str, aircraft: list[dict]):
        now = time.time()
        pipeline = self._redis.pipeline(transaction=False)
        queued = 0
        for ac in aircraft:
            hex_id = (ac.get("hex") or "").lower()
            if not hex_id:
                continue
            record = dict(ac)
            record["_source"] = source
            lat, lon = record.get("lat"), record.get("lon")
            # An aircraft with no position is still cached and still counts
            # as this source's - it just cannot answer a radius query, which
            # is what the in-process cache does too.
            has_position = lat is not None and lon is not None
            await self._merge(
                keys=[self._record(hex_id), self._written, self._geo,
                      self._source_key(source), self._fresh],
                args=[hex_id, json.dumps(record), float(record.get("seen", 1e9)), now,
                      STALE_AFTER_SECONDS,
                      lon if has_position else "", lat if has_position else ""],
                client=pipeline,
            )
            queued += 1
        if queued:
            await pipeline.execute()

    async def prune(self):
        cutoff = time.time() - STALE_AFTER_SECONDS
        stale = await self._redis.zrangebyscore(self._written, "-inf", f"({cutoff}")
        if stale:
            pipeline = self._redis.pipeline(transaction=False)
            pipeline.zrem(self._written, *stale)
            pipeline.zrem(self._fresh, *stale)
            pipeline.zrem(self._geo, *stale)
            pipeline.delete(*[self._record(h) for h in stale])
            await pipeline.execute()
        # Attribution zsets are never read beyond their own window, so
        # trimming them here stops them growing for the life of the
        # deployment - the mistake usage_log made.
        source_cutoff = time.time() - SOURCE_ATTRIBUTION_SECONDS
        async for key in self._redis.scan_iter(match=f"{self._prefix}:src:*"):
            await self._redis.zremrangebyscore(key, "-inf", f"({source_cutoff}")

    async def _records(self, hexes: list[str]) -> list[dict]:
        if not hexes:
            return []
        raw = await self._redis.mget([self._record(h) for h in hexes])
        out = []
        for blob in raw:
            # A record can expire between the index lookup and this read;
            # the index entry is tidied by the next prune.
            if not blob:
                continue
            try:
                out.append(json.loads(blob))
            except ValueError:
                logger.warning("discarding unreadable cache record")
        return out

    async def query(self, lat: float, lon: float, radius_nm: float) -> list[dict]:
        hexes = await self._redis.geosearch(
            self._geo, longitude=lon, latitude=lat,
            radius=radius_nm * NM_TO_KM, unit="km",
        )
        return await self._records(list(hexes))

    async def query_by_source(self, source: str) -> list[dict]:
        cutoff = time.time() - SOURCE_ATTRIBUTION_SECONDS
        hexes = await self._redis.zrangebyscore(self._source_key(source), cutoff, "+inf")
        return await self._records(list(hexes))

    def size(self) -> int:
        """Its one caller is the admin dashboard, which is a sync route -
        it runs in a threadpool, so it cannot await, and must not be made
        async because the rest of it is blocking DB work.

        So this returns the count as of the last poll cycle (at most
        POLL_INTERVAL_SECONDS old) rather than doing a round trip from a
        sync context. "Cache holds N aircraft" does not need to be
        to-the-second."""
        return self._size

    async def refresh_size(self) -> int:
        self._size = await self._redis.zcard(self._written)
        return self._size

    async def close(self):
        await self._redis.aclose()


def build_cache(url: str | None = None):
    """The cache this process should use.

    No REDIS_URL means the in-process cache and the single-worker behaviour
    this deployment has always had - so nothing breaks for anyone who does
    not set it, and Redis is not a new hard dependency.
    """
    url = url if url is not None else os.environ.get("REDIS_URL", "")
    if not url:
        return AircraftCache()
    cache = RedisAircraftCache(url)
    logger.info("aircraft cache: redis at %s", url)
    return cache
