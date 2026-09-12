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
import os
import time
from dataclasses import dataclass

import httpx
from sqlalchemy.orm import Session

from . import models
from .cache import (  # re-exported: all of these lived here before the cache moved out
    SOURCE_ATTRIBUTION_SECONDS,
    STALE_AFTER_SECONDS,
    AircraftCache,
    CachedAircraft,
    build_cache,
)
from .cache import distance_nm as _distance_nm
from .models import FeederKey, FeederProvider
from .runtime_settings import SettingsStore

logger = logging.getLogger("aggregator")

USER_AGENT = "2E0LXY-ADSB-Aggregator/1.0 (+https://github.com/2E0LXY/ESP32-ADS-B)"
POLL_INTERVAL_SECONDS = int(os.environ.get("POLL_INTERVAL_SECONDS", "15"))
# How long a source is still credited with an aircraft after last reporting
# it. Long enough to ride out a gap between messages from a single receiver,
# short enough that a feeder that goes offline stops claiming the sky.
# A source that keeps failing is retried on a widening interval rather than
# every cycle - an upstream that has started refusing us should not cost a
# request and a log line every fifteen seconds indefinitely.
SOURCE_BACKOFF_MAX_SECONDS = 15 * 60
# Per-device polling areas. A device asking for a 5 nm radius still needs the
# cache filled a bit wider than that, or an aircraft is only cached once it
# is already overhead; and nobody gets to make the aggregator poll the whole
# hemisphere.
MIN_POLL_RADIUS_NM = 25.0
# The starting values for the editable settings of the same name. Once a
# deployment is running, the admin panel decides these - see
# app/runtime_settings.py.
MAX_POLL_RADIUS_NM = 250.0
# Each region is one request per upstream per cycle. Beyond this many the
# regions are rotated across cycles instead.
MAX_POLL_REGIONS = int(os.environ.get("MAX_POLL_REGIONS", "6"))
# Sources to leave alone entirely, comma separated, by the names used below.
#
# airplanes.live is off by default because it now answers 403 to every
# request from us, and not because of this deployment's address - the same
# request is refused from unrelated networks too. A source that cannot
# succeed should not sit red in the admin panel forever implying an outage
# somebody could fix, nor keep spending requests at someone else's server
# to re-learn the same answer. Clear this variable to try it again if their
# access rules change.
# Only the default for whether each source is polled; after first boot the
# admin panel's per-source switches decide, and this is what a fresh
# database starts from.
DISABLED_SOURCES = {
    name.strip()
    for name in os.environ.get("DISABLED_SOURCES", "airplaneslive").split(",")
    if name.strip()
}


@dataclass
class SourceHealth:
    name: str
    ok: bool = True
    last_success: float | None = None
    last_error: str | None = None
    consecutive_errors: int = 0
    last_attempt: float | None = None


class Aggregator:
    def __init__(self, home_lat: float, home_lon: float, home_radius_nm: float, session_factory,
                 cache=None, leadership=None, settings=None):
        self.cache = cache if cache is not None else build_cache()
        # Only the leader polls upstream. Without this, running N workers
        # would ask each community API for the same sky N times every
        # cycle. None means "always the leader", which is what a
        # single-process deployment is - see app/leader.py.
        self._leadership = leadership
        # Editable from the admin panel rather than only from .env plus a
        # restart - see app/runtime_settings.py. A store with no session
        # factory serves the environment defaults and never reads a
        # database, which is what a caller passing nothing gets.
        self.settings = settings if settings is not None else SettingsStore()
        self.home_lat = home_lat
        self.home_lon = home_lon
        self.home_radius_nm = home_radius_nm
        self._session_factory = session_factory
        # Every source, whether or not it is currently polled: a source
        # turned back on in the admin panel needs somewhere to record its
        # first attempt, and the dashboard should show a disabled source as
        # disabled rather than omitting it and implying it does not exist.
        self._health: dict[str, SourceHealth] = {
            name: SourceHealth(name) for name in ("adsbfi", "airplaneslive", "adsblol")
        }
        self._feeder_cycle: dict[FeederProvider, itertools.cycle] = {}
        self._region_cursor = 0
        self._task: asyncio.Task | None = None

    def poll_regions(self) -> list[tuple[float, float, float]]:
        """The areas to poll upstream this cycle.

        One global HOME_LAT/HOME_LON meant the cache only ever contained
        aircraft near the operator, so a customer anywhere else queried it
        correctly and got nothing. Poll where the devices actually are
        instead, and keep the configured home as the fallback for a
        deployment with no located devices yet.

        Nearby devices are merged rather than polled separately: a street of
        receivers is one area to an upstream API, and asking three times for
        the same sky is rude to a free service and no more useful.
        """
        max_radius = self.settings.get("max_poll_radius_nm")
        max_regions = self.settings.get("max_poll_regions")
        db = self._session_factory()
        try:
            devices = db.query(models.Device).all()
            located = [loc for loc in (d.location() for d in devices) if loc]
        finally:
            db.close()
        if not located:
            return [(self.home_lat, self.home_lon, self.home_radius_nm)]

        merged: list[tuple[float, float, float]] = []
        for lat, lon, radius in located:
            radius = max(MIN_POLL_RADIUS_NM, min(radius, max_radius))
            for index, (mlat, mlon, mradius) in enumerate(merged):
                if _distance_nm(lat, lon, mlat, mlon) <= max(radius, mradius):
                    # Cover both from one point, widened enough to still
                    # reach the far edge of each.
                    separation = _distance_nm(lat, lon, mlat, mlon)
                    merged[index] = (
                        (lat + mlat) / 2,
                        (lon + mlon) / 2,
                        min(max_radius, max(radius, mradius) + separation / 2),
                    )
                    break
            else:
                merged.append((lat, lon, radius))

        if len(merged) > max_regions:
            # Every region costs a request to each upstream on every cycle.
            # Past this many, rotate through them across cycles rather than
            # multiplying the load on free APIs without limit; a device's
            # area is then refreshed less often, not dropped.
            start = self._region_cursor % len(merged)
            self._region_cursor += max_regions
            rotated = merged[start:] + merged[:start]
            return rotated[:max_regions]
        return merged

    def health(self) -> dict[str, SourceHealth]:
        return self._health

    def polls_upstream(self) -> bool:
        """Whether this process is the one polling. A follower's source
        health is its own last attempt, which may be from before it lost
        the role, so the admin panel needs to say which it is looking at."""
        return self._leadership is None or self._leadership.is_leader

    def start(self):
        self._task = asyncio.create_task(self._loop())

    async def stop(self):
        if self._task:
            self._task.cancel()

    async def _loop(self):
        async with httpx.AsyncClient(timeout=10.0, headers={"User-Agent": USER_AGENT}) as client:
            while True:
                try:
                    if self._leadership is None or self._leadership.is_leader:
                        await self._poll_all(client)
                        await self.cache.prune()
                    # Every worker keeps its own copy of the count the admin
                    # dashboard reads, leader or not; it is one cheap read
                    # and a follower's dashboard should not show zero.
                    await self.cache.refresh_size()
                    # Pick up admin-panel changes. The worker that made the
                    # change applied it immediately; this is how the others
                    # find out, within one cycle. In a thread because every
                    # statement in reload() is blocking SQLAlchemy.
                    await asyncio.to_thread(self.settings.reload)
                except asyncio.CancelledError:
                    raise
                except Exception:  # noqa: BLE001
                    # This loop is the only thing filling the cache. An
                    # unhandled error here used to end it for the lifetime of
                    # the process, and the only symptom was a service that
                    # quietly returned fewer and fewer aircraft.
                    logger.exception("poll cycle failed")
                await asyncio.sleep(self.settings.get("poll_interval_seconds"))

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
        # Worked out once per cycle and handed to all three pollers, rather
        # than each of them asking the database for itself. It is the same
        # answer three times over, and it was three synchronous queries on
        # the event loop every fifteen seconds - the loop the live feeder
        # listeners also run on.
        regions = await self._current_regions()
        await asyncio.gather(
            self._poll_adsbfi(client, regions),
            self._poll_airplaneslive(client, regions),
            self._poll_adsblol(client, regions),
            return_exceptions=True,
        )

    async def _current_regions(self) -> list[tuple[float, float, float]]:
        try:
            return await asyncio.to_thread(self.poll_regions)
        except Exception as exc:  # noqa: BLE001
            # Reading device locations is a convenience; not being able to
            # is no reason to stop polling entirely.
            logger.warning("could not read device locations (%s) - polling the home area only", exc)
            return [(self.home_lat, self.home_lon, self.home_radius_nm)]

    def source_enabled(self, name: str) -> bool:
        return self.settings.source_enabled(name)

    def _source_is_backed_off(self, name: str) -> bool:
        """True while a repeatedly failing source is being left alone.

        airplanes.live began answering 403 to every request, and without
        this the poll loop asked it again every fifteen seconds forever and
        wrote a warning each time - a dead source producing thousands of log
        lines a day and a steady trickle of pointless requests at someone
        else's server. Back off instead, and keep retrying occasionally so
        the source recovers on its own when whatever changed changes back.
        """
        health = self._health[name]
        if health.consecutive_errors == 0:
            return False
        delay = min(
            SOURCE_BACKOFF_MAX_SECONDS,
            self.settings.get("poll_interval_seconds") * (2 ** min(health.consecutive_errors, 12)),
        )
        return time.time() - (health.last_attempt or 0) < delay

    async def _record(self, name: str, coro):
        if not self.source_enabled(name):
            coro.close()  # never awaited, so close it rather than leak a warning
            return
        health = self._health[name]
        if self._source_is_backed_off(name):
            coro.close()  # never awaited, so close it rather than leak a warning
            return
        health.last_attempt = time.time()
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
            # Only the first few failures are worth a line each; after that
            # the backoff above is doing the talking and repeating the same
            # warning every cycle just buries everything else.
            if health.consecutive_errors <= 3:
                logger.warning("poll failed for %s: %s", name, exc)
            elif health.consecutive_errors % 20 == 0:
                logger.warning(
                    "poll still failing for %s after %d attempts: %s",
                    name, health.consecutive_errors, exc,
                )

    async def _poll_adsbfi(self, client: httpx.AsyncClient, regions):
        if not self.source_enabled("adsbfi"):
            return
        for lat, lon, radius in regions:
            url = (
                f"https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}"
                f"/dist/{radius:.0f}"
            )
            await self._record("adsbfi", self._fetch(client, url, "ac"))

    async def _poll_airplaneslive(self, client: httpx.AsyncClient, regions):
        if not self.source_enabled("airplaneslive"):
            return
        # No feeder-key pooling here yet: airplanes.live's own API doesn't
        # take a bearer/query-param key today (access is IP/account based on
        # their end) - the hook is here so it's a one-line change once/if
        # they document one.
        for lat, lon, radius in regions:
            url = f"https://api.airplanes.live/v2/point/{lat}/{lon}/{radius:.0f}"
            await self._record("airplaneslive", self._fetch(client, url, "ac"))

    async def _poll_adsblol(self, client: httpx.AsyncClient, regions):
        if not self.source_enabled("adsblol"):
            return
        for lat, lon, radius in regions:
            url = f"https://api.adsb.lol/v2/point/{lat}/{lon}/{radius:.0f}"
            await self._record("adsblol", self._fetch(client, url, "ac"))

    async def _fetch(self, client: httpx.AsyncClient, url: str, list_key: str) -> list[dict]:
        response = await client.get(url)
        response.raise_for_status()
        payload = response.json()
        return payload.get(list_key) or []
