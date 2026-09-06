# 2E0LXY ADS-B Aggregator

Backend for the ESP32-ADS-B project. Replaces the bare single-file aggregator
set up earlier directly on the VPS with a full customer/device/API-key
management system, an admin panel, and feeder-key pooling.

## What this does

- Polls adsb.fi, airplanes.live, and adsb.lol centrally on a shared cache and
  dedupes by ICAO hex, so devices never hit those public APIs directly (see
  the provider-terms comment in the firmware's `fetchAdsbV2Aircraft()` for why
  that matters at more than a handful of devices).
- Customer accounts: sign up, register a device, get an issued API key.
- Admin panel (separate login from customer accounts): create/suspend/delete
  accounts, view device activity, view upstream source health, audit log of
  every admin action.
- Feeder-key pooling: any account that already runs its own receiver and
  feeds one of these networks can donate that personal key; the aggregator
  round-robins across all donated keys plus its own default access, so no
  single credential is asked to carry more than its owner's personal
  allowance while total capacity scales with how many feeders opt in.

## Deploying (replaces the existing bare aggregator container)

This assumes the VPS already has Docker and Caddy set up from the earlier
session (Caddy already reverse-proxies `adsb.2e0lxy.uk` to `127.0.0.1:8090` -
no Caddyfile change needed, this keeps the same port).

```bash
cd /opt
git clone https://github.com/2E0LXY/ESP32-ADS-B.git adsb-repo   # or: cd adsb-repo && git pull
cd adsb-repo/server

cp .env.example .env
nano .env   # set SESSION_SECRET, ADMIN_BOOTSTRAP_EMAIL, ADMIN_BOOTSTRAP_PASSWORD, HOME_LAT/LON/RADIUS_NM

# Stop and remove the old bare aggregator container if it's still running
docker stop adsb-aggregator 2>/dev/null; docker rm adsb-aggregator 2>/dev/null

docker compose up -d --build
docker compose logs -f   # confirm it starts cleanly and the poll loop connects
```

Then visit `https://adsb.2e0lxy.uk/admin/login` and log in with the
`ADMIN_BOOTSTRAP_EMAIL`/`ADMIN_BOOTSTRAP_PASSWORD` from `.env`.

## First-run checklist

1. Generate a real `SESSION_SECRET`: `python3 -c "import secrets; print(secrets.token_hex(32))"`
   The app refuses to start without one (outside `DEBUG=1`) - this isn't
   optional, a weak or default secret lets anyone forge a login session.
2. Set `ADMIN_BOOTSTRAP_EMAIL`/`ADMIN_BOOTSTRAP_PASSWORD` before first boot -
   this only runs once, on an empty database.
3. Confirm `docker compose logs` shows successful polls of all three
   upstream sources (or check `/admin` - "Upstream source health").
4. Create a test customer account at `/signup`, add a device, issue a key,
   and confirm `curl -H "Authorization: Bearer <key>" "https://adsb.2e0lxy.uk/v1/aircraft?lat=53.73&lon=-1.57&radius=50"`
   returns aircraft.

## Firmware follow-up (not done yet)

The ESP32 firmware's `aggregator` provider currently sends **no** API key -
it queries `/v1/aircraft` with no `Authorization` header, which this backend
now requires. Until the firmware is updated to send one, requests from the
device will get a 401. This needs a small firmware change (an "API key"
field in the Data API admin page for the aggregator provider, sent as
`Authorization: Bearer <key>`) - flag this back to whoever is working the
firmware side.

## Data model / files

- `app/models.py` - Account, Device, ApiKey, FeederKey, AdminUser, AuditLog, UsageLog
- `app/aggregator.py` - background poll loop, in-memory cache, feeder-key round-robin
- `app/security.py` - password hashing, session tokens, API key generation/hashing
- `app/routers/public.py` - signup/login, customer dashboard, `/v1/aircraft`
- `app/routers/admin.py` - admin login, dashboard, account management, audit log
- SQLite by default (`DATABASE_URL` in `.env`) - fine at this scale; point at
  Postgres later if it's ever needed, nothing else here is SQLite-specific.

## Known gaps / next steps

- No email sending yet - signup has no email verification, and there's no
  "forgot password" flow. Fine for an invite-only or early-access launch;
  add an SMTP/transactional-email integration before fully open self-serve
  signup.
- The aggregator cache is in-process memory - fine for one container
  instance. If this is ever scaled to multiple instances, the cache needs to
  move to something shared (Redis) - flagged in `aggregator.py`.
- `HOME_LAT`/`HOME_LON`/`HOME_RADIUS_NM` are global to the whole deployment,
  not per-device - fine while every device is near the same receiver
  location; would need to become a per-device parameter for a
  multi-location deployment.
- Admin panel has no "change your own password" page yet - see the note in
  `.env.example` for how to rotate the bootstrap admin's password manually
  in the meantime.
