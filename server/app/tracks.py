"""Where each aircraft has been, so a trail can be drawn behind it.

Every other part of this pipeline answers "where is it now". A position
report carries no history, so clicking an aircraft could say its altitude
and its route but not the one thing the map makes obvious at a glance:
the arc it flew to get here - the hold it is stuck in, the turn onto
final, the fact that the aircraft overhead has come down 8,000 ft in the
last four minutes.

Held here rather than on the receivers, for the same reason as everything
else in this service: one poll cycle feeds every device and every browser,
and an ESP32 with 320 KB of internal RAM has no business keeping a
position history for two hundred aircraft.

Sampled once per poll cycle from the merged cache, not per source. An
aircraft reported by adsb.fi, adsb.lol and OpenSky in the same cycle is
one aircraft in one place, and appending three points would treble the
memory for a trail that is three copies of the same corner.

Memory is the constraint this is written around. The obvious shape - a
list of dicts, or of tuples - costs about 170 bytes a point once Python's
object headers are counted, and at a few thousand aircraft that is tens of
megabytes on a 1 GB VPS that is already running the database, the image
caches and the feeder listeners. Each track is instead one array('f'): 16
bytes a point, flat, no per-point objects at all.
"""

import array
import math
import time

# Four floats a point: latitude, longitude, altitude in feet, and seconds
# since this track started. float32 holds a latitude to about half a metre
# at these latitudes, which is far finer than the source data.
VALUES_PER_POINT = 4
# 30 minutes at the default 15 second poll. Enough to show an approach, a
# hold, or a descent, without keeping an hour of straight-line cruise that
# says nothing a heading arrow does not.
MAX_POINTS = 120
# Below this, an aircraft has not meaningfully moved: parked, or a position
# jittering on its last decimal. About 90 m at the equator.
MIN_MOVE_DEGREES = 0.0008
# ...but a straight leg still gets a point occasionally, so a trail is not
# one long line with no timestamps in the middle.
FORCE_POINT_AFTER_SECONDS = 120.0
# A track with nothing added for this long is dropped. Longer than the
# cache's own five minutes, so an aircraft that flickers out of range and
# back keeps its trail rather than starting again.
EXPIRE_AFTER_SECONDS = 15 * 60


class Track:
    """One aircraft's recent positions, oldest first."""

    __slots__ = ("points", "started_at", "updated_at")

    def __init__(self, started_at: float):
        self.points = array.array("f")
        self.started_at = started_at
        self.updated_at = started_at

    def __len__(self) -> int:
        return len(self.points) // VALUES_PER_POINT

    def last_position(self) -> tuple[float, float] | None:
        if not self.points:
            return None
        return self.points[-VALUES_PER_POINT], self.points[-VALUES_PER_POINT + 1]

    def append(self, lat: float, lon: float, altitude: float, at: float):
        self.points.extend((lat, lon, altitude, at - self.started_at))
        self.updated_at = at
        # Oldest first out. Slicing an array copies, but only when a track is
        # actually full and only 16 bytes a point - cheaper than the
        # bookkeeping a ring buffer would need to serve points in order.
        excess = len(self.points) - MAX_POINTS * VALUES_PER_POINT
        if excess > 0:
            del self.points[:excess]

    def as_list(self, now: float) -> list[list[float]]:
        """[latitude, longitude, altitude ft, seconds ago], oldest first."""
        out = []
        elapsed = now - self.started_at
        raw = self.points
        for i in range(0, len(raw), VALUES_PER_POINT):
            out.append([
                round(raw[i], 5), round(raw[i + 1], 5),
                # Altitude is NaN for a report that had none - on the ground,
                # or a position-only message. JSON has no NaN, so it becomes
                # null and the caller decides what to draw.
                None if math.isnan(raw[i + 2]) else round(raw[i + 2]),
                round(elapsed - raw[i + 3], 1),
            ])
        return out


class TrackStore:
    """Every tracked aircraft's recent history, keyed by ICAO hex.

    Only the worker that polls upstream fills this, because it is derived
    from the poll. With one worker - the configuration this deployment runs,
    and the right one for a single vCPU - that is every worker. Running
    several behind Redis would give each its own view, and the honest fix
    then is to move the points into Redis rather than to pretend otherwise;
    /admin/system says which mode is in force.
    """

    def __init__(self, max_aircraft: int = 4000):
        self._tracks: dict[str, Track] = {}
        self._max_aircraft = max_aircraft
        self.samples = 0
        self.dropped = 0

    def observe(self, aircraft: list[dict], now: float | None = None):
        """One poll cycle's worth of positions."""
        now = time.time() if now is None else now
        for entry in aircraft:
            hex_id = (entry.get("hex") or "").strip().lower()
            lat, lon = entry.get("lat"), entry.get("lon")
            if not hex_id or lat is None or lon is None:
                continue
            track = self._tracks.get(hex_id)
            if track is None:
                if len(self._tracks) >= self._max_aircraft:
                    # A busy sky is not a reason to grow without bound. The
                    # aircraft still appears on the map; it just has no
                    # trail until room frees up.
                    self.dropped += 1
                    continue
                track = self._tracks[hex_id] = Track(now)
            elif not self._worth_recording(track, lat, lon, now):
                track.updated_at = now  # still here, just not moving
                continue
            altitude = _altitude_feet(entry.get("alt_baro"))
            track.append(lat, lon, altitude, now)
            self.samples += 1

    @staticmethod
    def _worth_recording(track: Track, lat: float, lon: float, now: float) -> bool:
        previous = track.last_position()
        if previous is None:
            return True
        if now - track.updated_at >= FORCE_POINT_AFTER_SECONDS:
            return True
        return (abs(lat - previous[0]) >= MIN_MOVE_DEGREES or
                abs(lon - previous[1]) >= MIN_MOVE_DEGREES)

    def get(self, hex_id: str, now: float | None = None) -> list[list[float]]:
        track = self._tracks.get((hex_id or "").strip().lower())
        if track is None:
            return []
        return track.as_list(time.time() if now is None else now)

    def prune(self, now: float | None = None) -> int:
        now = time.time() if now is None else now
        cutoff = now - EXPIRE_AFTER_SECONDS
        stale = [h for h, t in self._tracks.items() if t.updated_at < cutoff]
        for hex_id in stale:
            del self._tracks[hex_id]
        return len(stale)

    def stats(self) -> dict:
        points = sum(len(t) for t in self._tracks.values())
        return {
            "aircraft": len(self._tracks),
            "points": points,
            # What it is actually costing, which is the number worth
            # watching on a small VPS.
            "bytes": points * VALUES_PER_POINT * 4,
            "samples": self.samples,
            "dropped": self.dropped,
        }


def _altitude_feet(value) -> float:
    """Altitude as a number, or NaN when there is not one.

    "ground" is a string every source here uses, and a position-only report
    carries no altitude at all. Both have to survive into a fixed-width
    array, and NaN is the only value float32 has that means "no reading".
    """
    if value is None:
        return math.nan
    if isinstance(value, str):
        return 0.0 if value.strip().lower() == "ground" else math.nan
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan
