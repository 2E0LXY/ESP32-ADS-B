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
- Raw feed ingestion: a customer's own receiver (readsb/dump1090/PiAware,
  already feeding FlightAware/FlightRadar24/etc.) can additionally push its
  live SBS/BaseStation output straight to this backend - one dedicated TCP
  port per feeder-enabled device (see "Feeder ingestion" below). Their
  aircraft show up on their own live map (`/account/my-feed/<device>`) and
  are merged into the same shared cache every device's `/v1/aircraft`
  reads from.
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

## Firmware side (done)

The ESP32 firmware's `aggregator` provider sends the device's API key as
`Authorization: Bearer <key>` on every `/v1/aircraft` request, read from an
"Aggregator API key" field on the Data API admin page. With no key saved,
the firmware refuses to fetch and shows "API key required" rather than
silently taking a 401. Nothing further is needed here - just issue each
device a key from `/account` and paste it into that field.

The firmware also gained a **FlyItalyADSB** provider (a separate, unrelated
public feed - not part of this aggregator) with its own API key field, for
users who'd rather use FlyItalyADSB's own key/endpoint directly instead of
this backend.

## Feeder ingestion (customers' own receivers) - firewall requirement

Each feeder-enabled device gets one dedicated TCP port (default range
30100-30999, set by `FEEDER_PORT_RANGE_START`/`FEEDER_PORT_RANGE_END` in
`.env`) that their own readsb/dump1090/PiAware setup connects **out** to -
no inbound connection to the customer's network is ever needed, but the
**VPS's own firewall** does need that whole range open for **inbound** TCP,
since that's how their receiver reaches us:

```bash
sudo ufw allow 30100:30999/tcp
```

(adjust if you change the range). This is separate from the feeder-key
pooling feature - a feeder key is someone else's personal API credential for
an existing public network; feeder ingestion is a customer's own receiver's
raw data being pushed straight into this backend.

## Data model / files

- `app/models.py` - Account, Device, ApiKey, FeederKey, AdminUser, AuditLog, UsageLog
- `app/aggregator.py` - background poll loop, in-memory cache, feeder-key round-robin
- `app/sbs.py` - SBS/BaseStation protocol decoder for incoming feeder connections
- `app/feed_ingest.py` - per-device TCP listeners that accept customers' own feeds
- `app/security.py` - password hashing, session tokens, API key generation/hashing
- `app/routers/public.py` - signup/login, customer dashboard, feeder controls, `/v1/aircraft`
- `app/routers/admin.py` - admin login, dashboard, account management, audit log
- SQLite by default (`DATABASE_URL` in `.env`) - fine at this scale; point at
  Postgres later if it's ever needed, nothing else here is SQLite-specific.

## Known gaps / next steps

- Feeder ingestion has no authentication beyond "knowing which port was
  assigned to you" - standard feeder software (readsb, dump1090, PiAware)
  has no way to send a custom auth handshake before its SBS stream, which
  is why this uses one port per device rather than a shared port with a
  token. Acceptable for now (a port number isn't guessable, and it's shown
  only to the logged-in owner), but if abuse becomes a problem, an
  allowlist of expected source IPs per device would tighten this further.
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
