import datetime
import enum

from sqlalchemy import (
    Boolean,
    Column,
    DateTime,
    Enum,
    Float,
    ForeignKey,
    Integer,
    String,
    Text,
)
from sqlalchemy.orm import relationship

from .database import Base


def utcnow():
    return datetime.datetime.now(datetime.timezone.utc)


class Account(Base):
    """A customer. Owns devices and, optionally, donated feeder keys."""

    __tablename__ = "accounts"

    id = Column(Integer, primary_key=True)
    email = Column(String(255), unique=True, nullable=False, index=True)
    password_hash = Column(String(255), nullable=False)
    is_active = Column(Boolean, default=True, nullable=False)
    created_at = Column(DateTime(timezone=True), default=utcnow, nullable=False)

    devices = relationship("Device", back_populates="account", cascade="all, delete-orphan")
    feeder_keys = relationship("FeederKey", back_populates="account", cascade="all, delete-orphan")


class Device(Base):
    """One customer's physical receiver. Holds zero or more API keys."""

    __tablename__ = "devices"

    id = Column(Integer, primary_key=True)
    account_id = Column(Integer, ForeignKey("accounts.id"), nullable=False, index=True)
    name = Column(String(120), nullable=False, default="My receiver")
    claim_code = Column(String(16), unique=True, nullable=True, index=True)
    firmware_version = Column(String(64), nullable=True)
    last_seen_at = Column(DateTime(timezone=True), nullable=True)
    last_seen_ip = Column(String(64), nullable=True)
    # A customer's own physical receiver (already feeding FlightAware/FR24/
    # etc.) can additionally push its raw SBS/BaseStation output straight to
    # this backend - see app/feed_ingest.py. Each feeder gets one dedicated
    # TCP port because standard feeder software (readsb, dump1090, PiAware)
    # only knows how to open an outbound connection to a fixed host:port; it
    # has no way to send a custom auth handshake first, so per-device ports
    # are how every established feeder network (adsb.fi, FlightAware, etc.)
    # actually tells feeds apart.
    feeder_enabled = Column(Boolean, default=False, nullable=False)
    feeder_port = Column(Integer, unique=True, nullable=True)
    feeder_last_message_at = Column(DateTime(timezone=True), nullable=True)
    # Where this receiver actually is. The aggregator polls the upstream APIs
    # for the areas its devices are in, so a customer in Cornwall is not
    # served from a cache only ever filled around the operator's own house -
    # which is what a single global HOME_LAT/HOME_LON meant.
    #
    # Two sources, deliberately separate rather than one field overwritten by
    # whichever wrote last. reported_* is what the device sends with every
    # /v1/aircraft request, so a receiver that moves (a hotel, a hotspot)
    # follows itself with no configuration. manual_* is what the owner typed
    # in the dashboard, used when a device has never reported - a new
    # receiver, or one whose firmware predates this.
    reported_lat = Column(Float, nullable=True)
    reported_lon = Column(Float, nullable=True)
    reported_radius_nm = Column(Float, nullable=True)
    reported_at = Column(DateTime(timezone=True), nullable=True)
    manual_lat = Column(Float, nullable=True)
    manual_lon = Column(Float, nullable=True)
    manual_radius_nm = Column(Float, nullable=True)
    # Estimated from the aircraft this device's feeder reports - see
    # app/site_estimate.py. Ranks below both of the above: it is a guess, and
    # a receiver that states its own position or an owner who typed one in
    # both know better than we do.
    inferred_lat = Column(Float, nullable=True)
    inferred_lon = Column(Float, nullable=True)
    inferred_spread_nm = Column(Float, nullable=True)
    inferred_at = Column(DateTime(timezone=True), nullable=True)
    # A read-only public link to this receiver's live map, for anyone the
    # owner chooses to send it to - a club, a forum post, a family member.
    #
    # The token is the whole credential, so it is long and random rather
    # than derived from the device id: a guessable link is a public link
    # whether the owner meant it or not. Kept nullable and cleared rather
    # than flagged off, so revoking really does mean the old link stops
    # resolving, and re-enabling mints a different one.
    share_token = Column(String(64), unique=True, nullable=True, index=True)
    share_created_at = Column(DateTime(timezone=True), nullable=True)
    created_at = Column(DateTime(timezone=True), default=utcnow, nullable=False)

    def location(self) -> tuple[float, float, float] | None:
        """Effective location: what the device reports, else what the owner
        set, else nothing - and "nothing" means the caller falls back to the
        deployment default rather than this guessing."""
        if self.reported_lat is not None and self.reported_lon is not None:
            return (self.reported_lat, self.reported_lon, self.reported_radius_nm or 50.0)
        if self.manual_lat is not None and self.manual_lon is not None:
            return (self.manual_lat, self.manual_lon, self.manual_radius_nm or 50.0)
        if self.inferred_lat is not None and self.inferred_lon is not None:
            # The spread is how scattered the evidence was, not how far this
            # receiver reaches, so poll a normal radius around it rather than
            # treating a tight estimate as a tiny coverage area.
            return (self.inferred_lat, self.inferred_lon, 50.0)
        return None

    def location_source(self) -> str:
        if self.reported_lat is not None and self.reported_lon is not None:
            return "reported by the receiver"
        if self.manual_lat is not None and self.manual_lon is not None:
            return "set here"
        if self.inferred_lat is not None and self.inferred_lon is not None:
            return "estimated from your feed"
        return "server default"

    account = relationship("Account", back_populates="devices")
    api_keys = relationship("ApiKey", back_populates="device", cascade="all, delete-orphan")


class ApiKey(Base):
    """Only the SHA-256 hash of the key is ever stored - the plaintext is
    shown to the customer exactly once, at issuance, the same pattern GitHub
    and Stripe use for their own tokens. A leaked database row is therefore
    useless to an attacker without also breaking SHA-256 preimage
    resistance, unlike storing keys in the clear or reversibly encrypted."""

    __tablename__ = "api_keys"

    id = Column(Integer, primary_key=True)
    device_id = Column(Integer, ForeignKey("devices.id"), nullable=False, index=True)
    key_prefix = Column(String(12), nullable=False, index=True)
    key_hash = Column(String(64), nullable=False, unique=True)
    created_at = Column(DateTime(timezone=True), default=utcnow, nullable=False)
    revoked_at = Column(DateTime(timezone=True), nullable=True)
    last_used_at = Column(DateTime(timezone=True), nullable=True)

    device = relationship("Device", back_populates="api_keys")


class FeederProvider(str, enum.Enum):
    adsbfi = "adsbfi"
    airplaneslive = "airplaneslive"
    adsblol = "adsblol"


class FeederKey(Base):
    """A personal upstream credential an account has donated to the shared
    poll pool (see aggregator.py). Each key is only ever used within its own
    owner's personal-use allowance - pooling several multiplies the
    aggregator's total effective upstream capacity without any single key
    exceeding what its owner is personally entitled to."""

    __tablename__ = "feeder_keys"

    id = Column(Integer, primary_key=True)
    account_id = Column(Integer, ForeignKey("accounts.id"), nullable=False, index=True)
    provider = Column(Enum(FeederProvider), nullable=False)
    credential = Column(String(512), nullable=False)  # provider-specific: API key, UUID, etc.
    label = Column(String(120), nullable=True)
    enabled = Column(Boolean, default=True, nullable=False)
    created_at = Column(DateTime(timezone=True), default=utcnow, nullable=False)

    account = relationship("Account", back_populates="feeder_keys")


class AdminUser(Base):
    """Separate identity space from customer Accounts on purpose - a bug or
    leaked session in the customer-facing side must never grant admin
    access, and vice versa."""

    __tablename__ = "admin_users"

    id = Column(Integer, primary_key=True)
    email = Column(String(255), unique=True, nullable=False, index=True)
    password_hash = Column(String(255), nullable=False)
    created_at = Column(DateTime(timezone=True), default=utcnow, nullable=False)


class AuditLog(Base):
    """Every admin-initiated change, so "who deleted this account" or "who
    issued this key" always has an answer."""

    __tablename__ = "audit_log"

    id = Column(Integer, primary_key=True)
    at = Column(DateTime(timezone=True), default=utcnow, nullable=False)
    actor = Column(String(255), nullable=False)  # admin email, or "account:<id>" for self-service actions
    action = Column(String(64), nullable=False)
    target = Column(String(255), nullable=True)
    detail = Column(Text, nullable=True)


class UsageLog(Base):
    """One row per /v1/aircraft request, for quotas, abuse detection, and
    per-device activity in the admin panel. Pruned periodically - see
    prune_usage_log() - so this table doesn't grow without bound."""

    __tablename__ = "usage_log"

    id = Column(Integer, primary_key=True)
    device_id = Column(Integer, ForeignKey("devices.id"), nullable=False, index=True)
    at = Column(DateTime(timezone=True), default=utcnow, nullable=False, index=True)
    ip = Column(String(64), nullable=True)
    aircraft_returned = Column(Integer, nullable=True)
