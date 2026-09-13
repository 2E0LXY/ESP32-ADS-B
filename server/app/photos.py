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

**Nothing here ever makes a caller wait.** A type with no cached photograph
is queued and answered "not yet"; the picture appears on a later request.
The first version did the search, the download and the crop while the device
held the connection open, which took longer than the ESP32's fifteen-second
read timeout and failed with HTTPC_ERROR_READ_TIMEOUT - so the device never
got a photograph and the work was thrown away each time. Same reasoning, and
the same shape, as the route resolver in app/routes.py.
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
# The default for a fresh deployment; the admin panel owns it after that
# (see app/runtime_settings.py), which is why PhotoStore.enabled() exists
# rather than every caller reading this constant.
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
# One at a time. These are other people's servers and a photograph is never
# urgent - the device asks again on its next rotation.
WORKERS = 1
# Bounded so a sky full of unphotographed types cannot grow without limit.
MAX_QUEUE = 200

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


def _is_plausible_photo(result: dict, model: str,
                        operator_required: tuple[str, ...] = ()) -> tuple[bool, str]:
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
    # For an operator-specific photograph the airline has to be named in the
    # title, and every word of it. Searching "Jet2 Boeing 737-800" happily
    # returns a Ryanair 737 - the search is full-text and generous - and
    # caching that as Jet2's picture would reproduce the fault this is meant
    # to fix, with the extra insult of having asked for the right thing.
    for term in operator_required:
        if term not in title:
            return False, f"title does not mention {term!r}"
    return True, ""


# Words in an airline's registered name that are no help in a photo search
# and would wrongly reject a good picture if required in its title. "Jet2.com"
# is titled "Jet2" on a photograph, and "easyJet UK" is just "easyJet".
OPERATOR_NOISE = {"ltd", "limited", "inc", "plc", "llc", "uk", "com", "the"}
OPERATOR_PATTERN = re.compile(r"^[A-Z0-9]{2,4}$")


def operator_terms(airline_name: str | None) -> list[str]:
    """The words of an airline name worth searching and matching on.

    Conservative on purpose: it drops a trailing ".com", anything in
    brackets, and the qualifiers above, and keeps everything else. Trimming
    harder would turn "Air France" into "France" and "British Airways" into
    "British", and match the wrong carrier's aeroplane - which is the exact
    fault this whole path exists to fix.
    """
    name = (airline_name or "").strip()
    if not name:
        return []
    name = re.sub(r"\(.*?\)", " ", name)
    name = re.sub(r"\.com\b", " ", name, flags=re.IGNORECASE)
    words = [w for w in re.split(r"[\s/,]+", name.lower()) if len(w) > 2]
    return [w for w in words if w not in OPERATOR_NOISE]


class PhotoStore:
    def __init__(self, cache_dir: str = CACHE_DIR, enabled: bool = PHOTOS_ENABLED,
                 settings=None):
        self._dir = cache_dir
        self._enabled = enabled
        # Switchable from the admin panel without a restart. The
        # constructor argument stays the default and what the tests use.
        self._settings = settings
        self._client: httpx.AsyncClient | None = None
        # A search that fails is not cached as a miss, so a systemic failure
        # - the host unreachable from this deployment, say - retries for ever
        # and silently returns 404 to every caller. Counted and reported so
        # that condition is visible in the log instead of invisible at DEBUG.
        self.search_failures = 0
        self.hits = 0
        self.fetches = 0
        self.misses = 0
        self.rejected = 0
        self.queued_now = 0
        # Photographs served that are of the right operator, not just the
        # right type. The whole point of the operator search, so worth
        # being able to see it working rather than inferring it.
        self.operator_hits = 0
        self._queue: asyncio.Queue[tuple[str, str, int, str, str]] | None = None
        self._queued: set[str] = set()
        self._workers: list[asyncio.Task] = []

    def start(self):
        os.makedirs(self._dir, exist_ok=True)
        # Whatever client is already set is kept. In the deployment there
        # never is one, so this reads as "create it"; a second start() then
        # cannot orphan the first client with its connections still open,
        # and a test that injected a fake upstream keeps it instead of
        # silently having its traffic sent to the real Openverse.
        if self._client is None:
            self._client = httpx.AsyncClient(
                timeout=DOWNLOAD_TIMEOUT_SECONDS, headers={"User-Agent": USER_AGENT},
                follow_redirects=True,
            )
        self._queue = asyncio.Queue(maxsize=MAX_QUEUE)
        self._workers = [asyncio.create_task(self._worker()) for _ in range(WORKERS)]

    async def stop(self):
        for task in self._workers:
            task.cancel()
        for task in self._workers:
            try:
                await task
            except asyncio.CancelledError:
                pass
        self._workers.clear()
        if self._client:
            await self._client.aclose()
            self._client = None

    async def _worker(self):
        while True:
            code, model, width, operator, airline_name = await self._queue.get()
            try:
                await self._fetch(code, model, width, operator, airline_name)
            except asyncio.CancelledError:
                raise
            except Exception:
                logger.exception("photo fetch failed for %s", code)
            finally:
                self._queued.discard(f"{code}@{operator}-{width}")
                self.queued_now = len(self._queued)
                self._queue.task_done()

    def _enqueue(self, code: str, model: str, width: int,
                 operator: str = "", airline_name: str = ""):
        key = f"{code}@{operator}-{width}"
        if self._queue is None or key in self._queued:
            return
        try:
            self._queue.put_nowait((code, model, width, operator, airline_name))
        except asyncio.QueueFull:
            return
        self._queued.add(key)
        self.queued_now = len(self._queued)

    def configured(self) -> bool:
        if self._settings is not None:
            return bool(self._settings.get("aircraft_photos"))
        return self._enabled

    def _paths(self, designator: str, width: int,
               operator: str = "") -> tuple[str, str, str]:
        # An operator's own photograph is a separate cache entry, so the
        # generic one stays available as the fallback and neither overwrites
        # the other.
        name = f"{designator}@{operator}-{width}" if operator else f"{designator}-{width}"
        base = os.path.join(self._dir, name)
        return base + ".png", base + ".json", base + ".miss"

    async def photo(self, designator: str, model: str, width: int = DEFAULT_WIDTH) -> bytes | None:
        """The cached photo for a type, or None. See resolve() for why None
        is not one answer but three."""
        data, _state, _match = await self.resolve(designator, model, width)
        return data

    async def resolve(self, designator: str, model: str,
                      width: int = DEFAULT_WIDTH, airline: str | None = None,
                      airline_name: str | None = None
                      ) -> tuple[bytes | None, str, str]:
        """The photo and, when there is not one, WHY there is not one.

        None used to mean any of three different things, and the endpoint
        turned all of them into a 404. The firmware treats a 404 as final -
        it writes a marker file and never asks again - so the first request
        for any type, which is necessarily "queued, not fetched yet",
        permanently blacklisted that type on that receiver. The photo the
        queue then fetched seconds later was never asked for again, which is
        why a device could run for hours reporting "none available" for
        everything while the server held the pictures.

        So the three cases are now distinguishable:

          ready    the bytes, cached on disk
          missing  searched, and nothing attribution-free exists - the one
                   case a caller may remember
          pending  queued; ask again shortly
          off      switched off deployment-wide, or not a designator

        Also says WHICH photograph it is, because one photograph per type
        means a Jet2 737-800 and a Ryanair 737-800 share a picture, and
        whichever livery the search found is the one every operator of that
        type gets shown. So when an airline is named, its own photograph is
        looked for first and the generic one is the fallback - and the third
        return value is "airline" or "type" so the caller can say which,
        rather than presenting someone else's livery as this aircraft.
        """
        code = (designator or "").strip().upper()
        if not self.configured() or not DESIGNATOR_PATTERN.match(code) or not model:
            return None, "off", "type"
        if width not in ALLOWED_WIDTHS:
            width = DEFAULT_WIDTH

        operator = (airline or "").strip().upper()
        terms = operator_terms(airline_name)
        if operator and terms and OPERATOR_PATTERN.match(operator):
            operator_image, _meta, operator_miss = self._paths(code, width, operator)
            operator_cached = await asyncio.to_thread(_read_if_present, operator_image)
            if operator_cached is not None:
                self.hits += 1
                self.operator_hits += 1
                return operator_cached, "ready", "airline"
            # Nothing yet. Queue the operator search, then fall through and
            # serve the generic photograph if there is one: a picture of the
            # right type now beats no picture at all, and the operator's own
            # replaces it on a later poll once it arrives. A search that
            # already came back empty is not repeated - most airline-and-type
            # pairs simply have no attribution-free photograph.
            if self._client is not None and not await asyncio.to_thread(
                    _miss_is_fresh, operator_miss, MISS_TTL_SECONDS):
                self._enqueue(code, model, width, operator, airline_name or "")

        image_path, _meta_path, miss_path = self._paths(code, width)
        cached = await asyncio.to_thread(_read_if_present, image_path)
        if cached is not None:
            self.hits += 1
            return cached, "ready", "type"
        if await asyncio.to_thread(_miss_is_fresh, miss_path, MISS_TTL_SECONDS):
            self.misses += 1
            return None, "missing", "type"
        if self._client is None:
            return None, "off", "type"

        # Queued, not fetched here: a caller must never wait on a search and
        # a download. The device asks again on its next rotation and gets the
        # picture then, which is exactly how route lookups behave.
        self._enqueue(code, model, width)
        return None, "pending", "type"

    async def _fetch(self, code: str, model: str, width: int,
                     operator: str = "", airline_name: str = "") -> bytes | None:
        image_path, meta_path, miss_path = self._paths(code, width, operator)
        terms = tuple(operator_terms(airline_name)) if operator else ()
        # "Jet2 Boeing 737-800" rather than "Boeing 737-800". The airline
        # words also become a requirement on the title - see the note in
        # _is_plausible_photo - so a generic 737 coming back from this
        # search is rejected rather than cached as Jet2's.
        query = f"{' '.join(terms)} {model}" if terms else model
        self.fetches += 1
        try:
            response = await self._client.get(
                OPENVERSE_URL,
                params={"q": query, "license": FREE_LICENCES,
                        "page_size": CANDIDATES, "mature": "false"},
                timeout=SEARCH_TIMEOUT_SECONDS,
            )
            response.raise_for_status()
            results = (response.json() or {}).get("results") or []
        except (httpx.HTTPError, ValueError) as exc:
            # Not recorded as a miss: a search that failed is not a type
            # without a photograph, and caching it as one would hide a real
            # photo for a month. But it does need saying out loud - the first
            # few times, then occasionally, so a permanent failure is
            # obvious without one line per request.
            self.search_failures += 1
            if self.search_failures <= 3 or self.search_failures % 25 == 0:
                logger.warning("photo search failed for %s (%s) [%d so far]: %s: %s",
                               code, query, self.search_failures, type(exc).__name__, exc)
            return None

        for result in results:
            ok, reason = _is_plausible_photo(result, model, terms)
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
                "airline": operator or None,
                "title": result.get("title"),
                "licence": result.get("license"),
                "licence_version": result.get("license_version"),
                "source": result.get("source"),
                "source_url": result.get("foreign_landing_url") or url,
                "creator": result.get("creator"),
            })
            logger.info("photo %s%s cached from %s (%s, %d bytes)",
                        code, f" for {operator}" if operator else "",
                        result.get("source"), result.get("license"), len(data))
            return data

        await asyncio.to_thread(_write_miss, miss_path)
        # An operator miss is the common case, not a fault: most
        # airline-and-type pairs have no CC0 photograph, and the generic one
        # is still served. Logged at a lower level so a busy sky does not
        # fill the log with something entirely expected.
        if operator:
            logger.debug("photo %s for %s: nothing attribution-free, "
                         "the type photograph stands", code, operator)
        else:
            logger.info("photo %s: no attribution-free photograph found for %r",
                        code, query)
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
            "enabled": self.configured(),
            "hits": self.hits,
            "operator_hits": self.operator_hits,
            "fetches": self.fetches,
            "misses": self.misses,
            "rejected_candidates": self.rejected,
            "search_failures": self.search_failures,
            "queued": self.queued_now,
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
