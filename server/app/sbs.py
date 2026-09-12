"""Parser for the SBS-1/BaseStation text protocol.

This is what readsb, dump1090(-fa), and PiAware all speak on their "SBS
output" port (traditionally 30003) - the same format every established
feeder network (FlightAware, FlightRadar24, adsb.fi, ...) already accepts
feeds in, so building against it means a customer's existing feeder setup
can point an extra output at us with no new software.

Format (22 comma-separated fields, one message per line):
  MSG,<transmission type 1-8>,<session>,<aircraft>,<hex ident>,<flight id>,
  <date generated>,<time generated>,<date logged>,<time logged>,<callsign>,
  <altitude>,<ground speed>,<track>,<lat>,<lon>,<vertical rate>,<squawk>,
  <alert>,<emergency>,<spi>,<is on ground>

Different transmission types populate different subsets of the trailing
fields - a real decoder has to accumulate state per aircraft across several
message types (identification, position, velocity all arrive separately),
which is what AircraftState/StreamDecoder below do.
"""

import time
from dataclasses import dataclass, field


@dataclass
class AircraftState:
    hex: str
    flight: str | None = None
    lat: float | None = None
    lon: float | None = None
    alt_baro: object = None  # int, or the literal string "ground"
    gs: float | None = None
    track: float | None = None
    baro_rate: float | None = None
    squawk: str | None = None
    on_ground: bool = False
    updated_at: float = field(default_factory=time.time)

    def to_dict(self) -> dict:
        d = {
            "hex": self.hex,
            "seen": max(0.0, time.time() - self.updated_at),
            "messages": 1,
            "mlat": [],
        }
        if self.flight:
            d["flight"] = self.flight
        if self.lat is not None and self.lon is not None:
            d["lat"] = self.lat
            d["lon"] = self.lon
        if self.on_ground:
            d["alt_baro"] = "ground"
        elif self.alt_baro is not None:
            d["alt_baro"] = self.alt_baro
        if self.gs is not None:
            d["gs"] = self.gs
        if self.track is not None:
            d["track"] = self.track
        if self.baro_rate is not None:
            d["baro_rate"] = self.baro_rate
        if self.squawk:
            d["squawk"] = self.squawk
        return d

    def has_position(self) -> bool:
        return self.lat is not None and self.lon is not None


def _float(value: str) -> float | None:
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _int_or_ground(value: str) -> object:
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


class StreamDecoder:
    """One instance per feeder TCP connection. Feed it raw lines as they
    arrive; get back the updated AircraftState whenever a line yields a
    usable update (bad/short/non-MSG lines are silently ignored, matching
    how permissive real-world SBS consumers have to be - malformed lines
    from a feeder are routine, not exceptional)."""

    def __init__(self):
        self.aircraft: dict[str, AircraftState] = {}

    def feed_line(self, line: str) -> AircraftState | None:
        line = line.strip()
        if not line:
            return None
        fields = line.split(",")
        if len(fields) < 22 or fields[0] != "MSG":
            return None
        try:
            transmission_type = int(fields[1])
        except ValueError:
            return None
        hex_ident = fields[4].strip().lower()
        if not hex_ident:
            return None

        state = self.aircraft.get(hex_ident)
        if state is None:
            state = AircraftState(hex=hex_ident)
            self.aircraft[hex_ident] = state
        state.updated_at = time.time()

        callsign = fields[10].strip()
        if callsign:
            state.flight = callsign
        altitude = _int_or_ground(fields[11])
        if altitude is not None:
            state.alt_baro = altitude
        gs = _float(fields[12])
        if gs is not None:
            state.gs = gs
        track = _float(fields[13])
        if track is not None:
            state.track = track
        lat = _float(fields[14])
        lon = _float(fields[15])
        if lat is not None and lon is not None:
            state.lat = lat
            state.lon = lon
        vrate = _float(fields[16])
        if vrate is not None:
            state.baro_rate = vrate
        squawk = fields[17].strip()
        if squawk:
            state.squawk = squawk
        is_on_ground = fields[21].strip()
        if is_on_ground in ("-1", "1", "true", "True"):
            state.on_ground = True
        elif is_on_ground in ("0", "false", "False"):
            state.on_ground = False

        del transmission_type  # every type updates whichever fields it carries; no per-type branching needed
        return state

    def prune_older_than(self, seconds: float):
        cutoff = time.time() - seconds
        stale = [h for h, s in self.aircraft.items() if s.updated_at < cutoff]
        for h in stale:
            del self.aircraft[h]
