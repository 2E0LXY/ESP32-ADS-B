import logging
import os
import sys

from fastapi import FastAPI

from . import models, security
from .aggregator import Aggregator
from .routes import RouteResolver
from .database import Base, SessionLocal, add_missing_columns, engine
from .feed_ingest import FeedIngestManager
from .logos import LogoStore
from .photos import PhotoStore
from .reference import ReferenceData
from .retention import RETENTION_DAYS, UsageLogPruner
from .routers import admin, public

logging.basicConfig(level=logging.INFO)
# httpx logs every request it makes at INFO. This service makes two upstream
# polls every fifteen seconds plus one adsbdb lookup per new callsign, so on
# a busy sky that is thousands of lines an hour and `docker compose logs
# --tail=100` returns nothing but those. The interesting lines - route
# resolutions, feeder connections, image caching, anything that went wrong -
# were being buried by a library narrating its own success.
logging.getLogger("httpx").setLevel(logging.WARNING)
logging.getLogger("httpcore").setLevel(logging.WARNING)
logger = logging.getLogger("main")

DEBUG = os.environ.get("DEBUG") == "1"

if security.SESSION_SECRET == "insecure-dev-secret-change-me" and not DEBUG:
    sys.exit(
        "Refusing to start: SESSION_SECRET is unset (or still the dev default). "
        "Set a long random value in the environment (see .env.example), or set "
        "DEBUG=1 to run locally without one."
    )

app = FastAPI(title="2E0LXY ADS-B Aggregator")


def _bootstrap_admin(db):
    """Creates the first admin account from the environment on a fresh
    database, so there's a way in without shelling into the container to
    insert a row by hand. No-ops once any admin user already exists."""
    if db.query(models.AdminUser).count() > 0:
        return
    email = os.environ.get("ADMIN_BOOTSTRAP_EMAIL")
    password = os.environ.get("ADMIN_BOOTSTRAP_PASSWORD")
    if not email or not password:
        logger.warning(
            "No admin user exists yet and ADMIN_BOOTSTRAP_EMAIL/ADMIN_BOOTSTRAP_PASSWORD "
            "are not set - the admin panel has no way to log in until you set them "
            "(see .env.example) and restart."
        )
        return
    db.add(models.AdminUser(email=email.strip().lower(), password_hash=security.hash_password(password)))
    db.commit()
    logger.info("Bootstrapped initial admin user %s", email)


@app.on_event("startup")
async def startup():
    Base.metadata.create_all(bind=engine)
    # Existing deployments predate the per-device location columns; without
    # this they would raise on the first query after an upgrade.
    add_missing_columns(Base)
    db = SessionLocal()
    try:
        _bootstrap_admin(db)
    finally:
        db.close()

    home_lat = float(os.environ.get("HOME_LAT", "53.73"))
    home_lon = float(os.environ.get("HOME_LON", "-1.57"))
    home_radius_nm = float(os.environ.get("HOME_RADIUS_NM", "50"))
    app.state.aggregator = Aggregator(home_lat, home_lon, home_radius_nm, SessionLocal)
    app.state.aggregator.start()

    # Resolves callsign -> route on behalf of every device, so the ESP32
    # never opens its own TLS connection to adsbdb. See app/routes.py.
    app.state.routes = RouteResolver()
    app.state.routes.start()

    # Operator names, aircraft models, countries and - most usefully - the
    # silhouette for each of 2,735 type designators. Loaded once, here, so no
    # request pays for reading a CSV. See app/reference.py.
    app.state.reference = ReferenceData()
    app.state.reference.load()

    # Airline logos, fetched once each and then served off this deployment's
    # own disk. See app/logos.py for why lookup is by domain, not by name.
    app.state.logos = LogoStore()
    app.state.logos.start()
    if not app.state.logos.configured():
        logger.info(
            "LOGO_DEV_TOKEN is not set - operator badges will fall back to "
            "initials instead of real logos (see .env.example)"
        )

    # Aircraft type photographs, CC0 and public-domain-mark only, cropped to
    # the panel's band and cached on disk. See app/photos.py for why the
    # licence restriction rules out Wikimedia Commons for civil types.
    app.state.photos = PhotoStore()
    app.state.photos.start()

    # One usage_log row is written per device poll and nothing ever deleted
    # one, so the table grew forever - the first thing that would have
    # filled the disk. See app/retention.py.
    app.state.usage_pruner = UsageLogPruner(SessionLocal)
    app.state.usage_pruner.start()
    logger.info("usage_log retention: %d days", RETENTION_DAYS)

    app.state.feed_ingest = FeedIngestManager(app.state.aggregator, SessionLocal)
    await app.state.feed_ingest.sync_from_db()


@app.on_event("shutdown")
async def shutdown():
    await app.state.usage_pruner.stop()
    await app.state.photos.stop()
    await app.state.logos.stop()
    await app.state.routes.stop()
    await app.state.aggregator.stop()
    await app.state.feed_ingest.stop_all()


app.include_router(public.router)
app.include_router(admin.router)
