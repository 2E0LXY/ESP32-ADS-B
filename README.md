# ESP32 ADS-B

Firmware for the **Waveshare ESP32-S3 Touch-LCD-4 Rev 4.0, 480 × 480, non-touch panel**. It retrieves nearby ADS-B and MLAT aircraft, plots them on an OpenStreetMap base map on the LCD, and provides a password-protected web administration interface on the local network.

Current firmware: **v2.1.1**

![Boot screen](assets/boot-screen-preview.png)

## Main features

- Live aircraft map on the 480 × 480 display and in the browser
- OpenStreetMap tiles cached in LittleFS for the physical display
- ADS-B and MLAT aircraft shown with distinct colours and heading markers
- Receiver latitude, longitude, and radius configurable from the web interface
- Receiver changes applied to the browser map, LCD map, API query area, and distance calculations
- Selectable aircraft-data providers with editable API credentials
- Wi-Fi network scanning and connection management
- Local firmware upload plus automatic update checks from GitHub Releases
- Password-protected administration with a change-password page
- BOOT-button map/table switching for the non-touch screen
- Boot screen displays the management IP address after Wi-Fi connects

## First login

After Wi-Fi connects, the LCD displays the address of the web interface. Open that IP address in a browser on the same network, or try `http://adsb-map.local/`.

| Setting | Initial value |
| --- | --- |
| Username | `admin` |
| Password | `aircraft` |
| Setup access point | `ADSBMAP` |
| Setup portal | `http://192.168.4.1/` |

Change the management password in **Device** after installation. The replacement password must contain at least eight characters.

## Web administration

Each sidebar entry opens a separate page. The footer on every page shows `Firmware (c) 2E0LXY D.Loxley 2026`, the installed version, and the GitHub update control.

### Overview

Connection, aircraft, provider, display, and firmware status at a glance.

![Overview page](docs/screenshots/overview.png)

### Map

Live OpenStreetMap view of the received aircraft. Save a new receiver position or radius here; the same values immediately control the LCD map, provider query bounds, and aircraft-distance calculations.

![Map page](docs/screenshots/map.png)

### Aircraft

Searchable live table containing ICAO address, callsign, registration, altitude, speed, heading, coordinates, source, distance, and data age.

![Aircraft page](docs/screenshots/aircraft.png)

### Display

Select the physical map or table page, set brightness, enable or disable the zero-mile alert, and request an immediate refresh.

![Display page](docs/screenshots/display.png)

### Wi-Fi

View the active connection, scan nearby networks, and move the receiver to a different 2.4 GHz Wi-Fi network.

![Wi-Fi page](docs/screenshots/wifi.png)

### Data API

Select an aircraft provider and replace or clear the credentials required by that provider.

![Data API page](docs/screenshots/api.png)

### Firmware

Install a local `.bin` image, check GitHub for a new release, or install the latest release directly. The green update button pulses when a newer semantic version is available.

![Firmware page](docs/screenshots/firmware.png)

### Device

Change the management password, reopen the Wi-Fi setup portal, or reboot the ESP32.

![Device page](docs/screenshots/device.png)

## Aircraft-data providers

| Web selection | Endpoint pattern | Credentials | Notes |
| --- | --- | --- | --- |
| OpenSky Network | `/api/states/all` | Optional OAuth client ID and secret | Uses a receiver-centred bounding box; falls back to anonymous access when credentials are blank. |
| adsb.fi Open Data | `/api/v3/lat/.../lon/.../dist/...` | None | Free/open data endpoint. |
| airplanes.live | `/v2/point/...` | None | ADS-B Exchange v2-compatible response. |
| adsb.lol Open API | `/v2/point/...` | None currently | Free/open API; the key field is retained in case the service changes. |
| ADSB One / API archive | `/v2/point/...` | None | Experimental legacy-compatible source. |
| ADS-B Exchange via RapidAPI | `/v2/lat/.../lon/.../dist/...` | RapidAPI key | Requires the relevant RapidAPI subscription. |

The ESP32 stores credentials in Preferences/NVS. Existing secrets are never returned by the status API, and submitting an empty credential field preserves the stored value unless **Clear** is selected.

## GitHub OTA updates

The firmware checks the latest release from `2E0LXY/ESP32-ADS-B` at startup and every six hours. If a newer version exists, the footer button flashes green. Pressing it downloads the release asset containing `firmware` or `adsb-map`, validates the ESP32 image header, writes it through the Arduino Update API, and reboots only after a successful write.

Do not remove power while an update is being installed. The `default_16MB.csv` partition layout supplies two application slots for OTA updates.

Creating and pushing a tag such as `v2.1.1` runs `.github/workflows/release.yml`, builds both upgrade and factory images, generates SHA-256 checksums, and attaches them to a GitHub Release.

## Build

Install [PlatformIO](https://platformio.org/), clone the repository, and run:

```powershell
pio run
```

The OTA image is generated at:

```text
.pio/build/waveshare_esp32_s3_lcd_4/firmware.bin
```

## USB installation

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

- Short BOOT-button press: toggles the LCD between the map and nearest-aircraft table
- Aircraft data refresh: every 30 seconds or on demand
- ADS-B aircraft: cyan/operator-coloured symbol
- MLAT aircraft: violet/red symbol
- Route lookup: public ADSBDB callsign endpoint, cached for six hours
- Zero-mile aircraft: optional 200 ms buzzer alert
- OSM failure: falls back to the built-in radar-style map

## OpenStreetMap usage

Map data is © OpenStreetMap contributors. The browser and LCD show attribution. The LCD requests only the tiles visible for the configured receiver area, identifies this firmware in its User-Agent, and caches tiles persistently in LittleFS. Do not modify the firmware to bulk-download tiles from the public OpenStreetMap tile service.

## Security notes

- All web-management and JSON API routes use HTTP Basic authentication.
- Change the initial password before placing the receiver on a shared network.
- The admin interface is intended for a trusted local network and does not provide TLS.
- Wi-Fi and provider credentials remain in ESP32 NVS and are excluded from Git.

## Copyright

Firmware (c) 2026 2E0LXY / D. Loxley. All rights reserved. No open-source licence is granted unless a `LICENSE` file is added to the repository.
