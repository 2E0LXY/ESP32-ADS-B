import datetime
import os
import shutil

from fastapi import APIRouter, Depends, Form, Request, status
from fastapi.responses import HTMLResponse, RedirectResponse
from fastapi.templating import Jinja2Templates
from sqlalchemy.orm import Session

from .. import models, security
from ..aggregator import POLL_INTERVAL_SECONDS, Aggregator
from .. import runtime_settings
from ..cache import RedisAircraftCache
from ..database import get_db
from ..deps import get_current_admin

router = APIRouter(prefix="/admin")
templates = Jinja2Templates(directory="app/templates")


def _log(db: Session, actor: str, action: str, target: str | None = None, detail: str | None = None):
    db.add(models.AuditLog(actor=actor, action=action, target=target, detail=detail))
    db.commit()


@router.get("/login", response_class=HTMLResponse)
def admin_login_form(request: Request):
    return templates.TemplateResponse(request, "admin_login.html", {})


@router.post("/login")
def admin_login(
    request: Request,
    email: str = Form(...),
    password: str = Form(...),
    db: Session = Depends(get_db),
):
    email = email.strip().lower()
    admin = db.query(models.AdminUser).filter(models.AdminUser.email == email).first()
    if not admin or not security.verify_password(password, admin.password_hash):
        return templates.TemplateResponse(
            request, "admin_login.html", {"flash": "Incorrect email or password", "flash_error": True}
        )
    token = security.create_session_token(email, "admin")
    response = RedirectResponse("/admin", status_code=status.HTTP_303_SEE_OTHER)
    response.set_cookie("admin_session", token, httponly=True, samesite="lax", secure=True)
    return response


@router.get("/logout")
def admin_logout():
    response = RedirectResponse("/admin/login")
    response.delete_cookie("admin_session")
    return response


@router.get("", response_class=HTMLResponse)
def admin_dashboard(
    request: Request,
    admin: models.AdminUser = Depends(get_current_admin),
    db: Session = Depends(get_db),
):
    aggregator: Aggregator = request.app.state.aggregator
    health = {}
    for name, h in aggregator.health().items():
        health[name] = h
        # health() now lists every source, polled or not, so that a source
        # switched off can still be switched back on from /admin/settings.
        # Without this the template would render "Up, never succeeded".
        h.enabled = aggregator.source_enabled(name)
        h.last_success_str = (
            datetime.datetime.fromtimestamp(h.last_success, tz=datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
            if h.last_success
            else "Never"
        )
    recent_devices = (
        db.query(models.Device)
        .filter(models.Device.last_seen_at.isnot(None))
        .order_by(models.Device.last_seen_at.desc())
        .limit(20)
        .all()
    )
    # Everything here is already recorded; none of it needed a firmware
    # change. The row previously showed a name, an account, a timestamp and
    # an empty Firmware column, which answered almost nothing about a
    # receiver that had gone quiet or was returning nothing.
    day_ago = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=1)
    for device in recent_devices:
        location = device.location()
        device.location_str = (
            f"{location[0]:.3f}, {location[1]:.3f} @ {location[2]:.0f} nm" if location else "Unknown"
        )
        device.location_from = device.location_source()
        recent = (
            db.query(models.UsageLog)
            .filter(models.UsageLog.device_id == device.id)
            .order_by(models.UsageLog.at.desc())
            .first()
        )
        device.last_aircraft = recent.aircraft_returned if recent else None
        device.polls_today = (
            db.query(models.UsageLog)
            .filter(models.UsageLog.device_id == device.id,
                    models.UsageLog.at >= day_ago)
            .count()
        )
        device.feeder_state = (
            "Off" if not device.feeder_enabled
            else f"Port {device.feeder_port}" if device.feeder_port else "No port"
        )
    return templates.TemplateResponse(
        request,
        "admin_dashboard.html",
        {
            "health": health,
            "cache_size": aggregator.cache.size(),
            "poll_interval": POLL_INTERVAL_SECONDS,
            "account_count": db.query(models.Account).count(),
            "device_count": db.query(models.Device).count(),
            "active_key_count": db.query(models.ApiKey).filter(models.ApiKey.revoked_at.is_(None)).count(),
            "feeder_key_count": db.query(models.FeederKey).filter(models.FeederKey.enabled.is_(True)).count(),
            "recent_devices": recent_devices,
            # So an operator can see the retention job is actually running,
            # rather than finding out from a full disk that it isn't.
            "retention_days": request.app.state.settings.get("usage_log_retention_days"),
            # Which process is actually polling. With one worker this is
            # always "this one"; with several, a dashboard showing source
            # health from a follower is showing that worker's last attempt
            # before it lost the role, which is misleading without saying so.
            "polls_upstream": aggregator.polls_upstream(),
            "shared_cache": getattr(getattr(request.app.state, "leadership", None),
                                    "shared", False),
            "usage_pruned": getattr(getattr(request.app.state, "usage_pruner", None),
                                    "total_removed", None),
        },
    )


@router.get("/accounts", response_class=HTMLResponse)
def admin_accounts(
    request: Request,
    admin: models.AdminUser = Depends(get_current_admin),
    db: Session = Depends(get_db),
):
    accounts = db.query(models.Account).order_by(models.Account.created_at.desc()).all()
    for account in accounts:
        for device in account.devices:
            device.active_key_count = (
                db.query(models.ApiKey)
                .filter(models.ApiKey.device_id == device.id, models.ApiKey.revoked_at.is_(None))
                .count()
            )
    return templates.TemplateResponse(request, "admin_accounts.html", {"accounts": accounts})


@router.post("/accounts")
def admin_create_account(
    email: str = Form(...),
    password: str = Form(...),
    admin: models.AdminUser = Depends(get_current_admin),
    db: Session = Depends(get_db),
):
    email = email.strip().lower()
    if not db.query(models.Account).filter(models.Account.email == email).first():
        db.add(models.Account(email=email, password_hash=security.hash_password(password)))
        db.commit()
        _log(db, admin.email, "create_account", email)
    return RedirectResponse("/admin/accounts", status_code=status.HTTP_303_SEE_OTHER)


@router.post("/accounts/{account_id}/toggle")
def admin_toggle_account(
    account_id: int,
    admin: models.AdminUser = Depends(get_current_admin),
    db: Session = Depends(get_db),
):
    account = db.query(models.Account).filter(models.Account.id == account_id).first()
    if account:
        account.is_active = not account.is_active
        db.commit()
        _log(db, admin.email, "suspend_account" if not account.is_active else "reactivate_account", account.email)
    return RedirectResponse("/admin/accounts", status_code=status.HTTP_303_SEE_OTHER)


@router.post("/accounts/{account_id}/delete")
def admin_delete_account(
    account_id: int,
    admin: models.AdminUser = Depends(get_current_admin),
    db: Session = Depends(get_db),
):
    account = db.query(models.Account).filter(models.Account.id == account_id).first()
    if account:
        _log(db, admin.email, "delete_account", account.email)
        db.delete(account)
        db.commit()
    return RedirectResponse("/admin/accounts", status_code=status.HTTP_303_SEE_OTHER)


@router.get("/audit-log", response_class=HTMLResponse)
def admin_audit_log(
    request: Request,
    admin: models.AdminUser = Depends(get_current_admin),
    db: Session = Depends(get_db),
):
    entries = db.query(models.AuditLog).order_by(models.AuditLog.at.desc()).limit(200).all()
    return templates.TemplateResponse(request, "admin_audit_log.html", {"entries": entries})


# --- settings ----------------------------------------------------------

@router.get("/settings", response_class=HTMLResponse)
def admin_settings_form(
    request: Request,
    admin: models.AdminUser = Depends(get_current_admin),
    flash: str | None = None,
    flash_error: int = 0,
):
    return templates.TemplateResponse(
        request,
        "admin_settings.html",
        {
            "groups": _grouped_settings(request.app.state.settings),
            "flash": flash,
            "flash_error": bool(flash_error),
        },
    )


def _grouped_settings(store) -> list[tuple[str, list]]:
    """Definitions in display order, each carrying its current value.

    Grouped so the form reads as sections rather than one undifferentiated
    column of numbers.
    """
    groups: dict[str, list] = {}
    values = store.all()
    for definition in runtime_settings.DEFINITIONS:
        definition.current = values[definition.name]
        groups.setdefault(definition.group, []).append(definition)
    return list(groups.items())


@router.post("/settings")
async def admin_save_settings(
    request: Request,
    admin: models.AdminUser = Depends(get_current_admin),
):
    form = await request.form()
    store = request.app.state.settings
    try:
        changed = store.set_many(runtime_settings.from_form(form), admin.email)
    except ValueError as exc:
        # Nothing is saved on a bad value - see set_many - so the form can
        # be re-shown with the reason and the operator's other edits are
        # not silently half-applied.
        return RedirectResponse(f"/admin/settings?flash={exc}&flash_error=1",
                                status_code=status.HTTP_303_SEE_OTHER)
    if not changed:
        message = "No changes"
    else:
        message = f"Saved {len(changed)} setting{'s' if len(changed) > 1 else ''}"
        # Other workers pick these up on their next poll cycle; this one
        # applied them the moment set_many returned.
        if getattr(request.app.state, "leadership", None) and request.app.state.leadership.shared:
            message += " - other workers within one poll cycle"
    return RedirectResponse(f"/admin/settings?flash={message}",
                            status_code=status.HTTP_303_SEE_OTHER)


@router.post("/settings/prune-now")
async def admin_prune_now(
    request: Request,
    admin: models.AdminUser = Depends(get_current_admin),
    db: Session = Depends(get_db),
):
    """Runs the retention prune immediately instead of waiting up to six
    hours for the background task - so an operator who has just shortened
    the window can see the effect, and so a disk filling up can be dealt
    with now."""
    removed = await request.app.state.usage_pruner.run_once()
    _log(db, admin.email, "prune_usage_log", detail=f"{removed} rows")
    return RedirectResponse(f"/admin/settings?flash=Pruned {removed} usage rows",
                            status_code=status.HTTP_303_SEE_OTHER)


# --- system ------------------------------------------------------------

def _directory_bytes(path: str) -> int:
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                continue  # deleted under us; a size report is not worth failing over
    return total


@router.get("/system", response_class=HTMLResponse)
async def admin_system(
    request: Request,
    admin: models.AdminUser = Depends(get_current_admin),
    db: Session = Depends(get_db),
):
    """What this process is actually doing, and what cannot be changed from
    here.

    Everything on /admin/settings takes effect while running. The things on
    this page - the shared cache, the worker count, the feeder port range -
    are read once at startup, so a form for them would be lying. They are
    shown read-only with what to edit and what it needs instead.
    """
    from ..database import DATABASE_URL
    from ..feed_ingest import PORT_RANGE_END, PORT_RANGE_START
    from ..leader import LEASE_SECONDS

    state = request.app.state
    cache = state.aggregator.cache
    shared = isinstance(cache, RedisAircraftCache)
    leadership = getattr(state, "leadership", None)

    redis_ok = None
    redis_detail = ""
    if shared:
        try:
            await cache._redis.ping()
            redis_ok = True
            redis_detail = f"reachable at {cache.url}"
        except Exception as exc:  # noqa: BLE001
            # Worth reporting rather than raising: the page is how an
            # operator finds out the shared cache is broken.
            redis_ok = False
            redis_detail = f"{type(exc).__name__}: {exc}"

    db_path = DATABASE_URL.replace("sqlite:///", "") if DATABASE_URL.startswith("sqlite") else None
    db_bytes = 0
    if db_path:
        for suffix in ("", "-wal", "-shm"):
            try:
                db_bytes += os.path.getsize(db_path + suffix)
            except OSError:
                pass

    disk = None
    if db_path and os.path.exists(os.path.dirname(db_path) or "."):
        usage = shutil.disk_usage(os.path.dirname(db_path) or ".")
        disk = {"total": usage.total, "used": usage.used, "free": usage.free,
                "percent": round(usage.used / usage.total * 100) if usage.total else 0}

    photos = state.photos.stats() if hasattr(state.photos, "stats") else {}
    return templates.TemplateResponse(
        request,
        "admin_system.html",
        {
            "shared_cache": shared,
            "redis_ok": redis_ok,
            "redis_detail": redis_detail,
            "leader_identity": getattr(leadership, "identity", "single process"),
            "polls_upstream": state.aggregator.polls_upstream(),
            "lease_seconds": LEASE_SECONDS,
            "cache_size": cache.size(),
            "feeder_ports": f"{PORT_RANGE_START}-{PORT_RANGE_END}",
            "feeder_listeners": len(getattr(state.feed_ingest, "_servers", {}) or {}),
            "database": db_path or DATABASE_URL.split("://")[0],
            "database_bytes": db_bytes,
            "usage_rows": db.query(models.UsageLog).count(),
            "settings_rows": db.query(models.Setting).count(),
            "disk": disk,
            "photo_stats": photos,
            "logo_bytes": _directory_bytes(getattr(state.logos, "_dir", "")),
            "photo_bytes": _directory_bytes(getattr(state.photos, "_dir", "")),
            "reference_stats": state.reference.stats(),
            "logo_stats": state.logos.stats(),
            "pruner": state.usage_pruner,
        },
    )


# --- admin's own password ----------------------------------------------

@router.get("/password", response_class=HTMLResponse)
def admin_password_form(
    request: Request,
    admin: models.AdminUser = Depends(get_current_admin),
    flash: str | None = None,
    flash_error: int = 0,
):
    return templates.TemplateResponse(
        request, "admin_password.html",
        {"email": admin.email, "flash": flash, "flash_error": bool(flash_error)},
    )


@router.post("/password")
def admin_change_password(
    current: str = Form(...),
    replacement: str = Form(...),
    confirmation: str = Form(...),
    admin: models.AdminUser = Depends(get_current_admin),
    db: Session = Depends(get_db),
):
    """The bootstrap password comes from the environment and was, until
    now, the only admin password there could ever be - changing it meant
    editing .env and restarting, and the old one stayed in that file."""
    def back(message: str, error: bool = True):
        suffix = "&flash_error=1" if error else ""
        return RedirectResponse(f"/admin/password?flash={message}{suffix}",
                                status_code=status.HTTP_303_SEE_OTHER)

    if not security.verify_password(current, admin.password_hash):
        # Deliberately checked even though the session already proves who
        # this is: it stops a borrowed, unlocked browser being enough to
        # lock the real operator out.
        return back("Current password is incorrect")
    if replacement != confirmation:
        return back("The new passwords do not match")
    if len(replacement) < 12:
        return back("Use at least 12 characters")
    if security.verify_password(replacement, admin.password_hash):
        return back("That is already your password")

    row = db.query(models.AdminUser).filter(models.AdminUser.id == admin.id).first()
    row.password_hash = security.hash_password(replacement)
    db.commit()
    _log(db, admin.email, "change_admin_password", admin.email)
    return back("Password changed", error=False)
