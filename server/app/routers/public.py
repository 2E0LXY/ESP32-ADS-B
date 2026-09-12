import asyncio
import datetime

from fastapi import APIRouter, Cookie, Depends, Form, Header, Request, status
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse, Response
from fastapi.templating import Jinja2Templates
from sqlalchemy.orm import Session

from .. import models, security
from ..aggregator import Aggregator
from ..database import get_db
from ..deps import get_current_account, require_device_api_key
from ..feed_ingest import allocate_port
from ..logos import DEFAULT_SIZE, code_for_callsign

router = APIRouter()
templates = Jinja2Templates(directory="app/templates")


def _aggregator(request: Request) -> Aggregator:
    return request.app.state.aggregator


def _routes(request: Request):
    return request.app.state.routes


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
    # Attach the route to each aircraft so the device does not have to ask
    # adsbdb itself - on the ESP32 that cost ~2.2s of blocked network task
    # per callsign and needed more contiguous internal RAM than it had.
    # lookup() never blocks: an unknown callsign is queued and comes back
    # with a route on a later poll.
    resolver = _routes(request)
    enriched = []
    for entry in aircraft:
        route = resolver.lookup(entry.get("flight"))
        enriched.append({**entry, "route": route} if route else entry)
    aircraft = enriched
    ip = request.client.host if request.client else None
    # In a thread, not inline: this endpoint is "async def", so a synchronous
    # commit here stops the whole event loop until SQLite lets go - and the
    # feeder listeners live on that same loop, so a device poll that waited
    # on a write lock stopped reading a customer's live SBS stream with it.
    await asyncio.to_thread(_record_poll, db, device, ip, lat, lon, radius, len(aircraft))
    return {"ac": aircraft, "total": len(aircraft)}


def _record_poll(
    db: Session,
    device: models.Device,
    ip: str | None,
    lat: float,
    lon: float,
    radius: float,
    returned: int,
):
    device.last_seen_at = datetime.datetime.now(datetime.timezone.utc)
    device.last_seen_ip = ip
    # The device already tells us where it is on every request, so record it:
    # that is what lets the aggregator poll upstream for this customer's sky
    # rather than only the operator's. A receiver that moves - a hotel, a
    # phone hotspot - follows itself with no configuration at all.
    if -85.0 <= lat <= 85.0 and -180.0 <= lon <= 180.0 and 0 < radius <= 250:
        device.reported_lat = lat
        device.reported_lon = lon
        device.reported_radius_nm = radius
        device.reported_at = device.last_seen_at
    db.add(models.UsageLog(device_id=device.id, ip=ip, aircraft_returned=returned))
    db.commit()


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


@router.post("/devices/{device_id}/rename")
def rename_device(
    device_id: int,
    name: str = Form(...),
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    device = _owned_device(db, account, device_id)
    if device:
        # Same trim/limit/fallback as add_device(), so a device can never end
        # up with a name that could not have been given to it at creation.
        device.name = name.strip()[:120] or "My receiver"
        db.commit()
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


@router.post("/devices/{device_id}/location")
def set_device_location(
    device_id: int,
    latitude: str = Form(...),
    longitude: str = Form(...),
    radius: str = Form(...),
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    """Owner-set location, used until the device reports its own.

    Not an override of a reporting device: a receiver that says where it is
    is more reliable than a remembered form field, and silently preferring
    the form would make a moved receiver appear stuck at its old address.
    """
    device = _owned_device(db, account, device_id)
    if not device:
        return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    # Blank fields clear it, which is the only way back to the deployment
    # default once something has been entered.
    if not latitude.strip() and not longitude.strip():
        device.manual_lat = device.manual_lon = device.manual_radius_nm = None
        db.commit()
        return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)
    try:
        lat = float(latitude)
        lon = float(longitude)
        nm = float(radius) if radius.strip() else 50.0
    except ValueError:
        return RedirectResponse("/account?error=location", status_code=status.HTTP_303_SEE_OTHER)
    if not (-85.0 <= lat <= 85.0 and -180.0 <= lon <= 180.0 and 5 <= nm <= 250):
        return RedirectResponse("/account?error=location", status_code=status.HTTP_303_SEE_OTHER)
    device.manual_lat = lat
    device.manual_lon = lon
    device.manual_radius_nm = nm
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
    assigned = await asyncio.to_thread(_enable_feeder_row, db, account, device_id)
    if assigned:
        await request.app.state.feed_ingest.start_for_device(*assigned)
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


def _enable_feeder_row(db: Session, account: models.Account, device_id: int):
    device = _owned_device(db, account, device_id)
    if not device:
        return None
    if not device.feeder_port:
        port = allocate_port(db)
        if port is None:
            # Every port in the configured range is already assigned - a
            # capacity problem for the operator to fix (widen the range),
            # not something the customer can do anything about.
            return None
        device.feeder_port = port
    device.feeder_enabled = True
    db.commit()
    return device.id, device.feeder_port


@router.post("/devices/{device_id}/feeder/disable")
async def disable_feeder(
    device_id: int,
    request: Request,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    port = await asyncio.to_thread(_disable_feeder_row, db, account, device_id)
    await request.app.state.feed_ingest.stop_for_device(port)
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


def _disable_feeder_row(db: Session, account: models.Account, device_id: int) -> int | None:
    device = _owned_device(db, account, device_id)
    if not device:
        return None
    device.feeder_enabled = False
    port = device.feeder_port
    db.commit()
    return port


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
    return templates.TemplateResponse(
        request,
        "feed_map.html",
        {
            "device": device,
            "shared": False,
            "aircraft_url": f"/account/my-feed/{device.id}/aircraft",
        },
    )


@router.get("/account/my-feed/{device_id}/aircraft")
async def my_feed_aircraft(
    device_id: int,
    request: Request,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    # Browsers on this page poll it every three seconds, so it is the most
    # frequent database read in the service - and the one most able to stall
    # the loop the feeder listeners run on.
    device = await asyncio.to_thread(_owned_device, db, account, device_id)
    if not device:
        return {"ac": []}
    aircraft = await _aggregator(request).cache.query_by_source(f"feeder:{device.id}")
    return {"ac": aircraft, "total": len(aircraft)}


# --- Public share links ---------------------------------------------------
#
# A read-only view of one receiver's live map that needs no account. The
# token in the URL is the entire credential, so these routes deliberately
# expose nothing else about the owner: no email, no API key, no port, no
# other device. Revoking is deleting the token, not hiding a flag, so an
# old link genuinely stops working.


@router.post("/devices/{device_id}/share")
def enable_share(
    device_id: int,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    """Mints a link, or replaces the existing one.

    Rotating and creating are the same operation on purpose: "give me a new
    link" is what you want after sharing one too widely, and it should not
    need a separate button that could be confused with revoking.
    """
    device = _owned_device(db, account, device_id)
    if device:
        device.share_token = security.generate_share_token()
        device.share_created_at = datetime.datetime.now(datetime.timezone.utc)
        db.commit()
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


@router.post("/devices/{device_id}/share/revoke")
def revoke_share(
    device_id: int,
    account: models.Account = Depends(get_current_account),
    db: Session = Depends(get_db),
):
    device = _owned_device(db, account, device_id)
    if device:
        device.share_token = None
        device.share_created_at = None
        db.commit()
    return RedirectResponse("/account", status_code=status.HTTP_303_SEE_OTHER)


def _shared_device(db: Session, token: str) -> models.Device | None:
    # Guard against the empty/short token that a truncated paste or a bare
    # /share/ would otherwise turn into a query for "any device with a null
    # token", which is every device that has never been shared.
    if not token or len(token) < 16:
        return None
    return db.query(models.Device).filter(models.Device.share_token == token).first()


# Unlisted, not secret-proof: anyone holding the link can open it, which is
# what the owner asked for. Keeping it out of search results is still worth
# doing, since a link pasted into a public forum would otherwise be indexed
# and become findable by people the owner never sent it to.
NO_INDEX = {"X-Robots-Tag": "noindex, nofollow, noarchive"}


@router.get("/share/{token}", response_class=HTMLResponse)
async def shared_feed_page(token: str, request: Request, db: Session = Depends(get_db)):
    device = await asyncio.to_thread(_shared_device, db, token)
    if not device:
        return templates.TemplateResponse(
            request, "share_missing.html", {}, status_code=status.HTTP_404_NOT_FOUND,
            headers=NO_INDEX,
        )
    return templates.TemplateResponse(
        request,
        "feed_map.html",
        {
            "device": device,
            "shared": True,
            "aircraft_url": f"/share/{token}/aircraft",
        },
        headers=NO_INDEX,
    )


@router.get("/share/{token}/aircraft")
async def shared_feed_aircraft(token: str, request: Request, db: Session = Depends(get_db)):
    device = await asyncio.to_thread(_shared_device, db, token)
    if not device:
        return JSONResponse({"ac": [], "total": 0}, status_code=status.HTTP_404_NOT_FOUND,
                            headers=NO_INDEX)
    aircraft = await _aggregator(request).cache.query_by_source(f"feeder:{device.id}")
    return JSONResponse({"ac": aircraft, "total": len(aircraft)}, headers=NO_INDEX)


# --- Airline logos --------------------------------------------------------
#
# Open, unauthenticated and heavily cacheable on purpose: these are public
# brand images, they are needed by the shared feed links which have no
# session, and the point of the endpoint is that a logo is fetched from
# logo.dev once for the whole deployment rather than once per viewer.
#
# 404 is a normal answer, not an error. It means "we have no logo for this
# airline", and every caller answers it the same way: draw its own initials
# badge. That is why the img tags using this carry an onerror.

LOGO_CACHE_HEADERS = {
    # A week in the browser, and a stale copy is fine while a fresh one is
    # fetched: an airline logo changes on the order of never.
    "Cache-Control": "public, max-age=604800, stale-while-revalidate=86400",
}


@router.get("/logo/callsign/{callsign}.png")
async def logo_for_callsign(callsign: str, request: Request, size: int = DEFAULT_SIZE):
    """The operator's logo for a flight callsign, e.g. RYR2BH -> Ryanair."""
    code = code_for_callsign(callsign)
    if not code:
        return Response(status_code=status.HTTP_404_NOT_FOUND, headers=LOGO_CACHE_HEADERS)
    return await _logo_response(request, code, size)


@router.get("/logo/airline/{code}.png")
async def logo_for_airline(code: str, request: Request, size: int = DEFAULT_SIZE):
    """The logo for an ICAO airline code directly, e.g. RYR."""
    return await _logo_response(request, code.strip().upper(), size)


async def _logo_response(request: Request, code: str, size: int) -> Response:
    store = request.app.state.logos
    data = await store.logo(code, size)
    if data is None:
        return Response(status_code=status.HTTP_404_NOT_FOUND, headers=LOGO_CACHE_HEADERS)
    return Response(content=data, media_type="image/png", headers=LOGO_CACHE_HEADERS)
