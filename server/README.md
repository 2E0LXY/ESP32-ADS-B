# 2E0LXY ADS-B Aggregator

Backend for the ESP32-ADS-B project. Replaces the bare single-file aggregator
set up earlier directly on the VPS with a full customer/device/API-key
management system, an admin panel, and feeder-key pooling.

## What this does

- Polls adsb.fi, airplanes.live, adsb.lol and (with credentials) the OpenSky
  Network centrally on a shared cache and dedupes by ICAO hex, so devices
  never hit those public APIs directly (see the provider-terms comment in the
  firmware's `fetchAdsbV2Aircraft()` for why that matters at more than a
  handful of devices).
- Optional airline schedules from AirLabs - scheduled and estimated times,
  terminal, gate, baggage belt, delay - attached to the aircraft already on
  their way, none of which an ADS-B broadcast carries.
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

## Feeder ingestion (customers' own receivers) - networking requirements

Each feeder-enabled device gets one dedicated TCP port (default range
30100-30999, set by `FEEDER_PORT_RANGE_START`/`FEEDER_PORT_RANGE_END` in
`.env`) that their own readsb/dump1090/PiAware setup connects **out** to -
no inbound connection to the customer's network is ever needed, but the
**VPS's own firewall** does need that whole range open for **inbound** TCP,
since that's how their receiver reaches us:

```bash
sudo ufw allow 30100:30999/tcp
```

(adjust if you change the range).

**Container networking: this is why `docker-compose.yml` uses
`network_mode: host` rather than Docker's default published-ports list.**
Publishing an 800+ port range the normal way (`"30100-30999:30100-30999"`)
makes Docker create one NAT/iptables rule per port at container start - on a
small VPS that's enough forked `iptables` processes in a row to exhaust the
process table and hang the whole machine (this actually happened during
initial deployment). Host networking skips Docker's per-port NAT layer
entirely: the app binds ports directly on the VPS's own network stack. The
`docker-compose.yml` command override pins the main HTTP/admin port to
`127.0.0.1` explicitly (so it isn't exposed outside of Caddy's reverse
proxy) while the feeder listeners still bind `0.0.0.0` themselves in
`feed_ingest.py`, since those are meant to be reachable directly. If you
ever narrow `FEEDER_PORT_RANGE_START`/`END` to a small handful of ports,
reverting to normal published ports (dropping `network_mode: host`) would
be fine too - host mode is only needed because the range is so wide.

This is separate from the feeder-key
pooling feature - a feeder key is someone else's personal API credential for
an existing public network; feeder ingestion is a customer's own receiver's
raw data being pushed straight into this backend.

## Data model / files

- `app/models.py` - Account, Device, ApiKey, FeederKey, AdminUser, AuditLog, UsageLog
- `app/aggregator.py` - background poll loop, feeder-key round-robin
- `app/cache.py` - the aircraft cache: in-process by default, Redis when shared
- `app/leader.py` - which worker does the work that must only happen once
- `app/retention.py` - prunes `usage_log` so it stops growing without bound
- `app/runtime_settings.py` - the settings an operator can change from the admin panel
- `app/log_buffer.py` - recent log lines in memory, for `/admin/logs`
- `app/feed_guard.py` - rejects aircraft a feeder could not really have heard
- `app/opensky.py` - OpenSky as a fourth source: one OAuth token, SI units converted
- `app/schedules.py` - AirLabs schedule lookups, cached and resolved in the background
- `app/alerts.py` - the aircraft worth looking up for, out of the ones already polled
- `app/logbook.py` - every airframe seen before, so a first sighting is recognisable
- `app/commands.py` - what an owner may ask their own receiver to do, and nothing else
- `app/balloons.py` - radiosondes, amateur high-altitude flights and airships
- `app/tracks.py` - recent position history, for the trail behind a selected aircraft
- `app/units.py` - the SI conversions the two metric upstreams need
- `app/sbs.py` - SBS/BaseStation protocol decoder for incoming feeder connections
- `app/feed_ingest.py` - per-device TCP listeners that accept customers' own feeds
- `app/security.py` - password hashing, session tokens, API key generation/hashing
- `app/routers/public.py` - signup/login, customer dashboard, feeder controls, `/v1/aircraft`
- `app/routers/admin.py` - admin login, dashboard, account management, audit log
- SQLite by default (`DATABASE_URL` in `.env`) - fine at this scale; point at
  Postgres later if it's ever needed, nothing else here is SQLite-specific.

## Admin panel

`/admin` (separate login from customer accounts):

- **Dashboard** - upstream source health, including sources that are
  switched off rather than failing; cache size; recently active devices
  with firmware, address, location, last result, polls in 24h and feeder
  state.
- **Settings** - `/admin/settings`. Poll interval, maximum polling areas
  and area radius, which upstream sources are polled, the OpenSky and
  AirLabs credentials, usage-history retention and whether aircraft photos
  are fetched. These take effect
  immediately: no restart, so no feeder connection is dropped. Values are
  validated (a poll interval of zero would hammer three free public APIs in
  a tight loop), a bad value rejects the whole submission rather than
  half-applying it, and every change is written to the audit log. There is
  also a "prune usage history now" button for when the retention window has
  just been shortened.

  API keys are handled differently from every other setting on that page.
  They are write-only: the form shows whether one is stored, never what it
  is, an empty box means "leave the stored key alone" rather than "clear it"
  (there is a checkbox for clearing), the audit log records that a key was
  set or cleared and never its value, and no key is ever sent to a receiver.
  Both integrations do nothing at all until their key is present - OpenSky
  switched on without credentials says so once and is skipped rather than
  recording an error every fifteen seconds.
- **System** - `/admin/system`. What this process is actually doing: cache
  mode, whether Redis is reachable, which worker holds the polling role,
  database and image-cache sizes, disk free, reference-data counts, feeder
  listeners, and whether the two key-gated integrations are configured (with
  how long the held OpenSky token is still good for, so an operator who has
  just pasted credentials can see the exchange succeed). It also lists, explicitly, what **cannot** be changed without a
  restart and why - `REDIS_URL` and the worker count are read once at
  startup, so a switch there would be lying.
- **Password** - `/admin/password`. Until this existed the only admin
  password was `ADMIN_BOOTSTRAP_PASSWORD`, so it is still sitting in `.env`
  on the server; change it here and then clear that line.
- **Devices** - `/admin/devices`. Every registered device, searchable by
  name, account email, IP or firmware, with the two actions an operator
  actually needs: revoke one device's API keys (cutting off a single
  receiver without suspending its owner's whole account, which was the only
  lever before) and stop accepting one receiver's raw feed. Stopping a feed
  closes its listener immediately rather than at the next restart - feeder
  ingestion has no authentication beyond knowing the port, so a feed
  injecting nonsense has to be stoppable now. Both are audit-logged.
- **Logs** - `/admin/logs`. The last few hundred log lines from the worker
  that served the request, filterable by level and text, with optional
  auto-refresh and a `logs.txt` view for pasting or grepping. Tracebacks are
  kept whole, since `logger.exception()` is used throughout and the
  traceback is the useful half. In memory and lost on restart: it answers
  "what just happened" without an SSH session, and `docker compose logs`
  remains the full history.
- **Accounts** and **Audit log** as before.

The environment variables in `.env.example` set where a *fresh* deployment
starts. Once a setting has been saved in the panel, the stored value wins -
editing `.env` and restarting will not undo it.

## Capacity, measured

All of these were measured on this codebase rather than estimated. A device
polls `/v1/aircraft` every 30 seconds (`REFRESH_MS` in the firmware), so
**devices / 30 = requests per second**.

| | |
|---|---|
| One poll, 46 aircraft cached, through the real endpoint | 6.9 ms mean, 8.5 ms p95 |
| Single-thread ceiling from that | ~145 requests/second |
| Process memory with all reference data loaded | ~94 MB |
| `poll_regions()` at 1,000 scattered devices, every 15 s | 131 ms (<1% of one core) |
| `usage_log` on disk | 95 bytes/row including indexes |

Roughly 300 receivers is comfortable on one vCPU: 10 requests/second, some
10-15% of one thread, ~3.2 Mbit/s out. What binds, in order:

1. **`usage_log` growth.** One row per poll is 2,880 rows per device per
   day - at 300 devices, ~82 MB a day and ~2.5 GB a month. This grew
   forever until `app/retention.py` was added, and a full disk stops
   feeder ingestion and device polls alike. It is the first thing that
   would have taken the VPS down, ahead of CPU, RAM or bandwidth.
2. **Worker count, which used to be architectural rather than
   hardware.** The aircraft cache was an in-process dict, so a second
   uvicorn worker would have had its own empty cache and served half the
   receivers nothing - extra vCPU bought nothing at all. `app/cache.py`
   moves it to Redis when `REDIS_URL` is set, and `app/leader.py` keeps
   upstream polling and the feeder listeners in exactly one worker. Only
   then is more CPU worth buying.
3. **SQLite's single writer.** Two writes per poll (a usage row and the
   device's reported position) behind one lock with a 3 s busy timeout,
   competing with feeder writes. Fine at 20 writes/second; spiky nearer
   70.
4. **Sky freshness, not request capacity.** `MAX_POLL_REGIONS` (6) means
   geographically scattered devices rotate their areas across cycles, so
   a device outside the busiest six areas sees an older cache. This is
   about *where* receivers are, not how many - 300 around one county is
   nothing, 300 across Europe is the real limit.

RAM is not the constraint and adding more does not help: ~94 MB of 1 GB,
with the cache bounded by how many aircraft are in the sky rather than how
many devices are asking.

### Running more than one worker

Only worth doing with more than one vCPU, and only with Redis:

1. Uncomment `REDIS_URL` in `.env` and bring up the `redis` service in
   `docker-compose.yml`.
2. Add `--workers N` to the uvicorn command in `docker-compose.yml`.

Every worker then answers `/v1/aircraft` from one shared view of the sky,
while one of them holds a 30-second lease and does the work that must
happen once: polling the upstream APIs (otherwise each community API is
asked for the same sky N times a cycle) and binding the per-device feeder
ports (only one process can bind a given port). If that worker dies the
lease lapses and another takes over, opening the feeder listeners itself.

With `REDIS_URL` unset none of this is on the path and the service behaves
exactly as it always has.

## Known gaps / next steps

Each device has its own location, so the aggregator polls the areas its
devices are actually in rather than one global point. A device reports its
position with every `/v1/aircraft` request, so a receiver that moves follows
itself; the account dashboard can set one for a device that has never
reported. A feeder's receiver position is also estimated from the low-altitude
aircraft it reports (`app/site_estimate.py`), which ranks below both of
those - an SBS stream carries no station position, but an antenna only hears
what is above its horizon, so low traffic gives the site away.
`HOME_LAT`/`HOME_LON`/`HOME_RADIUS_NM` remain the fallback for a
deployment with no located devices. Nearby devices are merged into one poll
area, and the number of areas per cycle is capped by `MAX_POLL_REGIONS`
(default 6), rotating across cycles beyond that so no free upstream API is
asked for an unbounded number of regions.

Schema changes add nullable columns on startup (`add_missing_columns`), so an
existing deployment upgrades without a manual migration. Anything that is not
a nullable column addition still needs a migration written by hand.

Routes (callsign -> origin/destination) are resolved server-side and attached
to each aircraft in `/v1/aircraft`, so devices never query adsbdb themselves.
One resolution is shared by every customer who can see that flight. Lookups
are queued and never block a device request: an unresolved callsign simply
comes back without a route and picks one up on a later poll. See
`app/routes.py`.

The phone and tablet app lives at `/app`, session-authenticated, installable
to a home screen. It is a page rather than a native app and it is served from
here rather than from the receiver, for one concrete reason: a page served
over HTTPS cannot talk to a device at `http://192.168.1.228`, because
browsers block that as mixed content. Coming from this side it works
identically at home and away, and it drives the panel through the command
relay below rather than by addressing it directly - a test pins that, because
the day a LAN address appears in that page is the day the app quietly stops
working away from home.

One request per refresh returns everything it shows - aircraft, alerts,
balloons, the logbook - because a phone on mobile data should not make one
request per panel of the screen. It never carries a device API key: a browser
must not be given one, since anything in a page can be read by anything else
that ends up in that page. Polling stops while the app is in the background.

Remote control works without opening a port on anybody's router. A receiver
sits on a home network the internet cannot reach, so rather than being told
what to do it **asks**: `/v1/commands` rides back on the polling it already
does, and an owner queues instructions with `POST /devices/{id}/command`.

That inverts the trust relationship - a server handing instructions to a
device on somebody's home network - so `app/commands.py` is written as a
security boundary rather than a convenience. Only the account that owns a
device may queue for it (checked against the session); the action must be one
of a small closed set with its value validated server-side; the queue is
capped; and the firmware checks every value again on receipt, because a
device should not do as it is told merely because the instruction arrived
over TLS from the right host.

What is absent from that list matters as much as what is on it. A command can
change what the panel shows, how bright it is, its range, and whether the
screensaver runs - what somebody standing in front of it could change by
touching it. Nothing can change where it sends data, what credentials it
holds, what network it joins, or what firmware it runs.

Alerts (`/v1/alerts`) pick the notable aircraft out of the ones already being
polled, so a display need not present an airliner and an aircraft squawking
7700 identically. Two kinds to begin with, both free because the data is
already in the cache: emergency squawks (7500 hijack, 7600 radio failure,
7700 general emergency, plus the explicit emergency field some feeds carry)
and military aircraft, matched against the same callsign-prefix table the
display uses to name their operators.

The restraint is the point: an aircraft squawking 7700 for twenty minutes is
one event, not eighty, so the same aircraft and kind is quiet for half an
hour after firing. Held in memory and lost on restart - an alert matters
while it is happening and for a while after, and persisting them would add
the one table that grows with events, which is the mistake `usage_log` had to
be taught out of.

A logbook (`/v1/logbook`) records every aircraft this deployment has ever
seen, so a first sighting can be announced as one - spotting is collecting,
and this is the only part of the service that remembers anything across a
restart. It feeds a third kind of alert.

Its table is bounded by distinct airframes rather than by traffic, so unlike
`usage_log` it is meant to be kept rather than pruned. The write rate is what
it is designed around: two hundred aircraft on a fifteen-second poll would be
576,000 row touches a day on the same SQLite file the feeder listeners and
every device poll share. Every known hex is therefore held in memory and
loaded once at startup, so deciding whether an aircraft is new is a set
lookup; only a genuinely new airframe writes immediately, and last-seen times
are swept in batches every few minutes.

Radiosondes also carry SondeHub's landing forecast where there is one - where
it comes down and how long until it does. That is the one genuinely
actionable thing on the balloon page: a sonde is free to recover and there is
a whole hobby in collecting them. Only the end of the forecast path is kept;
SondeHub sends the whole path as a JSON string inside each record, and
passing that on would multiply the response a receiver parses by a hundred
for a line nothing draws. The predictions endpoint only covers 100 km, so a
balloon further out simply has none.

Balloons are a separate sky on their own endpoint (`/v1/balloons`) and their
own page, off until switched on at `/admin/settings`. Three kinds: weather
balloons (radiosondes) and amateur high-altitude flights, both from SondeHub,
plus lighter-than-air aircraft that do carry a transponder, picked out of the
aircraft already polled by ADS-B emitter category B2 at no extra request.

**No API key is needed or wanted here.** SondeHub's GET endpoints are open.
The obvious keyed alternative, aprs.fi, cannot do this job: its API is
callsign-only by design with no geographic search, so it could never answer
"what is near this receiver", and its terms forbid redistributing its data to
a service offering the same features. SondeHub already bridges APRS-IS
itself, so the amateur flights arrive anyway.

Polled far more slowly than aircraft - two minutes by default against
fifteen seconds - because SondeHub asks that its telemetry endpoints not be
polled hard, and a balloon climbing at 5 m/s has not gone anywhere in fifteen
seconds. Altitudes and speeds arrive in SI and are converted on the way in
(`app/units.py`); left alone, a sonde at 30,000 m would read as a perfectly
plausible 30,000 ft.

Aircraft photographs are looked up per type, and per operator of that type
where one exists. One photograph per type meant a Jet2 737-800 and a Ryanair
737-800 shared a picture, so a Jet2 flight was shown a Ryanair aeroplane -
the right aircraft in the wrong livery, beside the name of the airline it is
not. A receiver can pass `?airline=<ICAO>` and the aggregator searches for
that airline's own aircraft first, requiring the airline to be named in the
photograph's title, and falls back to the generic one. `X-Photo-Match` on
the response says which was served, so the panel labels a stand-in livery
rather than presenting it as the flight. Most airline-and-type pairs have no
attribution-free photograph; that answer is cached so the search is not
repeated.

Aircraft trails are drawn on the web map: clicking an aircraft fetches
`/v1/track/{hex}` (or the session- and share-authenticated equivalents the map
pages use) and draws the arc it flew in on, coloured by its current altitude.
The panel draws them too, on a tap. Fetched only for the aircraft actually clicked - a trail per aircraft on every
three-second poll would be roughly a hundred times the payload for something
nobody is looking at, which is why the points live on their own endpoint rather
than being attached to the aircraft list. The panel does not draw them yet.

Schedules follow the same pattern as routes: `app/schedules.py` queues an
AirLabs lookup and attaches the answer as `sched` on a later poll, so a
device never waits on it. The firmware does not display these fields yet -
its ArduinoJson filter drops what it does not know, so the server can attach
them harmlessly until the panel is taught to show a terminal, gate and
delay. The web map is where they are visible today.

Devices can be renamed from the account dashboard at any time; the name is
cosmetic and does not affect the API key or the feeder port.

Customers whose receiver software cannot push SBS out on its own can run the
feeder client in `tools/` instead - a dependency-free Python script with a
systemd unit for Debian and a batch launcher for Windows. See
`tools/README.md`.

Tests live in `tests/` and run against the real app through TestClient:

```
python3 -m venv .venv && .venv/bin/pip install -r requirements-dev.txt
.venv/bin/python -m pytest tests/ -q
```

Nothing has to be running for the suite: the database is a throwaway SQLite
file and Redis is an in-process fake. If `fakeredis` and `lupa` are missing
the Redis cache and leader-election tests skip rather than fail, which
means the parity they assert goes unchecked - so install from
`requirements-dev.txt` rather than picking packages by hand.

- Feeder ingestion has no authentication beyond "knowing which port was
  assigned to you" - standard feeder software (readsb, dump1090, PiAware)
  has no way to send a custom auth handshake before its SBS stream, which
  is why this uses one port per device rather than a shared port with a
  token. Acceptable for now (a port number isn't guessable, and it's shown
  only to the logged-in owner), but if abuse becomes a problem, an
  allowlist of expected source IPs per device would tighten this further.
- **airplanes.live is disabled by default.** It answers 403 to every request,
  and not because of any one deployment's address - the same request is
  refused from unrelated networks. Set `DISABLED_SOURCES=` (empty) to try it
  again if their access rules change, or list other source names there to
  turn them off. A disabled source is not polled and does not appear in the
  admin panel's health table at all, rather than sitting there red.
- A public share link (`/share/<token>`) is unlisted, not access-controlled:
  anyone holding the URL sees that receiver's live map, which is the whole
  point of it. It is served with `X-Robots-Tag: noindex` so a link pasted
  somewhere public does not become findable in search, and it exposes only
  the station name and its aircraft - no account, key, port or other device.
  Revoking clears the token, so the old URL genuinely stops resolving.
  Worth knowing before sharing one: a map of what a single station hears
  implies roughly where that station is, so it is not a way to publish a
  feed anonymously.
- No email sending yet - signup has no email verification, and there's no
  "forgot password" flow. Fine for an invite-only or early-access launch;
  add an SMTP/transactional-email integration before fully open self-serve
  signup.
- Multi-worker operation works but has not been run in production here: the
  Redis cache and the leader lease are covered by tests (including parity
  against the in-process cache) and the deployment still defaults to one
  worker with no Redis. Turn it on deliberately, with more than one vCPU,
  and watch `/admin` for which worker holds the polling role - see
  "Capacity, measured" above.
- Feeder ingest still has no authentication beyond the per-device port,
  which is how every feeder network works (readsb and friends cannot send a
  credential first). Injected aircraft are now checked for plausibility
  rather than trusted (`app/feed_guard.py`): impossible coordinates and
  altitudes, aircraft teleporting between messages, and - for a receiver
  whose position is known independently of its own feed - anything far
  beyond its horizon. `/admin/devices` shows what each feeder is having
  rejected. The remaining gap is a feeder with no owner-set and no
  reported position, which gets every check except the range one, so
  plausible-looking coordinates anywhere on earth would still be accepted
  from it. Before open signup, consider
  pinning a feeder to its last-seen source IP.
- The bootstrap admin's password is still in `.env` on the server from
  first boot. `/admin/password` can change it now; clear that line
  afterwards.
