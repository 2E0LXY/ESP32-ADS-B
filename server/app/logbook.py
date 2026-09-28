"""What this deployment has seen before, and what it never has.

Plane spotting is fundamentally collecting. Everything else here answers
"what is in the sky now"; this is the only part that remembers, and "you
have never seen a Beluga before" is the moment that makes a display feel
alive rather than decorative.

The engineering problem is not the size of the table - it is bounded by the
number of distinct airframes, which grows slowly - but the WRITE RATE. Two
hundred aircraft on a fifteen-second poll is 576,000 row touches a day, on
the same SQLite file the feeder listeners and every device poll share. That
would be far heavier than the traffic it is recording.

So the database is touched as little as possible:

* Every hex ever seen is held in memory, loaded once at startup. Deciding
  whether an aircraft is new is then a set lookup, not a query.
* A genuinely new aircraft is the only thing that writes immediately, and
  those are rare by definition - a handful a day on a busy receiver once
  the first week is past.
* "Last seen" is batched and swept every few minutes rather than written
  per poll, because nothing depends on it being to the second.
"""

import asyncio
import datetime
import logging
import time

logger = logging.getLogger("logbook")

# A gap this long before the same aircraft counts as a new visit rather
# than the same one still overhead.
VISIT_GAP_SECONDS = 60 * 60
# How often the pending last-seen updates are written.
FLUSH_EVERY_SECONDS = 5 * 60
# Newly seen aircraft kept in memory for the "recently new" list.
MAX_RECENT_FIRSTS = 50


class FirstSighting:
    __slots__ = ("hex", "flight", "registration", "type_code", "operator", "at")

    def __init__(self, **fields):
        for slot in self.__slots__:
            setattr(self, slot, fields.get(slot))

    def as_dict(self, now: float) -> dict:
        out = {"hex": self.hex, "age": round(max(0.0, now - self.at), 1)}
        for key, value in (("flight", self.flight), ("reg", self.registration),
                           ("type", self.type_code), ("operator", self.operator)):
            if value:
                out[key] = value
        return out


class Logbook:
    def __init__(self, session_factory=None, settings=None):
        self._session_factory = session_factory
        self._settings = settings
        # Every hex ever recorded. The whole design rests on this: a few
        # thousand short strings is a megabyte or so, against a database
        # query per aircraft per poll.
        self._known: set[str] = set()
        # hex -> (last_seen epoch, identity fields), waiting to be written.
        self._pending: dict[str, tuple[float, dict]] = {}
        self._recent: list[FirstSighting] = []
        self._last_flush = 0.0
        self.loaded = False
        self.firsts = 0
        self.flushes = 0
        self.rows_written = 0

    def enabled(self) -> bool:
        return bool(self._settings and self._settings.get("logbook_enabled")
                    and self._session_factory is not None)

    # --- startup ---------------------------------------------------------
    def load(self):
        """Reads every known hex. Synchronous - call it from a thread."""
        if self._session_factory is None:
            return
        from . import models

        db = self._session_factory()
        try:
            self._known = {row[0] for row in db.query(models.Sighting.hex).all()}
            self.loaded = True
            logger.info("logbook: %d aircraft seen before", len(self._known))
        finally:
            db.close()

    # --- the poll path ---------------------------------------------------
    def observe(self, aircraft: list[dict], now: float | None = None) -> list[FirstSighting]:
        """Records this cycle and returns whatever had never been seen.

        Returns rather than announces, so the alert watcher can decide what
        to do with a first sighting without the logbook knowing about it.
        """
        if not self.enabled():
            return []
        now = time.time() if now is None else now
        firsts = []
        for entry in aircraft:
            hex_id = (entry.get("hex") or "").strip().lower()
            if not hex_id:
                continue
            identity = _identity(entry)
            self._pending[hex_id] = (now, identity)
            if hex_id not in self._known:
                self._known.add(hex_id)
                first = FirstSighting(hex=hex_id, at=now, **identity)
                firsts.append(first)
                self._recent.append(first)
                self.firsts += 1
        if len(self._recent) > MAX_RECENT_FIRSTS:
            del self._recent[:len(self._recent) - MAX_RECENT_FIRSTS]
        return firsts

    def due_for_flush(self, now: float | None = None) -> bool:
        now = time.time() if now is None else now
        return bool(self._pending) and now - self._last_flush >= FLUSH_EVERY_SECONDS

    async def flush(self, now: float | None = None) -> int:
        """Writes what is pending, in a thread - every statement below is
        blocking SQLAlchemy and the event loop is shared with the feeder
        listeners."""
        if not self.enabled() or not self._pending:
            return 0
        now = time.time() if now is None else now
        batch, self._pending = self._pending, {}
        self._last_flush = now
        written = await asyncio.to_thread(self._write, batch)
        self.flushes += 1
        self.rows_written += written
        return written

    def _write(self, batch: dict[str, tuple[float, dict]]) -> int:
        from . import models

        db = self._session_factory()
        try:
            rows = {row.hex: row for row in db.query(models.Sighting).filter(
                models.Sighting.hex.in_(list(batch))).all()}
            for hex_id, (seen_at, identity) in batch.items():
                when = datetime.datetime.fromtimestamp(seen_at, datetime.timezone.utc)
                row = rows.get(hex_id)
                if row is None:
                    db.add(models.Sighting(hex=hex_id, first_seen=when, last_seen=when,
                                           visits=1, **identity))
                    continue
                # A gap counts as a separate visit; ten minutes overhead
                # does not.
                previous = row.last_seen
                if previous is not None:
                    if previous.tzinfo is None:
                        previous = previous.replace(tzinfo=datetime.timezone.utc)
                    if (when - previous).total_seconds() >= VISIT_GAP_SECONDS:
                        row.visits = (row.visits or 0) + 1
                row.last_seen = when
                # Only ever fill blanks in: a later position-only report
                # must not erase a registration an earlier one carried.
                for field, value in identity.items():
                    if value and not getattr(row, field, None):
                        setattr(row, field, value)
            db.commit()
            return len(batch)
        except Exception:  # noqa: BLE001
            db.rollback()
            logger.exception("logbook flush failed")
            return 0
        finally:
            db.close()

    # --- reading back ----------------------------------------------------
    def recent_firsts(self, limit: int = 20, now: float | None = None) -> list[dict]:
        now = time.time() if now is None else now
        return [f.as_dict(now) for f in reversed(self._recent[-limit:])]

    def stats(self) -> dict:
        return {
            "enabled": self.enabled(),
            "loaded": self.loaded,
            "known": len(self._known),
            "firsts_since_restart": self.firsts,
            "pending": len(self._pending),
            "flushes": self.flushes,
            "rows_written": self.rows_written,
        }


def _identity(entry: dict) -> dict:
    """The names worth remembering an airframe by."""
    return {
        "flight": (entry.get("flight") or "").strip()[:16] or None,
        "registration": (entry.get("r") or "").strip()[:16] or None,
        "type_code": (entry.get("t") or "").strip()[:8] or None,
        "operator": (entry.get("ownOp") or "").strip()[:128] or None,
    }
