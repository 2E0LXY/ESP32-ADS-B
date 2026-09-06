import datetime

from fastapi import APIRouter, Cookie, Depends, Form, Header, Request, status
from fastapi.responses import HTMLResponse, RedirectResponse
from fastapi.templating import Jinja2Templates
from sqlalchemy.orm import Session

from .. import models, security
from ..aggregator import Aggregator
from ..database import get_db
from ..deps import get_current_account, require_device_api_key
from ..feed_ingest import allocate_port

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
    flash_key: str | None = Cookie(default=None),
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
    # Read-once: a freshly reissued key rides here in a short-lived signed
    # cookie (see reissue_key()) rather than being rendered directly by the
    # POST handler, so refreshing this page can never resubmit "reissue" and
    # silently revoke the key it just showed you.
    new_key = None
    if flash_key:
        payload = security.read_flash_token(flash_key)
        owned_device_ids = {d.id for d in devices}
        if payload and payload.get("device_id") in owned_device_ids:
            new_key = payload.get("key")
    response = templates.TemplateResponse(
        request,
        "account_dashboard.html",
        {"devices": devices, "feeder_keys": feeder_keys, "new_key": new_key},
    )
    if flash_key:
        response.delete_cookie("flash_key")
    return response


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


@router.post("/devices/{device_id}/reissue-key")
def reissue_key(
    device_id: int,
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
    # Redirect (not render directly) so refreshing the resulting page is a
    # plain GET, not a resubmission of this POST - rendering the template
    # here directly meant a browser refresh silently reissued (and thereby
    # revoked) a brand new key every time, which is why a key that worked
    # moments ago could 401 shortly after with nothing else having changed.
    # The plaintext rides across that redirect in a short-lived signed
    # cookie instead of a query string, so it's never persisted anywhere
    # (browser history, server access logs) beyond this once-only hop.
    response = RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    response.set_cookie(
        "flash_key",
        security.create_flash_token(device.id, plaintext),
        max_age=60,
        httponly=True,
        secure=True,
        samesite="lax",
    )
    return response


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


# --- Feeder ingestion (a customer's own receiver feeding this backend) -----


@router.post("/devices/{device_id}/feeder/enable")
async def enable_feeder(
    device_id: int,
    request: Request,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    device = _owned_device(db, account, device_id)
    if not device:
        return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    if not device.feeder_port:
        port = allocate_port(db)
        if port is None:
            # Every port in the configured range is already assigned - a
            # capacity problem for the operator to fix (widen the range),
            # not something the customer can do anything about.
            return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
        device.feeder_port = port
    device.feeder_enabled = True
    db.commit()
    await request.app.state.feed_ingest.start_for_device(device.id, device.feeder_port)
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


@router.post("/devices/{device_id}/feeder/disable")
async def disable_feeder(
    device_id: int,
    request: Request,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    device = _owned_device(db, account, device_id)
    if not device:
        return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    device.feeder_enabled = False
    port = device.feeder_port
    db.commit()
    await request.app.state.feed_ingest.stop_for_device(port)
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


@router.get("/account/my-feed/{device_id}", response_class=HTMLResponse)
def my_feed_page(
    device_id: int,
    request: Request,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    device = _owned_device(db, account, device_id)
    if not device:
        return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    return templates.TemplateResponse(request, "my_feed.html", {"device": device})


@router.get("/account/my-feed/{device_id}/aircraft")
async def my_feed_aircraft(
    device_id: int,
    request: Request,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    device = _owned_device(db, account, device_id)
    if not device:
        return {"ac": []}
    aircraft = await _aggregator(request).cache.query_by_source(f"feeder:{device.id}")
    return {"ac": aircraft, "total": len(aircraft)}
