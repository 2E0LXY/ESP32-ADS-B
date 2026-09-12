"""Aircraft type photographs, licence-free, cached on this deployment's disk.

A silhouette says what class an aircraft is. A photograph of the actual type
says rather more, and the screensaver has room for one.

**Only CC0 and Public Domain Mark.** That is the whole reason this uses
Openverse rather than Wikimedia Commons. Commons has excellent aircraft
photography, but civil aircraft photos there are effectively all CC BY-SA -
measured across A320, B738, B38M, C172, AT76 and SR22, none of which had an
attribution-free option. Only military and government photographs are public
domain, which is not much use for a departure board. Openverse aggregates
Flickr and others and lets the search itself be restricted by licence, and
CC0 and PDM waive attribution entirely: no credits page, no share-alike
question, nothing to carry.

The licence is nonetheless recorded next to every cached image, along with
the source URL and title. Attribution is not required, but being unable to
say where a picture came from is its own problem.

Searching by model name returns engine close-ups, cabins, museum pieces and
cockpit shots alongside whole aircraft, so the candidate filtering below is
the substance of this module rather than the fetching.
"""

import asyncio
import io
import json
import logging
import os
import re

import httpx

logger = logging.getLogger("photos")

OPENVERSE_URL = "https://api.openverse.org/v1/images/"
# Both waive attribution. Nothing else is acceptable here - see the module
# docstring; adding "by" or "by-sa" would quietly create an obligation this
# project has no way to honour on a 5x7 pixel font.
FREE_LICENCES = "cc0,pdm"
USER_AGENT = "2E0LXY-ADSB-Aggregator/1.0 (+https://github.com/2E0LXY/ESP32-ADS-B)"

CACHE_DIR = os.environ.get("PHOTO_CACHE_DIR", "/data/photos")
PHOTOS_ENABLED = os.environ.get("AIRCRAFT_PHOTOS", "1") != "0"
# The panel draws these in a landscape band, so one shape at a few widths.
ALLOWED_WIDTHS = (160, 240, 320)
DEFAULT_WIDTH = 240
PHOTO_ASPECT = 5 / 3  # width / height of the cached crop
MISS_TTL_SECONDS = 30 * 24 * 60 * 60  # a type with no free photo rarely gains one
SEARCH_TIMEOUT_SECONDS = 15
DOWNLOAD_TIMEOUT_SECONDS = 20
MAX_SOURCE_BYTES = 8 * 1024 * 1024
CANDIDATES = 20

DESIGNATOR_PATTERN = re.compile(r"^[A-Z0-9]{2,4}$")

# Words that mean the photograph is of a part, an interior or a model rather
# than an aircraft in view. Checked against the title, which on Flickr is
# usually descriptive.
REJECT_WORDS = (
    "engine", "nacelle", "cockpit", "cabin", "interior", "seat", "galley",
    "landing gear", "wheel", "tyre", "tire", "winglet", "wingtip", "tail fin",
    "logo", "livery detail", "model", "diecast", "lego", "simulator", "sim ",
    "museum", "wreck", "crash", "scrap", "poster", "screenshot", "map",
    "diagram", "drawing", "blueprint", "patch", "badge", "ticket", "timetable",
)


def _is_plausible_photo(result: dict, model: str) -> tuple[bool, str]:
    """Whether a search result looks like a photograph of the whole aircraft.

    Returns the reason on rejection so a poor choice can be explained rather
    than silently made.
    """
    title = str(result.get("title") or "").lower()
    if not title:
        return False, "no title"
    for word in REJECT_WORDS:
        if word in title:
            return False, f"title mentions {word.strip()!r}"

    width = result.get("width") or 0
    height = result.get("height") or 0
    if not width or not height:
        return False, "no dimensions"
    if width < 480:
        return False, f"too small ({width}px wide)"
    ratio = width / height
    # Aircraft in flight or on a stand are photographed landscape. A portrait
    # or square frame is usually a detail, a tail, or a person by an aeroplane.
    if not 1.2 <= ratio <= 2.4:
        return False, f"aspect {ratio:.2f} is not landscape"

    # The model name should actually appear. Openverse full-text search is
    # generous, and "737" matches an article about an airport that mentions
    # one. Require the significant words.
    significant = [w for w in re.split(r"[\s\-]+", model.lower()) if len(w) > 2]
    if significant and not any(w in title for w in significant):
        return False, "title does not mention the model"
    return True, ""


class PhotoStore:
    def __init__(self, cache_dir: str = CACHE_DIR, enabled: bool = PHOTOS_ENABLED):
        self._dir = cache_dir
        self._enabled = enabled
        self._client: httpx.AsyncClient | None = None
        self._locks: dict[str, asyncio.Lock] = {}
        self.hits = 0
        self.fetches = 0
        self.misses = 0
        self.rejected = 0

    def start(self):
        os.makedirs(self._dir, exist_ok=True)
        self._client = httpx.AsyncClient(
            timeout=DOWNLOAD_TIMEOUT_SECONDS, headers={"User-Agent": USER_AGENT},
            follow_redirects=True,
        )

    async def stop(self):
        if self._client:
            await self._client.aclose()
            self._client = None

    def configured(self) -> bool:
        return self._enabled

    def _paths(self, designator: str, width: int) -> tuple[str, str, str]:
        base = os.path.join(self._dir, f"{designator}-{width}")
        return base + ".png", base + ".json", base + ".miss"

    async def photo(self, designator: str, model: str, width: int = DEFAULT_WIDTH) -> bytes | None:
        """The cached photo for a type, fetching one if there is not yet one.

        None means there is no free photograph of this type, or photos are
        switched off. The caller draws its silhouette instead, which is what
        it did before.
        """
        code = (designator or "").strip().upper()
        if not self._enabled or not DESIGNATOR_PATTERN.match(code) or not model:
            return None
        if width not in ALLOWED_WIDTHS:
            width = DEFAULT_WIDTH
        image_path, _meta_path, miss_path = self._paths(code, width)

        cached = await asyncio.to_thread(_read_if_present, image_path)
        if cached is not None:
            self.hits += 1
            return cached
        if await asyncio.to_thread(_miss_is_fresh, miss_path, MISS_TTL_SECONDS):
            self.misses += 1
            return None
        if self._client is None:
            return None

        lock = self._locks.setdefault(f"{code}-{width}", asyncio.Lock())
        async with lock:
            cached = await asyncio.to_thread(_read_if_present, image_path)
            if cached is not None:
                self.hits += 1
                return cached
            return await self._fetch(code, model, width)

    async def _fetch(self, code: str, model: str, width: int) -> bytes | None:
        image_path, meta_path, miss_path = self._paths(code, width)
        self.fetches += 1
        try:
            response = await self._client.get(
                OPENVERSE_URL,
                params={"q": model, "license": FREE_LICENCES,
                        "page_size": CANDIDATES, "mature": "false"},
                timeout=SEARCH_TIMEOUT_SECONDS,
            )
            response.raise_for_status()
            results = (response.json() or {}).get("results") or []
        except (httpx.HTTPError, ValueError) as exc:
            # Not recorded as a miss: a search that failed is not a type
            # without a photograph, and caching it as one would hide a real
            # photo for a month.
            logger.debug("photo search %s (%s): %s", code, model, exc)
            return None

        for result in results:
            ok, reason = _is_plausible_photo(result, model)
            if not ok:
                self.rejected += 1
                logger.debug("photo %s rejected %r: %s", code, result.get("title"), reason)
                continue
            url = result.get("url")
            if not url:
                continue
            data = await self._download_and_crop(url, width)
            if data is None:
                continue
            await asyncio.to_thread(_write_atomically, image_path, data)
            await asyncio.to_thread(_write_json, meta_path, {
                # Attribution is not required by CC0 or PDM, but not being
                # able to say where an image came from is its own problem.
                "designator": code,
                "model": model,
                "title": result.get("title"),
                "licence": result.get("license"),
                "licence_version": result.get("license_version"),
                "source": result.get("source"),
                "source_url": result.get("foreign_landing_url") or url,
                "creator": result.get("creator"),
            })
            logger.info("photo %s cached from %s (%s, %d bytes)",
                        code, result.get("source"), result.get("license"), len(data))
            return data

        await asyncio.to_thread(_write_miss, miss_path)
        logger.info("photo %s: no attribution-free photograph found for %r", code, model)
        return None

    async def _download_and_crop(self, url: str, width: int) -> bytes | None:
        try:
            response = await self._client.get(url)
            response.raise_for_status()
        except httpx.HTTPError as exc:
            logger.debug("photo download %s: %s", url, exc)
            return None
        raw = response.content
        if not raw or len(raw) > MAX_SOURCE_BYTES:
            return None
        # Pillow work is CPU-bound and this runs on the event loop the live
        # feeder listeners share, so it goes to a thread like everything else.
        return await asyncio.to_thread(_to_panel_png, raw, width)

    def credits(self) -> list[dict]:
        """Every cached photo's source. Not a licence requirement; simply
        being able to answer "where did that picture come from"."""
        out = []
        try:
            names = sorted(os.listdir(self._dir))
        except OSError:
            return out
        for name in names:
            if not name.endswith(".json"):
                continue
            try:
                with open(os.path.join(self._dir, name), encoding="utf-8") as handle:
                    out.append(json.load(handle))
            except (OSError, ValueError):
                continue
        return out

    def stats(self) -> dict:
        return {
            "enabled": self._enabled,
            "hits": self.hits,
            "fetches": self.fetches,
            "misses": self.misses,
            "rejected_candidates": self.rejected,
        }


def _to_panel_png(raw: bytes, width: int) -> bytes | None:
    """Centre-crop to the panel's band shape, scale, and re-encode as PNG.

    PNG rather than JPEG because the ESP32 already has PNGdec and decoding
    it is a solved, proven path on that board; adding a JPEG decoder to the
    firmware to save a few kilobytes of transfer is not a trade worth making.
    """
    from PIL import Image

    try:
        with Image.open(io.BytesIO(raw)) as image:
            image = image.convert("RGB")
            target_height = max(1, int(round(width / PHOTO_ASPECT)))
            source_ratio = image.width / image.height
            if source_ratio > PHOTO_ASPECT:
                # Wider than the band: trim the sides, keeping the middle,
                # which is where an aircraft in a well-framed photo sits.
                crop_width = int(round(image.height * PHOTO_ASPECT))
                left = (image.width - crop_width) // 2
                image = image.crop((left, 0, left + crop_width, image.height))
            else:
                crop_height = int(round(image.width / PHOTO_ASPECT))
                top = (image.height - crop_height) // 2
                image = image.crop((0, top, image.width, top + crop_height))
            image = image.resize((width, target_height), Image.LANCZOS)
            # Quantised to a 256-colour adaptive palette before encoding.
            # PNG is a poor container for a photograph - a truecolour 240x144
            # frame came out at 61 KB - and the panel is RGB565, so it cannot
            # show more than 65,536 colours anyway and at this size shows far
            # fewer distinctly. PNG rather than JPEG because PNGdec is
            # already proven on this board and adding a JPEG decoder to the
            # firmware to save transfer is not a trade worth making.
            image = image.convert("P", palette=Image.ADAPTIVE, colors=256)
            out = io.BytesIO()
            image.save(out, format="PNG", optimize=True)
            return out.getvalue()
    except Exception as exc:  # noqa: BLE001 - any decode failure is just a skip
        logger.debug("photo conversion failed: %s", exc)
        return None


def _read_if_present(path: str) -> bytes | None:
    try:
        with open(path, "rb") as handle:
            return handle.read()
    except OSError:
        return None


def _miss_is_fresh(path: str, ttl: float) -> bool:
    import time
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
        logger.debug("could not record photo miss %s: %s", path, exc)


def _write_json(path: str, payload: dict):
    try:
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=1)
    except OSError as exc:
        logger.debug("could not record photo provenance %s: %s", path, exc)


def _write_atomically(path: str, data: bytes):
    temporary = path + ".part"
    try:
        with open(temporary, "wb") as handle:
            handle.write(data)
        os.replace(temporary, path)
    except OSError as exc:
        logger.warning("could not cache photo %s: %s", path, exc)
        try:
            os.unlink(temporary)
        except OSError:
            pass
