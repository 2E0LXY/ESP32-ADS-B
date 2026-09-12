"""Estimates where a feeder's receiver is, from the aircraft it reports.

An SBS/BaseStation stream carries no station identity and no station
position - every field describes an aircraft. But the aircraft themselves
give the answer away: an antenna only hears what is above its horizon, so
the lower an aircraft is when heard, the closer it must be.

At 3000 ft the radio horizon is roughly 70 nm and in practice a rooftop
antenna hears such traffic far nearer than that; at 37000 ft it is 240 nm
and tells you almost nothing. So this weighs low traffic heavily and
ignores the high stuff, then takes a median rather than a mean, because one
distant aircraft caught through a gap in the terrain should not drag the
estimate across a county.

The result is an estimate and is treated as one: it ranks below both a
position the device reports for itself and one its owner typed in.
"""

import math
import statistics
import time

# Above this, an aircraft is visible from so far away that its position says
# almost nothing about where the receiver is.
MAX_USEFUL_ALTITUDE_FT = 12000
# Enough distinct airframes that a single unusual track cannot define the
# estimate. A busy site reaches this in minutes; a quiet one takes longer,
# which is correct - it has less evidence.
MIN_SAMPLES = 12
# Beyond this the samples are old enough that a receiver could have moved.
MAX_SAMPLE_AGE_SECONDS = 60 * 60


def radio_horizon_nm(altitude_ft: float) -> float:
    """Line-of-sight distance to an aircraft at this altitude, from sea level.

    The standard 1.23 * sqrt(feet) approximation, in nautical miles. Used as
    the upper bound on how far away a sample could possibly have been.
    """
    return 1.23 * math.sqrt(max(0.0, altitude_ft))


def estimate_site(samples) -> tuple[float, float, float] | None:
    """Returns (lat, lon, confidence_radius_nm), or None if it cannot tell.

    samples: iterable of (lat, lon, altitude_ft, timestamp).

    The confidence radius is the median distance from the estimate to the
    low-altitude samples - a rough "the receiver is somewhere within about
    this far" rather than a precise error bound. It is not the coverage
    radius and should not be used as one.
    """
    # The wall clock, not the newest sample: taking "now" from the data
    # itself means a set of uniformly stale samples measures its own age as
    # zero and never expires, which is exactly the case this guards against.
    now = time.time()
    usable = [
        (lat, lon, alt)
        for lat, lon, alt, seen in samples
        if alt is not None
        and alt <= MAX_USEFUL_ALTITUDE_FT
        and now - seen <= MAX_SAMPLE_AGE_SECONDS
        and -90 <= lat <= 90
        and -180 <= lon <= 180
    ]
    if len(usable) < MIN_SAMPLES:
        return None

    # Median independently per axis. Not a true geometric median, but it is
    # robust to outliers, needs no iteration, and at these distances the
    # difference is far smaller than the uncertainty already present.
    lat = statistics.median(s[0] for s in usable)
    lon = statistics.median(s[1] for s in usable)
    spread = statistics.median(_distance_nm(lat, lon, s[0], s[1]) for s in usable)
    return (lat, lon, spread)


def _distance_nm(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r_nm = 3440.065
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlambda / 2) ** 2
    return 2 * r_nm * math.asin(math.sqrt(a))


class SiteSampler:
    """Collects low-altitude sightings for one feeder, bounded in size.

    One sample per airframe, replaced as it is re-seen, so a single aircraft
    circling overhead for an hour counts once rather than ten thousand times
    - otherwise a flying school next door would define the estimate.
    """

    def __init__(self, limit: int = 400):
        self._by_hex: dict[str, tuple[float, float, float, float]] = {}
        self._limit = limit

    def add(self, hex_id: str, lat, lon, altitude_ft, seen_at: float):
        if not hex_id or lat is None or lon is None:
            return
        if not isinstance(altitude_ft, (int, float)):
            return  # "ground" or missing - tells us nothing about range
        if altitude_ft > MAX_USEFUL_ALTITUDE_FT:
            return
        if len(self._by_hex) >= self._limit and hex_id not in self._by_hex:
            oldest = min(self._by_hex, key=lambda h: self._by_hex[h][3])
            del self._by_hex[oldest]
        self._by_hex[hex_id] = (lat, lon, float(altitude_ft), seen_at)

    def estimate(self):
        return estimate_site(self._by_hex.values())

    def __len__(self):
        return len(self._by_hex)
