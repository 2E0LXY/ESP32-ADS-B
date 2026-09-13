"""OpenSky Network as a fourth upstream source, polled once for everyone.

The firmware can already talk to OpenSky itself, but that means every
receiver holding its own credential and spending its own allowance, and a
TLS handshake plus an OAuth token exchange on a board where a handshake
costs ~500 ms and ~32 KB of contiguous internal RAM. Polling it here
instead pools one credential across every device and costs the receivers
nothing - the aircraft arrive in the same /v1/aircraft response they
already fetch.

Two things about this API that the code has to get right:

* OAuth2 client credentials only. Basic authentication with a username and
  password is no longer accepted, and tokens last 30 minutes - so one is
  fetched, cached, and refreshed a little early rather than re-fetched per
  request.
* SI units. A state vector reports altitude in metres and velocity in
  metres per second, while every other source here - and the firmware, and
  the display - works in feet and knots. Merging them unconverted would
  put an airliner at "11,000 ft" while adsb.fi has it at 36,000, and the
  two records would fight over the same hex on every poll.
"""

import logging
import math
import time

import httpx

logger = logging.getLogger("opensky")

TOKEN_URL = ("https://auth.opensky-network.org/auth/realms/opensky-network"
             "/protocol/openid-connect/token")
STATES_URL = "https://opensky-network.org/api/states/all"
# Tokens last 30 minutes. Refreshed at 25 so a poll never races the expiry.
TOKEN_LIFETIME_SECONDS = 25 * 60
# A state vector is a fixed-order array; these are the indices this uses.
# Named rather than inlined because "state[13]" in the middle of a
# conversion is unreadable and one transposed pair would be silent.
ICAO24, CALLSIGN, COUNTRY = 0, 1, 2
LAST_CONTACT = 4
LONGITUDE, LATITUDE, BARO_ALTITUDE = 5, 6, 7
ON_GROUND, VELOCITY, TRUE_TRACK, VERTICAL_RATE = 8, 9, 10, 11
GEO_ALTITUDE, SQUAWK = 13, 14
CATEGORY = 17
STATE_VECTOR_LENGTH = 17  # the shortest response worth trusting

METRES_TO_FEET = 3.280839895
MPS_TO_KNOTS = 1.943844
# Feet per minute, which is what baro_rate means everywhere else here.
MPS_TO_FEET_PER_MINUTE = 196.8503937


class OpenSkyClient:
    """Fetches and normalises state vectors. Holds one access token."""

    def __init__(self):
        self._token = ""
        self._token_expires_at = 0.0
        self.last_error: str | None = None

    def configured(self, client_id: str, client_secret: str) -> bool:
        return bool(client_id and client_secret)

    def token_seconds_left(self) -> int:
        """How long the held token is still good for, 0 if none is held.

        Reported on /admin/system so an operator who has just pasted a
        client ID and secret can see the exchange succeed, rather than
        having to infer it from aircraft counts.
        """
        if not self._token:
            return 0
        return max(0, int(self._token_expires_at - time.time()))

    async def _access_token(self, http: httpx.AsyncClient, client_id: str,
                            client_secret: str) -> str:
        if self._token and time.time() < self._token_expires_at:
            return self._token
        response = await http.post(
            TOKEN_URL,
            data={"grant_type": "client_credentials",
                  "client_id": client_id, "client_secret": client_secret},
            timeout=15.0,
        )
        response.raise_for_status()
        payload = response.json()
        token = payload.get("access_token") or ""
        if not token:
            raise ValueError("no access_token in the OpenSky token response")
        # Honour what the server says it granted, but never trust it past
        # the documented 30 minutes.
        lifetime = min(float(payload.get("expires_in", TOKEN_LIFETIME_SECONDS)),
                       TOKEN_LIFETIME_SECONDS)
        self._token = token
        self._token_expires_at = time.time() + max(60.0, lifetime - 60.0)
        logger.info("OpenSky token obtained, good for %.0f minutes", lifetime / 60)
        return token

    async def fetch_region(self, http: httpx.AsyncClient, client_id: str,
                           client_secret: str, lat: float, lon: float,
                           radius_nm: float) -> list[dict]:
        """Aircraft in a bounding box around one polling area."""
        token = await self._access_token(http, client_id, client_secret)
        # A degree of latitude is 60 nm; longitude shrinks with the cosine of
        # the latitude, and at 54 north that is a factor of 1.7 - a square
        # box in degrees would be far narrower than intended on the ground.
        latitude_span = radius_nm / 60.0
        longitude_span = radius_nm / (60.0 * max(0.1, math.cos(math.radians(lat))))
        response = await http.get(
            STATES_URL,
            params={"lamin": lat - latitude_span, "lamax": lat + latitude_span,
                    "lomin": lon - longitude_span, "lomax": lon + longitude_span},
            headers={"Authorization": f"Bearer {token}"},
            timeout=20.0,
        )
        if response.status_code == 401:
            # The token was rejected - drop it so the next cycle fetches a
            # fresh one rather than retrying the same rejected string.
            self._token = ""
            self._token_expires_at = 0.0
        response.raise_for_status()
        return normalise(response.json())


def normalise(payload) -> list[dict]:
    """State vectors into the shape the cache and the firmware expect.

    Anything unparseable is skipped rather than raised on: one malformed
    vector in a response of hundreds should cost that one aircraft, not the
    whole poll.
    """
    if not isinstance(payload, dict):
        return []
    states = payload.get("states") or []
    reported_at = payload.get("time")
    aircraft = []
    for state in states:
        if not isinstance(state, (list, tuple)) or len(state) <= STATE_VECTOR_LENGTH:
            continue
        hex_id = str(state[ICAO24] or "").strip().lower()
        latitude, longitude = state[LATITUDE], state[LONGITUDE]
        if not hex_id or latitude is None or longitude is None:
            continue
        entry: dict = {"hex": hex_id, "lat": latitude, "lon": longitude}

        callsign = str(state[CALLSIGN] or "").strip()
        if callsign:
            entry["flight"] = callsign
        country = str(state[COUNTRY] or "").strip()
        if country:
            entry["cou"] = country

        on_ground = bool(state[ON_GROUND])
        if on_ground:
            # "ground" is the string every other source uses, and the
            # firmware tests for it rather than for a number.
            entry["alt_baro"] = "ground"
        elif state[BARO_ALTITUDE] is not None:
            entry["alt_baro"] = round(state[BARO_ALTITUDE] * METRES_TO_FEET)
        if state[GEO_ALTITUDE] is not None:
            entry["alt_geom"] = round(state[GEO_ALTITUDE] * METRES_TO_FEET)
        if state[VELOCITY] is not None:
            entry["gs"] = round(state[VELOCITY] * MPS_TO_KNOTS, 1)
        if state[TRUE_TRACK] is not None:
            entry["track"] = state[TRUE_TRACK]
        if state[VERTICAL_RATE] is not None:
            entry["baro_rate"] = round(state[VERTICAL_RATE] * MPS_TO_FEET_PER_MINUTE)
        squawk = str(state[SQUAWK] or "").strip()
        if squawk:
            entry["squawk"] = squawk
        if len(state) > CATEGORY and state[CATEGORY]:
            entry["category"] = state[CATEGORY]

        # seen is seconds-ago, which is what the cache's freshness
        # comparison is built on. OpenSky gives an absolute last_contact.
        last_contact = state[LAST_CONTACT]
        if isinstance(last_contact, (int, float)) and isinstance(reported_at, (int, float)):
            entry["seen"] = max(0.0, round(reported_at - last_contact, 1))
        aircraft.append(entry)
    return aircraft
