import datetime

from fastapi import APIRouter, Depends, Form, Request, status
from fastapi.responses import HTMLResponse, RedirectResponse
from fastapi.templating import Jinja2Templates
from sqlalchemy.orm import Session

from .. import models, security
from ..aggregator import POLL_INTERVAL_SECONDS, Aggregator
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
