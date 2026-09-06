import datetime

from fastapi import Cookie, Depends, Header, HTTPException, status
from sqlalchemy.orm import Session

from . import models, security
from .database import get_db


def get_current_admin(
    admin_session: str | None = Cookie(default=None),
    db: Session = Depends(get_db),
) -> models.AdminUser:
    email = security.read_session_token(admin_session or "", "admin") if admin_session else None
    if not email:
        raise HTTPException(status.HTTP_303_SEE_OTHER, headers={"Location": "/admin/login"})
    admin = db.query(models.AdminUser).filter(models.AdminUser.email == email).first()
    if not admin:
        raise HTTPException(status.HTTP_303_SEE_OTHER, headers={"Location": "/admin/login"})
    return admin


def get_current_account(
    account_session: str | None = Cookie(default=None),
    db: Session = Depends(get_db),
) -> models.Account:
    email = security.read_session_token(account_session or "", "account") if account_session else None
    if not email:
        raise HTTPException(status.HTTP_303_SEE_OTHER, headers={"Location": "/login"})
    account = db.query(models.Account).filter(models.Account.email == email).first()
    if not account or not account.is_active:
        raise HTTPException(status.HTTP_303_SEE_OTHER, headers={"Location": "/login"})
    return account


def require_device_api_key(
    authorization: str | None = Header(default=None),
    db: Session = Depends(get_db),
) -> models.Device:
    """Every /v1/aircraft request from a device goes through here. A device
    with no key, an unknown key, or a revoked key all get the same 401 -
    deliberately not distinguishing "revoked" from "never existed" in the
    response, so a caller can't use the API to enumerate which keys used to
    be valid."""
    if not authorization or not authorization.lower().startswith("bearer "):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Missing API key")
    plaintext = authorization.split(" ", 1)[1].strip()
    key_hash = security.hash_api_key(plaintext)
    row = (
        db.query(models.ApiKey)
        .filter(models.ApiKey.key_hash == key_hash, models.ApiKey.revoked_at.is_(None))
        .first()
    )
    if not row or not row.device.account.is_active:
        # A suspended account's devices lose access immediately, without
        # needing to separately revoke every key it owns - suspension is
        # the single switch that actually cuts off a customer.
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Invalid or revoked API key")
    row.last_used_at = datetime.datetime.now(datetime.timezone.utc)
    db.commit()
    return row.device
