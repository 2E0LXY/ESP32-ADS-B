"""Offline reference data: operators, aircraft types, countries, liveries.

The feeds identify an aircraft by ICAO hex, callsign and type designator and
leave it at that. Several of them send no operator name, no country and no
model. These lists fill that in, on the server, once per aircraft per poll -
so the device shows "BOEING 737-800, RYANAIR" without carrying 450 KB of
lookup tables or doing a single extra request.

The silhouette matters most. The firmware guesses an aircraft's shape from 91
hand-written designator prefixes, which is why anything outside those 91 read
as a generic dart. ICAOList.csv gives the real class and engine configuration
for 2,767 designators, so the shape can be derived properly and sent with the
aircraft. The device keeps its prefix table as the fallback for anyone using a
different provider.

Provenance is unresolved (see reference/README.md), which is why this is
server-side only and nothing here is compiled into a released firmware binary
or shipped in the USB installer.
"""

import csv
import logging
import os

logger = logging.getLogger("reference")

REFERENCE_DIR = os.environ.get("REFERENCE_DIR", "reference")

# Silhouette names, matching the firmware's planeShapeName() and the browser's
# PLANE_SHAPES exactly. A name either side does not recognise draws nothing.
SHAPE_HEAVY = "heavy"
SHAPE_AIRLINER = "airliner"
SHAPE_TWIN = "twin"
SHAPE_LIGHT = "light"
SHAPE_FIGHTER = "fighter"
SHAPE_HELICOPTER = "helicopter"
SHAPE_GLIDER = "glider"
SHAPE_BALLOON = "balloon"
SHAPE_DRONE = "drone"

# Classes that are not aeroplanes however many engines they have. Gyrocopters
# and tiltrotors get the rotorcraft outline because it is much closer than any
# fixed-wing one, and neither has a shape of its own.
ROTORCRAFT_CLASSES = {"helicopter", "gyrocopter", "tiltrotor"}


def _normalise_class(value: str) -> str:
    """ICAOList.csv spells LandPlane four different ways, including
    'Landplace' and 'Landplne'. Fold case and let the typos through rather
    than dropping 29 aircraft over spelling."""
    return (value or "").strip().lower()


def shape_for_type(type_class: str, engines: str) -> str | None:
    """The silhouette for a class and engine configuration.

    None means "nothing better than the caller already has": the row carried
    no engine data, so the device's own category-based guess stands.
    """
    folded = _normalise_class(type_class)
    if folded in ROTORCRAFT_CLASSES:
        return SHAPE_HELICOPTER

    # "2/Turboprop/Turboshaft", "4/Jet", "0/Glider", or "/" when unknown.
    count_text, _, kind = (engines or "").partition("/")
    kind = kind.strip().lower()
    if not kind:
        return None
    try:
        count = int(count_text)
    except ValueError:
        count = 0

    if "glider" in kind:
        return SHAPE_GLIDER
    if "jet" in kind or "rocket" in kind:
        # No weight in this data, so a 2-jet business jet and a 737 both land
        # on "airliner". The ADS-B emitter category carries weight and the
        # firmware still uses it where the type is unknown; refining this
        # would need a weight column these lists do not have.
        if count >= 4:
            return SHAPE_HEAVY
        if count >= 2:
            return SHAPE_AIRLINER
        return SHAPE_FIGHTER
    if "turboprop" in kind or "turboshaft" in kind:
        # A single turboprop is a PC-12 or a TBM, which reads as a light
        # aircraft rather than a regional twin.
        return SHAPE_TWIN if count >= 2 else SHAPE_LIGHT
    if "piston" in kind:
        return SHAPE_TWIN if count >= 2 else SHAPE_LIGHT
    if "electric" in kind:
        return SHAPE_LIGHT
    return None


def _titlecase(value: str) -> str:
    """These lists are shouted: "RYANAIR DAC", "BOEING, 737-800". Mixed case
    reads better in the browser, and the firmware upper-cases for the LCD
    itself anyway.

    Words with no vowel, or of three characters or fewer, are left alone.
    Plain capitalisation turned KLM into "Klm" and DAC into "Dac", which is
    worse than leaving them shouted.
    """
    words = []
    for word in (value or "").strip().split():
        if not word.isupper() or len(word) <= 3 or not any(v in word for v in "AEIOU"):
            words.append(word)
        else:
            words.append(word.capitalize())
    return " ".join(words)


# Designators that carry no class or engine data but are perfectly
# meaningful, and which feeds do send. ICAOList.csv files them all as class
# "0" with no engines, so a blanket "skip the blank rows" rule threw away
# eleven useful mappings along with the one genuinely empty row.
#
# PARA takes the glider outline and SHIP the balloon one as the nearest
# available; there is no paraglider or airship silhouette.
GENERIC_DESIGNATOR_SHAPES = {
    "GLID": SHAPE_GLIDER,
    "PARA": SHAPE_GLIDER,
    "BALL": SHAPE_BALLOON,
    "SHIP": SHAPE_BALLOON,
    "GYRO": SHAPE_HELICOPTER,
    "UHEL": SHAPE_HELICOPTER,
    "DRON": SHAPE_DRONE,
    "UAV": SHAPE_DRONE,
    "FFLO": SHAPE_DRONE,
    "VFHC": SHAPE_DRONE,
    "ULAC": SHAPE_LIGHT,
}


def _is_placeholder_type(designator: str, type_class: str, engines: str) -> bool:
    """True for a row that can never usefully describe an aircraft.

    ZZZZ is ICAO's "type not yet assigned a designator", and would otherwise
    be reported as an aircraft's model. The ZZZZ- prefixed rows are the same
    thing with a model attached, and cannot match a designator a feed sends.
    """
    if designator == "ZZZZ" or designator.startswith("ZZZZ-"):
        return True
    if designator in GENERIC_DESIGNATOR_SHAPES:
        return False
    return _normalise_class(type_class) in ("", "0") and (engines or "").strip() in ("", "/")


def _clean_iata(value: str | None) -> str:
    code = (value or "").strip().upper()
    return code if code.isalnum() and len(code) == 2 else ""


class ReferenceData:
    def __init__(self, directory: str = REFERENCE_DIR):
        self._dir = directory
        self.airlines: dict[str, dict] = {}
        self.types: dict[str, dict] = {}
        self.type_aliases: dict[str, str] = {}
        self.hex_ranges: list[tuple[int, int, str]] = []
        self.liveries: dict[str, dict] = {}
        self.military: dict[str, str] = {}
        self.loaded = False

    def _path(self, name: str) -> str:
        return os.path.join(self._dir, name)

    def _rows(self, name: str):
        path = self._path(name)
        try:
            # utf-8-sig: several of these files carry a byte-order mark, which
            # would otherwise become part of the first column's name.
            with open(path, newline="", encoding="utf-8-sig") as handle:
                yield from csv.DictReader(handle)
        except OSError as exc:
            logger.warning("reference data %s unavailable: %s", name, exc)

    def load(self):
        """Reads every list. Synchronous and called once at startup; a
        missing or unreadable file degrades that one lookup rather than
        failing the service."""
        self._load_airlines()
        self._load_types()
        self._load_hex_ranges()
        self._load_liveries()
        self._load_military()
        self.loaded = True
        logger.info(
            "reference data: %d airlines, %d types (%d aliases), %d hex ranges, "
            "%d liveries, %d military operators",
            len(self.airlines), len(self.types), len(self.type_aliases),
            len(self.hex_ranges), len(self.liveries), len(self.military),
        )

    def _load_airlines(self):
        for row in self._rows("Airlines.csv"):
            code = (row.get("3Ltr") or "").strip().upper()
            if len(code) != 3:
                continue
            self.airlines[code] = {
                "name": _titlecase(row.get("Company", "")),
                "country": _titlecase(row.get("Country", "")),
                "telephony": _titlecase(row.get("Telephony", "")),
            }
        # ICAO.txt is the smaller but richer list - it carries IATA codes and
        # an active/defunct status - so it is applied second and wins where
        # the two disagree.
        for row in self._rows("ICAO.txt"):
            code = (row.get("ICAO") or "").strip().upper()
            if len(code) != 3:
                continue
            entry = self.airlines.setdefault(code, {})
            entry.update({
                "name": (row.get("Airline Name") or entry.get("name") or "").strip(),
                "country": (row.get("Country") or entry.get("country") or "").strip(),
                "telephony": _titlecase(row.get("Callsign") or entry.get("telephony") or ""),
                # "---" is this list's way of saying an operator has no
                # IATA code, and it is not one.
                "iata": _clean_iata(row.get("IATA")),
                "status": (row.get("Status") or "").strip(),
            })

    def _load_types(self):
        for row in self._rows("ICAOList.csv"):
            designator = (row.get("Aircraft TypeDesignator") or "").strip().upper()
            if not designator:
                continue
            manufacturer_model = (row.get("MANUFACTURER, Model") or "").strip()
            manufacturer, _, model = manufacturer_model.partition(",")
            type_class = (row.get("Class") or "").strip()
            engines = (row.get("Number+Engine Type") or "").strip()
            if _is_placeholder_type(designator, type_class, engines):
                continue
            self.types[designator] = {
                "class": type_class,
                "engines": engines,
                "manufacturer": _titlecase(manufacturer),
                "model": model.strip(),
                "name": _titlecase(f"{manufacturer} {model}".replace(",", " ")),
                # The generic designators have no class or engine data to
                # derive from, so their meaning is stated outright.
                "shape": GENERIC_DESIGNATOR_SHAPES.get(designator)
                         or shape_for_type(type_class, engines),
            }
        # Retired designators, so an older feed sending CL61 still resolves.
        for row in self._rows("ICAOTypeConversion.csv"):
            old = (row.get("OldICAOTypeCode") or "").strip().upper()
            new = (row.get("NewICAOTypeCode") or "").strip().upper()
            if old and new and old != new:
                self.type_aliases[old] = new

    def _load_hex_ranges(self):
        # Headerless: start,end,country,(trailing blank)
        path = self._path("ICAOHexRange.csv")
        try:
            with open(path, newline="", encoding="utf-8-sig") as handle:
                for fields in csv.reader(handle):
                    if len(fields) < 3:
                        continue
                    try:
                        start = int(fields[0].strip(), 16)
                        end = int(fields[1].strip(), 16)
                    except ValueError:
                        continue  # a header or a comment line
                    country = fields[2].strip()
                    if not country or country.startswith("("):
                        continue  # "(unallocated)" is not an answer
                    self.hex_ranges.append((start, end, country))
        except OSError as exc:
            logger.warning("reference data ICAOHexRange.csv unavailable: %s", exc)
            return
        self.hex_ranges.sort()

    def _load_liveries(self):
        for row in self._rows("MixedColourSchemes.csv"):
            registration = (row.get("Registration") or "").strip().upper()
            if not registration:
                continue
            self.liveries[registration] = {
                "scheme": _titlecase(row.get("Scheme", "")),
                "operator": _titlecase(row.get("RegisteredOwners", "")),
            }

    def _load_military(self):
        for row in self._rows("MilICAOOperatorLookUp.csv"):
            owner = (row.get("RegisteredOwner") or "").strip()
            code = (row.get("ICAOOperatorCode") or "").strip().upper()
            if owner and code:  # many rows have no code yet
                self.military[code] = owner

    # --- lookups ---------------------------------------------------------

    def airline_for_callsign(self, callsign: str | None) -> dict | None:
        """Callsigns are the operator's ICAO code plus a flight number."""
        name = (callsign or "").strip().upper()
        if len(name) < 4:
            return None  # a bare code is not a flight
        return self.airlines.get(name[:3])

    def aircraft_type(self, designator: str | None) -> dict | None:
        code = (designator or "").strip().upper()
        if not code:
            return None
        return self.types.get(self.type_aliases.get(code, code))

    def country_for_hex(self, hex_id: str | None) -> str | None:
        try:
            address = int((hex_id or "").strip(), 16)
        except ValueError:
            return None
        # Binary search would be faster, but 199 ranges against a few hundred
        # aircraft is nothing next to the JSON encoding of the same response.
        for start, end, country in self.hex_ranges:
            if start <= address <= end:
                return country
            if start > address:
                break  # sorted, so nothing later can match
        return None

    def enrich(self, aircraft: dict) -> dict:
        """Fills in what the feed left out. Never overwrites a value the feed
        actually sent - an upstream that knows the registered operator knows
        better than a lookup by callsign prefix."""
        extra = {}

        airline = self.airline_for_callsign(aircraft.get("flight"))
        if airline:
            if not aircraft.get("ownOp") and airline.get("name"):
                extra["ownOp"] = airline["name"]
            if airline.get("telephony"):
                extra["telephony"] = airline["telephony"]
            if airline.get("iata"):
                extra["operator_iata"] = airline["iata"]

        type_info = self.aircraft_type(aircraft.get("t"))
        if type_info:
            if type_info.get("name"):
                extra["type_name"] = type_info["name"]
            if type_info.get("class"):
                extra["type_class"] = type_info["class"]
            if type_info.get("engines"):
                extra["type_engines"] = type_info["engines"]
            # The whole point: a silhouette derived from real class and engine
            # data rather than guessed from a callsign prefix.
            if type_info.get("shape"):
                extra["shape"] = type_info["shape"]

        if not aircraft.get("cou"):
            country = self.country_for_hex(aircraft.get("hex"))
            if country:
                extra["cou"] = country

        livery = self.liveries.get((aircraft.get("r") or "").strip().upper())
        if livery and livery.get("scheme"):
            extra["livery"] = livery["scheme"]

        military = self.military.get((aircraft.get("flight") or "").strip().upper()[:3])
        if military and not extra.get("ownOp") and not aircraft.get("ownOp"):
            extra["ownOp"] = military

        return {**aircraft, **extra} if extra else aircraft

    def stats(self) -> dict:
        return {
            "loaded": self.loaded,
            "airlines": len(self.airlines),
            "types": len(self.types),
            "type_aliases": len(self.type_aliases),
            "hex_ranges": len(self.hex_ranges),
            "liveries": len(self.liveries),
            "military_operators": len(self.military),
        }
