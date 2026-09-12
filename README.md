# ESP32 ADS-B

[![Build and release](https://github.com/2E0LXY/ESP32-ADS-B/actions/workflows/release.yml/badge.svg)](https://github.com/2E0LXY/ESP32-ADS-B/actions/workflows/release.yml)
[![USB installer](https://github.com/2E0LXY/ESP32-ADS-B/actions/workflows/pages.yml/badge.svg)](https://2e0lxy.github.io/ESP32-ADS-B/)

[Install the firmware directly over USB](https://2e0lxy.github.io/ESP32-ADS-B/) · Chrome or Edge on desktop · no local flashing tools required

Firmware for the **Waveshare ESP32-S3 Touch-LCD-4 Rev 4.0, 480 × 480, non-touch panel**. It retrieves nearby ADS-B and MLAT aircraft, plots them on an OpenStreetMap base map on the LCD, and provides a password-protected web administration interface on the local network.

Also runs on the **Waveshare ESP32-S3-Touch-LCD-7 / -4.3, 800 × 480, GT911 touch** boards; see [Supported hardware](#supported-hardware).

Current firmware: **v2.6.0**

### Unreleased

Display:

- **Real airline logos on the screensaver**, replacing the three-initial tile. Fetched once per airline from the aggregator's own cache, kept on the microSD card and drawn from there afterwards; an airline with no logo, or a callsign carrying no airline prefix, keeps the initials tile. Guarded so it cannot cost the feed: a logo is only fetched when the largest free internal block is at least 48 KB, the decoded image is staged in PSRAM and never in internal RAM, and the fetch runs on the network task rather than the core driving the panel
- **The screensaver shows the nearest aircraft** and reconsiders every ten seconds, instead of rotating through everything overhead every six. The aircraft only changes when something genuinely overtakes it, which is a better trigger for a full-screen repaint than a timer
- **The device's address appears in the top corner of both screensaver frames.** It is the one page with no header, so it was the one place the management address could not be read off the screen
- **The full model name and the radio callsign** on the screensaver where the aggregator resolved them: "Boeing 737-800" rather than "B738", and the callsign heard on the air, which is rarely the trading name - Jet2 answers to Channex
- **Unclassified traffic gets an aircraft silhouette** in the browser map rather than the old arrowhead, which read as a different kind of object next to the other eleven shapes. Traffic reporting on the ground with no type and no category now draws the surface-vehicle square, so airport ground stations and service vehicles stop appearing as aircraft on the taxiways

Browser:

- **Clicking an aircraft on the map works.** Tracking could only be set from the table while the map re-asserted it on every refresh, so a marker click was overridden seconds later and closing a popup was undone. Clicking a marker now selects it, clicking it again or closing the popup releases it, and the map recentres only when the selection changes rather than fighting a pan every five seconds
- **Four-level Wi-Fi signal bands** driven by RSSI rather than a derived percentage, with the level in words beside the reading in the top corner: strong at or above -60 dBm, normal to -70, weak to -80, bad below
- **A feed indicator in the header** naming the active provider, green when it is returning data and red when it is failing, so feed health is readable from any page
- **Check for updates lives only on the Firmware page.** It was in the footer of every page and duplicated as an Overview quick action
- **Radar range presets highlight on press** instead of waiting for the next status poll, which made a press look as though it had not registered

Wi-Fi:

- **Up to six saved networks.** The ESP32 remembers exactly one, so moving the receiver between places meant retyping a password already entered. Every network that works is now kept, newest first, and the Wi-Fi page lists them with a Forget button. Additive by design: the stock connection path at boot is unchanged and tried first, and the saved list is only walked when that fails or a working connection has been down for a minute

Backend:

- **Airline logos cached server-side** and served from `/logo/callsign/<callsign>.png`, so a logo is fetched from logo.dev once for the whole deployment rather than once per viewer, and the account token never reaches a browser. Requires `LOGO_DEV_TOKEN`; without it every logo reports as missing and badges fall back to initials. See [Aggregator backend](#aggregator-backend)
- **Operator, model, country and livery enrichment** from offline ICAO lists covering 6,008 operators and 2,735 type designators, including the aircraft silhouette resolved from real class and engine data. Retires the firmware's 91 hand-written callsign prefixes for anyone using this provider
- **Operator names are tidied** rather than passed through as filed: corporate form, trailing registration addresses and the registered company in front of a trading name are all dropped, so "JET2.COM LTD" reads "Jet2.com"
- **airplanes.live is disabled by default.** It answers 403 to every request from any address, so it sat permanently red in the admin panel implying an outage and kept spending requests to re-learn the same answer
- **The event loop no longer stalls on the database.** Several hot paths made synchronous SQLite calls straight from coroutines, which stops reading every live feed until the database answers. Those now run in threads, the database runs in WAL mode with an explicit busy timeout, and the upstream poll areas are worked out once per cycle instead of three times
- **Public share links** for a receiver's live map: an unlisted read-only URL, revocable and replaceable, that needs no account
- **Two feeder faults fixed.** Attribution was recorded only on the record that won the freshness comparison, so the traffic nearest a customer's own receiver was exactly what vanished from their map; and a feeder's merge loop raised on its first pass and died, leaving the connection up and draining normally while contributing nothing

### v2.6.0 display, server-side routes, and the aggregator backend

Firmware:

- **Real aircraft silhouettes on the LCD.** Icons were two or three filled triangles, which cannot express a fuselage with a swept wing and a tailplane, so every airframe read as the same arrowhead. Each class is now a traced outline filled by a scanline polygon fill, with engine nacelles, a propeller arc on light singles, and a rotor on helicopters
- **Punctuation in the panel font.** The 5x7 font defined only `0-9`, `A-Z`, `>` and `-`, so everything else rendered blank: `ALT:3.4KFT` appeared as `ALT 3 4KFT` and coordinates lost their decimal points. A dropped decimal point does not read as a missing glyph, it reads as a different number
- **Screensaver rebuilt as a full departure board**, carrying every field the feed supplies: operator, callsign, route codes and full airport names, type, altitude both barometric and geometric, speed, track, vertical rate, distance, squawk and its meaning, signal, message count, hex, ADS-B/MLAT, age, country, and position. Emergency squawks (7500/7600/7700) render in red
- **Squawk codes are explained** rather than shown as four bare digits: the emergency codes, the routine conspicuity codes (1200/7000/2000/0000/7777), and discrete assignments
- **Operator names from the callsign** for roughly eighty ICAO airline prefixes, so a callsign identifies its airline even before a route lookup returns
- **Selectable pixel clock and bounce-buffer size** in the web UI, with the resulting refresh rate shown, for tuning the RGB panel against the artefacts described under [Known limitations](#known-limitations). Also a direct-draw mode that renders into the panel's own framebuffer instead of copying a frame each time
- **Table page scrolls properly.** A tap used to advance to the next page, which made the table unscrollable; swipe gesture thresholds were retuned with peak-excursion tracking so a scroll is no longer read as a page change
- **53 KB of internal RAM reclaimed** by moving the PNG decoder and the route cache into PSRAM, which is what the TLS handshake allocation failures were competing for

Data:

- **Routes are resolved by the server, not the device.** The ESP32 queried adsbdb itself at roughly 2.2 seconds of blocked network task per callsign, needing more contiguous internal RAM for the TLS handshake than was free. That is what the intermittent `PK verify failed 0x4290` errors actually were: an allocation failure reported as a certificate failure. The aggregator now attaches origin and destination to each aircraft in the response the device already fetches, so there is no extra connection, no handshake and no throttle. One resolution serves every customer who can see that flight, which is also less load on adsbdb than before
- **Fetch-phase instrumentation.** A slow refresh now reports where its time went, and a slow route lookup logs the largest free internal block at that instant

Backend (`server/`), see [Aggregator backend](#aggregator-backend):

- **Per-device receiver location**, so the aggregator polls upstream for each customer's sky rather than only the operator's. A device reports its own position on every request, so a receiver that moves follows itself
- **Receiver position inferred from the feed** for a feeder that never states one, from the radio horizon of the low aircraft it hears
- **Public share links**: an unlisted read-only URL for one receiver's live map, viewable by anyone the owner sends it to, with no account
- **Feeder client** for Debian and Windows in `server/tools/`, for receiver software that cannot push SBS out on its own
- **Devices and feeder stations can be renamed** from the account dashboard
- **Live "my feed" map** showing only what a customer's own receiver is reporting, with track-aligned icons, an altitude colour ramp, and full detail on click

### Also in v2.6.0

- **LCD Overview page**: adds a route (`RTE`) column next to the nearest-aircraft strip, sourced from the same route cache used by the Table and Map pages
- **LCD Table page**: redesigned as an airport-departure-board style layout — alternating row colours, a yellow callsign, a single-letter green `A` / red `M` source instead of the word ADSB/MLAT, and tightened columns that give the freed width to the route column
- **LCD Table page route column**: shows the richest airport name that fits the available width — full names, then city names, then a departure-board-style abbreviation (`LON STAN`), then raw ICAO/IATA codes — instead of only ever showing short codes
- **Browser Aircraft/Overview tables**: the small logo-badge column is replaced by the operator's full name; the badge itself now appears in the map popup instead
- **Browser Aircraft/Overview tables**: the FROM/TO column shows the full airport names (e.g. "London Heathrow Airport -> John F Kennedy International Airport") instead of raw codes
- **Browser map**: clicking a marker now opens a popup with the complete aircraft detail set (operator badge, registration, altitude, speed, squawk, category, signal, country, emergency state, and more), not just a short summary
- **Browser map**: clicking a row in the Aircraft/Overview table jumps to the map, zooms in on that aircraft, and opens its popup; click the same row again to stop tracking
- **Browser map**: a "Show 5 min trails" checkbox draws each aircraft's recent track as a line, with the tracked aircraft's trail highlighted
- **Route cache persistence**: resolved routes now survive a reboot, saved to SD (or LittleFS without a card) after each batch of new lookups and reloaded at boot, so a receiver that sees the same flights daily doesn't start every session cache-cold
- **LCD Table/Overview route column**: distinguishes a route that's still queued for lookup (`---`) from one adsbdb was actually asked about and had nothing on file (`NO ROUTE` / `NO RTE`) - the latter is expected for most private/GA registrations, which have no scheduled route to look up
- Adds `scripts/flash_remote.ps1`, which starts the `esp_rfc2217_server.py` serial-to-network bridge if it isn't already running, then builds and flashes over it — useful for driving a board's USB port from another machine on the network
- **New Marine section and LCD page**: live AIS vessel tracking, centred on the receiver's saved position. Adds a new browser admin page (provider selector, credential and radius settings, map, vessel table) and a new physical LCD page in the Overview/Table/Map/Radar/Marine swipe cycle. Selectable providers: [AISstream.io](https://aisstream.io) (free WebSocket push, no polling, the default), AISHub, MyShipTracking, and Datalastic (all three polled once a minute). Adds the `links2004/WebSockets` library dependency
- **New "2E0LXY Aggregator" data provider**: polls adsb.fi, airplanes.live and adsb.lol centrally on a shared backend cache instead of this device querying those public APIs directly; requires a per-device API key issued from an account at `adsb.2e0lxy.uk/account`, entered in a new "Aggregator API key" field on the Data API page
- **New "FlyItalyADSB" data provider**: community feed (1,800+ receivers), licensed CC BY-SA 4.0 with commercial use explicitly permitted up to 100 requests/minute; requires a free API key issued instantly by email, entered in a new field on the Data API page
- **Data API page**: every provider's help text now states its actual licensing/commercial-use terms (which ones require a key, which restrict to personal/non-commercial use, which explicitly permit commercial use) instead of leaving that undocumented
- **New idle screensaver**: after the panel is untouched for a configurable time (Display page, 1-120 minutes, off by default), shows a rotating single-aircraft display in an airport-departure-board style - operator badge, callsign, route, aircraft type, and a departing/arriving/en-route guess - restricted to aircraft within about 5 miles of slant range (distance and altitude combined) so it only ever shows something plausibly visible or audible overhead, not anything across the whole tracking radius. Any tap, swipe, or boot-button press dismisses it back to the page it interrupted
- **New multi-tenant aggregator backend** (`server/`): a separate FastAPI service for anyone self-hosting the "2E0LXY Aggregator" provider above - customer accounts, per-device API keys, an admin panel, feeder-key pooling across donated personal upstream credentials, and raw SBS/BaseStation feed ingestion from a customer's own receiver. See `server/README.md` for deployment; this is backend infrastructure, not part of the ESP32 firmware itself

### v2.5.1 correctness and hardening

- Corrects the RGB panel timings to the Waveshare Rev 4.0 reference and sets an explicit 16.5 MHz pixel clock, raising the refresh rate from 42 Hz to about 59 Hz
- Reports at boot whether the vertical-blank DMA restart is actually supported by the running sdkconfig
- Validates TLS certificates on every outbound HTTPS request instead of trusting any certificate
- Requires a per-boot request token on every state-changing web route, so cached Basic credentials alone can no longer drive the device from another site
- Persists staged-update metadata so firmware staged on SD before a reboot can still be verified and installed
- Evicts cached map tiles when the view changes or storage runs low, and deletes tiles that fail to decode
- Restores the previous Wi-Fi network automatically if new credentials fail to connect within 45 seconds
- Scales LCD brightness to the full 0-255 range instead of writing the raw percentage
- Reports genuine LCD map rebuild progress and keeps the admin interface responsive while tiles download
- Falls back to the release SHA256SUMS.txt when the GitHub API omits an asset digest
- Accepts signing keys up to RSA-4096 rather than only RSA-2048
- Stops compiling OpenSky credentials into the binary unless `ADSB_BAKE_CREDENTIALS` is defined
- Fetches the aircraft table only for pages that display it

### v2.5.0 SD storage and safer updates

- Detects the optional microSD card at boot and from a new Firmware-page rescan control
- Moves the physical OpenStreetMap tile cache to SD when a card is available, with automatic LittleFS fallback
- Holds up to 250 live aircraft records in PSRAM while reducing reserved internal RAM
- Parses provider responses directly from the network with field filtering instead of retaining a second full response copy
- Builds large aircraft web responses in PSRAM to prevent the earlier Map/Table/reboot memory peak
- Stages GitHub firmware on SD and verifies its ESP32 header, exact size, SHA-256 and RSA release signature before OTA installation
- Applies the same release integrity and signature checks to direct OTA when no SD card is fitted
- Reports card type, capacity, cache location, aircraft capacity and staged-update state in the admin interface
- Prevents Radar screen wrap and white DMA flecks by recovering RGB DMA on vertical blank and reducing full-frame redraw pressure

### v2.4.1 reliability fixes

- Rejects empty, malformed, undersized, and non-ESP32 firmware uploads without rebooting
- Handles Arduino-ESP32 raw POST callbacks safely before accessing multipart upload state
- Releases large aircraft-response buffers before route lookups to prevent TLS allocation failures
- Retries unsuccessful route lookups after five minutes instead of caching failures for six hours
- Validates numeric settings and credential lengths on both the browser and ESP32
- Reports live feed result, HTTP status, and request duration in **Data API**
- Deduplicates Wi-Fi scan results and fixes phone-width overflow on the Wi-Fi page
- Checks that release tags, firmware, README, installer, and manifest versions agree before publishing
- Removes the management password from serial output

## Quick start

1. Open the [online USB installer](https://2e0lxy.github.io/ESP32-ADS-B/) in desktop Chrome or Edge and connect the ESP32-S3 by USB.
2. Install the factory image, restart the receiver, and connect to the `ADSB_WIFI` setup network (open, no password) if no saved Wi-Fi is available.
3. Choose a 2.4 GHz Wi-Fi network. The LCD waits for Wi-Fi and then displays the receiver's LAN address.
4. Open that address, sign in with `admin` / `aircraft`, and immediately set a new password in **Device**.
5. Set the receiver latitude, longitude, radius, and zoom in **Map**, then choose an aircraft feed in **Data API**.

![Boot screen](assets/boot-screen-preview.png)

## Main features

- Live aircraft map on the 480 × 480 display and in the browser
- OpenStreetMap tiles cached on microSD when fitted, with automatic LittleFS fallback
- Up to 250 live aircraft records held in PSRAM without consuming the card's write life
- ADS-B and MLAT aircraft shown with distinct colours and heading markers
- Receiver latitude, longitude, and radius configurable from the web interface
- Receiver changes applied to the browser map, LCD map, API query area, and distance calculations
- Browser zoom saved to NVS and reused by the physical LCD map after reboot
- Full Overview aircraft table with operator names and all common provider fields
- Click any aircraft row to jump to the browser map, zoom in, and track it; toggle 5-minute movement trails on the map
- Selectable aircraft-data providers with editable API credentials
- Wi-Fi network scanning and connection management
- Local firmware upload plus automatic update checks from GitHub Releases
- Password-protected administration with a change-password page
- Persistent Map, Radar, and Table display modes selectable from the web interface or BOOT button
- Animated aircraft radar with four saved-radius range rings and red out-of-range rim targets
- First-time USB flashing from the GitHub Pages Web Serial installer
- Centred, wide-screen admin pages with receiver quick actions and live health summaries
- One-click radar range presets, Wi-Fi quality meter, recovery links, and privacy-safe diagnostic export
- Boot screen displays the management IP address after Wi-Fi connects
- Six physical LCD pages in a swipe cycle: Overview, Table, Map, Radar, Marine, and an idle departure-board screensaver
- Live AIS vessel tracking on the LCD and in the browser, from a choice of four marine providers
- Real aircraft silhouettes per airframe class, coloured by altitude and pointed along the track
- Route lookups resolved by the aggregator backend rather than by the device, so a callsign costs no TLS handshake on the ESP32
- Resolved routes cached to SD (or LittleFS) and reloaded at boot, so a receiver that sees the same flights daily never starts cache-cold
- Selectable pixel clock, bounce-buffer size, and direct-draw rendering for tuning the RGB panel
- Optional multi-tenant aggregator backend in `server/` with accounts, per-device keys, own-receiver feed ingestion, and public share links
- Real airline logos on the idle screensaver, cached on the receiver's own card after one fetch
- Up to six saved Wi-Fi networks, tried in turn when the usual one cannot be reached
- Operator name, full model name, radio callsign, country and silhouette resolved by the aggregator from offline ICAO lists

## Supported hardware

| Board | Panel | Touch | Backlight | PlatformIO env |
| --- | --- | --- | --- | --- |
| Waveshare ESP32-S3-Touch-LCD-4 Rev 4.0 | 480 × 480 | None | Full 0-255 PWM via the CH32 expander | `waveshare_esp32_s3_lcd_4` |
| Waveshare ESP32-S3-Touch-LCD-7 / -4.3 | 800 × 480 | GT911, swipe to change page | On/off only via the CH422G expander (no PWM on this board) | `ws_lcd_7_app` |

Panel geometry, pins, and the expander driver are all resolved from `src/board_config.h` and `src/boards/*.h`, so the same application source builds for either board. Set the environment's `board_build.*` and `-DADSB_BOARD_*` build flag in `platformio.ini` to switch boards; see the `## Build` section below for the exact commands.

## First login

After Wi-Fi connects, the LCD displays the address of the web interface. Open that IP address in a browser on the same network, or try `http://adsb-map.local/`.

| Setting | Initial value |
| --- | --- |
| Username | `admin` |
| Password | `aircraft` |
| Setup access point | `ADSB_WIFI` (open, no password) |
| Setup portal | `http://192.168.4.1/` |

Change the management password in **Device** after installation. The replacement password must contain at least eight characters.

## Web administration

Each sidebar entry opens a separate page. The footer on every page shows `Firmware (c) 2E0LXY D.Loxley 2026` and the installed version. The header carries the device and feed indicators, uptime, and the Wi-Fi signal level. Checking for a firmware update lives on the **Firmware** page only.

| Page | Live information | Main controls | Saved after reboot |
| --- | --- | --- | --- |
| Overview | Receiver health, traffic totals, provider, Wi-Fi, uptime, and full aircraft table | Refresh traffic, open Map/Radar, open Firmware | — |
| Map | Receiver position, range, zoom, OpenStreetMap tiles, and aircraft | Position, radius, centre, zoom, click-to-track, 5-minute trails | Yes |
| Aircraft | Every available aircraft field, operator name, source, age, signal, and emergency | Search, source filter, click a row to track it on the map | — |
| Display | Active LCD page, brightness, alert state, and map-tile rebuild state | Map/Radar/Table, brightness, range presets, zero-mile alert, refresh | Yes |
| Wi-Fi | SSID, four-level signal quality, IP, gateway, DNS, scan results, and up to six saved networks | Scan, copy address, connect to a different network, forget a saved one | Wi-Fi credentials |
| Data API | Selected provider, request health, aircraft count, latency, and credential state | Select feed, edit/clear credentials, refresh test | Yes |
| Marine | Live AIS vessel positions, connection state, vessel count | Provider selection, credential, tracking radius, browser vessel map and table | Yes |
| Firmware | Installed/latest version, update availability, and release status | GitHub OTA, local `.bin` upload, installer/release recovery links | Firmware only |
| Device | Identity, runtime, memory, network, display, and feed diagnostics | Password, diagnostic JSON, setup portal, reboot | Password |

### Overview

Connection, aircraft, provider, display, and firmware status at a glance, followed by the complete horizontally scrollable live-aircraft table. Quick actions refresh the feed, open the browser map, switch the LCD to Radar, or check GitHub for an update.

![Overview page](docs/screenshots/overview.png)

### Map

Live OpenStreetMap view of the received aircraft. Save a new receiver position, radius, and current browser zoom here; the same values immediately control the LCD map, provider query bounds, and aircraft-distance calculations and remain stored after reboot. Click a marker for its full detail popup, click a row on the Aircraft/Overview page to jump here and track that aircraft, or enable **Show 5 min trails** to draw each aircraft's recent track.

![Map page](docs/screenshots/map.png)

### Aircraft

Searchable live table containing operator name, ICAO address, callsign, registration, aircraft type, distance, coordinates, barometric/geometric altitude, speed, vertical rate, heading, squawk, category, ground state, data age, messages, signal, country, full-name route, source, and emergency state. Click a row to jump to the Map page and track that aircraft.

![Aircraft page](docs/screenshots/aircraft.png)

### Display

Select the physical Map, Radar, or Table page, set brightness, enable or disable the zero-mile alert, and request an immediate refresh. One-click range presets set 10, 25, 50, or 100 nautical miles. Radar mode uses the saved receiver position and radius, draws a moving sweep, and marks out-of-range aircraft in red at the rim. The page also reports LCD tile-cache rebuild progress after a position, range, or zoom change. The selected page remains active after reboot.

A **Screensaver** toggle and an idle-time field (1-120 minutes, off by default) control the idle screensaver: after the panel goes untouched for that long, it switches to a rotating departure-board style display of whichever aircraft are currently overhead (within about 5 miles of slant range - distance and altitude combined, so a jet at cruise directly above doesn't count as "overhead"), showing the airline's real logo where one is available, the callsign, route, full model name, radio callsign, and a departing/arriving/en-route guess. It shows whichever aircraft is nearest and reconsiders every ten seconds. The device's own address sits in the top corner, since this is the only page without a header. Any tap, swipe, or the boot button dismisses it back to the page it interrupted.

Everything on that page, and where it comes from:

| Line | Content | Source |
| --- | --- | --- |
| Logo tile | The airline's real logo, or three initials in a tinted square | Fetched once from the aggregator and cached on the microSD card |
| Photograph | A picture of the aircraft type, top right, opposite the logo | Fetched once per type from the aggregator and cached on the card. Only on the 800 x 480 board: reserving the space on the 480 x 480 one would truncate the route codes, which matter more |
| Operator | Trading name with corporate form removed, e.g. "Ryanair" not "RYANAIR DAC" | Resolved by the aggregator from the operator list, else the feed, else a compiled-in prefix table |
| Route | Airport codes as the headline, full names on a later row | adsbdb, resolved by the aggregator and cached |
| Type line | Full model, registration and callsign, e.g. "Boeing 737 Max 8  EI-IHG  RYR282D" | Model from the ICAO type list; the four-character designator when unresolved |
| `ALT` / `SPD` / `TRK` / `VR` | Altitude, speed, track and vertical rate | The feed |
| `RADIO` | Radio telephony callsign | The operator list; shown only when it differs from the operator name |
| `SQK` and its meaning | Squawk, and what the code signifies | Emergency and conspicuity codes are named; a discrete code is labelled as ATC-assigned |
| Position row | Hex, source, age, country, coordinates, both altitudes | The feed, with country filled in from the ICAO hex range when absent |

**About the `RADIO` line.** That is the callsign spoken on the air, which is frequently nothing like the airline's name: Jet2 is "Channex", British Airways is "Speedbird", Aer Lingus is "Shamrock", TUI is "Tomjet". Callsigns are assigned by ICAO and tend to outlive rebrands, which is why Jet2 still answers to the name of Channel Express, the cargo airline it grew out of. It is the name you would hear if you were listening to air traffic control alongside the display, so it is shown when it adds something and suppressed when it merely repeats the operator name.

**Panel tuning.** Three further controls exist for the RGB panel itself, because the right values depend on the individual board and on what else is competing for the PSRAM bus. **Pixel clock** (9-21 MHz) sets the panel clock and shows the refresh rate each choice produces. **Bounce buffer** sets how many scanlines of internal DMA RAM the panel driver refills ahead of the scan; larger values are more tolerant of a busy bus but take internal RAM away from the TLS handshake. **Direct draw** renders straight into the panel's own framebuffer instead of composing a frame and copying it. Changing the pixel clock or the bounce buffer reboots the device. See [Known limitations](#known-limitations) for what these are for.

![Display page](docs/screenshots/display.png)

### Wi-Fi

View the active connection, signal quality, LAN address, gateway, and DNS server; copy the management address; scan nearby networks; and move the receiver to a different 2.4 GHz Wi-Fi network. The quality bar and the reading in the header are coloured by RSSI in four bands: green at or above -60 dBm, blue to -70, amber to -80, red below.

**Saved networks.** Every network that connects successfully is remembered, newest first, up to six, and listed with a Forget button. If the usual one cannot be reached at boot the receiver tries the others in turn, so moving it between a house and a club site needs no password retyped. Forgetting the network currently in use does not disconnect it; it stops the receiver rejoining it later.

![Wi-Fi page](docs/screenshots/wifi.png)

### Data API

Select an aircraft provider and replace or clear the credentials required by that provider; the help text below the dropdown explains that provider's licence terms, commercial-use restrictions, and where to get a personal API key if one applies (see [Aircraft-data providers](#aircraft-data-providers)). Live feed health shows the last result, HTTP status, request time, aircraft count, and credential state, with a manual refresh test for troubleshooting.

![Data API page](docs/screenshots/api.png)

### Marine

Live ship positions, centred on the same receiver position used for aircraft. Select a provider, enter its credential and a tracking radius (5-250 nm; typical VHF AIS coastal range is 20-40 nm), then save. The page shows connection status, vessel count, time since the last update, a Leaflet map, and a searchable vessel table (name, MMSI, type, navigational status, speed, course, heading, distance).

| Provider | Credential | Access | Notes |
| --- | --- | --- | --- |
| AISstream.io | Free API key | Push (WebSocket) | Recommended default. No polling interval; vessels appear as they broadcast. |
| AISHub | Username | Free, contribution-based | Requires an AISHub account that shares your own local AIS receiver's data with their network; the username alone, without a contributing receiver, may return nothing. Polled once a minute (AISHub rejects more frequent requests). |
| MyShipTracking | API key | Freemium, billed per vessel returned | Polled once a minute. |
| Datalastic | API key | Freemium, free trial | Radius is capped at 50 nm by the provider regardless of the value set here. Polled once a minute. |

Two marine providers from common comparison lists are deliberately not offered: **NavAPI/Seametrix**'s public API covers ports, sea routes, and SECA zones only, with no vessel-position endpoint at all; **VesselFinder**'s area-based live feed ("LiveData") is a custom flat-fee subscription product requiring sales contact, not a self-service bounding-box API like the others.

### Firmware

Install a local `.bin` image, check GitHub for a new release, or install the latest release directly. The green update button pulses when a newer semantic version is available. With microSD present the download is staged there; otherwise direct OTA is used. Both **GitHub** routes require the published size, SHA-256 digest, RSA signature and ESP32 image header to validate before installation. A **local `.bin` upload is not signature-checked** — it is validated only for the ESP32 image header and a minimum size, so upload only images you built or trust. The page reports card state and has a manual rescan control.

![Firmware page](docs/screenshots/firmware.png)

### Device

Review device identity, uptime, memory, network, display, and feed status; download a privacy-safe diagnostic JSON file; change the management password; reopen the Wi-Fi setup portal; or reboot the ESP32. The diagnostic export deliberately excludes Wi-Fi passwords, API secrets, and the management password.

![Device page](docs/screenshots/device.png)

## Aircraft-data providers

Selecting a provider in **Data API** shows that provider's own help text below the dropdown, covering its licence, whether it permits commercial use, and where to get a personal API key if one is offered or required.

| Web selection | Endpoint pattern | Credentials | Notes |
| --- | --- | --- | --- |
| 2E0LXY Aggregator (recommended) | `/v1/aircraft?lat=...&lon=...&radius=...` | Required API key | Our own backend at `adsb.2e0lxy.uk`; polls adsb.fi/airplanes.live/adsb.lol centrally on a shared cache so this device never queries those public APIs directly. Sign up and register the device at `adsb.2e0lxy.uk/account` to get a key. |
| OpenSky Network | `/api/states/all` | Optional OAuth client ID and secret | Uses a receiver-centred bounding box; falls back to anonymous access when credentials are blank. OpenSky is an academic/research network - its terms require a separate paid licence for any for-profit or commercial use. |
| adsb.fi Open Data | `/api/v3/lat/.../lon/.../dist/...` | None | Free/open data endpoint restricted to personal, non-commercial use (1 request/second limit, no redistribution) - see `github.com/adsbfi/opendata`. |
| airplanes.live | `/v2/point/...` | None | ADS-B Exchange v2-compatible response. Personal/educational use only; blocks cloud/datacenter IP ranges and asks that anything beyond personal use be arranged via `contact@airplanes.live`. |
| adsb.lol Open API | `/v2/point/...` | None | Free/open API licensed under the Open Database Licence (ODbL) - commercial use is explicitly permitted provided adsb.lol is credited as the data source. |
| ADSB One / API archive | `/v2/point/...` | None | Experimental legacy-compatible source; its server may reject requests, so keep another provider available. |
| ADS-B Exchange via RapidAPI | `/v2/lat/.../lon/.../dist/...` | RapidAPI key | Paid commercial subscription and key required. |
| FlyItalyADSB | `/v2/lat/.../lon/.../dist/...` (kilometres, not nm) | Required API key | Community feed (1,800+ receivers, Mediterranean-focused). Licensed under CC BY-SA 4.0 - commercial use is explicitly permitted up to 100 requests/minute with attribution. Free key issued instantly by email - see `flyitalyadsb.com/api-documentation`. |

The ESP32 stores credentials in Preferences/NVS. Existing secrets are never returned by the status API, and submitting an empty credential field preserves the stored value unless **Clear** is selected.

## GitHub OTA updates

The firmware checks the latest release from `2E0LXY/ESP32-ADS-B` at startup and every six hours. If a newer version exists, the footer button flashes green. Pressing it downloads the release firmware and detached signature. The ESP32 validates the exact asset size, SHA-256 digest, RSA signature and image header before writing the inactive OTA slot, and reboots only after a successful complete write. With microSD fitted, the verified image is staged on the card first.

Remote receivers do not need inbound access or to be on the maintainer's LAN. They make an outbound HTTPS request to GitHub. The notification is the pulsing green button in the receiver's local admin interface; this firmware does not send email, SMS, or a phone push notification.

Do not remove power while an update is being installed. The `default_16MB.csv` partition layout supplies two application slots for OTA updates.

Creating and pushing a tag such as `v2.5.0` runs `.github/workflows/release.yml`, builds both upgrade and factory images, signs the OTA image with the protected `FIRMWARE_SIGNING_KEY` repository secret, generates SHA-256 checksums, and attaches them to a GitHub Release. A successful release then triggers `.github/workflows/pages.yml`, which publishes the same factory asset to the online USB installer.

| Release asset | Use |
| --- | --- |
| `ESP32-ADSB-firmware.bin` | Normal OTA or local web update; preserves settings |
| `ESP32-ADSB-firmware.bin.sig` | Detached RSA/SHA-256 signature required by GitHub OTA |
| `ESP32-ADSB-factory.bin` | First installation or full recovery; clears saved settings |
| `SHA256SUMS.txt` | Integrity hashes for the published firmware files |

## Build

Install [PlatformIO](https://platformio.org/), clone the repository, and run:

```powershell
pio run
```

This builds the default `waveshare_esp32_s3_lcd_4` environment (the 480 × 480 WS4 board). For the 800 × 480 WS7 board, select its environment explicitly:

```powershell
pio run -e ws_lcd_7_app
```

The WS7 build uses the prebuilt Arduino ESP-IDF libraries and is quick. Those libraries already enable `CONFIG_LCD_RGB_RESTART_IN_VSYNC`, so the panel's DMA-desync recovery (`restartAtNextVsync()`) works as shipped and the boot log prints `RGB vsync restart supported: yes`. An earlier revision set `custom_sdkconfig` to turn that option plus the RGB/GDMA IRAM-safe flags on, in the mistaken belief the restart was a no-op. It changed nothing and boot-looped the board with `Cache disabled but cached memory region accessed`, because an IRAM-safe RGB ISR cannot read a framebuffer that lives in PSRAM. Those options are not set. The frame roll remains an open defect - see the comment in `platformio.ini`.

The OTA image is generated at:

```text
.pio/build/waveshare_esp32_s3_lcd_4/firmware.bin
.pio/build/ws_lcd_7_app/firmware.bin
```

To build, flash, and (optionally) keep a serial bridge running automatically from another machine on the network, see `scripts/flash_remote.ps1`.

## USB installation

For a new board or recovery install, open the [online USB firmware installer](https://2e0lxy.github.io/ESP32-ADS-B/) in desktop Chrome or Edge. It uses Web Serial to identify the ESP32-S3 and writes the complete factory image. A factory install clears saved Wi-Fi, API credentials, location, zoom, display mode, and admin password.

![Online USB installer](docs/screenshots/installer.png)

For later updates, use the device administration interface's Firmware page or flashing GitHub update button; those OTA paths preserve settings.

### PlatformIO alternative

Connect the board by USB, then run:

```powershell
pio run -t upload
```

To select the development board explicitly on Windows:

```powershell
pio run -t upload --upload-port COM25
```

If the board does not enter download mode, hold **BOOT**, tap **RESET**, begin the upload, and then release **BOOT**.

## Physical display behaviour

- Short BOOT-button press (or a swipe on the WS7 touch panel): cycles the LCD through Overview, Table, Map, Radar, and Marine, and saves the selection
- Screensaver page: appears on its own after the idle time set on the **Display** page, showing the nearest aircraft overhead as a departure board with the airline's logo, full model name and radio callsign, and the device's address in the top corner
- Overview page: map on the left with a compact nearest-aircraft strip (callsign, distance, altitude, route) on the right
- Table page: airport-departure-board style list with operator badge, callsign, distance, source, direction, altitude, and the fullest airport name that fits the panel width
- Marine page: the same base map with live AIS vessel positions plotted as heading-oriented ship markers; shows vessel count and AIS connection state, or `AIS NOT CONFIGURED` until an API key is saved in the Marine admin page
- Aircraft data refresh: every 30 seconds or on demand
- ADS-B aircraft: cyan/operator-coloured symbol
- MLAT aircraft: violet/red symbol
- AIS vessel positions: event-driven over a persistent WebSocket, not polled - a quiet Marine page in low-traffic water is normal
- Route lookup: public ADSBDB callsign endpoint, cached for six hours and persisted to SD (or LittleFS without a card) so a reboot doesn't start cold; the Table page shows the fullest name that fits (full name, then city, then a departure-board-style abbreviation, then the raw code), while the Overview and Map pages show the raw code. `NO ROUTE` / `NO RTE` means adsbdb was asked and had nothing on file (typically a private/GA registration), distinct from `---` which means still queued
- Zero-mile aircraft: optional 200 ms buzzer alert
- OSM failure: falls back to the built-in radar-style map

## OpenStreetMap usage

Map data is © OpenStreetMap contributors. The browser and LCD show attribution. The LCD requests only the tiles visible for the configured receiver area, identifies this firmware in its User-Agent, and caches tiles on microSD when available or LittleFS otherwise. Do not modify the firmware to bulk-download tiles from the public OpenStreetMap tile service.

## Known limitations

- **Frame roll and flickering scanlines on the 800 x 480 panel.** The top few
  lines of the frame can appear at the bottom, and text can be cut mid-word
  with the halves at different offsets. Both are the same fault at different
  magnitudes: the RGB panel's bounce buffer misses its refill deadline while
  the CPU saturates the same PSRAM bus, so the scan position slips. The
  panel's DMA recovery on vertical blank is supported and does fire; the
  artefacts happen anyway.

  The pixel clock, bounce buffer and direct-draw controls on the **Display**
  page are there to trade this off against the internal RAM the TLS handshake
  needs. A 40-scanline bounce buffer clears the display but leaves too little
  contiguous internal RAM for HTTPS; 20 or less leaves HTTPS working but the
  artefacts return. Moving route lookups to the backend and reclaiming 53 KB
  of internal RAM widened that margin but did not remove the trade. The real
  fix is to stop `present()` copying 768 KB from PSRAM to PSRAM every frame
  and draw only the regions that changed, which is not done.

  The 480 x 480 board is not affected in the same way; it has a smaller frame
  and more headroom.

- **Internal RAM, not PSRAM, is the constraint on this board.** There is 8 MB
  of PSRAM and the largest free *internal* block settles at about 31,700
  bytes, which is what a TLS handshake competes for. That single figure
  explains several design decisions that otherwise look odd: why route
  lookups moved to the server, why the screensaver's decoded logo is staged
  in PSRAM and never allowed to fall back to internal RAM, and why a logo is
  only fetched when at least 28 KB of contiguous internal memory is free.

  That 28 KB threshold is measured rather than cautious. The aircraft fetch
  completes a handshake at 31,700 every thirty seconds with a 39 KB response
  body, so a logo at a fifth of that size on the same task faces conditions
  the feed already survives. An earlier attempt used 48 KB, which is more
  than this board ever has free, so the guard could never pass and no logo
  was ever fetched. If you change it, read the number off
  `heap[fetch-start]` in the serial log rather than guessing.

- **Do not set the RGB panel sdkconfig options via `custom_sdkconfig`.**
  `CONFIG_LCD_RGB_RESTART_IN_VSYNC` is already enabled in the stock prebuilt
  Arduino libraries, and enabling `CONFIG_LCD_RGB_ISR_IRAM_SAFE` boot-loops
  this board with a cache-disabled panic. See the comment in
  `platformio.ini`.

- **A local `.bin` upload is not signature-checked.** It is validated for the
  ESP32 image header and a minimum size only. GitHub OTA is fully verified;
  a hand-uploaded image is trusted.

- **The admin interface has no TLS** and is intended for a trusted local
  network.

- **The backend has open gaps before commercial launch**, listed at the end of
  [`server/README.md`](server/README.md): feeder ingestion is authenticated
  only by the per-device port, there is no email verification or password
  reset, the aggregator cache is in-process so it cannot be scaled to a
  second instance as written, and map tiles still come from the public
  OpenStreetMap tile service.

## Aggregator backend

`server/` holds a separate FastAPI service, not part of the ESP32 firmware. It is what the **2E0LXY Aggregator** provider in **Data API** talks to, and anyone can self-host it. Full deployment instructions, configuration and known gaps are in [`server/README.md`](server/README.md); this is what it does.

**Why it exists.** Every free ADS-B API restricts how often a client may ask and, in several cases, forbids commercial use outright. One backend polling on behalf of many receivers stays inside those limits where a fleet of devices each querying directly would not. It also removes work the ESP32 is poorly suited to: a TLS handshake per callsign needed more contiguous internal RAM than the device had free.

| Function | What it does |
| --- | --- |
| Central polling | Polls adsb.fi, airplanes.live and adsb.lol into one shared in-process cache, deduplicating by ICAO hex and preferring the freshest report of each aircraft |
| Per-device polling areas | Polls the sky each registered receiver is actually under, merging receivers that are close together into one area, rotating across areas past a configurable limit so free APIs are not asked for the whole hemisphere |
| Source backoff | A source that starts failing is retried on a widening interval instead of every cycle, and stops writing a warning per attempt |
| Route resolution | Resolves callsign to origin, destination, full airport names and operating airline via adsbdb, caching hits for six hours and misses for thirty minutes, and attaches the result to each aircraft in the device's own response |
| Customer accounts | Email and password signup, per-account device list, per-device API keys that are shown once and stored hashed |
| Admin panel | Separate admin login, account list, usage log, and per-source feed health |
| Feeder-key pooling | An account can donate an upstream credential it already holds; each poll cycle round-robins across the pool, so no one credential carries more than its owner's allowance |
| Own-receiver feed ingestion | A customer's receiver pushes its raw SBS/BaseStation output to a dedicated TCP port; those aircraft join the shared cache and are served to every device like any upstream's |
| Receiver position inference | For a feeder that never states where it is, estimates the position from the radio horizon of the low aircraft it hears |
| My-feed map | A live map of only what one customer's own receiver is reporting, with track-aligned icons, an altitude colour ramp, and every field the feed carries on click |
| Public share links | An unlisted read-only URL for that map, for anyone the owner sends it to, with no account needed; revocable and replaceable |
| Operator and type enrichment | Fills in operator name, telephony, IATA code, aircraft model, country from the ICAO hex range, and special livery from offline lists covering 6,008 operators and 2,735 type designators |
| Aircraft silhouettes | Resolves each aircraft's shape from its real ICAO class and engine configuration and sends it with the aircraft, so the device is not limited to the 91 callsign prefixes it can carry itself |
| Aircraft photographs | Finds a licence-free photograph of each aircraft type, crops it to the panel's band, and caches it. CC0 and Public Domain Mark only, so nothing needs crediting |
| Airline logos | Fetches each operator's logo from logo.dev once for the whole deployment, keeps it on disk beside the database, and serves it from `/logo/callsign/<callsign>.png`, so a viewer never contacts logo.dev and the token never leaves the server |
| Feeder client | `server/tools/` has a standard-library Python forwarder with a systemd unit, a Windows launcher, and a `--check` mode, for receiver software that cannot push SBS out on its own |

**Public share links.** The owner presses Create link in the feeder table of the account dashboard and gets a URL of the form `/share/<token>`. The token is 24 random bytes and is the entire credential, so the link is unlisted rather than access-controlled: anyone holding it can view the map, which is the point. Pages are served with `X-Robots-Tag: noindex` so a link pasted somewhere public does not become searchable. A link holder sees the station name and its aircraft, and nothing else: no account, no API key, no feeder port, no other device. Revoking clears the token, so the old URL stops resolving; New link mints a different one. Worth knowing before sharing: a map of what one station hears implies roughly where that station is, so this is not a way to publish a feed anonymously.

**Operator, type and country lookups.** `server/reference/` holds offline lists that fill in what the feeds leave out: operator name, radio telephony and IATA code from the callsign prefix, aircraft manufacturer and model from the type designator, country from the ICAO hex address range, and special liveries by registration. The device gets all of it attached to the aircraft it was already fetching.

The most useful part is the silhouette. The firmware can only carry 91 hand-written designator prefixes, so anything outside them drew a generic shape. The server derives the shape from real class and engine data for 2,735 designators and sends it, and the device prefers it over its own guess while keeping that guess as the fallback for anyone using a different provider.

That split is also a licensing decision: the provenance of those lists is not established, so they stay server-side and out of every released binary and the USB installer. `server/reference/README.md` records what each file is, why the work-in-progress type list is deliberately unused, and what needs resolving before commercial launch.

**What the aggregator attaches to each aircraft.** These arrive in the `/v1/aircraft` response the device already fetches, so none of them costs an extra request:

| Field | Content |
| --- | --- |
| `shape` | The silhouette, resolved from the type's real ICAO class and engine configuration |
| `type_name` | Manufacturer and model, e.g. "Boeing 737 Max 8" |
| `type_class`, `type_engines` | The underlying class and engine string the shape came from |
| `ownOp` | Operator trading name, only when the feed did not supply one |
| `telephony` | Radio callsign, e.g. "Channex" for Jet2 |
| `operator_iata` | Two-letter IATA code where the operator has one |
| `cou` | Country, from the ICAO hex address range, only when the feed did not supply one |
| `livery` | Special colour scheme by registration, where one is recorded |
| `route` | Origin, destination, both airports' full names and cities, and the operating airline |

**If you add a field here, add it to the firmware's filter as well.** The ESP32 parses that response through an ArduinoJson filter listing every field it keeps, to avoid holding a second copy of a 39 KB body in memory. A field absent from that list is discarded during parsing, before any code that reads it runs. This is not hypothetical: `shape`, `type_name` and `telephony` were all sent, read and displayed correctly in code, and silently dropped in transit, for exactly this reason. The list is the `fields[]` array in `fetchAdsbV2Aircraft()` in `src/main.cpp`.

**Aircraft type photographs.** `/aircraft-photo/B738.png` returns a photograph of that type, cropped to the panel's 5:3 band and quantised to a 256-colour palette, or 404 when there is no licence-free one. The caller passes only the designator; the model name the search needs comes from the reference lists here, which is the point of the device not carrying them.

**Restricted to CC0 and Public Domain Mark, and that decides the source.** Wikimedia Commons has better aircraft photography, but its civil aircraft photos are effectively all CC BY-SA - measured across A320, B738, B38M, C172, AT76 and SR22, none of which had an attribution-free option, because only military and government photographs there are public domain. Openverse aggregates Flickr and others and lets the search itself be filtered by licence, and CC0 and PDM waive attribution entirely: no credits page to maintain, no share-alike question, nothing to carry into a commercial product. `AIRCRAFT_PHOTOS=0` turns the feature off.

**Choosing a usable photograph is the substance, not fetching one.** Searching by model name returns engine close-ups, cockpits, cabins, diecast models and museum pieces alongside aircraft. Candidates are rejected on a keyword list, on being smaller than 480 px wide, on any aspect ratio outside 1.2 to 2.4 since aircraft are photographed landscape, and on a title that never mentions the model, because full-text search happily matches an airport article that mentions a 737. Against live results that keeps nine or ten of every ten and rejects exactly the engine and detail shots.

**Nothing here makes the device wait.** A type with no cached photograph is queued and answered "not yet"; the picture appears on a later request. The first version searched, downloaded and cropped while the device held the connection open, which took longer than its fifteen-second read timeout and failed with `HTTPC_ERROR_READ_TIMEOUT` - so no photograph ever arrived and the work was thrown away each time. Same shape as the route resolver, for the same reason.

Licence, creator, title and source URL are recorded beside every cached image and served at `/aircraft-photo/credits`. Neither licence requires it. It exists because being unable to say where a picture came from is its own problem, and a claim of "licence-free" should be checkable rather than asserted.

**Airline logos.** `/logo/callsign/RYR2BH.png` and `/logo/airline/RYR.png` return the operator's logo, or 404 when there is not one, which every caller answers by drawing its own initials badge instead.

Lookup is by airline domain rather than by company name. That is not a style choice: on logo.dev's name path `fallback=404` is ignored and a generated monogram comes back with `200 OK`, so an airline we cannot match is indistinguishable from one we can and the cache fills with monograms we could draw ourselves. On the domain path the 404 is real. The ICAO prefix table is keyed to match the firmware's own operator list so the panel and the browser agree on who is flying, and every domain in it was checked against the live API rather than assumed.

A logo is fetched once per deployment, written atomically so a reader never sees a half-written file, and served from disk thereafter. Concurrent requests for the same logo make one upstream call. A 404 is remembered for a week, so a newly added airline is not re-asked on every page view. A transport error is deliberately not remembered, since a network blip is not the same as "this airline has no logo". Set `LOGO_DEV_TOKEN` to enable it; left unset, every logo reports as missing and the badges fall back to initials.

The free tier requires an attribution link for commercial use, so there is one on every page that shows a logo. Do not remove it without moving to a paid plan.

**Deployment shape.** One Docker container behind a reverse proxy, SQLite in a mounted volume, host networking so the feeder port range needs no per-port NAT rule. Schema changes for nullable columns and missing indexes are applied at boot; anything beyond that needs a hand-written migration.

## Security notes

- All web-management and JSON API routes use HTTP Basic authentication.
- Every state-changing route additionally requires an `X-ADSB-Token` header carrying a token regenerated at each boot. This blocks cross-site requests that would otherwise ride on cached Basic credentials.
- Outbound HTTPS and the AIS WebSocket connection both validate certificates against the ESP-IDF root bundle. Build with `-DADSB_TLS_INSECURE=1` only if that bundle is unavailable in your toolchain.
- Change the initial password before placing the receiver on a shared network.
- The admin interface is intended for a trusted local network and does not provide TLS.
- Wi-Fi, provider, and AIS credentials remain in ESP32 NVS and are excluded from Git.

## Diagnostics and troubleshooting

- If the boot screen says **Wi-Fi connecting**, wait for the LAN address before opening the admin interface. `192.168.4.1` is only the temporary setup portal address.
- If Wi-Fi fails, connect to the `ADSB_WIFI` access point (open, no password) and open `http://192.168.4.1/`.
- If the LCD says **Rebuilding LCD map**, leave the receiver powered while it downloads and caches tiles for the newly saved position, range, or zoom.
- If aircraft stop updating, open **Data API**, run **Refresh / test feed**, and check the returned HTTP status and latency.
- The browser map uses Leaflet and OpenStreetMap tiles loaded from the internet. On an isolated network the **Map** page shows a fallback message; every other page, and the LCD map, still work from cached tiles.
- For support, download the JSON file from **Device → Download diagnostics**. It contains useful runtime state without passwords or API secrets.
- For recovery, use the online USB installer. A factory flash erases configuration, while normal OTA firmware preserves it.

## Refreshing the documentation screenshots

With the receiver online, install Playwright, set `NODE_PATH` if required by the local Node installation, and run:

```powershell
$env:ADSB_SCREENSHOT_URL = "http://receiver-ip/"
$env:ADSB_SCREENSHOT_USERNAME = "admin"
$env:ADSB_SCREENSHOT_PASSWORD = "your-password"
node scripts\capture_admin_screenshots.cjs
```

The script captures every current admin section plus the public USB installer into `docs/screenshots/`. It waits for live receiver data and map tiles, reports browser errors, and replaces the visible SSID with `Home Wi-Fi` before saving documentation images.

## Copyright

Firmware (c) 2026 2E0LXY / D. Loxley. All rights reserved. No open-source licence is granted unless a `LICENSE` file is added to the repository.

## Build flags

| Flag | Default | Effect |
| --- | --- | --- |
| `ADSB_ENABLE_TOUCH` | `0` | The Rev 4.0 480 x 480 panel has no touch controller. Set to `1` to probe the GT911 at boot and poll it each loop. |
| `ADSB_TLS_INSECURE` | `0` | Set to `1` to skip TLS certificate validation. Only for toolchains without the ESP-IDF certificate bundle. |
| `ADSB_BAKE_CREDENTIALS` | undefined | Compiles OpenSky credentials from `credentials.json` into the image. Never define this for a build you intend to publish; compiled-in secrets are recoverable with `strings firmware.bin`. |
| `DISPLAY_DIAGNOSTIC` | undefined | Replaces normal operation with a colour-cycle panel test. |

Serial ports are no longer hard-coded. Select one per invocation:

```powershell
pio run -t upload --upload-port COM25
```

## Panel timings

The RGB timings come from the Waveshare Rev 4.0 reference, retained at
`docs/hardware-reference-rev3-ST7701.h`: HPW 8, HBP 10, HFP 50, VPW 2, VBP 18,
VFP 8, with an explicit 16.5 MHz pixel clock giving roughly 59 Hz over the
548 x 508 total. If the panel shows tearing or a horizontal offset, these
constants at the top of `src/main.cpp` are the first thing to adjust.
