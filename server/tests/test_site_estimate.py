"""Estimating a feeder's receiver position from the aircraft it reports.

The interesting question is not whether the arithmetic runs, it is whether
the answer lands near the real antenna given traffic that looks like real
traffic - biased in one direction, with high-altitude overflights and the
occasional distant outlier mixed in. So these build that and check the
distance to the truth.
"""

import math
import random
import time

from app import models
from app.site_estimate import (
    MAX_USEFUL_ALTITUDE_FT,
    MIN_SAMPLES,
    SiteSampler,
    _distance_nm,
    estimate_site,
    radio_horizon_nm,
)

LEEDS = (53.8659, -1.6606)  # Leeds Bradford


def _offset(lat, lon, bearing_deg, distance_nm):
    r = 3440.065
    b = math.radians(bearing_deg)
    d = distance_nm / r
    p1 = math.radians(lat)
    l1 = math.radians(lon)
    p2 = math.asin(math.sin(p1) * math.cos(d) + math.cos(p1) * math.sin(d) * math.cos(b))
    l2 = l1 + math.atan2(
        math.sin(b) * math.sin(d) * math.cos(p1), math.cos(d) - math.sin(p1) * math.sin(p2)
    )
    return (math.degrees(p2), math.degrees(l2))


def _traffic(site, count, max_range_nm=25, seed=1, altitude=4000):
    rng = random.Random(seed)
    now = time.time()
    out = []
    for i in range(count):
        lat, lon = _offset(site[0], site[1], rng.uniform(0, 360), rng.uniform(1, max_range_nm))
        out.append((lat, lon, altitude, now - rng.uniform(0, 60)))
    return out


def test_estimate_lands_near_the_real_receiver():
    estimate = estimate_site(_traffic(LEEDS, 60))
    assert estimate is not None
    lat, lon, spread = estimate
    # Within a few miles of the truth from ordinary traffic around it.
    assert _distance_nm(LEEDS[0], LEEDS[1], lat, lon) < 5
    assert 0 < spread < 30


def test_too_few_samples_gives_no_answer():
    # Better to say nothing than to place a receiver from three sightings.
    assert estimate_site(_traffic(LEEDS, MIN_SAMPLES - 1)) is None


def test_high_altitude_traffic_is_ignored():
    high = [(lat, lon, 37000, seen) for lat, lon, _, seen in _traffic(LEEDS, 60)]
    # Visible from 240 nm away, so it says almost nothing about the site.
    assert estimate_site(high) is None


def test_a_distant_outlier_does_not_drag_the_estimate():
    samples = _traffic(LEEDS, 40)
    far = _offset(LEEDS[0], LEEDS[1], 90, 200)
    samples += [(far[0], far[1], 3000, time.time())] * 3
    lat, lon, _ = estimate_site(samples)
    # A median, not a mean: three freak receptions through a gap in the
    # terrain must not move the answer across the country.
    assert _distance_nm(LEEDS[0], LEEDS[1], lat, lon) < 6


def test_stale_samples_are_dropped():
    old = [(lat, lon, 4000, seen - 7200) for lat, lon, _, seen in _traffic(LEEDS, 60)]
    # A receiver could have moved in two hours; refuse rather than guess.
    assert estimate_site(old) is None


def test_one_aircraft_circling_cannot_define_the_estimate():
    sampler = SiteSampler()
    now = time.time()
    # A flying school orbiting a few miles away, reported ten thousand times.
    for i in range(10000):
        pos = _offset(LEEDS[0], LEEDS[1], i % 360, 5)
        sampler.add("aaaaaa", pos[0], pos[1], 2000, now)
    assert len(sampler) == 1
    assert sampler.estimate() is None  # one airframe is not evidence


def test_sampler_is_bounded():
    sampler = SiteSampler(limit=50)
    now = time.time()
    for i in range(500):
        pos = _offset(LEEDS[0], LEEDS[1], i % 360, 10)
        sampler.add(f"hex{i:05d}", pos[0], pos[1], 3000, now + i)
    assert len(sampler) == 50


def test_sampler_ignores_ground_and_high_traffic():
    sampler = SiteSampler()
    now = time.time()
    sampler.add("aaa001", LEEDS[0], LEEDS[1], "ground", now)
    sampler.add("aaa002", LEEDS[0], LEEDS[1], MAX_USEFUL_ALTITUDE_FT + 1, now)
    sampler.add("aaa003", LEEDS[0], LEEDS[1], None, now)
    assert len(sampler) == 0


def test_radio_horizon_grows_with_altitude():
    # Sanity on the physics the altitude cut-off is justified by.
    assert radio_horizon_nm(3000) < radio_horizon_nm(37000)
    assert 60 < radio_horizon_nm(3000) < 80
    assert 200 < radio_horizon_nm(37000) < 260


def test_inferred_position_ranks_below_reported_and_manual():
    device = models.Device(name="x", inferred_lat=1.0, inferred_lon=2.0, inferred_spread_nm=4)
    assert device.location()[:2] == (1.0, 2.0)
    assert device.location_source() == "estimated from your feed"

    device.manual_lat, device.manual_lon, device.manual_radius_nm = 10.0, 20.0, 40
    assert device.location() == (10.0, 20.0, 40)

    device.reported_lat, device.reported_lon, device.reported_radius_nm = 30.0, 40.0, 60
    # A receiver that states its own position beats anything we deduced.
    assert device.location() == (30.0, 40.0, 60)
    assert device.location_source() == "reported by the receiver"


def test_spread_is_not_used_as_the_poll_radius():
    device = models.Device(name="x", inferred_lat=1.0, inferred_lon=2.0, inferred_spread_nm=3)
    # The spread says how scattered the evidence was, not how far the
    # receiver reaches - polling 3 nm around it would cache nothing useful.
    assert device.location()[2] >= 50


def test_samples_survive_a_reconnect():
    """A feeder that drops and reconnects must not lose its evidence.

    The sampler used to be created per connection, so on a flaky link - and
    the deployment log shows reconnects - it never accumulated the twelve
    airframes an estimate needs, and no estimate ever appeared.
    """
    from app.feed_ingest import FeedIngestManager

    manager = FeedIngestManager(aggregator=None, session_factory=None)
    now = time.time()

    # First connection: six sightings, not yet enough to answer.
    first = manager._samplers.setdefault(1, SiteSampler())
    for i in range(6):
        pos = _offset(LEEDS[0], LEEDS[1], i * 60, 8)
        first.add(f"first{i:02d}", pos[0], pos[1], 3000, now)
    assert first.estimate() is None

    # Reconnect - same device, so the same sampler.
    second = manager._samplers.setdefault(1, SiteSampler())
    assert second is first
    for i in range(8):
        pos = _offset(LEEDS[0], LEEDS[1], i * 45 + 20, 10)
        second.add(f"second{i:02d}", pos[0], pos[1], 3000, now)

    estimate = second.estimate()
    assert estimate is not None, "evidence from before the reconnect was lost"
    assert _distance_nm(LEEDS[0], LEEDS[1], estimate[0], estimate[1]) < 6


def test_each_device_samples_separately():
    from app.feed_ingest import FeedIngestManager

    manager = FeedIngestManager(aggregator=None, session_factory=None)
    now = time.time()
    for i in range(20):
        pos = _offset(LEEDS[0], LEEDS[1], i * 18, 8)
        manager._samplers.setdefault(1, SiteSampler()).add(f"a{i:02d}", pos[0], pos[1], 3000, now)
    # A second feeder elsewhere must not pick up the first one's sightings.
    assert manager._samplers.setdefault(2, SiteSampler()).estimate() is None
    assert manager._samplers[1].estimate() is not None
