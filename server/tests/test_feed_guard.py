"""Aircraft a feeder could not really have heard are rejected.

Feeder ingestion has no authentication beyond knowing which TCP port was
assigned to you - standard feeder software cannot send a credential before
its SBS stream, which is why every feeder network works this way. So
anyone who learns or scans a port can push whatever they like into the
cache every customer's panel reads, and nothing checked what arrived.

What makes it checkable without a credential is physics: a receiver is an
antenna on a roof and hears what is within line of sight of it.
"""

import math

import pytest

from app.feed_guard import (
    MAX_ALTITUDE_FT,
    MAX_GROUND_SPEED_KT,
    FeedGuard,
    distance_nm,
)

LEEDS = (53.73, -1.57)
LONDON = (51.50, -0.13)
NEW_YORK = (40.64, -73.78)


def _ac(hex_id="4ca001", lat=LEEDS[0], lon=LEEDS[1], **extra):
    entry = {"hex": hex_id, "seen": 1.0}
    if lat is not None:
        entry["lat"], entry["lon"] = lat, lon
    entry.update(extra)
    return entry


def _guard(centre=LEEDS, **kwargs):
    guard = FeedGuard(device_id=1, **kwargs)
    guard.set_trusted_centre(centre)
    return guard


# --- the range check ---------------------------------------------------

def test_an_aircraft_within_the_receivers_horizon_is_accepted():
    guard = _guard()
    kept = guard.filter([_ac(), _ac("4ca002", *LONDON)])  # Leeds is ~145 nm from London

    assert [entry["hex"] for entry in kept] == ["4ca001", "4ca002"]
    assert guard.rejected == 0


def test_an_aircraft_beyond_any_possible_range_is_rejected():
    """A Leeds antenna does not hear an aircraft over New York."""
    guard = _guard()

    kept = guard.filter([_ac("4ca001", *NEW_YORK)])

    assert kept == []
    assert guard.stats()["reasons"] == {"out_of_range": 1}


def test_the_range_limit_is_configurable():
    tight = _guard(max_range_nm=50)
    assert tight.filter([_ac("4ca001", *LONDON)]) == []
    assert tight.reasons["out_of_range"] == 1

    generous = _guard(max_range_nm=200)
    assert len(generous.filter([_ac("4ca001", *LONDON)])) == 1


def test_with_no_trusted_position_the_range_check_is_skipped_not_guessed():
    """A deployment cannot invent where a receiver is. The other checks
    still apply; this one is simply not made."""
    guard = FeedGuard(device_id=1)  # no centre set

    kept = guard.filter([_ac("4ca001", *NEW_YORK)])

    assert len(kept) == 1
    assert guard.rejected == 0
    assert guard.stats()["trusted_centre"] is None


def test_the_inferred_position_is_never_used_as_the_trusted_centre():
    """The check would otherwise certify whatever it was given: a garbage
    feed moves the inferred estimate until the garbage looks plausible.
    Device.independent_location() is what excludes it."""
    from app import models

    device = models.Device(account_id=1, name="rx")
    device.inferred_lat, device.inferred_lon = LEEDS
    assert device.independent_location() is None
    # But location() still uses it - that is for deciding where to poll,
    # which is a different question from whether to believe a feed.
    assert device.location()[:2] == LEEDS

    device.manual_lat, device.manual_lon = LONDON
    assert device.independent_location() == LONDON
    device.reported_lat, device.reported_lon = NEW_YORK
    assert device.independent_location() == NEW_YORK


# --- coordinates and altitude ------------------------------------------

@pytest.mark.parametrize("lat,lon", [
    (91.0, 0.5),          # off the planet
    (-90.1, 0.5),
    (53.7, 181.0),
    (53.7, -180.1),
    (0.0, 0.0),           # a receiver with no fix reporting zeroes
    (float("nan"), 1.0),
    (float("inf"), 1.0),
    ("53.7", -1.5),       # a decoder bug, or a hand-crafted line
])
def test_an_impossible_coordinate_is_rejected(lat, lon):
    guard = _guard()
    assert guard.filter([_ac("4ca001", lat, lon)]) == []
    assert guard.reasons["bad_position"] == 1


def test_a_position_near_but_not_at_the_null_island_is_fine():
    """0,0 is rejected because nothing real is ever exactly there - not
    because the Gulf of Guinea is implausible."""
    guard = FeedGuard(device_id=1)
    assert len(guard.filter([_ac("4ca001", 0.01, 0.01)])) == 1


@pytest.mark.parametrize("altitude", [-2000, 900000, MAX_ALTITUDE_FT + 1])
def test_an_impossible_altitude_is_rejected(altitude):
    guard = _guard()
    assert guard.filter([_ac(alt_baro=altitude)]) == []
    assert guard.reasons["bad_altitude"] == 1


@pytest.mark.parametrize("altitude", ["ground", 0, -1000, 37000, MAX_ALTITUDE_FT])
def test_a_plausible_altitude_is_accepted(altitude):
    """"ground" is a legitimate value from the decoder, not a number."""
    guard = _guard()
    assert len(guard.filter([_ac(alt_baro=altitude)])) == 1


def test_an_aircraft_with_no_position_is_not_rejected():
    """Normal: a first message, or an aircraft the receiver only has an
    identity for. There is nothing to check."""
    guard = _guard()
    kept = guard.filter([_ac("4ca001", lat=None, flight="RYR123")])
    assert len(kept) == 1
    assert guard.rejected == 0


# --- teleporting -------------------------------------------------------

def test_an_aircraft_that_teleports_is_rejected():
    """One hex reporting positions an ocean apart is two aircraft confused
    for one, or invented."""
    guard = FeedGuard(device_id=1)
    assert len(guard.filter([_ac("4ca001", *LEEDS)], now=1000.0)) == 1

    kept = guard.filter([_ac("4ca001", *NEW_YORK)], now=1060.0)

    assert kept == []
    assert guard.reasons["teleported"] == 1
    # The implied speed is the point, so check the fixture really is absurd.
    implied = distance_nm(*LEEDS, *NEW_YORK) / (60 / 3600.0)
    assert implied > MAX_GROUND_SPEED_KT


def test_normal_movement_is_not_teleporting():
    guard = FeedGuard(device_id=1)
    guard.filter([_ac("4ca001", 53.73, -1.57)], now=1000.0)

    # ~8 nm in a minute is about 480 kt - an airliner.
    kept = guard.filter([_ac("4ca001", 53.86, -1.57)], now=1060.0)

    assert len(kept) == 1
    assert guard.rejected == 0


def test_two_positions_moments_apart_are_not_judged():
    """Too short an interval to infer a speed from: a rounding error over
    one second looks supersonic."""
    guard = FeedGuard(device_id=1)
    guard.filter([_ac("4ca001", 53.73, -1.57)], now=1000.0)

    kept = guard.filter([_ac("4ca001", 53.75, -1.57)], now=1001.0)

    assert len(kept) == 1


def test_an_aircraft_returning_after_a_long_gap_is_treated_as_new():
    """The decoder forgets state after 300s, so an aircraft that comes back
    must not be charged for the distance covered while it was away."""
    guard = FeedGuard(device_id=1)
    guard.filter([_ac("4ca001", *LEEDS)], now=1000.0)

    kept = guard.filter([_ac("4ca001", *NEW_YORK)], now=1000.0 + 400)

    assert len(kept) == 1, "a forgotten aircraft cannot have teleported"


def test_a_rejected_position_is_not_remembered_for_the_next_comparison():
    """Otherwise one bad position poisons the check: everything afterwards
    is measured from somewhere the aircraft never was."""
    guard = _guard()
    guard.filter([_ac("4ca001", *LEEDS)], now=1000.0)
    guard.filter([_ac("4ca001", *NEW_YORK)], now=1100.0)  # rejected, out of range

    # Back where it was, a sensible distance on: accepted.
    kept = guard.filter([_ac("4ca001", 53.86, -1.57)], now=1200.0)

    assert len(kept) == 1


def test_the_remembered_positions_do_not_grow_without_bound():
    guard = FeedGuard(device_id=1)
    for n in range(50):
        guard.filter([_ac(f"4ca{n:03d}", 53.73, -1.57)], now=1000.0)
    assert len(guard._last_position) == 50

    guard.filter([_ac("4cbfff", 53.73, -1.57)], now=1000.0 + 400)

    assert len(guard._last_position) == 1, "stale positions must be forgotten"


# --- reporting ---------------------------------------------------------

def test_counts_are_kept_for_the_admin_panel():
    guard = _guard()
    guard.filter([
        _ac("4ca001"),
        _ac("4ca002", *NEW_YORK),
        _ac("4ca003", 0.0, 0.0),
        _ac("4ca004", alt_baro=900000),
    ])

    stats = guard.stats()
    assert stats["accepted"] == 1
    assert stats["rejected"] == 3
    assert stats["reasons"] == {"bad_position": 1, "bad_altitude": 1, "out_of_range": 1}
    assert stats["trusted_centre"] == LEEDS


def test_a_flood_of_rejections_does_not_flood_the_log(caplog):
    """A feed pushing nonsense produces one rejection per aircraft per
    merge cycle. A line each would be thousands an hour and would bury
    everything else - which is exactly what the httpx request logging
    did."""
    guard = _guard()
    with caplog.at_level("WARNING", logger="feed_guard"):
        for n in range(250):
            guard.filter([_ac(f"4c{n:04x}", *NEW_YORK)])

    assert guard.rejected == 250
    # First three, then every hundredth.
    assert 3 <= len(caplog.records) <= 6, [r.getMessage() for r in caplog.records]
    assert "out_of_range" in caplog.records[0].getMessage()


def test_the_distance_helper_agrees_with_a_known_pair():
    """Leeds to London is about 145 nm."""
    assert 140 < distance_nm(*LEEDS, *LONDON) < 150
    assert math.isclose(distance_nm(*LEEDS, *LEEDS), 0.0, abs_tol=1e-9)


# --- through the real listener -----------------------------------------

def _sbs_line(hex_ident: str, callsign: str, lat: float, lon: float, altitude=35000) -> bytes:
    """One valid SBS-1 "airborne position" line: 22 comma-separated fields."""
    return (
        f"MSG,3,1,1,{hex_ident},1,2026/09/11,17:40:00.000,2026/09/11,17:40:00.000,"
        f"{callsign},{altitude},450,270,{lat},{lon},0,7000,0,0,0,0\n"
    ).encode()


def _free_port() -> int:
    import socket

    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


async def test_an_injected_aircraft_never_reaches_the_shared_cache():
    """The whole point: a rejected position must not be served to every
    other customer's panel, and must not drag this receiver's own estimated
    location towards wherever it claimed to be."""
    import asyncio

    from app import models
    from app.aggregator import Aggregator
    from app.database import Base, SessionLocal, engine
    from app.feed_ingest import MERGE_INTERVAL_SECONDS, FeedIngestManager

    Base.metadata.drop_all(bind=engine)
    Base.metadata.create_all(bind=engine)
    db = SessionLocal()
    try:
        account = models.Account(email="guard@example.com", password_hash="x")
        db.add(account)
        db.commit()
        # The owner has set where this receiver is, so the range check has
        # something independent of the feed to work from.
        device = models.Device(account_id=account.id, name="Leeds rx",
                               manual_lat=LEEDS[0], manual_lon=LEEDS[1])
        db.add(device)
        db.commit()
        device_id = device.id
    finally:
        db.close()

    aggregator = Aggregator(0.0, 0.0, 50, SessionLocal)
    manager = FeedIngestManager(aggregator, SessionLocal)
    port = _free_port()
    await manager.start_for_device(device_id, port)
    try:
        _reader, writer = await asyncio.open_connection("127.0.0.1", port)
        writer.write(_sbs_line("4CA2D5", "RYR2BH", LEEDS[0], LEEDS[1]))
        writer.write(_sbs_line("ABCDEF", "INJECT1", NEW_YORK[0], NEW_YORK[1]))
        writer.write(_sbs_line("BADBAD", "INJECT2", 0.0, 0.0))
        await writer.drain()
        await asyncio.sleep(MERGE_INTERVAL_SECONDS + 0.5)
        cached = await aggregator.cache.query_by_source(f"feeder:{device_id}")
        writer.close()
    finally:
        await manager.stop_for_device(port)

    assert sorted(entry["hex"] for entry in cached) == ["4ca2d5"], (
        "an implausible aircraft reached the cache every customer reads"
    )
    stats = manager.guard_stats()[device_id]
    assert stats["rejected"] == 2
    assert stats["trusted_centre"] == LEEDS
    assert stats["reasons"] == {"out_of_range": 1, "bad_position": 1}


async def test_the_checks_can_be_turned_off_from_the_settings(monkeypatch):
    """An operator debugging their own feed may want everything through,
    and the switch has to actually reach this path."""
    import asyncio

    from app.aggregator import Aggregator
    from app.database import Base, SessionLocal, engine
    from app.feed_ingest import MERGE_INTERVAL_SECONDS, FeedIngestManager

    Base.metadata.create_all(bind=engine)
    aggregator = Aggregator(0.0, 0.0, 50, SessionLocal)
    aggregator.settings._values["feeder_position_checks"] = False
    manager = FeedIngestManager(aggregator, SessionLocal)
    port = _free_port()
    await manager.start_for_device(99, port)
    try:
        _reader, writer = await asyncio.open_connection("127.0.0.1", port)
        writer.write(_sbs_line("BADBAD", "INJECT2", 0.0, 0.0))
        await writer.drain()
        await asyncio.sleep(MERGE_INTERVAL_SECONDS + 0.5)
        cached = await aggregator.cache.query_by_source("feeder:99")
        writer.close()
    finally:
        await manager.stop_for_device(port)

    assert [entry["hex"] for entry in cached] == ["badbad"]
