"""Balloons, which no aircraft feed shows properly.

A radiosonde does not carry a transponder, so the three ADS-B sources this
service polls cannot see the several thousand weather balloons launched
worldwide every day, nor the amateur high-altitude flights that spend a
whole afternoon crossing a continent at 100,000 ft. Both are tracked, just
somewhere else: receivers on the ground decode their telemetry and upload
it to SondeHub.

Three kinds, gathered here so a receiver gets all of them in one request:

* Radiosondes - weather balloons, launched to a schedule (mostly 00Z and
  12Z) from fixed sites, up to about 100,000 ft and then down on a
  parachute. The bulk of what this will show.
* Amateur - high-altitude and pico balloons flown by radio amateurs,
  reporting over APRS and LoRa. SondeHub bridges APRS-IS itself, which is
  why this needs no APRS account of its own.
* Lighter-than-air aircraft that DO carry a transponder - airships, some
  tethered and advertising balloons. These are already in the aircraft
  cache, marked with ADS-B emitter category B2, and cost nothing extra to
  pick out.

No API key, for any of it. SondeHub's GET endpoints are open, and the one
obvious keyed alternative - aprs.fi - cannot do the job: its API is
callsign-only by design, with no geographic search, so it could never
answer "what is near this receiver". It is also explicit that its data may
not be redistributed to a service offering the same features.

Polled far more slowly than aircraft. SondeHub asks that its telemetry
endpoints not be polled regularly, and a balloon at 5 m/s has not gone
anywhere in fifteen seconds; two minutes is the default and it is a
setting.
"""

import asyncio
import logging
import time

import httpx

from .units import METRES_TO_FEET, MPS_TO_FEET_PER_MINUTE, MPS_TO_KNOTS

logger = logging.getLogger("balloons")

SONDEHUB_SONDES_URL = "https://api.v2.sondehub.org/sondes"
SONDEHUB_AMATEUR_URL = "https://api.v2.sondehub.org/amateur"
REQUEST_TIMEOUT_SECONDS = 20.0
# How far back a report may be and still count as "now". A radiosonde
# uploads every few seconds while it is being received, but coverage is
# patchy and a balloon between receivers should not vanish off the map.
SONDEHUB_WINDOW_SECONDS = 30 * 60
# Dropped from the store this long after its last report.
STALE_AFTER_SECONDS = 45 * 60
# SondeHub takes metres and this deployment thinks in nautical miles.
NM_TO_METRES = 1852.0
# Its geographic filter is generous with a big radius and the payloads are
# small, but a runaway radius would be rude to a free service.
MAX_RADIUS_NM = 1000.0
# The ADS-B emitter category for "any lighter than air (airship or
# balloon) regardless of weight" - DO-260B 2.2.3.2.5.2.
LIGHTER_THAN_AIR_CATEGORY = "B2"


class Balloon:
    __slots__ = ("id", "kind", "lat", "lon", "altitude_ft", "climb_fpm", "ground_kt",
                 "heading", "label", "detail", "source", "reported_at")

    def __init__(self, **fields):
        for slot in self.__slots__:
            setattr(self, slot, fields.get(slot))

    def as_dict(self, now: float) -> dict:
        out = {
            "id": self.id,
            "kind": self.kind,
            "lat": round(self.lat, 5),
            "lon": round(self.lon, 5),
            "seen": round(max(0.0, now - self.reported_at), 1),
            "source": self.source,
        }
        # Only what this balloon actually reported. A radiosonde carries a
        # thermometer and an amateur payload usually does not, and sending
        # nulls for the difference would cost the receiver parsing time for
        # fields it cannot show anyway.
        for key, value in (("alt", self.altitude_ft), ("climb", self.climb_fpm),
                           ("gs", self.ground_kt), ("track", self.heading),
                           ("name", self.label), ("info", self.detail)):
            if value is not None:
                out[key] = value
        return out


class BalloonTracker:
    """Polls SondeHub and keeps what is currently flying."""

    def __init__(self, settings=None, cache=None):
        self._settings = settings
        # The aircraft cache, so lighter-than-air traffic that IS on ADS-B
        # can be folded in without another request to anyone.
        self._cache = cache
        self._balloons: dict[str, Balloon] = {}
        self._client: httpx.AsyncClient | None = None
        self._task: asyncio.Task | None = None
        self._regions = None
        self.polls = 0
        self.errors = 0
        self.last_error: str | None = None
        self.last_poll_at: float | None = None

    # --- configuration, live from the admin panel ------------------------
    def enabled(self) -> bool:
        return bool(self._settings and self._settings.get("balloon_tracking"))

    def _radius_nm(self) -> float:
        value = self._settings.get("balloon_radius_nm") if self._settings else 250
        return min(float(value), MAX_RADIUS_NM)

    def _interval_seconds(self) -> float:
        value = self._settings.get("balloon_poll_seconds") if self._settings else 120
        return float(value)

    def _source_on(self, name: str) -> bool:
        return bool(self._settings and self._settings.get(name))

    # --- lifecycle -------------------------------------------------------
    def start(self, regions_provider=None):
        """regions_provider returns the areas to ask about, so balloons
        follow the same device locations the aircraft polling does rather
        than a second copy of that logic."""
        self._regions = regions_provider
        if self._client is None:
            self._client = httpx.AsyncClient(timeout=REQUEST_TIMEOUT_SECONDS)
        self._task = asyncio.create_task(self._loop())

    async def stop(self):
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        if self._client:
            await self._client.aclose()
            self._client = None

    async def _loop(self):
        while True:
            try:
                if self.enabled():
                    await self.poll_once()
            except asyncio.CancelledError:
                raise
            except Exception:  # noqa: BLE001
                # One bad cycle must not end the only thing filling this.
                self.errors += 1
                logger.exception("balloon poll failed")
            # Re-read every cycle so a changed interval takes effect without
            # a restart, and so switching the feature off stops the polling
            # rather than merely hiding the results.
            await asyncio.sleep(max(30.0, self._interval_seconds()))

    async def poll_once(self):
        if not self._regions:
            return
        # In a thread: the aggregator's region list is built from blocking
        # SQLAlchemy queries, and running those on the event loop would
        # stall the feeder listeners and every in-flight request that share
        # it - the same rule the aggregator follows for the same call.
        regions = await asyncio.to_thread(self._regions)
        if not regions:
            return
        found: list[Balloon] = []
        failures_before = self.errors
        for lat, lon, _radius in regions:
            if self._source_on("balloon_sondes"):
                found += await self._fetch(SONDEHUB_SONDES_URL, lat, lon, "sonde")
            if self._source_on("balloon_amateur"):
                found += await self._fetch(SONDEHUB_AMATEUR_URL, lat, lon, "amateur")
        if self._source_on("balloon_adsb") and self._cache is not None:
            found += await self._lighter_than_air()
        for balloon in found:
            self._balloons[balloon.id] = balloon
        self.polls += 1
        self.last_poll_at = time.time()
        # Cleared only by a cycle that actually worked. Clearing it
        # unconditionally here wiped the error the fetch had just recorded,
        # so a service that was failing every poll reported no error at all
        # - the admin page would have shown it as healthy.
        if self.errors == failures_before:
            self.last_error = None
        self.prune()

    async def _fetch(self, url: str, lat: float, lon: float, kind: str) -> list[Balloon]:
        try:
            response = await self._client.get(url, params={
                "lat": lat, "lon": lon,
                "distance": int(self._radius_nm() * NM_TO_METRES),
                "last": SONDEHUB_WINDOW_SECONDS,
            })
            response.raise_for_status()
            payload = response.json()
        except (httpx.HTTPError, ValueError) as exc:
            self.errors += 1
            self.last_error = f"{type(exc).__name__}: {exc}"
            logger.warning("SondeHub %s query failed: %s", kind, self.last_error)
            return []
        return parse_sondehub(payload, kind)

    async def _lighter_than_air(self) -> list[Balloon]:
        now = time.time()
        out = []
        for entry in await self._cache.snapshot():
            if str(entry.get("category") or "").upper() != LIGHTER_THAN_AIR_CATEGORY:
                continue
            lat, lon = entry.get("lat"), entry.get("lon")
            if lat is None or lon is None:
                continue
            altitude = entry.get("alt_baro")
            out.append(Balloon(
                id=f"adsb:{entry.get('hex')}", kind="airship",
                lat=lat, lon=lon,
                altitude_ft=None if isinstance(altitude, str) else altitude,
                climb_fpm=entry.get("baro_rate"), ground_kt=entry.get("gs"),
                heading=entry.get("track"),
                label=(entry.get("flight") or "").strip() or entry.get("hex"),
                detail=None, source="adsb",
                reported_at=now - float(entry.get("seen") or 0),
            ))
        return out

    # --- the request path ------------------------------------------------
    def query(self, lat: float, lon: float, radius_nm: float) -> list[dict]:
        from .cache import distance_nm

        now = time.time()
        out = []
        for balloon in self._balloons.values():
            if distance_nm(lat, lon, balloon.lat, balloon.lon) <= radius_nm:
                out.append(balloon.as_dict(now))
        out.sort(key=lambda b: b["seen"])
        return out

    def prune(self, now: float | None = None) -> int:
        now = time.time() if now is None else now
        cutoff = now - STALE_AFTER_SECONDS
        stale = [k for k, b in self._balloons.items() if b.reported_at < cutoff]
        for key in stale:
            del self._balloons[key]
        return len(stale)

    def stats(self) -> dict:
        kinds: dict[str, int] = {}
        for balloon in self._balloons.values():
            kinds[balloon.kind] = kinds.get(balloon.kind, 0) + 1
        return {
            "enabled": self.enabled(),
            "tracked": len(self._balloons),
            "kinds": kinds,
            "polls": self.polls,
            "errors": self.errors,
            "last_error": self.last_error,
            "last_poll_at": self.last_poll_at,
            "interval_seconds": self._interval_seconds(),
            "radius_nm": self._radius_nm(),
        }


def parse_sondehub(payload, kind: str) -> list[Balloon]:
    """SondeHub's reply into the shape a receiver is given.

    Both endpoints answer with an object keyed by serial or payload
    callsign, each value the latest telemetry for that balloon. Altitude is
    in METRES and the velocities in metres per second - the same trap as
    the OpenSky state vectors, and a balloon shown at 30,000 ft when it is
    at 30,000 m would look entirely plausible.
    """
    if not isinstance(payload, dict):
        return []
    out = []
    for key, entry in payload.items():
        if not isinstance(entry, dict):
            continue
        lat, lon = entry.get("lat"), entry.get("lon")
        if lat is None or lon is None:
            continue
        reported_at = _timestamp(entry.get("datetime") or entry.get("time_received"))
        altitude = entry.get("alt")
        climb = entry.get("vel_v")
        speed = entry.get("vel_h")
        out.append(Balloon(
            id=f"{kind}:{key}",
            kind=kind,
            lat=float(lat), lon=float(lon),
            altitude_ft=None if altitude is None else round(float(altitude) * METRES_TO_FEET),
            climb_fpm=None if climb is None else round(float(climb) * MPS_TO_FEET_PER_MINUTE),
            ground_kt=None if speed is None else round(float(speed) * MPS_TO_KNOTS, 1),
            heading=entry.get("heading"),
            label=str(entry.get("payload_callsign") or entry.get("serial") or key),
            # What kind of thing it is, where there is an answer: the sonde
            # model for a radiosonde, the modulation for an amateur flight.
            detail=(str(entry.get("type")) if entry.get("type")
                    else (str(entry.get("modulation")) if entry.get("modulation") else None)),
            source="sondehub",
            reported_at=reported_at,
        ))
    return out


def _timestamp(value) -> float:
    """SondeHub's ISO-8601 into epoch seconds, or now if it is unreadable.

    Now rather than zero on purpose: a balloon whose timestamp cannot be
    parsed is still a balloon that was just reported, and treating it as
    ancient would prune it immediately and hide a real flight.
    """
    if not value:
        return time.time()
    import datetime

    text = str(value).replace("Z", "+00:00")
    try:
        return datetime.datetime.fromisoformat(text).timestamp()
    except ValueError:
        return time.time()
