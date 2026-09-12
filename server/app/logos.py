"""Airline logos, fetched once and served from this deployment's own disk.

Every client that draws an operator badge - the my-feed map, a shared feed
link, the device's own browser UI - wants the same few dozen logos over and
over. Fetching them from logo.dev directly from each page would spend an API
call per viewer per aircraft for an image that has not changed in years, and
would put the account's publishable token in front of anyone who opened the
page. So the server fetches each logo once, keeps it on the mounted volume
alongside the database, and serves it from there afterwards.

Lookup is by airline domain, not by name. That is not a style choice: on the
name path logo.dev ignores fallback=404 and returns a generated monogram
with 200 OK, so a name we cannot match is indistinguishable from one we can,
and the cache fills with monograms we could draw ourselves. On the domain
path the 404 is real, which is what lets a miss fall through to the caller's
own badge. Verified against the live API, September 2026.

The ICAO prefixes match the firmware's own OPERATOR_NAMES table so the panel
and the browser agree on who is flying; every domain here was checked to
return a real logo rather than assumed.
"""

import asyncio
import logging
import os
import re
import time

import httpx

logger = logging.getLogger("logos")

# Publishable token. Deliberately not defaulted to a real key: this
# repository is public, and a committed token is someone else's quota to
# spend. Unset means the endpoint reports every logo as missing and callers
# draw their own badge, which is a working deployment, just a plainer one.
LOGO_TOKEN = os.environ.get("LOGO_DEV_TOKEN", "")
LOGO_URL = "https://img.logo.dev/{domain}"
CACHE_DIR = os.environ.get("LOGO_CACHE_DIR", "/data/logos")
# One of a fixed few, so a caller cannot fill the disk by asking for every
# pixel width between 16 and 800.
ALLOWED_SIZES = (64, 128, 256)
DEFAULT_SIZE = 128
# A miss is remembered, or a newly added airline would re-ask on every single
# page view for as long as logo.dev has nothing. A week is long enough to
# stop the traffic and short enough that a logo added upstream appears
# without anyone clearing the cache.
MISS_TTL_SECONDS = 7 * 24 * 60 * 60
FETCH_TIMEOUT_SECONDS = 10
MAX_LOGO_BYTES = 512 * 1024

CODE_PATTERN = re.compile(r"^[A-Z]{3}$")

AIRLINE_DOMAINS = {
    "BAW": "britishairways.com",
    "SHT": "britishairways.com",
    "EZY": "easyjet.com",
    "EJU": "easyjet.com",
    "RYR": "ryanair.com",
    "RUK": "ryanair.com",
    "EXS": "jet2.com",
    "TOM": "tui.co.uk",
    "VIR": "virginatlantic.com",
    "LOG": "loganair.co.uk",
    "EAG": "emeraldairlines.com",
    "EIN": "aerlingus.com",
    "BEE": "blueislands.com",
    "NPT": "westatlantic.eu",
    "DHK": "dhl.com",
    "BCS": "dhl.com",
    "DLH": "lufthansa.com",
    "GEC": "lufthansa-cargo.com",
    "EWG": "eurowings.com",
    "CFG": "condor.com",
    "AFR": "airfrance.com",
    "KLM": "klm.com",
    "SWR": "swiss.com",
    "AUA": "austrian.com",
    "BEL": "brusselsairlines.com",
    "IBE": "iberia.com",
    "VLG": "vueling.com",
    "AEA": "aireuropa.com",
    "TAP": "flytap.com",
    "SAS": "flysas.com",
    "FIN": "finnair.com",
    "NAX": "norwegian.com",
    "WZZ": "wizzair.com",
    "WUK": "wizzair.com",
    "LOT": "lot.com",
    "CTN": "croatiaairlines.com",
    "AEE": "aegeanair.com",
    "ICE": "icelandair.com",
    "BTI": "airbaltic.com",
    "CLX": "cargolux.com",
    "SXS": "sunexpress.de",
    "PGT": "flypgs.com",
    "THY": "turkishairlines.com",
    "UAE": "emirates.com",
    "QTR": "qatarairways.com",
    "ETD": "etihad.com",
    "SVA": "saudia.com",
    "FDB": "flydubai.com",
    "ABY": "airarabia.com",
    "GFA": "gulfair.com",
    "OMA": "omanair.com",
    "KAC": "kuwaitairways.com",
    "MEA": "mea.com.lb",
    "RJA": "rj.com",
    "ELY": "elal.com",
    "MSR": "egyptair.com",
    "RAM": "royalairmaroc.com",
    "ETH": "ethiopianairlines.com",
    "KQA": "kenya-airways.com",
    "AIC": "airindia.com",
    "PIA": "piac.com.pk",
    "SIA": "singaporeair.com",
    "CPA": "cathaypacific.com",
    "THA": "thaiairways.com",
    "MAS": "malaysiaairlines.com",
    "JAL": "jal.co.jp",
    "ANA": "ana.co.jp",
    "KAL": "koreanair.com",
    "AAR": "flyasiana.com",
    "CCA": "airchina.com",
    "CES": "ceair.com",
    "CSN": "csair.com",
    "AAL": "aa.com",
    "UAL": "united.com",
    "DAL": "delta.com",
    "SWA": "southwest.com",
    "JBU": "jetblue.com",
    "ACA": "aircanada.com",
    "WJA": "westjet.com",
    "AMX": "aeromexico.com",
    "LAN": "latamairlines.com",
    "AVA": "avianca.com",
    "QFA": "qantas.com",
    "ANZ": "airnewzealand.com",
    "FDX": "fedex.com",
    "UPS": "ups.com",
    "GTI": "atlasair.com",
    "NJE": "netjets.com",
    "EJA": "netjets.com",
    "VJT": "vistajet.com",
    "LXJ": "flexjet.com",
}


def code_for_callsign(callsign: str | None) -> str | None:
    """The ICAO airline prefix of a callsign, if we know that airline.

    Callsigns are the operator's three-letter ICAO code followed by a flight
    number - RYR2BH is Ryanair. Feeds pad them to eight characters, so strip
    first.
    """
    name = (callsign or "").strip().upper()
    if len(name) < 4:
        return None  # a bare three-letter code is not a flight
    prefix = name[:3]
    return prefix if prefix in AIRLINE_DOMAINS else None


class LogoStore:
    def __init__(self, cache_dir: str = CACHE_DIR, token: str = LOGO_TOKEN):
        self._dir = cache_dir
        self._token = token
        self._client: httpx.AsyncClient | None = None
        # One in-flight fetch per logo. Without this, a page opening with
        # twenty Ryanair aircraft on it starts twenty identical fetches.
        self._locks: dict[str, asyncio.Lock] = {}
        self.hits = 0
        self.fetches = 0
        self.misses = 0

    def start(self):
        os.makedirs(self._dir, exist_ok=True)
        self._client = httpx.AsyncClient(timeout=FETCH_TIMEOUT_SECONDS)

    async def stop(self):
        if self._client:
            await self._client.aclose()
            self._client = None

    def configured(self) -> bool:
        return bool(self._token)

    def _paths(self, code: str, size: int) -> tuple[str, str]:
        base = os.path.join(self._dir, f"{code}-{size}")
        return base + ".png", base + ".miss"

    async def logo(self, code: str, size: int = DEFAULT_SIZE) -> bytes | None:
        """The PNG for an airline code, or None if there is not one.

        None covers both "logo.dev has no logo for this airline" and "no
        token is configured", because the caller does the same thing in
        either case: draw its own badge.
        """
        if not CODE_PATTERN.match(code) or code not in AIRLINE_DOMAINS:
            return None
        if size not in ALLOWED_SIZES:
            size = DEFAULT_SIZE
        image_path, miss_path = self._paths(code, size)

        cached = await asyncio.to_thread(_read_if_present, image_path)
        if cached is not None:
            self.hits += 1
            return cached
        if await asyncio.to_thread(_miss_is_fresh, miss_path, MISS_TTL_SECONDS):
            self.misses += 1
            return None
        if not self._token or self._client is None:
            return None

        lock = self._locks.setdefault(code + str(size), asyncio.Lock())
        async with lock:
            # Another request may have filled it while this one waited.
            cached = await asyncio.to_thread(_read_if_present, image_path)
            if cached is not None:
                self.hits += 1
                return cached
            return await self._fetch(code, size, image_path, miss_path)

    async def _fetch(self, code: str, size: int, image_path: str, miss_path: str) -> bytes | None:
        domain = AIRLINE_DOMAINS[code]
        self.fetches += 1
        try:
            response = await self._client.get(
                LOGO_URL.format(domain=domain),
                params={
                    "token": self._token,
                    "size": size,
                    "format": "png",
                    # The real 404 this whole design depends on - see the
                    # module docstring.
                    "fallback": "404",
                },
            )
        except httpx.HTTPError as exc:
            # Not cached as a miss: a network blip is not "this airline has
            # no logo", and recording it as one would hide a real logo for a
            # week.
            logger.debug("logo %s (%s): %s", code, domain, exc)
            return None
        if response.status_code == 404:
            await asyncio.to_thread(_write_miss, miss_path)
            return None
        if response.status_code != 200:
            logger.debug("logo %s (%s): HTTP %s", code, domain, response.status_code)
            return None
        data = response.content
        if not data or len(data) > MAX_LOGO_BYTES or not data.startswith(b"\x89PNG"):
            # Whatever this is, it is not the PNG that was asked for; do not
            # put it in the cache where it would be served to every viewer.
            logger.warning("logo %s (%s): unexpected %d byte response", code, domain, len(data))
            return None
        await asyncio.to_thread(_write_atomically, image_path, data)
        logger.info("logo %s cached from %s (%d bytes)", code, domain, len(data))
        return data

    def stats(self) -> dict:
        return {
            "configured": self.configured(),
            "known_airlines": len(AIRLINE_DOMAINS),
            "hits": self.hits,
            "fetches": self.fetches,
            "misses": self.misses,
        }


def _read_if_present(path: str) -> bytes | None:
    try:
        with open(path, "rb") as handle:
            return handle.read()
    except OSError:
        return None


def _miss_is_fresh(path: str, ttl: float) -> bool:
    try:
        return (time.time() - os.path.getmtime(path)) < ttl
    except OSError:
        return False


def _write_miss(path: str):
    try:
        with open(path, "wb"):
            pass
        os.utime(path, None)
    except OSError as exc:
        logger.debug("could not record logo miss %s: %s", path, exc)


def _write_atomically(path: str, data: bytes):
    """Via a temporary file in the same directory, so a reader never sees a
    half-written PNG and a crash mid-write leaves no truncated cache entry."""
    temporary = path + ".part"
    try:
        with open(temporary, "wb") as handle:
            handle.write(data)
        os.replace(temporary, path)
    except OSError as exc:
        logger.warning("could not cache logo %s: %s", path, exc)
        try:
            os.unlink(temporary)
        except OSError:
            pass
