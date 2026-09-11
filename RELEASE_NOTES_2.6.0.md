# v2.6.0 — draft

**Not released.** Version strings are bumped and the release check passes,
but nothing is merged to `main` and no tag is pushed. Cutting the release is
a deliberate, separate step — see "Before releasing" below.

## Display

- **Real aircraft silhouettes.** Icons were two or three filled triangles,
  which cannot express a fuselage with a swept wing and a tailplane, so every
  aircraft read as an arrowhead. Each airframe class is now a traced outline
  filled by a scanline polygon fill, with engine nacelles, a propeller arc on
  light singles, and a rotor on helicopters. Altitude colours are unchanged.
- **Screensaver rebuilt as a departure board.** Operator tile, route codes as
  the headline, and every field the feed carries: altitude, speed, track,
  vertical rate, distance, squawk, signal, message count, full airport names,
  hex, ADS-B/MLAT, age, country, position, and both barometric and geometric
  altitude. Emergency squawks (7500/7600/7700) render in red.
- **Punctuation in the panel font.** The 5x7 font defined only `0-9`, `A-Z`,
  `>` and `-`; everything else rendered blank. `ALT:3.4KFT` was displayed as
  `ALT 3 4KFT` and coordinates lost their decimal points — a dropped point
  does not read as a missing glyph, it reads as a different number.
- **Selectable pixel clock** in the web UI (9–21 MHz), with the resulting
  refresh rate shown. Changing it reboots the device.

## Data

- **Routes are resolved server-side.** The device queried adsbdb itself, at
  about 2.2 s of blocked network task per callsign and needing more contiguous
  internal RAM for the TLS handshake than was free — which is what the
  intermittent "PK verify failed 0x4290" failures actually were. The
  aggregator now attaches the route to each aircraft in the response the
  device already fetches. One resolution serves every customer who can see
  that flight, so it is also less load on adsbdb than before.
- **Fetch-phase instrumentation.** A slow cycle now prints where its time
  went, and slow route lookups log the free internal block at that instant.

## Backend

- **Devices can be renamed** from the account dashboard.
- **Feeder client** for Debian and Windows in `server/tools/`, for receivers
  whose software cannot push SBS out on its own. Standard-library Python,
  systemd unit, Windows batch launcher, and a `--check` mode that tests both
  ends and names the failure.

## Known issues

- **The frame roll and flickering scanlines are not fixed.** The panel's DMA
  recovery works and fires; the artefacts happen anyway. Cause is the
  bounce-buffer refill missing its deadline while the CPU saturates the same
  PSRAM bus. The real fix is removing `present()`'s per-frame 768 KB
  PSRAM-to-PSRAM copy by drawing into the panel's own framebuffer. The pixel
  clock setting is a mitigation to test, not a cure.
- Do not set `CONFIG_LCD_RGB_RESTART_IN_VSYNC` and the IRAM-safe RGB/GDMA
  options via `custom_sdkconfig`. It boot-loops this board. See the comment
  in `platformio.ini`.

## Before releasing

1. Flash to hardware and confirm the panel, the icons and the screensaver.
2. Deploy the backend and confirm routes resolve — watch for `route XXX A>B`
   in the server log.
3. Merge to `main`.
4. Tag `v2.6.0` and push it; the workflow builds and publishes from the tag.
   `scripts/verify_release_version.py` fails the build if the tag and the four
   version strings disagree — currently they all read 2.6.0.
