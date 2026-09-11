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
