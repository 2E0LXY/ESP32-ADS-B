"""Instructions an owner queues for their own receiver.

The panel sits on a home network with no port open to the internet, so an
app cannot reach it from outside. Rather than asking anybody to forward a
port - which is the wrong thing to ask of a customer and the wrong thing to
talk them through - the device asks this service what to do next. It
already polls every fifteen seconds, and a pending command rides back on
that.

That inverts the trust relationship: this module decides what a server is
allowed to tell a device on somebody's home network. So the allowlist below
is the security boundary, and it is deliberately small.

What is NOT here matters as much as what is. Nothing can change where the
device sends its data, what credentials it holds, what network it joins, or
what firmware it runs. A command can change what the panel is showing and
how bright it is - the things a person standing in front of it could change
by touching it - and nothing else. Anything that could turn a compromised
account into a foothold on a home network stays off this list, and adding
to it should be a deliberate decision rather than a convenience.
"""

import datetime

# action -> a function that returns the cleaned value, or raises ValueError.
# Every action's value is validated here rather than trusted, and the device
# checks again on receipt: this is a boundary worth defending twice.
PAGES = {
    "overview", "table", "map", "radar",
    "marine", "marine-overview", "marine-table",
    "balloon", "balloon-overview", "balloon-table",
}
RADII = {10, 25, 50, 100}
# Deeper than anyone will queue by hand, shallow enough that a runaway
# client cannot fill the table.
MAX_PENDING = 20


def _page(value: str) -> str:
    page = (value or "").strip().lower()
    if page not in PAGES:
        raise ValueError(f"Unknown page {value!r}")
    return page


def _brightness(value: str) -> str:
    try:
        level = int(str(value).strip())
    except (TypeError, ValueError):
        raise ValueError("Brightness must be a number")
    if not 10 <= level <= 100:
        # Not zero: a panel that can be turned fully dark from the internet
        # is a panel somebody will think is broken.
        raise ValueError("Brightness must be 10 to 100")
    return str(level)


def _switch(value: str) -> str:
    text = (value or "").strip().lower()
    if text in ("1", "on", "true", "yes"):
        return "1"
    if text in ("0", "off", "false", "no"):
        return "0"
    raise ValueError("Expected on or off")


def _radius(value: str) -> str:
    try:
        radius = int(str(value).strip())
    except (TypeError, ValueError):
        raise ValueError("Radius must be a number")
    if radius not in RADII:
        raise ValueError(f"Radius must be one of {sorted(RADII)}")
    return str(radius)


def _nothing(value: str) -> None:
    return None


ACTIONS = {
    "page": _page,
    "brightness": _brightness,
    "screensaver": _switch,
    "radius": _radius,
    # No value: just go and fetch now rather than waiting out the interval.
    "refresh": _nothing,
}


def clean(action: str, value) -> tuple[str, str | None]:
    """The action and value as they will be stored, or ValueError."""
    name = (action or "").strip().lower()
    if name not in ACTIONS:
        raise ValueError(f"Unknown action {action!r}")
    return name, ACTIONS[name](value)


def queue(db, device, action: str, value, requested_by: str | None = None):
    """Adds one command for a device. Caller must already have checked that
    this account owns it - that check belongs with the session, not here."""
    from . import models

    name, cleaned = clean(action, value)
    pending = (
        db.query(models.DeviceCommand)
        .filter(models.DeviceCommand.device_id == device.id,
                models.DeviceCommand.delivered_at.is_(None))
        .count()
    )
    if pending >= MAX_PENDING:
        raise ValueError("Too many commands are already waiting for this device")
    row = models.DeviceCommand(device_id=device.id, action=name, value=cleaned,
                               requested_by=requested_by)
    db.add(row)
    db.commit()
    db.refresh(row)
    return row


def collect(db, device) -> list[dict]:
    """Everything waiting for this device, marked as handed over.

    Marked on collection rather than on a later acknowledgement: a receiver
    that takes a command and then reboots loses it, which is the right trade
    for "show the radar page" and stops the panel replaying stale
    instructions after an outage.
    """
    from . import models

    rows = (
        db.query(models.DeviceCommand)
        .filter(models.DeviceCommand.device_id == device.id,
                models.DeviceCommand.delivered_at.is_(None))
        .order_by(models.DeviceCommand.id)
        .all()
    )
    if not rows:
        return []
    now = datetime.datetime.now(datetime.timezone.utc)
    for row in rows:
        row.delivered_at = now
    db.commit()
    return [{"id": row.id, "action": row.action, "value": row.value} for row in rows]


def prune(db, older_than_days: int = 7) -> int:
    """Delivered commands are history, not state. Kept briefly so the app
    can show "sent", then dropped - this table must not become the next
    usage_log."""
    from . import models

    cutoff = (datetime.datetime.now(datetime.timezone.utc)
              - datetime.timedelta(days=older_than_days))
    removed = (
        db.query(models.DeviceCommand)
        .filter(models.DeviceCommand.delivered_at.isnot(None),
                models.DeviceCommand.created_at < cutoff)
        .delete(synchronize_session=False)
    )
    db.commit()
    return removed
