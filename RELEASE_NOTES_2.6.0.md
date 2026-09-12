# v2.6.0

Display work on the 800 x 480 panel, route lookups moved off the device, and
the aggregator backend that makes that possible.

## Display

- **Real aircraft silhouettes.** Icons were two or three filled triangles,
  which cannot express a fuselage with a swept wing and a tailplane, so every
  aircraft read as an arrowhead. Each airframe class is now a traced outline
  filled by a scanline polygon fill, with engine nacelles, a propeller arc on
  light singles, and a rotor on helicopters. Altitude colours are unchanged.
- **Punctuation in the panel font.** The 5x7 font defined only `0-9`, `A-Z`,
  `>` and `-`; everything else rendered blank. `ALT:3.4KFT` was displayed as
  `ALT 3 4KFT` and coordinates lost their decimal points - a dropped point
  does not read as a missing glyph, it reads as a different number.
- **Screensaver rebuilt as a departure board.** Operator tile, route codes as
  the headline, and every field the feed carries: altitude, speed, track,
  vertical rate, distance, squawk, signal, message count, full airport names,
  hex, ADS-B/MLAT, age, country, position, and both barometric and geometric
  altitude. Emergency squawks (7500/7600/7700) render in red.
- **Squawk codes are explained** rather than shown as four bare digits: the
  emergency codes, the routine conspicuity codes, and discrete assignments.
- **Operator names from the callsign** for about eighty ICAO airline
  prefixes, so a callsign identifies its airline before any route lookup
  returns.
- **Selectable pixel clock, bounce-buffer size and direct-draw rendering**,
  with the resulting refresh rate shown. See Known issues.
- **The Table page scrolls.** A tap advanced to the next page, which made the
  table unscrollable. Swipe thresholds were retuned with peak-excursion
  tracking so a scroll is no longer read as a page change.

## Data

- **Routes are resolved server-side.** The device queried adsbdb itself, at
  about 2.2 s of blocked network task per callsign and needing more contiguous
  internal RAM for the TLS handshake than was free - which is what the
  intermittent "PK verify failed 0x4290" failures actually were. The
  aggregator now attaches the route to each aircraft in the response the
  device already fetches. One resolution serves every customer who can see
  that flight, so it is also less load on adsbdb than before.
- **53 KB of internal RAM reclaimed** by moving the PNG decoder and route
  cache into PSRAM - the same RAM the TLS handshake was failing to get.
- **Fetch-phase instrumentation.** A slow cycle now prints where its time
  went, and slow route lookups log the free internal block at that instant.

## Backend

- **Per-device receiver location.** The aggregator polls the sky each
  registered receiver is actually under, merging nearby receivers into one
  area. A single global home position meant a customer anywhere else queried
  the cache correctly and got nothing.
- **Receiver position inferred from the feed** for a feeder that never states
  one, from the radio horizon of the low aircraft it hears.
- **Public share links.** An unlisted read-only URL for one receiver's live
  map, viewable by anyone the owner sends it to with no account. Revocable
  and replaceable; a link holder sees the station name and its aircraft and
  nothing else.
- **Live "my feed" map** of only what a customer's own receiver reports, with
  track-aligned icons, an altitude colour ramp and full detail on click.
- **Devices and feeder stations can be renamed** from the dashboard.
- **Feeder client** for Debian and Windows in `server/tools/`, for receivers
  whose software cannot push SBS out on its own. Standard-library Python,
  systemd unit, Windows batch launcher, and a `--check` mode that tests both
  ends and names the failure.
- **Feed attribution fixed.** An aircraft a customer's own receiver was
  tracking vanished from their map the moment an upstream API reported it
  fractionally fresher - so the traffic nearest the receiver was exactly what
  disappeared. Attribution is now recorded for every source that reports an
  aircraft, independently of whose values win the freshness comparison.
- **The event loop no longer stalls on the database.** Several hot paths made
  synchronous SQLite calls straight from coroutines, which stops reading
  every live feed until the database answers. Those now run in threads, the
  database runs in WAL mode with an explicit busy timeout, and the poll
  regions are worked out once per cycle instead of three times.
- **A feeder's merge loop no longer dies silently.** It raised on its first
  pass and stopped, leaving the connection up and draining normally while
  contributing nothing - a feed that looked healthy in the logs and an empty
  map for the customer.

## Known issues

- **The frame roll and flickering scanlines are not fixed.** The panel's DMA
  recovery works and fires; the artefacts happen anyway. Cause is the
  bounce-buffer refill missing its deadline while the CPU saturates the same
  PSRAM bus, so the scan position slips. A 40-scanline bounce buffer clears
  the display but starves the TLS handshake of contiguous internal RAM; 20 or
  less keeps HTTPS working and the artefacts return. Moving routes to the
  backend and reclaiming 53 KB widened that margin without removing the
  trade. The real fix is removing `present()`'s per-frame 768 KB
  PSRAM-to-PSRAM copy by drawing only the regions that changed. The pixel
  clock and bounce-buffer settings are there to test the trade-off, not to
  cure it.
- Do not set `CONFIG_LCD_RGB_RESTART_IN_VSYNC` and the IRAM-safe RGB/GDMA
  options via `custom_sdkconfig`. It boot-loops this board. See the comment
  in `platformio.ini`.
- A local `.bin` upload is still validated only for the ESP32 image header
  and a minimum size, not for a signature. GitHub OTA is fully verified.
