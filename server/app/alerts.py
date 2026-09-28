"""The aircraft worth looking up for.

Everything else in this service answers "what is in the sky". Almost all of
it is an airliner going where airliners go, and a display that treats a
Ryanair 737 and an aircraft squawking 7700 identically is asking the person
watching to spot the difference themselves.

Two kinds to begin with, both derived from data already in the cache, so
this costs no extra request to anyone:

* Emergency squawks. 7500 hijack, 7600 radio failure, 7700 general
  emergency - the three codes that mean something has gone wrong aboard.
  Some feeds also carry an explicit emergency field.
* Military. The reference lists already map callsign prefixes to their
  operators for the display; the same table answers "is this one of them".

Deliberately not persisted. An alert is interesting for as long as it is
happening and for a while afterwards, and writing them to SQLite would add
the one table that grows with events on a VPS where usage_log already had
to be taught to prune itself.
"""

import time

# The three codes that mean something is wrong aboard. Left as strings
# because that is how every source here sends them, and "7700" as an int
# would lose the leading zero on 0021-style codes elsewhere.
EMERGENCY_SQUAWKS = {
    "7500": "Hijack",
    "7600": "Radio failure",
    "7700": "General emergency",
}
# An emergency field is sent by some feeds; "none" is the common way of
# saying there isn't one.
NOT_AN_EMERGENCY = {"", "none", "no", "false", "0"}
# How long the same aircraft's same alert stays quiet after firing once. An
# aircraft squawking 7700 for twenty minutes is one event, not eighty.
REPEAT_AFTER_SECONDS = 30 * 60
# How many are kept for the feed to read back.
MAX_ALERTS = 200


class Alert:
    __slots__ = ("kind", "hex", "flight", "label", "detail", "lat", "lon", "at")

    def __init__(self, **fields):
        for slot in self.__slots__:
            setattr(self, slot, fields.get(slot))

    def as_dict(self, now: float) -> dict:
        out = {
            "kind": self.kind,
            "hex": self.hex,
            "label": self.label,
            "age": round(max(0.0, now - self.at), 1),
        }
        for key, value in (("flight", self.flight), ("detail", self.detail),
                           ("lat", self.lat), ("lon", self.lon)):
            if value is not None and value != "":
                out[key] = value
        return out


class AlertWatcher:
    """Looks at each poll cycle's aircraft and remembers the notable ones."""

    def __init__(self, settings=None, reference=None):
        self._settings = settings
        self._reference = reference
        self._alerts: list[Alert] = []
        # (hex, kind) -> when it last fired, so a continuing emergency is
        # one alert rather than one per poll.
        self._last_fired: dict[tuple[str, str], float] = {}
        self.raised = 0

    # --- configuration ---------------------------------------------------
    def enabled(self) -> bool:
        return bool(self._settings and self._settings.get("alerts_enabled"))

    def _kind_on(self, name: str) -> bool:
        return bool(self._settings and self._settings.get(name))

    def _retention_seconds(self) -> float:
        minutes = self._settings.get("alert_retention_minutes") if self._settings else 60
        return float(minutes) * 60.0

    # --- the poll path ---------------------------------------------------
    def observe(self, aircraft: list[dict], now: float | None = None):
        if not self.enabled():
            return
        now = time.time() if now is None else now
        for entry in aircraft:
            hex_id = (entry.get("hex") or "").strip().lower()
            if not hex_id:
                continue
            for kind, label, detail in self._notable(entry):
                self._raise(kind, hex_id, entry, label, detail, now)
        self.prune(now)

    def _notable(self, entry: dict):
        """Every reason this aircraft is worth mentioning."""
        if self._kind_on("alert_emergency"):
            squawk = str(entry.get("squawk") or "").strip()
            if squawk in EMERGENCY_SQUAWKS:
                yield "emergency", EMERGENCY_SQUAWKS[squawk], f"Squawk {squawk}"
            else:
                # Only when the squawk did not already say so, or a 7700
                # with an emergency field would raise the same thing twice.
                declared = str(entry.get("emergency") or "").strip().lower()
                if declared and declared not in NOT_AN_EMERGENCY:
                    yield "emergency", "Emergency declared", declared

        if self._kind_on("alert_military") and self._reference is not None:
            operator = self._military_operator(entry)
            if operator:
                yield "military", operator, (entry.get("t") or None)

    def _military_operator(self, entry: dict) -> str | None:
        prefix = (entry.get("flight") or "").strip().upper()[:3]
        if not prefix:
            return None
        return getattr(self._reference, "military", {}).get(prefix)

    def _raise(self, kind: str, hex_id: str, entry: dict, label: str, detail, now: float):
        key = (hex_id, kind)
        if now - self._last_fired.get(key, -1e9) < REPEAT_AFTER_SECONDS:
            return
        self._last_fired[key] = now
        self._alerts.append(Alert(
            kind=kind, hex=hex_id,
            flight=(entry.get("flight") or "").strip() or None,
            label=label, detail=detail,
            lat=entry.get("lat"), lon=entry.get("lon"), at=now,
        ))
        self.raised += 1
        # Newest last; trimmed from the front so the list cannot grow if
        # something pathological starts raising alerts faster than they
        # expire.
        if len(self._alerts) > MAX_ALERTS:
            del self._alerts[:len(self._alerts) - MAX_ALERTS]

    # --- reading back ----------------------------------------------------
    def recent(self, limit: int = 20, now: float | None = None) -> list[dict]:
        now = time.time() if now is None else now
        newest_first = sorted(self._alerts, key=lambda a: a.at, reverse=True)
        return [alert.as_dict(now) for alert in newest_first[:limit]]

    def prune(self, now: float | None = None) -> int:
        now = time.time() if now is None else now
        cutoff = now - self._retention_seconds()
        before = len(self._alerts)
        self._alerts = [a for a in self._alerts if a.at >= cutoff]
        # The cooldown record outlives the alert on purpose - it is what
        # stops a still-squawking aircraft raising a fresh alert the moment
        # its first one expires - but not for ever.
        stale = [k for k, at in self._last_fired.items()
                 if now - at > REPEAT_AFTER_SECONDS * 2]
        for key in stale:
            del self._last_fired[key]
        return before - len(self._alerts)

    def stats(self) -> dict:
        kinds: dict[str, int] = {}
        for alert in self._alerts:
            kinds[alert.kind] = kinds.get(alert.kind, 0) + 1
        return {
            "enabled": self.enabled(),
            "held": len(self._alerts),
            "kinds": kinds,
            "raised": self.raised,
            "retention_minutes": self._retention_seconds() / 60.0,
        }
