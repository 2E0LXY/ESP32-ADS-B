"""Central upstream polling, caching, and dedup.

One background loop polls the free public ADS-B APIs on behalf of every
device this backend serves, instead of each device (or each customer's
personal key) hitting those APIs directly. That is the entire reason this
service exists - see the ESP32 firmware's own comment in fetchAdsbV2Aircraft()
for the provider-terms research this was built to satisfy.

Feeder-key pooling: any account can donate a personal upstream credential
(FeederKey rows) it already holds - e.g. an API key earned by running its
own physical receiver and feeding one of these networks. Each poll cycle
round-robins across the pool plus this deployment's own default access, so
no single credential (including the operator's own) is asked to carry more
than its owner's personal allowance, while the aggregator's total capacity
scales with how many feeders opt in.
"""

import asyncio
import itertools
import logging
import math
import os
import time
from dataclasses import dataclass, field

import httpx
from sqlalchemy.orm import Session

from .models import FeederKey, FeederProvider

logger = logging.getLogger("aggregator")

USER_AGENT = "2E0LXY-ADSB-Aggregator/1.0 (+https://github.com/2E0LXY/ESP32-ADS-B)"
POLL_INTERVAL_SECONDS = int(os.environ.get("POLL_INTERVAL_SECONDS", "15"))
STALE_AFTER_SECONDS = 5 * 60  # matches the ESP32 firmware's own MLAT/route cache staleness window


@dataclass
class CachedAircraft:
    hex: str
    data: dict
    seen_at: float


class AircraftCache:
    """In-memory, single-process cache - fine for one aggregator instance.
    If this is ever scaled to multiple processes/instances, this needs to
    move to something shared (Redis) instead; flagged here rather than
    silently becoming a bug on the day someone adds a second worker."""

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
                if existing is None or ac.get("seen", 1e9) <= existing.data.get("seen", 1e9):
                    ac = dict(ac)
                    ac["_source"] = source
                    self._by_hex[hex_id] = CachedAircraft(hex_id, ac, now)

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
            if _distance_nm(lat, lon, ac_lat, ac_lon) <= radius_nm:
                result.append(entry.data)
        return result

    async def query_by_source(self, source: str) -> list[dict]:
        """Used by the "my feed" account page - only aircraft whose most
        recently merged record came from this exact source tag, so a
        feeder only ever sees what its own receiver actually contributed,
        not the whole shared cache."""
        async with self._lock:
            return [entry.data for entry in self._by_hex.values() if entry.data.get("_source") == source]

    def size(self) -> int:
        return len(self._by_hex)


def _distance_nm(lat1, lon1, lat2, lon2) -> float:
    r_nm = 3440.065
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlambda / 2) ** 2
    return 2 * r_nm * math.asin(math.sqrt(a))


@dataclass
class SourceHealth:
    name: str
    ok: bool = True
    last_success: float | None = None
    last_error: str | None = None
    consecutive_errors: int = 0


class Aggregator:
    def __init__(self, home_lat: float, home_lon: float, home_radius_nm: float, session_factory):
        self.cache = AircraftCache()
        self.home_lat = home_lat
        self.home_lon = home_lon
        self.home_radius_nm = home_radius_nm
        self._session_factory = session_factory
        self._health: dict[str, SourceHealth] = {
            name: SourceHealth(name) for name in ("adsbfi", "airplaneslive", "adsblol")
        }
        self._feeder_cycle: dict[FeederProvider, itertools.cycle] = {}
        self._task: asyncio.Task | None = None

    def health(self) -> dict[str, SourceHealth]:
        return self._health

    def start(self):
        self._task = asyncio.create_task(self._loop())

    async def stop(self):
        if self._task:
            self._task.cancel()

    async def _loop(self):
        async with httpx.AsyncClient(timeout=10.0, headers={"User-Agent": USER_AGENT}) as client:
            while True:
                await self._poll_all(client)
                await self.cache.prune()
                await asyncio.sleep(POLL_INTERVAL_SECONDS)

    def _next_feeder_credential(self, provider: FeederProvider) -> str | None:
        """Round-robins across enabled donated keys for this provider.
        Rebuilt each call from the DB so newly added/removed feeder keys
        take effect on the next poll without a restart."""
        db: Session = self._session_factory()
        try:
            rows = (
                db.query(FeederKey)
                .filter(FeederKey.provider == provider, FeederKey.enabled.is_(True))
                .all()
            )
        finally:
            db.close()
        if not rows:
            return None
        if provider not in self._feeder_cycle or getattr(self._feeder_cycle[provider], "_n", 0) != len(rows):
            cyc = itertools.cycle(rows)
            cyc._n = len(rows)  # type: ignore[attr-defined]
            self._feeder_cycle[provider] = cyc
        return next(self._feeder_cycle[provider]).credential

    async def _poll_all(self, client: httpx.AsyncClient):
        await asyncio.gather(
            self._poll_adsbfi(client),
            self._poll_airplaneslive(client),
            self._poll_adsblol(client),
            return_exceptions=True,
        )

    async def _record(self, name: str, coro):
        health = self._health[name]
        try:
            aircraft = await coro
            await self.cache.merge(name, aircraft)
            health.ok = True
            health.last_success = time.time()
            health.consecutive_errors = 0
        except Exception as exc:  # noqa: BLE001 - a single bad source must not take down the poll loop
            health.ok = False
            health.last_error = str(exc)
            health.consecutive_errors += 1
            logger.warning("poll failed for %s: %s", name, exc)

    async def _poll_adsbfi(self, client: httpx.AsyncClient):
        url = (
            f"https://opendata.adsb.fi/api/v3/lat/{self.home_lat}/lon/{self.home_lon}"
            f"/dist/{self.home_radius_nm}"
        )
        await self._record("adsbfi", self._fetch(client, url, "ac"))

    async def _poll_airplaneslive(self, client: httpx.AsyncClient):
        # No feeder-key pooling here yet: airplanes.live's own API doesn't
        # take a bearer/query-param key today (access is IP/account based on
        # their end) - the hook is here so it's a one-line change once/if
        # they document one.
        url = f"https://api.airplanes.live/v2/point/{self.home_lat}/{self.home_lon}/{self.home_radius_nm}"
        await self._record("airplaneslive", self._fetch(client, url, "ac"))

    async def _poll_adsblol(self, client: httpx.AsyncClient):
        url = f"https://api.adsb.lol/v2/point/{self.home_lat}/{self.home_lon}/{self.home_radius_nm}"
        await self._record("adsblol", self._fetch(client, url, "ac"))

    async def _fetch(self, client: httpx.AsyncClient, url: str, list_key: str) -> list[dict]:
        response = await client.get(url)
        response.raise_for_status()
        payload = response.json()
        return payload.get(list_key) or []
