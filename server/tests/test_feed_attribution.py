"""A feeder must keep seeing its own aircraft.

Reproduces the fault the hardware showed: the feeder's map was missing the
aircraft nearest the receiver while keeping distant ones, because those are
exactly the aircraft the aggregator also polls upstream for, and whichever
record was fractionally fresher used to take ownership.
"""

import asyncio
import time

import pytest

from app.aggregator import SOURCE_ATTRIBUTION_SECONDS, AircraftCache


def _ac(hexid, seen, lat=53.73, lon=-1.57):
    return {"hex": hexid, "flight": hexid.upper(), "lat": lat, "lon": lon, "seen": seen}


@pytest.mark.asyncio
async def test_feeder_keeps_its_aircraft_when_upstream_reports_fresher():
    cache = AircraftCache()
    # The receiver hears it, two seconds since its last message.
    await cache.merge("feeder:1", [_ac("abc123", seen=2.0)])
    # An upstream API reports the same aircraft a fraction fresher, which is
    # routine and says nothing about who can see it.
    await cache.merge("adsbfi", [_ac("abc123", seen=0.1)])

    mine = await cache.query_by_source("feeder:1")
    assert [a["hex"] for a in mine] == ["abc123"]
    # The fresher values still win for everyone else - this is about
    # attribution, not about serving stale positions.
    shared = await cache.query(53.73, -1.57, 50)
    assert shared[0]["seen"] == 0.1


@pytest.mark.asyncio
async def test_the_pattern_from_the_hardware():
    """Near aircraft are also polled upstream, distant ones are not. Before
    the fix that meant a feeder saw only its distant traffic."""
    cache = AircraftCache()
    near = [_ac(f"near{i:02d}", seen=2.0) for i in range(5)]
    far = [_ac(f"far{i:02d}", seen=2.0, lat=50.9, lon=0.5) for i in range(5)]
    await cache.merge("feeder:1", near + far)
    await cache.merge("adsbfi", [_ac(a["hex"], seen=0.1) for a in near])

    mine = {a["hex"] for a in await cache.query_by_source("feeder:1")}
    assert len(mine) == 10, "the feeder must still see everything it reported"


@pytest.mark.asyncio
async def test_attribution_expires_when_a_feeder_goes_quiet():
    cache = AircraftCache()
    await cache.merge("feeder:1", [_ac("abc123", seen=2.0)])
    entry = cache._by_hex["abc123"]
    entry.sources["feeder:1"] = time.time() - SOURCE_ATTRIBUTION_SECONDS - 1
    # A feeder that has stopped reporting must stop claiming the sky.
    assert await cache.query_by_source("feeder:1") == []


@pytest.mark.asyncio
async def test_two_feeders_both_keep_a_shared_aircraft():
    cache = AircraftCache()
    await cache.merge("feeder:1", [_ac("abc123", seen=3.0)])
    await cache.merge("feeder:2", [_ac("abc123", seen=1.0)])
    # Two receivers in range of the same aircraft both genuinely see it.
    assert len(await cache.query_by_source("feeder:1")) == 1
    assert len(await cache.query_by_source("feeder:2")) == 1


@pytest.mark.asyncio
async def test_a_source_that_never_reported_gets_nothing():
    cache = AircraftCache()
    await cache.merge("feeder:1", [_ac("abc123", seen=1.0)])
    assert await cache.query_by_source("feeder:2") == []
