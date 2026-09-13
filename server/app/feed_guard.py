"""Rejects aircraft a feeder could not really have heard.

Feeder ingestion has no authentication beyond knowing which TCP port was
assigned to you. That is not laziness: standard feeder software (readsb,
dump1090, PiAware) opens an outbound connection to a fixed host:port and
has no way to send a credential first, which is why every feeder network
works this way. But it does mean anyone who learns or scans a port can
push whatever they like into the cache that every device reads.

Nothing checked what arrived. An aircraft claiming to be at 0,0 at
900,000 feet, or one teleporting across the Atlantic between messages, was
merged and served to every customer's panel exactly like a real sighting.

What makes this checkable without a credential is physics. A receiver is
an antenna on a roof: it hears aircraft within line of sight and nothing
else. So for a feeder whose position we independently know, an aircraft
hundreds of miles beyond its horizon did not come from that antenna,
whatever the stream says.

"Independently" is the important word. A device's position can come from
three places (see Device.location()): what the receiver itself reports on
its /v1/aircraft polls, what the owner typed into the dashboard, or what
app/site_estimate.py infers from the aircraft this very feed reported. The
third cannot be used here - a garbage feed would move the estimate until
the garbage looked plausible, and the check would certify whatever it was
given. Only the first two count as a trusted centre.

With no trusted centre, the coordinate, altitude and teleport checks still
apply; the range check is skipped rather than guessed. That residual gap
is why the admin panel shows what each feeder is having rejected, and why
the owner setting a position for their device is worth encouraging.
"""

import logging
import math
import time

logger = logging.getLogger("feed_guard")

# Line of sight from an antenna at sea level to an aircraft at 40,000 ft is
# about 250 nm; a hilltop site with a good antenna can beat that, so the
# default is generous rather than tight. Editable in the admin panel.
DEFAULT_MAX_RANGE_NM = 300.0
# Below sea level happens (Dead Sea, pressure errors); -1,500 ft does not.
MIN_ALTITUDE_FT = -1500
# Well above any civil aircraft, and above the U-2 and Concorde too.
MAX_ALTITUDE_FT = 60000
# Faster than any aircraft that reports ADS-B. The SR-71 managed ~1,900 kt
# and is not flying; a position implying more is two aircraft confused for
# one, or made up.
MAX_GROUND_SPEED_KT = 2000.0
# Below this the time between two positions is too short to infer a speed
# from - a one-second gap and a rounding error would look supersonic.
MIN_TELEPORT_INTERVAL_SECONDS = 5.0
# How long a remembered position stays useful for the teleport check. The
# decoder prunes its own state at 300s; matching that means an aircraft
# that comes back after a gap is treated as new rather than as having
# crossed the intervening distance instantly.
POSITION_MEMORY_SECONDS = 300.0

REASONS = ("bad_position", "bad_altitude", "out_of_range", "teleported")


def distance_nm(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r_nm = 3440.065
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlambda / 2) ** 2
    return 2 * r_nm * math.asin(math.sqrt(a))


class FeedGuard:
    """Per-feeder plausibility checks, with counts for the admin panel.

    One instance per device, living as long as the listener does, because
    the teleport check needs to remember where each aircraft was last
    seen - and a feeder reconnects whenever its link hiccups, so that
    memory must outlive a single connection.
    """

    def __init__(self, device_id: int, max_range_nm: float = DEFAULT_MAX_RANGE_NM):
        self.device_id = device_id
        self.max_range_nm = max_range_nm
        # Only what the receiver reports or the owner typed - never the
        # position inferred from this feed. See the module docstring.
        self.trusted_centre: tuple[float, float] | None = None
        self.accepted = 0
        self.rejected = 0
        self.reasons = {reason: 0 for reason in REASONS}
        self._last_position: dict[str, tuple[float, float, float]] = {}
        self._logged = 0

    def set_trusted_centre(self, centre: tuple[float, float] | None):
        self.trusted_centre = centre

    def filter(self, aircraft: list[dict], now: float | None = None) -> list[dict]:
        """The aircraft worth merging. Rejections are counted, not raised."""
        now = time.time() if now is None else now
        self._forget_old(now)
        keepers = []
        for entry in aircraft:
            reason = self._reject_reason(entry, now)
            if reason is None:
                keepers.append(entry)
                self.accepted += 1
                self._remember(entry, now)
            else:
                self.rejected += 1
                self.reasons[reason] += 1
                self._maybe_log(entry, reason)
        return keepers

    # --- the checks ------------------------------------------------------

    def _reject_reason(self, entry: dict, now: float) -> str | None:
        lat, lon = entry.get("lat"), entry.get("lon")
        if lat is None or lon is None:
            # Positionless aircraft are normal - a first message, or an
            # aircraft the receiver only has an identity for. Nothing to
            # check, and the cache handles them.
            return None
        if not _is_plausible_coordinate(lat, lon):
            return "bad_position"
        altitude = entry.get("alt_baro")
        if isinstance(altitude, (int, float)) and not MIN_ALTITUDE_FT <= altitude <= MAX_ALTITUDE_FT:
            # "ground" is a string and legitimate, hence the isinstance.
            return "bad_altitude"
        if self.trusted_centre is not None:
            away = distance_nm(lat, lon, self.trusted_centre[0], self.trusted_centre[1])
            if away > self.max_range_nm:
                return "out_of_range"
        if self._has_teleported(entry, lat, lon, now):
            return "teleported"
        return None

    def _has_teleported(self, entry: dict, lat: float, lon: float, now: float) -> bool:
        previous = self._last_position.get((entry.get("hex") or "").lower())
        if previous is None:
            return False
        previous_lat, previous_lon, seen_at = previous
        elapsed = now - seen_at
        if elapsed < MIN_TELEPORT_INTERVAL_SECONDS:
            # Too short to infer a speed from: a rounding error over one
            # second looks supersonic.
            return False
        moved = distance_nm(lat, lon, previous_lat, previous_lon)
        return (moved / (elapsed / 3600.0)) > MAX_GROUND_SPEED_KT

    def _remember(self, entry: dict, now: float):
        hex_id = (entry.get("hex") or "").lower()
        lat, lon = entry.get("lat"), entry.get("lon")
        if hex_id and lat is not None and lon is not None:
            self._last_position[hex_id] = (lat, lon, now)

    def _forget_old(self, now: float):
        cutoff = now - POSITION_MEMORY_SECONDS
        stale = [key for key, value in self._last_position.items() if value[2] < cutoff]
        for key in stale:
            del self._last_position[key]

    # --- reporting -------------------------------------------------------

    def _maybe_log(self, entry: dict, reason: str):
        """Loud enough to notice, quiet enough not to become the log.

        A feed pushing nonsense produces one rejection per aircraft per
        merge cycle - a line each would be thousands an hour and would bury
        everything else, which is exactly what happened with the httpx
        request logging.
        """
        self._logged += 1
        if self._logged <= 3 or self._logged % 100 == 0:
            logger.warning(
                "feeder %s: rejected %s (%s) - %d of %d positions rejected so far",
                self.device_id, entry.get("hex") or "unknown", reason,
                self.rejected, self.rejected + self.accepted,
            )

    def stats(self) -> dict:
        return {
            "accepted": self.accepted,
            "rejected": self.rejected,
            "reasons": {name: count for name, count in self.reasons.items() if count},
            "trusted_centre": self.trusted_centre,
            "max_range_nm": self.max_range_nm,
        }


def _is_plausible_coordinate(lat, lon) -> bool:
    if not isinstance(lat, (int, float)) or not isinstance(lon, (int, float)):
        return False
    if math.isnan(lat) or math.isnan(lon) or math.isinf(lat) or math.isinf(lon):
        return False
    if not -90.0 <= lat <= 90.0 or not -180.0 <= lon <= 180.0:
        return False
    # Exactly 0,0 is the Gulf of Guinea and, far more often, a receiver
    # with no fix reporting zeroes. Nothing real is ever there.
    if lat == 0.0 and lon == 0.0:
        return False
    return True
