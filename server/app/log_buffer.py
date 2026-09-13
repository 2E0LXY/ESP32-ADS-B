"""Recent log lines, kept in memory so the admin panel can show them.

Watching this service meant `docker compose logs -f` over SSH. That is
fine when you are already at a terminal and have the VPS credentials to
hand, and useless otherwise - which covers "a customer says their receiver
stopped and I am on my phone". The information is all there, it just was
not reachable from the thing that already knows who you are.

So every log record also lands in a bounded ring buffer, and /admin/logs
renders it. Deliberately modest:

* In memory only, capped, and dropped on restart. It is for "what just
  happened", not an audit trail - the audit log is the durable record of
  who changed what, and the container's own logs remain the full history.
* Per process. With several workers each holds its own buffer and the
  page shows whichever worker served the request, which is why it says so
  rather than implying it is the whole service.
* Cheap on the logging path: formatting the message and appending to a
  deque under a lock. The message is built at emit time rather than when
  the page is read because the arguments are only valid now - an object
  logged with %s could have changed by then.
"""

import collections
import datetime
import logging
import os
import threading
import traceback

# Roughly a day of normal operation at this deployment's log rate, and
# small enough not to matter next to the reference data already resident.
CAPACITY = int(os.environ.get("LOG_BUFFER_LINES", "2000"))


class LogEntry:
    __slots__ = ("at", "level", "level_number", "logger", "message")

    def __init__(self, record: logging.LogRecord, message: str):
        self.at = datetime.datetime.fromtimestamp(record.created, tz=datetime.timezone.utc)
        self.level = record.levelname
        self.level_number = record.levelno
        self.logger = record.name
        self.message = message

    @property
    def at_str(self) -> str:
        return self.at.strftime("%Y-%m-%d %H:%M:%S")

    @property
    def css_class(self) -> str:
        if self.level_number >= logging.ERROR:
            return "bad"
        if self.level_number >= logging.WARNING:
            return "warn"
        return ""


class RingBufferHandler(logging.Handler):
    """Keeps the most recent records. Oldest are dropped, never the newest."""

    def __init__(self, capacity: int = CAPACITY):
        super().__init__()
        self._entries: collections.deque = collections.deque(maxlen=capacity)
        self._lock = threading.Lock()
        self.dropped = 0
        self.capacity = capacity

    def emit(self, record: logging.LogRecord):
        try:
            # Formatted here rather than at read time because the arguments
            # are only valid now - a mutable object logged with %s could
            # have changed by the time somebody opens the page.
            message = record.getMessage()
            if record.exc_info:
                # logger.exception() is used throughout this codebase and
                # the traceback is the useful half - a page showing only
                # "poll cycle failed" would send you back to SSH anyway.
                message += "\n" + "".join(traceback.format_exception(*record.exc_info)).rstrip()
        except Exception:  # noqa: BLE001
            # A handler that raises would break the call that logged, which
            # is never worth it for a convenience buffer.
            self.handleError(record)
            return
        with self._lock:
            if len(self._entries) == self._entries.maxlen:
                self.dropped += 1
            self._entries.append(LogEntry(record, message))

    def entries(self, minimum_level: int = 0, contains: str = "", limit: int = 500) -> list:
        """Newest first, filtered. A snapshot, so rendering cannot race a
        writer."""
        with self._lock:
            snapshot = list(self._entries)
        matched = []
        needle = contains.lower()
        for entry in reversed(snapshot):
            if entry.level_number < minimum_level:
                continue
            if needle and needle not in entry.message.lower() and needle not in entry.logger.lower():
                continue
            matched.append(entry)
            if len(matched) >= limit:
                break
        return matched

    def counts(self) -> dict:
        with self._lock:
            snapshot = list(self._entries)
        counts = {"total": len(snapshot), "warnings": 0, "errors": 0}
        for entry in snapshot:
            if entry.level_number >= logging.ERROR:
                counts["errors"] += 1
            elif entry.level_number >= logging.WARNING:
                counts["warnings"] += 1
        return counts


_handler: RingBufferHandler | None = None


def install() -> RingBufferHandler:
    """Attaches the buffer to the root logger, once.

    On the root logger rather than on each of this app's loggers, so a
    warning from SQLAlchemy, uvicorn or httpx is visible too - those are
    exactly the ones worth seeing when something is wrong and are the ones
    nobody would think to add by hand.
    """
    global _handler
    if _handler is None:
        _handler = RingBufferHandler()
        logging.getLogger().addHandler(_handler)
    return _handler


def handler() -> RingBufferHandler | None:
    return _handler
