import datetime

from fastapi import APIRouter, Depends, Form, Header, Request, status
from fastapi.responses import HTMLResponse, RedirectResponse
from fastapi.templating import Jinja2Templates
from sqlalchemy.orm import Session

from .. import models, security
from ..aggregator import Aggregator
from ..database import get_db
from ..deps import get_current_account, require_device_api_key

router = APIRouter()
templates = Jinja2Templates(directory="app/templates")


def _aggregator(request: Request) -> Aggregator:
    return request.app.state.aggregator


@router.get("/", response_class=HTMLResponse)
def root():
    # The device's own admin page links customers to their product's site,
    # not here - this backend has no reason to be a public landing page.
    return RedirectResponse("https://2e0lxy.uk/adsb/7-inch-ESP32-S3-ADSB-MLAT-Receiver-site/index.html")


@router.get("/v1/aircraft")
async def get_aircraft(
    lat: float,
    lon: float,
    radius: float,
    request: Request,
    device: models.Device = Depends(require_device_api_key),
    db: Session = Depends(get_db),
):
    aggregator = _aggregator(request)
    aircraft = await aggregator.cache.query(lat, lon, radius)
    device.last_seen_at = datetime.datetime.now(datetime.timezone.utc)
    device.last_seen_ip = request.client.host if request.client else None
    db.add(
        models.UsageLog(
            device_id=device.id,
            ip=device.last_seen_ip,
            aircraft_returned=len(aircraft),
        )
    )
    db.commit()
    return {"ac": aircraft, "total": len(aircraft)}


# --- Customer signup / login -------------------------------------------------


@router.get("/signup", response_class=HTMLResponse)
def signup_form(request: Request):
    return templates.TemplateResponse(request, "signup.html", {})


@router.post("/signup")
def signup(
    request: Request,
    email: str = Form(...),
    password: str = Form(...),
    db: Session = Depends(get_db),
):
    email = email.strip().lower()
    if len(password) < 8:
        return templates.TemplateResponse(
            request, "signup.html", {"flash": "Password must be at least 8 characters", "flash_error": True}
        )
    if db.query(models.Account).filter(models.Account.email == email).first():
        return templates.TemplateResponse(
            request, "signup.html", {"flash": "An account with that email already exists", "flash_error": True}
        )
    account = models.Account(email=email, password_hash=security.hash_password(password))
    db.add(account)
    db.commit()
    token = security.create_session_token(email, "account")
    response = RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    response.set_cookie("account_session", token, httponly=True, samesite="lax", secure=True)
    return response


@router.get("/login", response_class=HTMLResponse)
def login_form(request: Request):
    return templates.TemplateResponse(request, "login.html", {})


@router.post("/login")
def login(
    request: Request,
    email: str = Form(...),
    password: str = Form(...),
    db: Session = Depends(get_db),
):
    email = email.strip().lower()
    account = db.query(models.Account).filter(models.Account.email == email).first()
    if not account or not security.verify_password(password, account.password_hash) or not account.is_active:
        return templates.TemplateResponse(
            request, "login.html", {"flash": "Incorrect email or password", "flash_error": True}
        )
    token = security.create_session_token(email, "account")
    response = RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    response.set_cookie("account_session", token, httponly=True, samesite="lax", secure=True)
    return response


@router.get("/logout")
def logout():
    response = RedirectResponse("/login")
    response.delete_cookie("account_session")
    return response


# --- Customer dashboard -------------------------------------------------


@router.get("/account", response_class=HTMLResponse)
def account_dashboard(
    request: Request,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    devices = db.query(models.Device).filter(models.Device.account_id == account.id).all()
    for device in devices:
        device.active_key = (
            db.query(models.ApiKey)
            .filter(models.ApiKey.device_id == device.id, models.ApiKey.revoked_at.is_(None))
            .order_by(models.ApiKey.created_at.desc())
            .first()
        )
    feeder_keys = db.query(models.FeederKey).filter(models.FeederKey.account_id == account.id).all()
    return templates.TemplateResponse(
        request,
        "account_dashboard.html",
        {"devices": devices, "feeder_keys": feeder_keys},
    )


@router.post("/devices")
def add_device(
    name: str = Form(...),
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    device = models.Device(account_id=account.id, name=name.strip()[:120] or "My receiver")
    db.add(device)
    db.commit()
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


@router.post("/devices/{device_id}/reissue-key", response_class=HTMLResponse)
def reissue_key(
    device_id: int,
    request: Request,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    device = _owned_device(db, account, device_id)
    if not device:
        return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    # Revoke any existing keys for this device rather than accumulating
    # forgotten-about valid keys - a device only ever needs one live key.
    db.query(models.ApiKey).filter(
        models.ApiKey.device_id == device.id, models.ApiKey.revoked_at.is_(None)
    ).update({"revoked_at": datetime.datetime.now(datetime.timezone.utc)})
    plaintext, prefix, digest = security.generate_api_key()
    db.add(models.ApiKey(device_id=device.id, key_prefix=prefix, key_hash=digest))
    db.commit()
    devices = db.query(models.Device).filter(models.Device.account_id == account.id).all()
    for d in devices:
        d.active_key = (
            db.query(models.ApiKey)
            .filter(models.ApiKey.device_id == d.id, models.ApiKey.revoked_at.is_(None))
            .order_by(models.ApiKey.created_at.desc())
            .first()
        )
    feeder_keys = db.query(models.FeederKey).filter(models.FeederKey.account_id == account.id).all()
    return templates.TemplateResponse(
        request,
        "account_dashboard.html",
        {"devices": devices, "feeder_keys": feeder_keys, "new_key": plaintext},
    )


@router.post("/devices/{device_id}/delete")
def delete_device(
    device_id: int,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    device = _owned_device(db, account, device_id)
    if device:
        db.delete(device)
        db.commit()
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


@router.post("/feeder-keys")
def add_feeder_key(
    provider: str = Form(...),
    credential: str = Form(...),
    label: str = Form(""),
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    if provider not in (p.value for p in models.FeederProvider):
        return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    db.add(
        models.FeederKey(
            account_id=account.id,
            provider=models.FeederProvider(provider),
            credential=credential.strip(),
            label=label.strip()[:120] or None,
        )
    )
    db.commit()
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


@router.post("/feeder-keys/{key_id}/toggle")
def toggle_feeder_key(
    key_id: int,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    fk = db.query(models.FeederKey).filter(
        models.FeederKey.id == key_id, models.FeederKey.account_id == account.id
    ).first()
    if fk:
        fk.enabled = not fk.enabled
        db.commit()
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


@router.post("/feeder-keys/{key_id}/delete")
def delete_feeder_key(
    key_id: int,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    fk = db.query(models.FeederKey).filter(
        models.FeederKey.id == key_id, models.FeederKey.account_id == account.id
    ).first()
    if fk:
        db.delete(fk)
        db.commit()
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


def _owned_device(db: Session, account: models.Account, device_id: int) -> models.Device | None:
    return (
        db.query(models.Device)
        .filter(models.Device.id == device_id, models.Device.account_id == account.id)
        .first()
    )
