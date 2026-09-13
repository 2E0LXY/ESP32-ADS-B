"""/v1/aircraft carries the route, so the device never queries adsbdb.

Goes through the real endpoint, the real auth dependency and the real
cache; only adsbdb itself is stubbed.
"""

import asyncio
import re

import httpx
import pytest

from app import routes


@pytest.fixture()
def resolver_with_stub(client):
    """Replaces the running resolver's HTTP client with a canned adsbdb."""

    def handler(request):
        callsign = re.sub(r".*/callsign/", "", str(request.url))
        if callsign != "EZY51NR":
            return httpx.Response(404, json={"response": "unknown callsign"})
        return httpx.Response(
            200,
            json={
                "response": {
                    "flightroute": {
                        "airline": {"name": "easyJet", "icao": "EZY"},
                        "origin": {"iata_code": "CFU", "name": "Corfu", "municipality": "Corfu"},
                        "destination": {
                            "iata_code": "EDI",
                            "name": "Edinburgh Airport",
                            "municipality": "Edinburgh",
                        },
                    }
                }
            },
        )

    from app.main import app

    app.state.routes._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    return app.state.routes


def _issue_key(client) -> str:
    client.post("/signup", data={"email": "route@example.com", "password": "correct-horse"},
                follow_redirects=False)
    client.post("/devices", data={"name": "Test receiver"}, follow_redirects=False)

    from app import models
    from app.database import SessionLocal

    db = SessionLocal()
    try:
        device_id = db.query(models.Device).first().id
    finally:
        db.close()
    client.post(f"/devices/{device_id}/reissue-key", follow_redirects=False)
    # The plaintext key only exists inside the one-shot flash cookie, so
    # read it back the same way the dashboard does.
    from app import security

    payload = security.read_flash_token(client.cookies.get("flash_key"))
    return payload["key"]


def _seed(client, callsign):
    from app.main import app

    asyncio.get_event_loop().run_until_complete(
        app.state.aggregator.cache.merge(
            "test", [{"hex": "4ca2d6", "flight": callsign, "lat": 53.73, "lon": -1.57, "alt_baro": 37000}]
        )
    )


def test_aircraft_response_gains_a_route(client, resolver_with_stub):
    key = _issue_key(client)
    _seed(client, "EZY51NR")
    headers = {"Authorization": f"Bearer {key}"}
    params = {"lat": 53.73, "lon": -1.57, "radius": 50}

    # First poll: not resolved yet, so no route and no waiting.
    first = client.get("/v1/aircraft", params=params, headers=headers)
    assert first.status_code == 200, first.text
    assert first.json()["ac"][0].get("route") is None

    asyncio.get_event_loop().run_until_complete(
        asyncio.wait_for(resolver_with_stub._queue.join(), timeout=5)
    )

    # Second poll: the device gets the route attached to the aircraft it was
    # already fetching - no extra request from the device at all.
    second = client.get("/v1/aircraft", params=params, headers=headers).json()["ac"][0]
    assert second["route"]["origin"] == "CFU"
    assert second["route"]["destination"] == "EDI"
    assert second["route"]["destination_name"] == "Edinburgh Airport"


def test_unknown_callsign_leaves_the_aircraft_untouched(client, resolver_with_stub):
    key = _issue_key(client)
    _seed(client, "PRIVATE1")
    headers = {"Authorization": f"Bearer {key}"}
    params = {"lat": 53.73, "lon": -1.57, "radius": 50}

    client.get("/v1/aircraft", params=params, headers=headers)
    asyncio.get_event_loop().run_until_complete(
        asyncio.wait_for(resolver_with_stub._queue.join(), timeout=5)
    )
    entry = client.get("/v1/aircraft", params=params, headers=headers).json()["ac"][0]
    # No route key at all rather than an empty one - the firmware tells
    # "not looked up yet" from "looked up, nothing on file" by other means.
    assert "route" not in entry
    assert entry["flight"] == "PRIVATE1"


# --- nearest first, and capped -----------------------------------------

def _seed_many(client, count, spread_nm=90.0):
    """count aircraft at increasing distance from the receiver."""
    from app.main import app

    fleet = []
    for n in range(count):
        # ~1 nm per 1/60 degree of latitude, so this walks steadily north.
        fleet.append({"hex": f"4c{n:04x}", "flight": f"TST{n:04d}",
                      "lat": 53.73 + (spread_nm * (n + 1) / count) / 60.0,
                      "lon": -1.57, "alt_baro": 30000})
    asyncio.get_event_loop().run_until_complete(
        app.state.aggregator.cache.merge("test", fleet))
    return fleet


def test_aircraft_come_back_nearest_first(client):
    """The firmware keeps the first MAX_AIRCRAFT it sees and only then sorts
    by distance, so the order this endpoint returns decides which aircraft
    exist as far as the panel is concerned."""
    key = _issue_key(client)
    _seed_many(client, 20)

    body = client.get("/v1/aircraft", params={"lat": 53.73, "lon": -1.57, "radius": 250},
                      headers={"Authorization": f"Bearer {key}"}).json()

    latitudes = [entry["lat"] for entry in body["ac"]]
    assert latitudes == sorted(latitudes), "not ordered by distance from the receiver"
    assert body["ac"][0]["flight"] == "TST0000"


def test_a_crowded_sky_is_capped_at_what_the_device_can_hold(client):
    """Past the cap the device discards the rest. Capping here, after
    sorting, makes that "the nearest 250" instead of an arbitrary 250 - at
    100 nm on a busy evening the aircraft overhead could otherwise be the
    one dropped."""
    from app.routers.public import MAX_AIRCRAFT_PER_RESPONSE

    key = _issue_key(client)
    _seed_many(client, MAX_AIRCRAFT_PER_RESPONSE + 60)

    body = client.get("/v1/aircraft", params={"lat": 53.73, "lon": -1.57, "radius": 250},
                      headers={"Authorization": f"Bearer {key}"}).json()

    assert len(body["ac"]) == MAX_AIRCRAFT_PER_RESPONSE
    assert body["returned"] == MAX_AIRCRAFT_PER_RESPONSE
    # total is what is in range, so a device can say "250 of 310" rather
    # than presenting 250 as the whole sky.
    assert body["total"] == MAX_AIRCRAFT_PER_RESPONSE + 60
    # And the ones kept are the near ones.
    latitudes = [entry["lat"] for entry in body["ac"]]
    assert latitudes == sorted(latitudes)
    assert max(latitudes) < 53.73 + 90 / 60.0


def test_an_uncrowded_sky_reports_the_same_count_both_ways(client):
    key = _issue_key(client)
    _seed_many(client, 5)

    body = client.get("/v1/aircraft", params={"lat": 53.73, "lon": -1.57, "radius": 250},
                      headers={"Authorization": f"Bearer {key}"}).json()

    assert body["total"] == body["returned"] == len(body["ac"]) == 5
