"""The Redis cache must behave exactly like the in-process one.

The in-process dict was the single hard ceiling on this deployment: a
second uvicorn worker would have had its own empty cache and served half
the receivers nothing, so extra vCPU bought nothing at all. Moving it to
Redis lifts that - but only if switching REDIS_URL on cannot change what a
device sees.

So every scenario here runs twice, once through each implementation, from
the same test body. A behaviour that differs fails on one of the two.
"""

import time

import pytest

from app.cache import (
    SOURCE_ATTRIBUTION_SECONDS,
    STALE_AFTER_SECONDS,
    AircraftCache,
    RedisAircraftCache,
    build_cache,
)

LEEDS = (53.73, -1.57)


def _ac(hex_id, lat=LEEDS[0], lon=LEEDS[1], seen=1.0, **extra):
    ac = {"hex": hex_id, "flight": f"TST{hex_id[-2:]}", "seen": seen, "alt_baro": 30000}
    if lat is not None:
        ac["lat"], ac["lon"] = lat, lon
    ac.update(extra)
    return ac


@pytest.fixture(params=["memory", "redis"])
async def cache(request):
    if request.param == "memory":
        yield AircraftCache()
        return
    fakeredis = pytest.importorskip("fakeredis")
    pytest.importorskip("lupa")  # fakeredis needs it to run the merge script
    client = fakeredis.aioredis.FakeRedis(decode_responses=True)
    store = RedisAircraftCache("redis://fake", client=client)
    yield store
    await client.flushall()
    await client.aclose()


async def _age(cache, hex_id, seconds):
    """Pretend a cached record was written `seconds` ago.

    The two implementations track that differently - a field on the entry
    versus a score in a sorted set - so this is the one thing the shared
    scenarios cannot express identically.
    """
    if isinstance(cache, AircraftCache):
        cache._by_hex[hex_id].seen_at -= seconds
    else:
        current = await cache._redis.zscore(cache._written, hex_id)
        await cache._redis.zadd(cache._written, {hex_id: current - seconds})


def _hexes(records):
    return sorted(r["hex"] for r in records)


# --- what a device poll gets ------------------------------------------

async def test_an_aircraft_in_range_comes_back_and_one_outside_does_not(cache):
    await cache.merge("adsbfi", [
        _ac("4ca001"),                                  # overhead
        _ac("4ca002", lat=53.80, lon=-1.60),            # ~5 nm
        _ac("4ca003", lat=51.50, lon=-0.13),            # London, ~145 nm
    ])

    near = await cache.query(LEEDS[0], LEEDS[1], 25)
    assert _hexes(near) == ["4ca001", "4ca002"]
    wide = await cache.query(LEEDS[0], LEEDS[1], 250)
    assert _hexes(wide) == ["4ca001", "4ca002", "4ca003"]


async def test_the_record_carries_the_source_that_won(cache):
    await cache.merge("adsbfi", [_ac("4ca001")])
    record, = await cache.query(LEEDS[0], LEEDS[1], 25)
    assert record["_source"] == "adsbfi"
    assert record["flight"] == "TST01"
    assert record["alt_baro"] == 30000


async def test_the_fresher_record_wins_whichever_source_reported_last(cache):
    """seen is seconds-ago from upstream, so smaller is fresher. Last poll
    wins would let a staler source overwrite a better fix."""
    await cache.merge("adsbfi", [_ac("4ca001", seen=0.5, alt_baro=31000)])
    await cache.merge("adsblol", [_ac("4ca001", seen=9.0, alt_baro=12000)])

    record, = await cache.query(LEEDS[0], LEEDS[1], 25)
    assert record["alt_baro"] == 31000
    assert record["_source"] == "adsbfi"

    # And the other way round: a genuinely fresher record does replace it.
    await cache.merge("adsblol", [_ac("4ca001", seen=0.1, alt_baro=12000)])
    record, = await cache.query(LEEDS[0], LEEDS[1], 25)
    assert record["alt_baro"] == 12000
    assert record["_source"] == "adsblol"


async def test_a_position_update_moves_the_aircraft(cache):
    """The radius query has to follow the newest position, not the one the
    aircraft was first cached at."""
    await cache.merge("adsbfi", [_ac("4ca001", seen=5.0)])
    await cache.merge("adsbfi", [_ac("4ca001", lat=51.50, lon=-0.13, seen=1.0)])

    assert await cache.query(LEEDS[0], LEEDS[1], 25) == []
    moved, = await cache.query(51.50, -0.13, 25)
    assert moved["hex"] == "4ca001"


# --- attribution, for the "my feed" map --------------------------------

async def test_a_source_still_claims_an_aircraft_it_lost_the_merge_for(cache):
    """This is the bug that emptied a customer's own feed map: attribution
    used to live on the winning record, so an aircraft their receiver was
    tracking vanished the moment an upstream reported it fractionally
    fresher - which is likeliest for the traffic closest to them."""
    await cache.merge("feeder:1", [_ac("4ca001", seen=9.0)])
    await cache.merge("adsbfi", [_ac("4ca001", seen=0.5)])

    mine = await cache.query_by_source("feeder:1")
    assert _hexes(mine) == ["4ca001"]
    # The record served is still the freshest one, whoever it came from.
    assert mine[0]["_source"] == "adsbfi"


async def test_attribution_lapses_once_a_source_goes_quiet(cache):
    await cache.merge("feeder:1", [_ac("4ca001")])
    assert len(await cache.query_by_source("feeder:1")) == 1

    key = "feeder:1"
    stale = time.time() - SOURCE_ATTRIBUTION_SECONDS - 1
    if isinstance(cache, AircraftCache):
        cache._by_hex["4ca001"].sources[key] = stale
    else:
        await cache._redis.zadd(cache._source_key(key), {"4ca001": stale})

    assert await cache.query_by_source(key) == []


async def test_one_source_does_not_claim_anothers_aircraft(cache):
    await cache.merge("feeder:1", [_ac("4ca001")])
    await cache.merge("adsbfi", [_ac("4ca002", lat=53.80, lon=-1.60)])

    assert _hexes(await cache.query_by_source("feeder:1")) == ["4ca001"]
    assert _hexes(await cache.query_by_source("adsbfi")) == ["4ca002"]
    assert await cache.query_by_source("feeder:2") == []


# --- edges -------------------------------------------------------------

async def test_an_aircraft_with_no_position_is_kept_but_never_in_a_radius(cache):
    """Common for MLAT-less military and for a first message. It still
    belongs to its source, it just cannot answer "what is near me"."""
    await cache.merge("feeder:1", [_ac("4ca001", lat=None, lon=None)])

    assert await cache.query(LEEDS[0], LEEDS[1], 250) == []
    assert _hexes(await cache.query_by_source("feeder:1")) == ["4ca001"]
    assert cache.size() == 1 or await cache.refresh_size() == 1


async def test_an_aircraft_with_no_hex_is_dropped(cache):
    await cache.merge("adsbfi", [{"flight": "NOHEX", "lat": LEEDS[0], "lon": LEEDS[1]},
                                 {"hex": "", "lat": LEEDS[0], "lon": LEEDS[1]}])
    assert await cache.refresh_size() == 0
    assert await cache.query(LEEDS[0], LEEDS[1], 250) == []


async def test_a_hex_is_normalised_to_lower_case(cache):
    await cache.merge("adsbfi", [_ac("4CA001")])
    await cache.merge("adsbfi", [_ac("4ca001", seen=0.1)])
    assert await cache.refresh_size() == 1


async def test_merging_nothing_is_harmless(cache):
    await cache.merge("adsbfi", [])
    assert await cache.refresh_size() == 0


# --- pruning -----------------------------------------------------------

async def test_a_stale_aircraft_is_pruned_and_a_live_one_is_not(cache):
    await cache.merge("adsbfi", [_ac("4ca001"), _ac("4ca002", lat=53.80, lon=-1.60)])
    await _age(cache, "4ca001", STALE_AFTER_SECONDS + 5)

    await cache.prune()

    assert _hexes(await cache.query(LEEDS[0], LEEDS[1], 250)) == ["4ca002"]
    assert await cache.refresh_size() == 1


async def test_losing_a_merge_does_not_extend_an_aircrafts_life(cache):
    """Staleness means "how old is the record we are serving", so an update
    that was rejected for being staler must not reset the clock."""
    await cache.merge("adsbfi", [_ac("4ca001", seen=0.5)])
    await _age(cache, "4ca001", STALE_AFTER_SECONDS + 5)
    await cache.merge("adsblol", [_ac("4ca001", seen=9.0)])

    await cache.prune()

    assert await cache.query(LEEDS[0], LEEDS[1], 250) == []


async def test_winning_a_merge_does_extend_it(cache):
    await cache.merge("adsbfi", [_ac("4ca001", seen=9.0)])
    await _age(cache, "4ca001", STALE_AFTER_SECONDS + 5)
    await cache.merge("adsblol", [_ac("4ca001", seen=0.5)])

    await cache.prune()

    assert _hexes(await cache.query(LEEDS[0], LEEDS[1], 250)) == ["4ca001"]


async def test_pruning_an_empty_cache_is_harmless(cache):
    await cache.prune()
    assert await cache.refresh_size() == 0


# --- selection ---------------------------------------------------------

def test_no_redis_url_means_the_behaviour_this_has_always_had():
    """Redis must not become a hard dependency for anyone who has not asked
    for it."""
    assert isinstance(build_cache(""), AircraftCache)


async def test_a_redis_url_selects_the_shared_cache():
    pytest.importorskip("redis")
    cache = build_cache("redis://127.0.0.1:6379/0")
    assert isinstance(cache, RedisAircraftCache)
    # Constructing it must not connect: a worker has to start even if Redis
    # is briefly down, and the first command is what reconnects.
    await cache.close()


# --- the real endpoint, served from Redis ------------------------------

def test_a_device_poll_is_served_from_the_redis_cache(client):
    """Parity at the cache boundary is not quite enough: this drives the
    real /v1/aircraft, with the real auth dependency and the real
    enrichment, against the Redis implementation - the path a worker
    process would actually take."""
    fakeredis = pytest.importorskip("fakeredis")
    pytest.importorskip("lupa")

    from app import models, security
    from app.database import SessionLocal
    from app.main import app

    client.post("/signup", data={"email": "redis@example.com", "password": "correct-horse"},
                follow_redirects=False)
    client.post("/devices", data={"name": "Redis receiver"}, follow_redirects=False)
    db = SessionLocal()
    try:
        device_id = db.query(models.Device).first().id
    finally:
        db.close()
    client.post(f"/devices/{device_id}/reissue-key", follow_redirects=False)
    key = security.read_flash_token(client.cookies.get("flash_key"))["key"]

    redis_client = fakeredis.aioredis.FakeRedis(decode_responses=True)
    store = RedisAircraftCache("redis://fake", client=redis_client)
    original = app.state.aggregator.cache
    app.state.aggregator.cache = store
    try:
        # Seed through the portal's loop, the same one the endpoint runs on.
        client.portal.call(store.merge, "adsbfi", [
            _ac("4ca001", flight="RYR123"),
            _ac("4ca002", lat=51.50, lon=-0.13, flight="BAW999"),
        ])

        response = client.get(
            "/v1/aircraft?lat=53.73&lon=-1.57&radius=25",
            headers={"Authorization": f"Bearer {key}",
                     "User-Agent": "2E0LXY-ESP32-ADSB/2.6.0 (+x)"},
        )
        assert response.status_code == 200, response.text
        body = response.json()
        assert [entry["hex"] for entry in body["ac"]] == ["4ca001"]
        assert body["ac"][0]["flight"] == "RYR123"
    finally:
        app.state.aggregator.cache = original
        client.portal.call(redis_client.aclose)
