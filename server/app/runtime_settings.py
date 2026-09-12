"""Settings an operator can change from the admin panel, without a redeploy.

Everything tunable used to be an environment variable read once at import.
Changing the poll interval, turning a dead upstream source off, or altering
how much usage history to keep all meant editing `.env` on the VPS over
SSH and restarting the container - which drops every feeder connection and
loses a poll cycle. For settings that are safe to change while running,
that is a lot of disruption for a number.

So these live in the database, default to the environment variable that
used to be the only way to set them, and are validated on the way in. The
environment still decides the starting value for a fresh deployment;
nothing about an existing one changes until somebody edits it in the
admin panel.

What is deliberately NOT here: anything that cannot take effect without
restarting the process. REDIS_URL, the worker count, the feeder port
range, SESSION_SECRET and DATABASE_URL are all read once at startup, and
a form that appeared to change them would be lying. The admin panel shows
those as read-only with what to edit instead - see /admin/system.

Reads are pure memory. A worker reloads from the database on its poll
cycle and immediately after its own write, so with several workers a
change reaches the others within one poll interval rather than instantly.
That is fine for what is in here (an interval, a retention window, whether
a source is polled) and avoids a database read on any request path.
"""

import logging
import os

from . import models

logger = logging.getLogger("settings")


class Setting:
    """One editable setting: how to parse it, what is allowed, what it means."""

    def __init__(self, name: str, kind: str, default, *, label: str, help: str = "",
                 minimum=None, maximum=None, group: str = "General", unit: str = ""):
        self.name = name
        self.kind = kind  # "int" | "float" | "bool"
        self.default = default
        self.label = label
        self.help = help
        self.minimum = minimum
        self.maximum = maximum
        self.group = group
        self.unit = unit

    def parse(self, raw):
        """Turns form input or a stored string into the real value.

        Raises ValueError with something worth showing a human - an admin
        typing 0 into the poll interval should be told the range, not given
        a service that hammers three public APIs in a tight loop.
        """
        if self.kind == "bool":
            return str(raw).strip().lower() in ("1", "true", "on", "yes")
        try:
            value = int(raw) if self.kind == "int" else float(raw)
        except (TypeError, ValueError):
            raise ValueError(f"{self.label} must be a number")
        if self.minimum is not None and value < self.minimum:
            raise ValueError(f"{self.label} must be at least {self.minimum}{self.unit}")
        if self.maximum is not None and value > self.maximum:
            raise ValueError(f"{self.label} must be no more than {self.maximum}{self.unit}")
        return value

    def to_text(self, value) -> str:
        return "1" if (self.kind == "bool" and value) else "0" if self.kind == "bool" else str(value)


def _env_int(name: str, fallback: int) -> int:
    try:
        return int(os.environ.get(name, str(fallback)))
    except ValueError:
        logger.warning("%s is not a number - using %s", name, fallback)
        return fallback


def _source_default(name: str) -> bool:
    # airplanes.live is off by default because it answers 403 to every
    # request from anywhere, not just this deployment.
    disabled = {part.strip() for part in os.environ.get("DISABLED_SOURCES", "airplaneslive").split(",")}
    return name not in disabled


DEFINITIONS: list[Setting] = [
    Setting(
        "poll_interval_seconds", "int", _env_int("POLL_INTERVAL_SECONDS", 15),
        label="Upstream poll interval", unit="s", minimum=5, maximum=300, group="Polling",
        help="How often the aggregator asks each upstream source for aircraft. Lower "
             "is fresher and costs those free services more requests; the devices "
             "themselves only refresh every 30s, so below about 10s buys nothing.",
    ),
    Setting(
        "max_poll_regions", "int", _env_int("MAX_POLL_REGIONS", 6),
        label="Maximum polling areas", minimum=1, maximum=24, group="Polling",
        help="Each area is one request per source per cycle. Beyond this many, areas "
             "rotate across cycles - so scattered receivers see an older cache. Raise "
             "this before anything else when devices spread out geographically.",
    ),
    Setting(
        "max_poll_radius_nm", "float", 250.0,
        label="Maximum area radius", unit=" nm", minimum=25, maximum=250, group="Polling",
        help="The widest sky one area may cover, however large a radius a device asks "
             "for. Stops one device making the aggregator poll a hemisphere.",
    ),
    Setting(
        "source_adsbfi", "bool", _source_default("adsbfi"),
        label="Poll adsb.fi", group="Upstream sources",
        help="",
    ),
    Setting(
        "source_airplaneslive", "bool", _source_default("airplaneslive"),
        label="Poll airplanes.live", group="Upstream sources",
        help="Answers 403 to every request from every network we have tried, so this "
             "is off by default. Turn it on to check whether that has changed.",
    ),
    Setting(
        "source_adsblol", "bool", _source_default("adsblol"),
        label="Poll adsb.lol", group="Upstream sources",
        help="",
    ),
    Setting(
        "usage_log_retention_days", "int", _env_int("USAGE_LOG_RETENTION_DAYS", 30),
        label="Usage history kept", unit=" days", minimum=1, maximum=3650, group="Housekeeping",
        help="One row per device poll, about 2,880 a day per receiver. Each device's "
             "most recent row is always kept whatever this is set to, so the last "
             "result stays visible for a receiver that has gone quiet.",
    ),
    Setting(
        "aircraft_photos", "bool", os.environ.get("AIRCRAFT_PHOTOS", "1") != "0",
        label="Aircraft type photographs", group="Housekeeping",
        help="CC0 and public-domain only, cached on this deployment's disk. Turning "
             "this off makes the endpoint answer 503, so receivers retry later rather "
             "than recording 'no photo' permanently.",
    ),
]

BY_NAME = {definition.name: definition for definition in DEFINITIONS}

SOURCE_SETTINGS = {
    "adsbfi": "source_adsbfi",
    "airplaneslive": "source_airplaneslive",
    "adsblol": "source_adsblol",
}


def from_form(form) -> dict:
    """A complete submission built from an HTML form.

    An unticked checkbox is simply absent from a POST body, so a form read
    naively would treat "the operator just turned adsb.fi off" as "adsb.fi
    was not mentioned" and leave it on. Every boolean is therefore filled
    in explicitly as False when missing.
    """
    submitted = {}
    for definition in DEFINITIONS:
        if definition.kind == "bool":
            submitted[definition.name] = definition.name in form
        elif definition.name in form:
            submitted[definition.name] = form[definition.name]
    return submitted


class SettingsStore:
    """Current values, in memory, reloaded from the database on demand.

    With no session factory it serves the environment-derived defaults and
    never touches a database - which is what the aggregator gets in tests
    and what a caller with nothing configured gets.
    """

    def __init__(self, session_factory=None):
        self._session_factory = session_factory
        self._values = {d.name: d.default for d in DEFINITIONS}

    def get(self, name: str):
        return self._values[name]

    def all(self) -> dict:
        return dict(self._values)

    def source_enabled(self, source: str) -> bool:
        key = SOURCE_SETTINGS.get(source)
        return True if key is None else bool(self._values[key])

    def disabled_sources(self) -> set[str]:
        return {name for name, key in SOURCE_SETTINGS.items() if not self._values[key]}

    def reload(self) -> dict:
        """Reads stored overrides. Synchronous - call it from a thread."""
        if self._session_factory is None:
            return self.all()
        db = self._session_factory()
        try:
            rows = db.query(models.Setting).all()
            values = {d.name: d.default for d in DEFINITIONS}
            for row in rows:
                definition = BY_NAME.get(row.key)
                if definition is None:
                    # A setting removed in a later version, or a row from a
                    # newer one. Ignoring it beats refusing to start.
                    continue
                try:
                    values[row.key] = definition.parse(row.value)
                except ValueError:
                    logger.warning("stored value for %s is invalid (%r) - using the default",
                                   row.key, row.value)
            self._values = values
        finally:
            db.close()
        return self.all()

    def set_many(self, submitted: dict, actor: str) -> list[str]:
        """Validates and stores changes. Returns the names that changed.

        All or nothing: one bad value rejects the whole submission, so a
        form with a typo in it cannot half-apply.
        """
        if self._session_factory is None:
            raise RuntimeError("this settings store is read-only")
        parsed = {}
        for name, raw in submitted.items():
            definition = BY_NAME.get(name)
            if definition is None:
                continue
            parsed[name] = definition.parse(raw)  # ValueError propagates to the caller

        changed = [name for name, value in parsed.items() if self._values.get(name) != value]
        if not changed:
            return []
        db = self._session_factory()
        try:
            for name in changed:
                definition = BY_NAME[name]
                text = definition.to_text(parsed[name])
                row = db.query(models.Setting).filter(models.Setting.key == name).first()
                if row is None:
                    db.add(models.Setting(key=name, value=text, updated_by=actor))
                else:
                    row.value = text
                    row.updated_by = actor
                db.add(models.AuditLog(
                    actor=actor, action="change_setting", target=name,
                    detail=f"{self._values.get(name)} -> {parsed[name]}",
                ))
            db.commit()
        finally:
            db.close()
        self._values.update(parsed)
        logger.info("settings changed by %s: %s", actor, ", ".join(changed))
        return changed
