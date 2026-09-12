#include <Arduino.h>
#include <strings.h>  // strncasecmp, used by the icon type-designator table
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

// Board capability macros. Must come before anything that tests them,
// notably the SD backend selection below.
#include "board_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <new>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#if BOARD_SD_SDMMC
#include <SD_MMC.h>
#define SDCARD SD_MMC
#else
#include <SD.h>
#include <SPI.h>
#define SDCARD SD
#endif
#include <PNGdec.h>
#include <WebSocketsClient.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <math.h>

#include <vector>
#include <esp_random.h>

#if BOARD_EXPANDER_CH32
#include "WS_CH32_IO.h"
#else
#include "WS_CH422G.h"
#endif
#include "boot_asset.h"
#include "map_asset.h"
#include "opensky_secrets.h"
#include "web_ui.h"

// The Rev 4.0 480x480 panel has no touch controller. Building with
// -DADSB_ENABLE_TOUCH=1 re-enables GT911 probing and per-loop polling.
#ifndef ADSB_ENABLE_TOUCH
#define ADSB_ENABLE_TOUCH 0
#endif

// Set to 1 only if the certificate bundle is unavailable in your build.
#ifndef ADSB_TLS_INSECURE
#define ADSB_TLS_INSECURE 0
#endif

// HTTPS handshakes and ArduinoJson parsing exceed the default 8 KB loop stack.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Panel pins, timings and capabilities all resolve through src/board_config.h,
// selected by -DADSB_BOARD_WS4 or -DADSB_BOARD_WS7. Nothing about the display
// is hardcoded here any more.
//
// On the 480x480 board GPIO 1 and 2 are the ST7701 configuration SPI and are
// re-used as SDMMC CMD and CLK. That is only safe because the panel is fully
// initialised in setup() before mountSdCard() runs and its chip select idles
// high afterwards. Never re-initialise the display once the card is mounted.
#if PANEL_NEEDS_SPI_INIT
Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED, PANEL_SPI_CS, PANEL_SPI_SCK, PANEL_SPI_MOSI,
    GFX_NOT_DEFINED);
#endif

// The pixel clock is settable from the web UI rather than fixed at
// PANEL_PCLK_HZ, because it is the one lever on the frame roll and the
// flickering scanlines that can be tested without a rebuild each time. Both
// faults are the RGB bounce-buffer refill missing its deadline while the CPU
// saturates the same PSRAM bus; a slower pixel clock asks for fewer bytes per
// line and gives the refill more slack, at the cost of refresh rate. Which
// value is enough is an empirical question about this panel and this
// workload, so it belongs in a dropdown, not a #define.
//
// esp_lcd captures the clock when the panel is initialised, so this is
// applied at boot and a change reboots the device. The board's own
// PANEL_PCLK_HZ remains the default and the value NVS is seeded with.
uint32_t panelPclkHz = PANEL_PCLK_HZ;

// Selectable values, coarse enough to tell apart on a panel and bounded so a
// bad entry cannot leave the display unusable and the web UI unreachable.
// Bounce-buffer height, in scanlines, for the same reason the pixel clock
// is settable: the display faults have to be tested against the hardware,
// and a rebuild per value is not a workable loop.
//
// esp_lcd allocates TWO buffers of lines*width*2 bytes from internal DMA
// RAM, so 20 lines costs 64 KB at 800 px wide and 40 costs 128 KB - against
// roughly 40 KB of largest contiguous internal block free at idle and the
// ~32 KB mbedTLS needs per TLS handshake. Going up trades HTTPS for display
// stability; going to 0 removes bounce buffers altogether, which frees that
// internal RAM and removes the refill deadline entirely, at the cost of the
// LCD DMA reading PSRAM directly.
// Seeded from the panel driver's own compiled-in default rather than the
// build flag: only ws_lcd_7_app passes -DRGB_BOUNCE_BUFFER_LINES, and the
// library already resolves its own default when the flag is absent.
uint16_t panelBounceLines = 0;

// Draw straight into the panel's framebuffer instead of into a shadow copy
// that present() then memcpys across.
//
// The copy is the largest single consumer of PSRAM bandwidth on the device:
// 768 KB read plus 768 KB written on every render, on the same bus the LCD
// DMA is refilling its bounce buffers from. The jumps line up with fetch
// cycles, which is exactly when that bus is busiest, and no bounce buffer
// size removes them - 40 lines only reduces them, and costs enough internal
// RAM to leave largestInternal sitting at 31732, right on the threshold
// mbedTLS needs.
//
// Drawing direct removes the copy entirely and frees the 768 KB shadow
// buffer. The cost is that a frame appears progressively rather than all at
// once, which on mostly-static pages is invisible and on a full repaint
// looks like a fast wipe. Runtime-selectable because that trade has to be
// judged on the hardware.
bool panelDirectDraw = false;
constexpr uint16_t PANEL_BOUNCE_CHOICES[] = {0, 10, 20, 30, 40};

constexpr uint32_t PANEL_PCLK_CHOICES[] = {
    9000000L, 10000000L, 11000000L, 12000000L, 13000000L,
    14000000L, 15000000L, 16000000L, 16500000L, 18000000L, 21000000L,
};

// Constructed in setup() once the stored clock has been read, not at static
// init - hence pointers assigned later rather than initialisers here.
Arduino_ESP32RGBPanel *rgbpanel = nullptr;
Arduino_RGB_Display *gfx = nullptr;

void createDisplay(uint32_t pclkHz) {
  rgbpanel = new Arduino_ESP32RGBPanel(
      PANEL_PIN_DE, PANEL_PIN_VSYNC, PANEL_PIN_HSYNC, PANEL_PIN_PCLK,
      PANEL_PINS_R, PANEL_PINS_G, PANEL_PINS_B,
      PANEL_HSYNC_POLARITY, PANEL_HSYNC_FRONT_PORCH, PANEL_HSYNC_PULSE_WIDTH,
      PANEL_HSYNC_BACK_PORCH,
      PANEL_VSYNC_POLARITY, PANEL_VSYNC_FRONT_PORCH, PANEL_VSYNC_PULSE_WIDTH,
      PANEL_VSYNC_BACK_PORCH,
      PANEL_PCLK_ACTIVE_NEG, static_cast<int32_t>(pclkHz));
#if PANEL_NEEDS_SPI_INIT
  // The ST7701 needs an SPI register sequence before its RGB interface works.
  gfx = new Arduino_RGB_Display(
      PANEL_WIDTH, PANEL_HEIGHT, rgbpanel, PANEL_ROTATION, true,
      bus, GFX_NOT_DEFINED, st7701_type1_init_operations,
      sizeof(st7701_type1_init_operations));
#else
  // The ST7262 is a plain RGB driver with no configuration bus.
  gfx = new Arduino_RGB_Display(PANEL_WIDTH, PANEL_HEIGHT, rgbpanel, PANEL_ROTATION, true);
#endif
}

// Approximate refresh rate for a given pixel clock, so the UI can say what
// the trade costs. Total line and frame lengths include the blanking.
float panelRefreshHz(uint32_t pclkHz) {
  const uint32_t lineTicks = PANEL_WIDTH + PANEL_HSYNC_FRONT_PORCH +
                             PANEL_HSYNC_PULSE_WIDTH + PANEL_HSYNC_BACK_PORCH;
  const uint32_t frameLines = PANEL_HEIGHT + PANEL_VSYNC_FRONT_PORCH +
                              PANEL_VSYNC_PULSE_WIDTH + PANEL_VSYNC_BACK_PORCH;
  return static_cast<float>(pclkHz) / static_cast<float>(lineTicks * frameLines);
}

#if !ADSB_TLS_INSECURE
// Declared at global scope on purpose: an unnamed namespace would give these
// internal linkage and the asm-labelled bundle symbols would not resolve.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");
#endif

namespace {
// map_asset.h is a raw RGB565 array generated at 480x480. The boot screen
// is no longer one of these - it is a PNG decoded at boot, sized per panel,
// so it carries its own BOOT_IMAGE_W/H from boot_asset.h.
constexpr int ASSET_W = 480;
constexpr int ASSET_H = 480;
constexpr int W = layout::W;
constexpr int H = layout::H;
constexpr float DEFAULT_HOME_LAT = 53.73f;
constexpr float DEFAULT_HOME_LON = -1.57f;
constexpr uint16_t DEFAULT_RADIUS_NM = 60;
constexpr char TOKEN_URL[] = "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";
constexpr uint32_t REFRESH_MS = 30000;
constexpr int MAX_AIRCRAFT = 250;
constexpr int MAX_VESSELS = 250;
constexpr uint16_t DEFAULT_MARINE_RADIUS_NM = 25;  // typical VHF AIS coastal range
constexpr uint32_t MARINE_STALE_MS = 20UL * 60UL * 1000UL;  // AIS position reports are event-driven, not polled
// REST marine providers are polled, unlike AISstream's push WebSocket.
// AISHub's terms forbid querying more than once a minute; MyShipTracking
// and Datalastic bill per vessel per request, so this stays conservative
// for all three rather than tuning a separate interval per provider.
constexpr uint32_t MARINE_REST_REFRESH_MS = 60UL * 1000UL;
constexpr int ROUTE_CACHE_SIZE = 48;
constexpr int MAX_ROUTE_LOOKUPS_PER_REFRESH = 2;
constexpr uint32_t ROUTE_CACHE_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t ROUTE_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr char FIRMWARE_VERSION[] = "2.6.0";
constexpr char DEVICE_HOSTNAME[] = "adsb-map";
constexpr char WEB_USERNAME[] = "admin";
constexpr char GITHUB_OWNER[] = "2E0LXY";
constexpr char GITHUB_REPOSITORY[] = "ESP32-ADS-B";
constexpr char GITHUB_RELEASE_API[] = "https://api.github.com/repos/2E0LXY/ESP32-ADS-B/releases/latest";
// Sized for RSA-4096 so rotating the signing key does not silently disable
// every OTA path. verifyFirmwareSignature() checks the actual length.
constexpr size_t MAX_SIGNATURE_BYTES = 512;
constexpr size_t MIN_SIGNATURE_BYTES = 64;
constexpr uint64_t MIN_TILE_CACHE_FREE_BYTES = 192UL * 1024UL;
#if BOARD_SD_SDMMC
constexpr int SD_CLK_PIN = BOARD_SD_CLK;
constexpr int SD_CMD_PIN = BOARD_SD_CMD;
constexpr int SD_D0_PIN = BOARD_SD_D0;
#endif
constexpr char SD_UPDATE_DIR[] = "/adsb/update";
constexpr char SD_UPDATE_PART[] = "/adsb/update/firmware.bin.part";
constexpr char SD_UPDATE_FILE[] = "/adsb/update/firmware.bin";
constexpr char FIRMWARE_PUBLIC_KEY[] = R"KEY(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAzJLYuacEhXg2q+drT7MT
OxZRXBbr5AXIAE6ZqPthjnZazlzzDf8ctP2dZ3aAeY9JNCFFF9PPeW2M5wAoXhYf
nBRUW20KnO6SL1Yp09MfMh0bERxGKDbbzLl4iqHsNxwnlRcWVrNuCuNn6k0RJjra
1mXIL0kf6xGdbQBwEyOpA1guiGWymvQashwVGQ1pPR9F80UrFBQUDXt7TJHLty05
pAn/ixJRyZStSxPUF8J/W0/cCSS4lYCiRTaiZmuvdMtoR7fGV4iy9aS5lzUJ/qqD
df0jLYhXW4NWQtsm+m22kaDPUMeIlNP+frudTx9qHHKRgTjo4le6xOBlgkcxH/PK
rQIDAQAB
-----END PUBLIC KEY-----
)KEY";

class PsramAllocator : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  void deallocate(void *pointer) override { heap_caps_free(pointer); }
  void *reallocate(void *pointer, size_t size) override {
    return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};

PsramAllocator psramJsonAllocator;

// HTTPClient only de-chunks a response inside getString() and writeToStream().
// getStream() hands back the raw socket, so on a Transfer-Encoding: chunked
// reply ArduinoJson sees the hex chunk-length prefix first and parses "56f1"
// as a number: deserializeJson() returns Ok and the expected object is simply
// absent. Every Cloudflare-fronted API here chunks (adsb.fi, api.github.com),
// which is why adsb.fi logged "JSON Ok" with zero aircraft while
// airplanes.live, which sends Content-Length, worked.
//
// Buffer the body through writeToStream into PSRAM so HTTPClient's own
// de-chunking runs, then parse from the flat buffer.
class PsramSink : public Stream {
 public:
  ~PsramSink() { heap_caps_free(_data); }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t length) override {
    if (!reserve(_size + length + 1)) return 0;
    memcpy(_data + _size, data, length);
    _size += length;
    _data[_size] = '\0';
    return length;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  const char *data() const { return _data ? _data : ""; }
  size_t size() const { return _size; }

 private:
  bool reserve(size_t needed) {
    if (needed <= _capacity) return true;
    size_t want = _capacity ? _capacity : 8192;
    while (want < needed) want *= 2;
    char *grown = static_cast<char *>(
        heap_caps_realloc(_data, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!grown) return false;
    _data = grown;
    _capacity = want;
    return true;
  }
  char *_data = nullptr;
  size_t _size = 0;
  size_t _capacity = 0;
};

// ---------------------------------------------------------------------------
// Response body reader.
//
// Deliberately not HTTPClient::writeToStream(). That call has no overall
// deadline, so the only lever over a stalled body read was force-closing the
// socket from the *other* core - and that turned out to be the cause of a
// whole family of failures rather than a fix for one.
// NetworkClientSecure::stop() tears the mbedTLS session down (mbedtls_ssl_free
// and friends) while the fetching core may be sitting inside
// mbedtls_ssl_read() on that same context; mbedTLS is not built here for two
// cores touching one session, so it is a plain use-after-free. The logs bear
// that out exactly: "PK verify failed"/"Certificate matched but signature
// verification failed", "BIGNUM - Memory allocation failed" and "SSL - Memory
// allocation failed" only ever appear *after* a force-close line and never
// before one, and the first fetch after any boot always succeeds. The task
// watchdog aborts naming "CPU 0: network" are the same race landing somewhere
// that never yields.
//
// Reading the body here means the deadline belongs to the task that owns the
// session: a stall ends in an ordinary http.end() on this core, with nothing
// freed underneath another core, so the next connection starts from an intact
// heap. Not calling writeToStream() gives up HTTPClient's own de-chunking, so
// chunked replies are decoded below - several providers here are
// Cloudflare-fronted and do chunk (see the PsramSink comment above). Every
// idle pass yields, so however long the far end stays silent this loop cannot
// starve the idle task and trip the watchdog.
// ---------------------------------------------------------------------------
constexpr uint32_t BODY_IDLE_TIMEOUT_MS = 8000;
constexpr uint32_t BODY_TOTAL_TIMEOUT_MS = 20000;

enum class BodyRead : uint8_t { Complete, Stalled, ClosedEarly, OutOfMemory, NoStream };

const char *bodyReadName(BodyRead outcome) {
  switch (outcome) {
    case BodyRead::Complete: return "complete";
    case BodyRead::Stalled: return "STALLED";
    case BodyRead::ClosedEarly: return "CLOSED EARLY";
    case BodyRead::OutOfMemory: return "OUT OF PSRAM";
    case BodyRead::NoStream: return "NO STREAM";
  }
  return "unknown";
}

struct BodyReader {
  NetworkClient *stream = nullptr;
  PsramSink *sink = nullptr;
  uint32_t startedAt = 0;
  uint32_t lastProgressAt = 0;
  size_t received = 0;

  bool expired() const {
    return millis() - lastProgressAt >= BODY_IDLE_TIMEOUT_MS ||
           millis() - startedAt >= BODY_TOTAL_TIMEOUT_MS;
  }

  // Blocks until at least one byte is readable. false means stop, with
  // `outcome` saying why. Data already buffered wins over a closed socket:
  // a peer that sends the whole body then closes is a success, not a loss.
  bool waitForData(BodyRead &outcome) {
    for (;;) {
      if (stream->available() > 0) return true;
      if (!stream->connected()) { outcome = BodyRead::ClosedEarly; return false; }
      if (expired()) { outcome = BodyRead::Stalled; return false; }
      delay(2);
    }
  }

  bool copy(size_t count, BodyRead &outcome) {
    uint8_t buffer[512];
    size_t remaining = count;
    while (remaining) {
      if (!waitForData(outcome)) return false;
      const int avail = stream->available();
      if (avail <= 0) continue;
      size_t want = min(sizeof(buffer), remaining);
      want = min(want, static_cast<size_t>(avail));
      const int got = stream->read(buffer, want);
      if (got <= 0) { outcome = BodyRead::ClosedEarly; return false; }
      if (sink->write(buffer, got) != static_cast<size_t>(got)) {
        outcome = BodyRead::OutOfMemory;
        return false;
      }
      received += got;
      remaining -= got;
      lastProgressAt = millis();
    }
    return true;
  }

  // One CRLF-terminated line: chunk size headers and their trailers.
  bool readLine(String &line, BodyRead &outcome) {
    line = "";
    for (;;) {
      if (!waitForData(outcome)) return false;
      const int c = stream->read();
      if (c < 0) { outcome = BodyRead::ClosedEarly; return false; }
      lastProgressAt = millis();
      if (c == '\n') return true;
      if (c != '\r' && line.length() < 40) line += static_cast<char>(c);
    }
  }
};

BodyRead readResponseBody(HTTPClient &http, PsramSink &sink, int &expected,
                          size_t &received, bool &chunked) {
  BodyReader reader;
  reader.stream = http.getStreamPtr();
  reader.sink = &sink;
  reader.startedAt = millis();
  reader.lastProgressAt = reader.startedAt;
  expected = http.getSize();
  chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
  received = 0;
  if (!reader.stream) return BodyRead::NoStream;

  BodyRead outcome = BodyRead::Complete;
  if (!chunked && expected >= 0) {
    if (expected > 0 && !reader.copy(static_cast<size_t>(expected), outcome)) {
      received = reader.received;
      return outcome;
    }
    received = reader.received;
    return BodyRead::Complete;
  }
  if (!chunked) {
    // Neither a length nor chunked framing: the body runs until the peer
    // closes, which is a normal end here rather than a truncation.
    for (;;) {
      if (!reader.waitForData(outcome)) {
        received = reader.received;
        return outcome == BodyRead::ClosedEarly ? BodyRead::Complete : outcome;
      }
      const int avail = reader.stream->available();
      if (avail <= 0) continue;
      if (!reader.copy(static_cast<size_t>(avail), outcome)) {
        received = reader.received;
        return outcome;
      }
    }
  }
  // Transfer-Encoding: chunked - "<hex size>[;ext]" CRLF, data, CRLF, ending
  // with a zero-length chunk. HTTPClient strips this inside writeToStream();
  // doing it here is the price of owning the deadline.
  for (;;) {
    String header;
    if (!reader.readLine(header, outcome)) { received = reader.received; return outcome; }
    const size_t chunkSize = strtoul(header.c_str(), nullptr, 16);
    if (chunkSize == 0) break;  // final chunk; any trailers are ignored
    if (!reader.copy(chunkSize, outcome)) { received = reader.received; return outcome; }
    String terminator;
    if (!reader.readLine(terminator, outcome)) { received = reader.received; return outcome; }
  }
  received = reader.received;
  return BodyRead::Complete;
}

struct RouteCacheEntry {
  char callsign[9] = {};
  char origin[5] = {};
  char destination[5] = {};
  // Full airport names, used by the browser Aircraft/Overview table; the LCD
  // pages keep the short codes above since the on-panel font has no room for
  // full names.
  char originName[40] = {};
  char destinationName[40] = {};
  // City/municipality names, used by the browser table fallback and as the
  // input to the abbreviations below; full airport names don't fit even the
  // wider WS7 panel, but a city pair ("LONDON -> MADRID") does.
  char originCity[24] = {};
  char destinationCity[24] = {};
  // Departure-board-style abbreviation ("LON STAN"), used by the LCD Table
  // page: a city code plus the first distinguishing word of the airport
  // name. Computed once here rather than per frame.
  char originAbbrev[10] = {};
  char destinationAbbrev[10] = {};
  // Airline name, when the aggregator supplied one. adsbdb returns it
  // alongside the route, so the server gets it for free from a request it
  // is already making - a far better source than a prefix table compiled
  // into the firmware, which can only ever cover the operators someone
  // thought to add.
  char airline[32] = {};
  uint32_t resolvedAt = 0;
  uint32_t lastUsed = 0;
  bool occupied = false;
  bool hasRoute = false;
};

// ---------------------------------------------------------------------------
// Icon shape selection.
//
// Two independent sources, in tar1090's order of preference:
//   1. The ICAO type designator (exact airframe: B738, EC35, C172). tar1090
//      gets this from a ~12 MB sharded local database because it decodes raw
//      frames, where nothing on air ever says "Boeing 737". Every provider
//      here has already done that hex -> registration -> type lookup and hands
//      it over in the "t" field of each aircraft, so the exact airframe costs
//      us no storage at all and the database is simply not needed.
//   2. The ADS-B emitter category (DF17/18 TC 1-4, sets A/B/C/D code 0-7),
//      which is coarse and, per the ADS-B spec's own reputation, frequently
//      absent or mis-set - plenty of GA aircraft report A1 regardless.
//   3. A generic silhouette when both miss.
//
// The type table is prefix-matched, longest match wins, which is what makes
// short prefixes safe next to longer ones: "A109" (AgustaWestland) is tested
// before "A10" (A-10 Thunderbolt), and "C172" before "C17" (Globemaster), so
// neither pair collides. Only designators whose family is unambiguous at the
// prefix given are listed; anything else falls through to the category, which
// is the better answer for the cases this table deliberately omits.
// ---------------------------------------------------------------------------
enum class PlaneShape : uint8_t {
  Generic,
  LightProp,
  Twin,
  Airliner,
  HeavyJet,
  Fighter,
  Helicopter,
  Glider,
  Balloon,
  Drone,
  Ground,
};

struct TypeShapeRule {
  const char *prefix;
  PlaneShape shape;
};

constexpr TypeShapeRule TYPE_SHAPE_RULES[] = {
    // Rotorcraft. "EC" is safe as two characters: no Embraer designator
    // starts with it (they are E110/E120/E135/E145/E17x/E19x/E29x).
    {"EC", PlaneShape::Helicopter},   {"A109", PlaneShape::Helicopter},
    {"A119", PlaneShape::Helicopter}, {"A139", PlaneShape::Helicopter},
    {"A169", PlaneShape::Helicopter}, {"A189", PlaneShape::Helicopter},
    {"AW09", PlaneShape::Helicopter}, {"AW13", PlaneShape::Helicopter},
    {"AW16", PlaneShape::Helicopter}, {"AW18", PlaneShape::Helicopter},
    {"AS32", PlaneShape::Helicopter}, {"AS35", PlaneShape::Helicopter},
    {"AS50", PlaneShape::Helicopter}, {"AS55", PlaneShape::Helicopter},
    {"AS65", PlaneShape::Helicopter}, {"BK17", PlaneShape::Helicopter},
    {"EN48", PlaneShape::Helicopter}, {"B06", PlaneShape::Helicopter},
    {"B47", PlaneShape::Helicopter},  {"B412", PlaneShape::Helicopter},
    {"B429", PlaneShape::Helicopter}, {"B505", PlaneShape::Helicopter},
    {"R22", PlaneShape::Helicopter},  {"R44", PlaneShape::Helicopter},
    {"R66", PlaneShape::Helicopter},  {"S61", PlaneShape::Helicopter},
    {"S64", PlaneShape::Helicopter},  {"S70", PlaneShape::Helicopter},
    {"S76", PlaneShape::Helicopter},  {"S92", PlaneShape::Helicopter},
    {"H12", PlaneShape::Helicopter},  {"H13", PlaneShape::Helicopter},
    {"H14", PlaneShape::Helicopter},  {"H16", PlaneShape::Helicopter},
    {"H17", PlaneShape::Helicopter},  {"MD50", PlaneShape::Helicopter},
    {"MD52", PlaneShape::Helicopter}, {"MD53", PlaneShape::Helicopter},
    {"CH47", PlaneShape::Helicopter}, {"UH60", PlaneShape::Helicopter},
    {"LYNX", PlaneShape::Helicopter}, {"PUMA", PlaneShape::Helicopter},
    {"GAZL", PlaneShape::Helicopter},
    // Wide-bodies and other heavies. "A31" is deliberately absent: it would
    // swallow the A318/A319 narrow-bodies, so the A310 is listed in full.
    {"A30", PlaneShape::HeavyJet},    {"A310", PlaneShape::HeavyJet},
    {"A33", PlaneShape::HeavyJet},    {"A34", PlaneShape::HeavyJet},
    {"A35", PlaneShape::HeavyJet},    {"A38", PlaneShape::HeavyJet},
    {"A124", PlaneShape::HeavyJet},   {"A225", PlaneShape::HeavyJet},
    {"A400", PlaneShape::HeavyJet},   {"B74", PlaneShape::HeavyJet},
    {"B76", PlaneShape::HeavyJet},    {"B77", PlaneShape::HeavyJet},
    {"B78", PlaneShape::HeavyJet},    {"B52", PlaneShape::HeavyJet},
    {"IL76", PlaneShape::HeavyJet},   {"IL96", PlaneShape::HeavyJet},
    {"MD11", PlaneShape::HeavyJet},   {"C17", PlaneShape::HeavyJet},
    {"C5M", PlaneShape::HeavyJet},    {"KC13", PlaneShape::HeavyJet},
    {"C130", PlaneShape::HeavyJet},
    // Narrow-body airliners, including the A4 "high-vortex large" B757.
    {"A318", PlaneShape::Airliner},   {"A319", PlaneShape::Airliner},
    {"A320", PlaneShape::Airliner},   {"A321", PlaneShape::Airliner},
    {"A19N", PlaneShape::Airliner},   {"A20N", PlaneShape::Airliner},
    {"A21N", PlaneShape::Airliner},   {"B73", PlaneShape::Airliner},
    {"B38", PlaneShape::Airliner},    {"B39", PlaneShape::Airliner},
    {"B70", PlaneShape::Airliner},    {"B71", PlaneShape::Airliner},
    {"B72", PlaneShape::Airliner},    {"B75", PlaneShape::Airliner},
    {"BCS1", PlaneShape::Airliner},   {"BCS3", PlaneShape::Airliner},
    {"E17", PlaneShape::Airliner},    {"E19", PlaneShape::Airliner},
    {"E29", PlaneShape::Airliner},    {"E75", PlaneShape::Airliner},
    {"MD8", PlaneShape::Airliner},    {"MD9", PlaneShape::Airliner},
    {"F70", PlaneShape::Airliner},    {"F100", PlaneShape::Airliner},
    {"RJ1", PlaneShape::Airliner},    {"RJ7", PlaneShape::Airliner},
    {"RJ8", PlaneShape::Airliner},    {"B461", PlaneShape::Airliner},
    {"B462", PlaneShape::Airliner},   {"B463", PlaneShape::Airliner},
    {"SU95", PlaneShape::Airliner},
    // Regional turboprops, regional jets and business jets.
    {"AT4", PlaneShape::Twin},        {"AT5", PlaneShape::Twin},
    {"AT7", PlaneShape::Twin},        {"AT8", PlaneShape::Twin},
    {"DH8", PlaneShape::Twin},        {"SF34", PlaneShape::Twin},
    {"SB20", PlaneShape::Twin},       {"JS31", PlaneShape::Twin},
    {"JS32", PlaneShape::Twin},       {"JS41", PlaneShape::Twin},
    {"D228", PlaneShape::Twin},       {"D328", PlaneShape::Twin},
    {"L410", PlaneShape::Twin},       {"SW4", PlaneShape::Twin},
    {"E110", PlaneShape::Twin},       {"E120", PlaneShape::Twin},
    {"E135", PlaneShape::Twin},       {"E145", PlaneShape::Twin},
    {"E45X", PlaneShape::Twin},       {"CRJ", PlaneShape::Twin},
    {"BE20", PlaneShape::Twin},       {"BE9", PlaneShape::Twin},
    {"B190", PlaneShape::Twin},       {"B350", PlaneShape::Twin},
    {"CL30", PlaneShape::Twin},       {"CL35", PlaneShape::Twin},
    {"CL60", PlaneShape::Twin},       {"GLF", PlaneShape::Twin},
    {"GL5", PlaneShape::Twin},        {"GL6", PlaneShape::Twin},
    {"C25", PlaneShape::Twin},        {"C56", PlaneShape::Twin},
    {"C68", PlaneShape::Twin},        {"C750", PlaneShape::Twin},
    {"LJ", PlaneShape::Twin},         {"PRM1", PlaneShape::Twin},
    {"F2TH", PlaneShape::Twin},       {"FA7X", PlaneShape::Twin},
    {"FA8X", PlaneShape::Twin},       {"E55P", PlaneShape::Twin},
    {"E50P", PlaneShape::Twin},       {"BE40", PlaneShape::Twin},
    {"H25", PlaneShape::Twin},        {"PC24", PlaneShape::Twin},
    {"P180", PlaneShape::Twin},
    // Light aircraft. The Cessna singles are spelled out in full so "C17"
    // above keeps meaning the Globemaster rather than a 172.
    {"C150", PlaneShape::LightProp},  {"C152", PlaneShape::LightProp},
    {"C162", PlaneShape::LightProp},  {"C170", PlaneShape::LightProp},
    {"C172", PlaneShape::LightProp},  {"C175", PlaneShape::LightProp},
    {"C177", PlaneShape::LightProp},  {"C180", PlaneShape::LightProp},
    {"C182", PlaneShape::LightProp},  {"C185", PlaneShape::LightProp},
    {"C206", PlaneShape::LightProp},  {"C207", PlaneShape::LightProp},
    {"C208", PlaneShape::LightProp},  {"C210", PlaneShape::LightProp},
    {"C337", PlaneShape::LightProp},  {"T206", PlaneShape::LightProp},
    {"T210", PlaneShape::LightProp},  {"P28", PlaneShape::LightProp},
    {"P32", PlaneShape::LightProp},   {"P46", PlaneShape::LightProp},
    {"PA1", PlaneShape::LightProp},   {"PA2", PlaneShape::LightProp},
    {"PA3", PlaneShape::LightProp},   {"PA4", PlaneShape::LightProp},
    {"SR20", PlaneShape::LightProp},  {"SR22", PlaneShape::LightProp},
    {"S22", PlaneShape::LightProp},   {"DA40", PlaneShape::LightProp},
    {"DA42", PlaneShape::LightProp},  {"DA20", PlaneShape::LightProp},
    {"DV20", PlaneShape::LightProp},  {"M20", PlaneShape::LightProp},
    {"BE33", PlaneShape::LightProp},  {"BE35", PlaneShape::LightProp},
    {"BE36", PlaneShape::LightProp},  {"BE55", PlaneShape::LightProp},
    {"BE58", PlaneShape::LightProp},  {"AA5", PlaneShape::LightProp},
    {"G115", PlaneShape::LightProp},  {"GA8", PlaneShape::LightProp},
    {"RV", PlaneShape::LightProp},    {"TB9", PlaneShape::LightProp},
    {"TB10", PlaneShape::LightProp},  {"TB20", PlaneShape::LightProp},
    {"PC12", PlaneShape::LightProp},  {"TBM", PlaneShape::LightProp},
    {"EV97", PlaneShape::LightProp},  {"VL3", PlaneShape::LightProp},
    {"AT3", PlaneShape::LightProp},   {"F406", PlaneShape::LightProp},
    {"DR40", PlaneShape::LightProp},  {"SF25", PlaneShape::LightProp},
    {"WT9", PlaneShape::LightProp},   {"P208", PlaneShape::LightProp},
    // Fast jets. "A10" is reachable because "A109" above is longer and wins
    // the AgustaWestland case outright.
    {"F15", PlaneShape::Fighter},     {"F16", PlaneShape::Fighter},
    {"F18", PlaneShape::Fighter},     {"F22", PlaneShape::Fighter},
    {"F35", PlaneShape::Fighter},     {"F14", PlaneShape::Fighter},
    {"EUFI", PlaneShape::Fighter},    {"TOR", PlaneShape::Fighter},
    {"HAWK", PlaneShape::Fighter},    {"T38", PlaneShape::Fighter},
    {"GR4", PlaneShape::Fighter},     {"A10", PlaneShape::Fighter},
    // Sailplanes and lighter-than-air.
    {"AS21", PlaneShape::Glider},     {"AS25", PlaneShape::Glider},
    {"AS26", PlaneShape::Glider},     {"DG1", PlaneShape::Glider},
    {"DG4", PlaneShape::Glider},      {"DG8", PlaneShape::Glider},
    {"LS4", PlaneShape::Glider},      {"LS8", PlaneShape::Glider},
    {"JANU", PlaneShape::Glider},     {"VENT", PlaneShape::Glider},
    {"DISC", PlaneShape::Glider},     {"NIMB", PlaneShape::Glider},
    {"ARCU", PlaneShape::Glider},     {"DUOD", PlaneShape::Glider},
    {"BALL", PlaneShape::Balloon},    {"ZEPP", PlaneShape::Balloon},
};

// Emitter category set A/B/C. Set A is the one that carries real weight; set
// B covers the non-aeroplanes and set C is surface traffic, which should never
// be drawn as something airborne.
PlaneShape shapeForCategory(const char *category) {
  if (!category || !category[0] || !category[1]) return PlaneShape::Generic;
  const char set = static_cast<char>(toupper(static_cast<unsigned char>(category[0])));
  const char code = category[1];
  if (set == 'A') {
    switch (code) {
      case '1': return PlaneShape::LightProp;   // < 15.5t
      case '2': return PlaneShape::Twin;        // 15.5-75t
      case '3': return PlaneShape::Airliner;    // 75-300t
      case '4': return PlaneShape::Airliner;    // high-vortex large (B757)
      case '5': return PlaneShape::HeavyJet;    // > 300t
      case '6': return PlaneShape::Fighter;     // high performance
      case '7': return PlaneShape::Helicopter;  // rotorcraft
      default: return PlaneShape::Generic;      // A0, no information
    }
  }
  if (set == 'B') {
    switch (code) {
      case '1': return PlaneShape::Glider;
      case '2': return PlaneShape::Balloon;     // lighter-than-air
      case '4': return PlaneShape::LightProp;   // ultralight/hang-glider
      case '6': return PlaneShape::Drone;       // UAV
      default: return PlaneShape::Generic;      // B0/B3 skydiver/B7 space
    }
  }
  if (set == 'C') return PlaneShape::Ground;    // surface vehicles, obstacles
  return PlaneShape::Generic;
}

// OpenSky reports its own flat 0-20 category enum rather than the ADS-B
// set/code pair every other provider here uses. This used to be written
// straight out as "C<n>", which reads as ADS-B category set C - surface
// vehicles - for every aircraft in the sky. It went unnoticed while the
// shape table ignored anything it didn't recognise; it does not now.
void openSkyCategoryToAdsb(int openSkyCategory, char *out, size_t outSize) {
  const char *mapped = "A0";
  switch (openSkyCategory) {
    case 2: mapped = "A1"; break;   // light
    case 3: mapped = "A2"; break;   // small
    case 4: mapped = "A3"; break;   // large
    case 5: mapped = "A4"; break;   // high vortex large
    case 6: mapped = "A5"; break;   // heavy
    case 7: mapped = "A6"; break;   // high performance
    case 8: mapped = "A7"; break;   // rotorcraft
    case 9: mapped = "B1"; break;   // glider / sailplane
    case 10: mapped = "B2"; break;  // lighter-than-air
    case 11: mapped = "B3"; break;  // parachutist
    case 12: mapped = "B4"; break;  // ultralight / hang-glider
    case 14: mapped = "B6"; break;  // UAV
    case 15: mapped = "B7"; break;  // space / trans-atmospheric
    case 16:
    case 17:
    case 18:
    case 19:
    case 20: mapped = "C1"; break;  // surface vehicles and obstacles
    default: mapped = "A0"; break;  // 0/1 no information, 13 reserved
  }
  strncpy(out, mapped, outSize - 1);
  out[outSize - 1] = 0;
}

// Stable identifiers for the browser map, which renders whichever silhouette
// the device has already resolved rather than repeating the lookup itself.
const char *planeShapeName(PlaneShape shape) {
  switch (shape) {
    case PlaneShape::LightProp: return "light";
    case PlaneShape::Twin: return "twin";
    case PlaneShape::Airliner: return "airliner";
    case PlaneShape::HeavyJet: return "heavy";
    case PlaneShape::Fighter: return "fighter";
    case PlaneShape::Helicopter: return "helicopter";
    case PlaneShape::Glider: return "glider";
    case PlaneShape::Balloon: return "balloon";
    case PlaneShape::Drone: return "drone";
    case PlaneShape::Ground: return "ground";
    case PlaneShape::Generic: break;
  }
  return "generic";
}

// The aggregator resolves the silhouette from real ICAO class and engine
// data for thousands of designators - far more than the prefix table below
// can cover - and sends it with the aircraft. Accepted when present, which
// is why the names here must match planeShapeName() exactly.
PlaneShape shapeFromName(const char *name) {
  if (!name || !name[0]) return PlaneShape::Generic;
  if (!strcmp(name, "light")) return PlaneShape::LightProp;
  if (!strcmp(name, "twin")) return PlaneShape::Twin;
  if (!strcmp(name, "airliner")) return PlaneShape::Airliner;
  if (!strcmp(name, "heavy")) return PlaneShape::HeavyJet;
  if (!strcmp(name, "fighter")) return PlaneShape::Fighter;
  if (!strcmp(name, "helicopter")) return PlaneShape::Helicopter;
  if (!strcmp(name, "glider")) return PlaneShape::Glider;
  if (!strcmp(name, "balloon")) return PlaneShape::Balloon;
  if (!strcmp(name, "drone")) return PlaneShape::Drone;
  if (!strcmp(name, "ground")) return PlaneShape::Ground;
  return PlaneShape::Generic;
}

PlaneShape shapeForAircraft(const char *typeDesignator, const char *category, bool onGround) {
  PlaneShape shape = PlaneShape::Generic;
  size_t bestPrefix = 0;
  if (typeDesignator && typeDesignator[0]) {
    for (const TypeShapeRule &rule : TYPE_SHAPE_RULES) {
      const size_t length = strlen(rule.prefix);
      if (length <= bestPrefix) continue;  // a longer match already won
      if (strncasecmp(typeDesignator, rule.prefix, length) == 0) {
        bestPrefix = length;
        shape = rule.shape;
      }
    }
  }
  if (bestPrefix) return shape;
  shape = shapeForCategory(category);
  // Nothing in the type table, nothing in the category, but it says it is on
  // the ground: that is surface traffic, not an aircraft whose class we
  // happen not to know. Airport ground stations and service vehicles report
  // exactly this - no type, no category, zero speed, no altitude - and drawing
  // them as something airborne put aircraft on the taxiways and the tower.
  // Narrow on purpose: an airliner at the gate is also on the ground, but it
  // has a type designator and never reaches here.
  if (shape == PlaneShape::Generic && onGround) return PlaneShape::Ground;
  return shape;
}

struct AircraftDisplay {
  int x;
  int y;
  float latitude;
  float longitude;
  float track;
  int positionSource;
  float distanceMiles;
  int altitudeFt;
  int geometricAltitudeFt;
  float speedKnots;
  float verticalRateFpm;
  float ageSeconds;
  float signalDb;
  uint32_t messages;
  bool onGround;
  char flight[9];
  char hex[8];
  char registration[12];
  char aircraftType[12];
  char squawk[8];
  char category[8];
  char operatorName[36];
  char country[28];
  char emergency[16];
  // Sent by the aggregator from the offline ICAO lists: the full model name
  // rather than the four-character designator, and the radio callsign ATC
  // actually says. Neither can be derived on the device - the lists are
  // 450 KB - so they arrive already resolved or not at all.
  char typeName[40];
  char telephony[24];
  // Resolved once per fetch rather than per frame - the type table is a
  // linear scan and icons are redrawn several times a second.
  PlaneShape iconShape = PlaneShape::Generic;
};

struct VesselDisplay {
  int x;
  int y;
  double latitude;
  double longitude;
  float speedKnots;
  float courseOverGround;
  float heading;  // -1 if not broadcast; AIS separates heading from course
  float distanceMiles;
  uint32_t mmsi;
  uint32_t lastUpdateMs;
  char name[24];
  char shipType[24];
  char navStatus[24];
};

uint16_t *framebuffer = nullptr;
uint16_t *baseMap = nullptr;
// PNGdec's decoder object carries its own line and Huffman buffers and is
// 44.5 KB. As a plain global it sits in internal DRAM, which is the scarcest
// memory on this board - the same pool the RGB bounce buffers and every
// mbedTLS handshake compete for. It is only used to decode map tiles, where
// PSRAM's extra latency costs nothing noticeable, so it lives there instead.
// Placement-new into a PSRAM allocation, done once in setup().
PNG *pngDecoderPtr = nullptr;

void initPngDecoder() {
  void *block = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // Falls back to the ordinary heap, not a static array: a static fallback
  // would occupy the internal DRAM this is moving out of, whether or not it
  // was ever needed, which defeats the whole change.
  if (!block) block = malloc(sizeof(PNG));
  pngDecoderPtr = block ? new (block) PNG() : nullptr;
}
#define pngDecoder (*pngDecoderPtr)
int pngTileScreenX = 0;
int pngTileScreenY = 0;
uint32_t nextFetchAt = 0;
uint32_t tokenExpiresAt = 0;
String bearerToken;
int lastCount = 0;
int lastMlat = 0;
long creditsRemaining = -1;
// 48 entries at 180 bytes is 8.4 KB, and as a plain array that is 8.4 KB of
// internal DRAM for something read a handful of times a second. PSRAM, for
// the same reason as the PNG decoder above.
// A view rather than a bare pointer so every existing use site - the
// range-for loops, the indexing, taking the address of an element - keeps
// working unchanged, and the fixed size stays attached to the type.
struct RouteCacheView {
  RouteCacheEntry *data = nullptr;
  RouteCacheEntry *begin() const { return data; }
  // end() == begin() when the allocation failed, so every range-for over
  // the cache iterates zero times instead of walking off a null pointer.
  RouteCacheEntry *end() const { return data ? data + ROUTE_CACHE_SIZE : data; }
  RouteCacheEntry &operator[](int index) const { return data[index]; }
};
RouteCacheView routeCache;

void initRouteCache() {
  routeCache.data = static_cast<RouteCacheEntry *>(heap_caps_calloc(
      ROUTE_CACHE_SIZE, sizeof(RouteCacheEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  // Ordinary heap as the fallback, for the same reason as the PNG decoder:
  // a static array would still cost the internal DRAM being reclaimed.
  if (!routeCache.data)
    routeCache.data = static_cast<RouteCacheEntry *>(calloc(ROUTE_CACHE_SIZE, sizeof(RouteCacheEntry)));
}
AircraftDisplay *latestAircraft = nullptr;
VesselDisplay *latestVessels = nullptr;
int vesselCount = 0;
// The aircraft feed and marine tracking both need a persistent TLS session
// (AIS) or frequent HTTPS fetches (REST providers), competing for the same
// tight internal RAM - running both at once was the root of the recurring
// SSL alloc failures. Off by default; enabling it stops the aircraft feed
// entirely rather than share the budget between two live feeds.
bool marineTrackingEnabled = false;
String marineProvider = "aisstream";
String aisApiKey;
String aisHubUsername;
String myShipTrackingApiKey;
String datalasticApiKey;
bool aisConnected = false;
uint32_t aisLastMessageAt = 0;
uint32_t nextMarineFetchAt = 0;
uint16_t marineRadiusNm = DEFAULT_MARINE_RADIUS_NM;
// AIS's TLS handshake competes for the same tight internal RAM as the
// aircraft fetch; retrying every few seconds after a real failure just
// keeps hammering an already-fragmented heap. Back off exponentially
// (8s/16s/32s/60s cap) instead, reset on an actual successful connect.
// aisIntentionalDisconnect distinguishes that from the deliberate pause
// around each aircraft fetch, which isn't a failure and shouldn't be
// penalised.
uint32_t aisNextRetryAt = 0;
uint8_t aisConsecutiveFailures = 0;
bool aisIntentionalDisconnect = false;
WebSocketsClient aisWebSocket;
// Defined near setup(), inside this same anonymous namespace, but
// handleMarineCredentials() below needs to call it earlier in the file.
void connectAisWebSocket();

bool marineConfigured() {
  if (marineProvider == "aishub") return aisHubUsername.length() > 0;
  if (marineProvider == "myshiptracking") return myShipTrackingApiKey.length() > 0;
  if (marineProvider == "datalastic") return datalasticApiKey.length() > 0;
  return aisApiKey.length() > 0;
}
// Page order matches the swipe order on the panel: Overview is the first
// screen, then swipe right advances Table -> Map -> Radar -> Marine and wraps.
enum class DisplayPage : uint8_t { Overview = 0, Table = 1, Map = 2, Radar = 3, Marine = 4 };
constexpr uint8_t DISPLAY_PAGE_COUNT = 5;
DisplayPage displayPage = DisplayPage::Overview;
float radarSweepDegrees = 0.0f;
uint32_t nextRadarFrameAt = 0;
uint32_t nextMarineRenderAt = 0;
uint32_t nextMarinePruneAt = 0;
bool touchReady = false;
uint8_t touchAddress = 0;
uint32_t lastTouchAt = 0;
int lastTapX = 0;
int lastTapY = 0;

// Screen positions of the aircraft icons drawn on the current frame, so a
// tap can be matched back to a specific aircraft. Rebuilt on every render of
// a page that plots icons (Overview, Map, Radar); MAX_AIRCRAFT is already
// the hard cap on how many can exist at once.
struct IconHit { int16_t x, y; int16_t aircraftIndex; };
IconHit iconHits[MAX_AIRCRAFT];
int iconHitCount = 0;
int detailAircraftIndex = -1;
uint32_t detailShownAt = 0;
// How many rows into latestAircraft (sorted nearest-first) the table page's
// visible window starts. Reset whenever the page is left so it always
// re-opens at the nearest aircraft rather than wherever it was scrolled to.
int tableScrollOffset = 0;
constexpr int TABLE_VISIBLE_ROWS = 10;
volatile bool bootButtonPending = false;
WebServer webServer(80);
Preferences settingsStore;
String managementPassword;
String firmwareUploadError;
bool firmwareUploadStarted = false;
bool firmwareUploadComplete = false;
size_t firmwareUploadBytes = 0;
String apiProvider = "opensky";
String openSkyClientId;
String openSkyClientSecret;
String rapidApiKey;
String aggregatorApiKey;
String flyItalyApiKey;
bool soundAlerts = true;
// Screensaver: after screensaverIdleMinutes with no touch/button/web-UI
// interaction, the panel switches to a rotating single-aircraft
// departure-board style display (renderScreensaverPage()) instead of
// whatever page was showing; any interaction dismisses it straight back to
// that page. lastInteractionAt is deliberately updated by both physical
// input (loop()) and web-driven changes (handlePageControl(),
// handleDisplaySettings()) - a receiver being administered over the web
// shouldn't be treated as sitting idle.
bool screensaverEnabled = false;
uint16_t screensaverIdleMinutes = 5;
bool screensaverActive = false;
uint32_t lastInteractionAt = 0;
// How often the screensaver reconsiders what it is showing. Slow on purpose:
// each repaint is a full-screen clear, and the signature check inside
// renderScreensaverPage() means a tick on an unchanged sky costs nothing.
constexpr uint32_t SCREENSAVER_REFRESH_MS = 10000UL;
uint32_t screensaverRefreshAt = 0;
uint8_t brightnessPercent = 100;
bool webServerReady = false;
bool restartPending = false;
bool setupPortalPending = false;
uint32_t restartAt = 0;
uint32_t lastFetchCompletedAt = 0;
uint32_t feedRequestStartedAt = 0;
uint32_t feedRequestDurationMs = 0;
int feedHttpCode = 0;
String feedStatus = "Not fetched";
// The body read used to be HTTPClient::writeToStreamDataBlock(), which has no
// overall deadline of its own, so a stalled transfer was killed by
// force-closing the socket from the other core. That cross-core teardown races
// with mbedTLS on the fetching core and corrupts the heap - see the
// readResponseBody() comment above PsramSink for the full evidence trail. The
// read now owns its own deadline on its own core, so nothing outside it needs
// to reach in and stop it, and these globals are gone with the mechanism.
float homeLatitude = DEFAULT_HOME_LAT;
float homeLongitude = DEFAULT_HOME_LON;
uint16_t queryRadiusNm = DEFAULT_RADIUS_NM;
uint8_t physicalMapZoom = 7;
bool physicalMapReady = false;
bool physicalMapRefreshPending = false;
bool githubCheckPending = false;
bool githubInstallPending = false;
bool githubUpdateAvailable = false;
String githubLatestVersion;
String githubFirmwareUrl;
String githubSignatureUrl;
String githubFirmwareSha256;
size_t githubFirmwareSize = 0;
uint8_t githubSignature[MAX_SIGNATURE_BYTES] = {};
size_t githubSignatureSize = 0;
String githubUpdateStatus = "Not checked";
bool sdMounted = false;
String sdStatus = "Not checked";
String sdCardType = "None";
uint64_t sdTotalBytes = 0;
uint64_t sdUsedBytes = 0;
bool stagedUpdateReady = false;
String stagedUpdateVersion;
String stagedUpdateSha256;
size_t stagedUpdateSize = 0;
String csrfToken;
bool mapRebuildActive = false;
int mapRebuildDone = 0;
int mapRebuildTotal = 0;
// A tile that fails to fetch/decode leaves the pre-tile dark ring pattern
// from drawLocationFallback() showing through that square permanently, since
// the finished framebuffer is unconditionally snapshotted into baseMap. Track
// misses so a failed rebuild retries itself instead of leaving that patch
// baked into the persisted map until someone notices and forces a rescan.
int mapRebuildMissingTiles = 0;
uint8_t mapRebuildRetryCount = 0;
uint32_t nextMapRetryAt = 0;
bool openSkyAuthRetryPending = false;
bool pageSavePending = false;
uint32_t pageSaveAt = 0;

// Network I/O (aircraft/marine fetches, the map tile rebuild, the AIS
// WebSocket) runs on its own FreeRTOS task pinned to the other core, so a
// slow fetch or a weak Wi-Fi signal can no longer freeze touch polling and
// page rendering on the UI side - previously everything shared one loop(),
// and a multi-second blocking HTTP call meant swipes were missed outright.
// Both tasks still touch the same buffers (latestAircraft, latestVessels,
// baseMap, framebuffer), so every access on either side is wrapped in this
// mutex. needsRedraw lets the network task ask the UI task to repaint the
// current page after data it owns (mainly the physical map) changes,
// without the network task touching the display itself.
SemaphoreHandle_t dataMutex = nullptr;
volatile bool needsRedraw = false;
// Must be internal RAM, not PSRAM: writing to flash (any Preferences/NVS
// call - this task does that constantly, e.g. page-save, location/provider
// settings) briefly disables the cache that also serves PSRAM access, and
// the CPU cannot keep executing code or touching a stack that lives in that
// disabled region. A PSRAM-backed stack was tried here to ease internal-RAM
// pressure and instead crashed reliably on the very next NVS write
// ("esp_task_stack_is_sane_cache_disabled()" assert) - ESP-IDF's own sanity
// check catching exactly this.
//
// 8192 (a guess at what the original single loop task used, never actually
// verified) was then tried here and reliably stack-overflowed instead
// ("Stack canary watchpoint triggered (network)"), consistently inside
// mbedTLS certificate parsing - TLS handshakes have a genuinely deep,
// stack-hungry call chain. 12288 is the same size that ran this exact
// workload without any stack-overflow symptom when it was (briefly, and for
// the unrelated reason above) in PSRAM, so it's a size known to be
// sufficient, just moved back to the RAM tier that's actually safe here.
constexpr uint32_t NETWORK_TASK_STACK_BYTES = 12288;

// RAII lock: guarantees the mutex is released on every return path, even
// through the many early returns inside fetchAircraft()/fetchAdsbV2Aircraft()
// and friends. Wrapping the call site with this instead of hand-threading
// xSemaphoreGive() through every branch removes an entire class of
// forgot-to-unlock deadlock risk.
class MutexGuard {
 public:
  // Recursive, not a plain mutex: refreshPhysicalBaseMap() (called under this
  // guard) calls webServer.handleClient() between tiles to stay responsive,
  // and that can dispatch a handler - handlePageControl() does - that takes
  // this same mutex on the same task. A plain mutex would deadlock there;
  // recursive re-entry by the same task is a no-op until the outermost
  // guard releases.
  explicit MutexGuard(SemaphoreHandle_t m) : mutex(m) { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
  ~MutexGuard() { xSemaphoreGiveRecursive(mutex); }
  MutexGuard(const MutexGuard &) = delete;
  MutexGuard &operator=(const MutexGuard &) = delete;
 private:
  SemaphoreHandle_t mutex;
};

String previousWifiSsid;
String previousWifiPassword;
uint32_t wifiRollbackAt = 0;

// Saved Wi-Fi networks, most recently connected first.
//
// The ESP32 remembers exactly one network, so moving the receiver between
// two places - a house and a club site, a bench and a shed - meant retyping
// a password every time even though it had been entered before. These are
// kept alongside that, as a fallback: the stock autoConnect() path is tried
// first and unchanged, and this list is only walked when it fails or when a
// working connection is lost for long enough to look permanent.
//
// Stored as one JSON string rather than numbered key pairs so adding,
// removing and reordering is a single NVS write and cannot leave a
// half-updated set of slots behind.
constexpr int MAX_SAVED_NETWORKS = 6;
constexpr uint32_t WIFI_RETRY_AFTER_MS = 60000UL;   // how long disconnected before walking the list
constexpr uint32_t WIFI_JOIN_TIMEOUT_MS = 12000UL;  // per network, while walking it
uint32_t wifiDisconnectedSince = 0;

String savedNetworksJson() {
  return settingsStore.getString("wifi-nets", "[]");
}

// SSIDs only. A password that has been stored is never sent back out, the
// same rule the provider credentials follow.
void savedNetworkNames(JsonArray out) {
  JsonDocument doc;
  if (deserializeJson(doc, savedNetworksJson()) != DeserializationError::Ok) return;
  for (JsonObjectConst entry : doc.as<JsonArrayConst>()) {
    const char *ssid = entry["s"];
    // String, not the const char*: that pointer is into this function's own
    // document, which dies on return, and ArduinoJson would have kept the
    // pointer rather than the text.
    if (ssid && *ssid) out.add(String(ssid));
  }
}

void rememberWifiNetwork(const String &ssid, const String &password) {
  if (!ssid.length()) return;
  JsonDocument stored;
  if (deserializeJson(stored, savedNetworksJson()) != DeserializationError::Ok)
    stored.to<JsonArray>();

  JsonDocument updated;
  JsonArray list = updated.to<JsonArray>();
  // This network goes to the front: the most recently working credentials
  // are the ones worth trying first next time.
  JsonObject first = list.add<JsonObject>();
  first["s"] = ssid;
  first["p"] = password;
  for (JsonObjectConst entry : stored.as<JsonArrayConst>()) {
    if (list.size() >= MAX_SAVED_NETWORKS) break;
    const char *existing = entry["s"];
    if (!existing || !*existing || ssid == existing) continue;  // no duplicates
    JsonObject copy = list.add<JsonObject>();
    copy["s"] = existing;
    copy["p"] = entry["p"] | "";
  }
  String payload;
  serializeJson(list, payload);
  settingsStore.putString("wifi-nets", payload);
}

bool forgetWifiNetwork(const String &ssid) {
  JsonDocument stored;
  if (deserializeJson(stored, savedNetworksJson()) != DeserializationError::Ok) return false;
  JsonDocument updated;
  JsonArray list = updated.to<JsonArray>();
  bool removed = false;
  for (JsonObjectConst entry : stored.as<JsonArrayConst>()) {
    const char *existing = entry["s"];
    if (!existing || !*existing) continue;
    if (ssid == existing) { removed = true; continue; }
    JsonObject copy = list.add<JsonObject>();
    copy["s"] = existing;
    copy["p"] = entry["p"] | "";
  }
  if (!removed) return false;
  String payload;
  serializeJson(list, payload);
  settingsStore.putString("wifi-nets", payload);
  return true;
}

// Tries each saved network in turn. Blocking, by design: it only ever runs
// when there is no connection, so there is nothing else for this core to
// usefully do, and a non-blocking state machine here would be a lot of
// machinery for a path taken once per outage.
bool connectSavedWifiNetwork() {
  JsonDocument doc;
  if (deserializeJson(doc, savedNetworksJson()) != DeserializationError::Ok) return false;
  for (JsonObjectConst entry : doc.as<JsonArrayConst>()) {
    // The station can come back on its own part way through - the router
    // rebooted, the link recovered. Stop rather than spend the rest of the
    // list tearing down a connection that just succeeded. This also bounds
    // the common case well below the whole list's worth of timeouts, which
    // matters because this task also services the web server.
    if (WiFi.status() == WL_CONNECTED) return true;
    const char *ssid = entry["s"];
    if (!ssid || !*ssid) continue;
    const char *password = entry["p"] | "";
    Serial.printf("Wi-Fi: trying saved network %s\n", ssid);
    WiFi.begin(ssid, password);
    const uint32_t deadline = millis() + WIFI_JOIN_TIMEOUT_MS;
    while (static_cast<int32_t>(millis() - deadline) < 0) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Wi-Fi: connected to saved network %s\n", ssid);
        // Re-save so a network that actually works moves to the front.
        rememberWifiNetwork(ssid, password);
        return true;
      }
      delay(250);
    }
  }
  return false;
}

// Certificate validation for every outbound HTTPS request. The OTA image is
// separately RSA-signed, but the RapidAPI key and the OpenSky client secret
// were previously sent over an unauthenticated channel. Build with
// -DADSB_TLS_INSECURE=1 to fall back to the old behaviour.
void applyTlsPolicy(WiFiClientSecure &client) {
#if ADSB_TLS_INSECURE
  client.setInsecure();
#else
  client.setCACertBundle(
      rootca_crt_bundle_start,
      static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
#endif
}

// Diagnostic for the recurring "-32512 SSL - Memory allocation failed":
// mbedTLS needs one contiguous internal-RAM block per handshake, and total
// free heap alone doesn't say whether that block is available - fragmented
// heap can fail this allocation with plenty of free bytes left. Logging both
// numbers around each fetch cycle will show whether the largest block keeps
// shrinking cycle over cycle (a leak somewhere) or is already pinned at a
// low ceiling from the very first cycle (something else holding it, e.g.
// the web server's own connections or WiFiManager's leftover state).
void logHeapDiagnostics(const char *tag) {
  Serial.printf("heap[%s]: free=%u largestInternal=%u\n", tag,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

// Where a fetch cycle's wall-clock time actually goes. The networkTask
// already reported that one fetch blocked it for 121186 ms, but a single
// total says nothing about which of the half-dozen blocking calls inside a
// fetch ran long - every one of them is separately bounded, so the total
// being two minutes means one of those bounds is not holding, and guessing
// which is not good enough. These accumulate per cycle and are printed by
// networkTask on any cycle that runs long, including the ones that return
// early, which a summary line at the end of fetchAircraft would miss.
struct FetchPhaseTimings {
  uint32_t aisPauseMs = 0;
  uint32_t connectMs = 0;   // http.begin() + GET, i.e. DNS + TCP + TLS + headers
  uint32_t bodyMs = 0;
  uint32_t parseMs = 0;
  uint32_t routeMs = 0;     // the whole per-aircraft enrichment loop
  uint32_t routeWorstMs = 0;
  uint32_t routeSaveMs = 0; // flash write; also disables the cache, see below
  uint32_t renderMs = 0;
  uint8_t attempts = 0;
  uint8_t routeLookups = 0;
  void reset() { *this = FetchPhaseTimings{}; }
};
FetchPhaseTimings fetchPhases;

// Anything past this and the breakdown is worth the serial bandwidth. A
// healthy cycle on a working feed completes in well under a second.
constexpr uint32_t FETCH_PHASE_REPORT_MS = 5000;

// One User-Agent for every outbound request, always matching the running
// build. OpenStreetMap's tile policy requires an identifying, accurate UA.
const String &userAgent() {
  static const String agent =
      String("2E0LXY-ESP32-ADSB/") + FIRMWARE_VERSION +
      " (+https://github.com/2E0LXY/ESP32-ADS-B)";
  return agent;
}

String bytesToHex(const uint8_t *bytes, size_t length) {
  static const char digits[] = "0123456789abcdef";
  String result;
  result.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    result += digits[bytes[i] >> 4];
    result += digits[bytes[i] & 0x0f];
  }
  return result;
}

bool hexToBytes(const String &hex, uint8_t *output, size_t length) {
  if (hex.length() != length * 2) return false;
  for (size_t i = 0; i < length; ++i) {
    const char high = hex[i * 2];
    const char low = hex[i * 2 + 1];
    if (!isxdigit(static_cast<unsigned char>(high)) ||
        !isxdigit(static_cast<unsigned char>(low))) return false;
    char pair[3] = {high, low, 0};
    char *end = nullptr;
    const long value = strtol(pair, &end, 16);
    if (end != pair + 2) return false;
    output[i] = static_cast<uint8_t>(value);
  }
  return true;
}

bool verifyFirmwareSignature(const String &digest) {
  if (githubSignatureSize < MIN_SIGNATURE_BYTES) return false;
  uint8_t hash[32];
  if (!hexToBytes(digest, hash, sizeof(hash))) return false;
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const int parsed = mbedtls_pk_parse_public_key(
      &key, reinterpret_cast<const unsigned char *>(FIRMWARE_PUBLIC_KEY),
      strlen(FIRMWARE_PUBLIC_KEY) + 1);
  // Reject anything that is not the RSA key this firmware expects rather than
  // relying on mbedtls_pk_verify alone to notice a substituted key type.
  const bool usableKey = parsed == 0 && mbedtls_pk_can_do(&key, MBEDTLS_PK_RSA);
  const int verified = usableKey ? mbedtls_pk_verify(
      &key, MBEDTLS_MD_SHA256, hash, sizeof(hash), githubSignature,
      githubSignatureSize) : (parsed == 0 ? -1 : parsed);
  mbedtls_pk_free(&key);
  return verified == 0;
}

bool downloadFirmwareSignature() {
  githubSignatureSize = 0;
  if (!githubSignatureUrl.length()) return false;
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(12000); http.setConnectTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, githubSignatureUrl)) return false;
  http.addHeader("User-Agent", userAgent());
  const int code = http.GET();
  const int advertised = http.getSize();
  if (code == HTTP_CODE_OK && advertised >= static_cast<int>(MIN_SIGNATURE_BYTES) &&
      advertised <= static_cast<int>(MAX_SIGNATURE_BYTES)) {
    NetworkClient *stream = http.getStreamPtr();
    githubSignatureSize = stream->readBytes(githubSignature, advertised);
    if (githubSignatureSize != static_cast<size_t>(advertised)) githubSignatureSize = 0;
  }
  http.end();
  return githubSignatureSize >= MIN_SIGNATURE_BYTES;
}

const char *sdTypeName(sdcard_type_t type) {
  switch (type) {
    case CARD_MMC: return "MMC";
    case CARD_SD: return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    default: return "None";
  }
}

bool mountSdCard() {
  SDCARD.end();
  sdMounted = false;
  sdStatus = "No card detected";
  sdCardType = "None";
  sdTotalBytes = 0;
  sdUsedBytes = 0;
  stagedUpdateReady = false;
#if BOARD_SD_SDMMC
  if (!SDCARD.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN) ||
      !SDCARD.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) {
#else
  // Chip select sits on the expander, which the SD library cannot drive. The
  // card is the only device on this SPI bus, so CS is asserted once and left
  // low; BOARD_SD_CS_GPIO is an unused pin handed to the library as a decoy.
  SPI.begin(BOARD_SD_SCK, BOARD_SD_MISO, BOARD_SD_MOSI);
  WS_CH422G::writePin(BOARD_SD_CS_EXIO, false);
  if (!SDCARD.begin(BOARD_SD_CS_GPIO, SPI, 20000000)) {
#endif
    Serial.println("SD card not mounted; using PSRAM and LittleFS");
    return false;
  }
  if (SDCARD.cardType() == CARD_NONE) {
    SDCARD.end();
    return false;
  }
  SDCARD.mkdir("/adsb");
  SDCARD.mkdir(SD_UPDATE_DIR);
  sdMounted = true;
  sdStatus = "Ready";
  sdCardType = sdTypeName(SDCARD.cardType());
  sdTotalBytes = SDCARD.totalBytes();
  sdUsedBytes = SDCARD.usedBytes();
  // Restore the staging metadata so a firmware staged before a reboot can
  // still be validated and installed; discard the file if it cannot be.
  stagedUpdateVersion = settingsStore.getString("staged-ver", "");
  stagedUpdateSha256 = settingsStore.getString("staged-sha", "");
  stagedUpdateSize = settingsStore.getULong("staged-size", 0);
  stagedUpdateReady = SDCARD.exists(SD_UPDATE_FILE);
  if (stagedUpdateReady && (!stagedUpdateSize || stagedUpdateSha256.length() != 64)) {
    SDCARD.remove(SD_UPDATE_FILE);
    stagedUpdateReady = false;
    stagedUpdateVersion = "";
    stagedUpdateSha256 = "";
    stagedUpdateSize = 0;
    Serial.println("Discarded staged firmware with no stored metadata");
  }
  SDCARD.remove(SD_UPDATE_PART);
  Serial.printf("SD card ready: %s, %.1f MB free\n", sdCardType.c_str(),
                (sdTotalBytes - sdUsedBytes) / 1048576.0);
  return true;
}

bool sha256File(fs::FS &filesystem, const char *path, String &digest) {
  File file = filesystem.open(path, FILE_READ);
  if (!file) return false;
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts(&context, 0) == 0;
  uint8_t buffer[4096];
  while (ok && file.available()) {
    const size_t count = file.read(buffer, sizeof(buffer));
    if (!count) { ok = false; break; }
    ok = mbedtls_sha256_update(&context, buffer, count) == 0;
    delay(0);
  }
  uint8_t output[32];
  if (ok) ok = mbedtls_sha256_finish(&context, output) == 0;
  mbedtls_sha256_free(&context);
  file.close();
  if (ok) digest = bytesToHex(output, sizeof(output));
  return ok;
}

void finishFeedAttempt(const char *statusText, int httpCode = 0) {
  feedStatus = statusText;
  feedHttpCode = httpCode;
  feedRequestDurationMs = feedRequestStartedAt ? millis() - feedRequestStartedAt : 0;
}

bool parseStrictDouble(const String &rawValue, double &result) {
  String value = rawValue;
  value.trim();
  if (!value.length()) return false;
  char *end = nullptr;
  result = strtod(value.c_str(), &end);
  return end != value.c_str() && *end == '\0' && isfinite(result);
}

bool parseStrictLong(const String &rawValue, long &result) {
  String value = rawValue;
  value.trim();
  if (!value.length()) return false;
  char *end = nullptr;
  result = strtol(value.c_str(), &end, 10);
  return end != value.c_str() && *end == '\0';
}

bool validWifiPassword(const String &password) {
  if (!password.length()) return true;
  if (password.length() >= 8 && password.length() <= 63) return true;
  if (password.length() != 64) return false;
  for (size_t i = 0; i < password.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(password[i]))) return false;
  }
  return true;
}

void IRAM_ATTR onBootButtonFalling() {
  bootButtonPending = true;
}

bool touchReadRegister(uint16_t reg, uint8_t *data, size_t length) {
  if (!touchAddress || !data || !length) return false;
  Wire.beginTransmission(touchAddress);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(touchAddress), static_cast<int>(length)) != length) return false;
  for (size_t i = 0; i < length; ++i) data[i] = Wire.read();
  return true;
}

bool touchWriteRegister(uint16_t reg, uint8_t value) {
  if (!touchAddress) return false;
  Wire.beginTransmission(touchAddress);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool probeGt911() {
  Serial.print("I2C devices:");
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", address);
  }
  Serial.println();
  const uint8_t candidates[] = {0x5D, 0x14};
  for (uint8_t address : candidates) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() != 0) continue;
    touchAddress = address;
    uint8_t product[4] = {};
    if (touchReadRegister(0x8140, product, sizeof(product))) {
      Serial.printf("GT911 touch ready at 0x%02X, product %.4s\n", address, product);
      touchWriteRegister(0x814E, 0);
      return true;
    }
    touchAddress = 0;
  }
  Serial.println("GT911 touch controller not found");
  return false;
}

bool beginTouch() {
#if BOARD_EXPANDER_CH32
  // Give GT911 a dedicated reset pulse after panel power has stabilised.
  WS_CH32_IO::writeRegister(Wire, WS_CH32_IO::REG_OUTPUT,
                            WS_CH32_IO::PIN_SYS_EN | WS_CH32_IO::PIN_LCD_RST);
  delay(80);
  WS_CH32_IO::writeRegister(Wire, WS_CH32_IO::REG_OUTPUT,
                            WS_CH32_IO::OUT_DISPLAY_ON);
  delay(300);
  // Waveshare's Rev4 touch example reopens the shared bus after LCD init.
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  Wire.setClock(WS_CH32_IO::DEFAULT_I2C_FREQ);
  delay(100);
  return probeGt911();
#else
  // CH422G boards: the GT911 reset line is EXIO1 and its INT line is a plain
  // GPIO. Hold INT low across the reset release so the controller latches the
  // 0x5D address, then probe on the shared bus.
  pinMode(BOARD_TOUCH_INT, OUTPUT);
  digitalWrite(BOARD_TOUCH_INT, LOW);
  WS_CH422G::writePin(BOARD_TOUCH_RST_EXIO, false);
  delay(20);
  WS_CH422G::writePin(BOARD_TOUCH_RST_EXIO, true);
  delay(10);
  pinMode(BOARD_TOUCH_INT, INPUT);
  delay(120);
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  Wire.setClock(400000);
  delay(50);
  return probeGt911();
#endif
}

enum class TouchGesture : uint8_t { None, Tap, SwipeLeft, SwipeRight, SwipeUp, SwipeDown };

// A swipe must travel this far in its dominant axis, stay mostly on that
// axis, and finish inside the time limit. Anything shorter that lifts
// cleanly is a tap - and anything that doesn't clear the 2:1 dominance ratio
// on either axis falls through to a tap too, which used to silently double
// as "advance page" for a vertical drag that missed being a clean swipe.
// Horizontal and vertical thresholds differ because the panel does: 800 px
// across but only 480 down, and a thumb scrolling a table travels a shorter
// distance than one sweeping between pages.
constexpr int SWIPE_MIN_X = 70;
constexpr int SWIPE_MIN_Y = 45;
constexpr uint32_t SWIPE_MAX_MS = 700;
// How much the dominant axis must beat the other. This was 2:1, which a real
// vertical drag routinely fails - a 100 px scroll with 60 px of thumb wobble
// is unmistakably vertical to a person and was classified as a tap, which on
// a page with no icons to hit meant "advance page". Scrolling the table was
// effectively impossible.
constexpr float SWIPE_AXIS_DOMINANCE = 1.4f;

// Reads the current contact, if any. GT911 keeps point 0 at 0x8150 as
// x-lo, x-hi, y-lo, y-hi. The status byte's high bit means the coordinate
// buffer is ready and must be cleared by writing zero back.
bool touchPoint(int &x, int &y) {
  uint8_t statusByte = 0;
  if (!touchReadRegister(0x814E, &statusByte, 1)) return false;
  if ((statusByte & 0x80) == 0) return false;
  const bool hasPoint = (statusByte & 0x0F) > 0;
  bool valid = false;
  if (hasPoint) {
    uint8_t point[4] = {};
    if (touchReadRegister(0x8150, point, sizeof(point))) {
      x = point[0] | (point[1] << 8);
      y = point[2] | (point[3] << 8);
      valid = true;
    }
  }
  touchWriteRegister(0x814E, 0);
  return valid;
}

TouchGesture touchGesture() {
  if (!touchReady) return TouchGesture::None;
  static bool down = false;
  static int startX = 0, startY = 0, lastX = 0, lastY = 0;
  static uint32_t startedAt = 0;

  // Peak excursion, not just the last sample before the lift. Touch is
  // polled once per loop() and a render can leave a long gap between
  // samples, so the finger is often already travelling back toward where it
  // started by the time the last point is read. Judging the gesture on that
  // point alone turned real swipes into taps.
  static int peakX = 0, peakY = 0;

  int x = 0, y = 0;
  const bool contact = touchPoint(x, y);

  if (contact) {
    if (!down) {
      down = true;
      startX = lastX = x;
      startY = lastY = y;
      peakX = peakY = 0;
      startedAt = millis();
    } else {
      lastX = x;
      lastY = y;
      if (abs(x - startX) > abs(peakX)) peakX = x - startX;
      if (abs(y - startY) > abs(peakY)) peakY = y - startY;
    }
    return TouchGesture::None;
  }

  if (!down) return TouchGesture::None;
  down = false;
  const uint32_t heldFor = millis() - startedAt;
  const int deltaX = abs(peakX) > abs(lastX - startX) ? peakX : lastX - startX;
  const int deltaY = abs(peakY) > abs(lastY - startY) ? peakY : lastY - startY;
  if (millis() - lastTouchAt < 350) return TouchGesture::None;
  lastTouchAt = millis();
  if (heldFor <= SWIPE_MAX_MS && abs(deltaX) >= SWIPE_MIN_X &&
      abs(deltaX) > abs(deltaY) * SWIPE_AXIS_DOMINANCE) {
    return deltaX < 0 ? TouchGesture::SwipeLeft : TouchGesture::SwipeRight;
  }
  // A vertical drag that missed a clean horizontal swipe used to fall all
  // the way through to a tap, which - on any page where the release point
  // didn't land on an aircraft icon - advanced the page exactly like a
  // horizontal swipe would. Table scrolling needs this recognised as its
  // own gesture instead.
  if (heldFor <= SWIPE_MAX_MS && abs(deltaY) >= SWIPE_MIN_Y &&
      abs(deltaY) > abs(deltaX) * SWIPE_AXIS_DOMINANCE) {
    return deltaY < 0 ? TouchGesture::SwipeUp : TouchGesture::SwipeDown;
  }
  lastTapX = lastX;
  lastTapY = lastY;
  return TouchGesture::Tap;
}

bool bootButtonTapped() {
  static uint32_t lastPressAt = 0;
  noInterrupts();
  const bool pending = bootButtonPending;
  bootButtonPending = false;
  interrupts();
  if (!pending || millis() - lastPressAt < 350) return false;
  lastPressAt = millis();
  return true;
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void pixel(int x, int y, uint16_t c) {
  if ((unsigned)x < W && (unsigned)y < H) framebuffer[y * W + x] = c;
}

void line(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    pixel(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void disc(int cx, int cy, int r, uint16_t c) {
  for (int y = -r; y <= r; ++y)
    for (int x = -r; x <= r; ++x)
      if (x*x + y*y <= r*r) pixel(cx+x, cy+y, c);
}

void filledRect(int x, int y, int width, int height, uint16_t c) {
  for (int yy = y; yy < y + height; ++yy)
    for (int xx = x; xx < x + width; ++xx) pixel(xx, yy, c);
}

// Standard sorted-scanline triangle fill - the only solid-shape primitive
// available besides disc()/filledRect(), used to build bold aircraft
// silhouettes instead of the thin wireframe outlines a plain line() gives.
void filledTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
  auto swapInt = [](int &a, int &b) { const int t = a; a = b; b = t; };
  if (y0 > y1) { swapInt(x0, x1); swapInt(y0, y1); }
  if (y0 > y2) { swapInt(x0, x2); swapInt(y0, y2); }
  if (y1 > y2) { swapInt(x1, x2); swapInt(y1, y2); }
  auto edge = [](int ya, int xa, int yb, int xb, int y) {
    if (yb == ya) return static_cast<float>(xa);
    return xa + (xb - xa) * static_cast<float>(y - ya) / (yb - ya);
  };
  for (int y = y0; y <= y2; ++y) {
    const float xLeftFull = edge(y0, x0, y2, x2, y);
    const float xOther = (y < y1) ? edge(y0, x0, y1, x1, y) : edge(y1, x1, y2, x2, y);
    int xa = lroundf(xLeftFull), xb = lroundf(xOther);
    if (xa > xb) swapInt(xa, xb);
    for (int x = xa; x <= xb; ++x) pixel(x, y, c);
  }
}

const uint8_t *glyph(char ch) {
  static const uint8_t chars[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
  };
  // Punctuation. Without these the font silently rendered a blank for every
  // one of them, which is not a cosmetic problem: "ALT:3.4KFT" came out as
  // "ALT 3 4KFT" and a coordinate pair as "53 7326 -1 4579". A missing
  // decimal point does not look like a missing glyph, it looks like a
  // different number.
  static const char punctuationKeys[] = ".,:;/'()[]+=*?!#%<>_&@$\"";
  static const uint8_t punctuation[][5] = {
      {0x00, 0x60, 0x60, 0x00, 0x00},  // .
      {0x00, 0x50, 0x30, 0x00, 0x00},  // ,
      {0x00, 0x36, 0x36, 0x00, 0x00},  // :
      {0x00, 0x56, 0x36, 0x00, 0x00},  // ;
      {0x20, 0x10, 0x08, 0x04, 0x02},  // /
      {0x00, 0x00, 0x07, 0x00, 0x00},  // '
      {0x00, 0x1C, 0x22, 0x41, 0x00},  // (
      {0x00, 0x41, 0x22, 0x1C, 0x00},  // )
      {0x00, 0x7F, 0x41, 0x41, 0x00},  // [
      {0x00, 0x41, 0x41, 0x7F, 0x00},  // ]
      {0x08, 0x08, 0x3E, 0x08, 0x08},  // +
      {0x14, 0x14, 0x14, 0x14, 0x14},  // =
      {0x14, 0x08, 0x3E, 0x08, 0x14},  // *
      {0x02, 0x01, 0x51, 0x09, 0x06},  // ?
      {0x00, 0x00, 0x5F, 0x00, 0x00},  // !
      {0x14, 0x7F, 0x14, 0x7F, 0x14},  // #
      {0x23, 0x13, 0x08, 0x64, 0x62},  // %
      {0x08, 0x14, 0x22, 0x41, 0x00},  // <
      {0x41, 0x22, 0x14, 0x08, 0x00},  // >
      {0x40, 0x40, 0x40, 0x40, 0x40},  // _
      {0x36, 0x49, 0x55, 0x22, 0x50},  // &
      {0x32, 0x49, 0x79, 0x41, 0x3E},  // @
      {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // $
      {0x00, 0x07, 0x00, 0x07, 0x00},  // "
  };
  static_assert(sizeof(punctuation) / sizeof(punctuation[0]) ==
                    sizeof(punctuationKeys) - 1,
                "every punctuation key needs exactly one bitmap");
  static const uint8_t blank[5] = {};
  static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
  if (ch >= '0' && ch <= '9') return chars[ch-'0'];
  if (ch >= 'A' && ch <= 'Z') return chars[10+ch-'A'];
  if (ch == '-') return dash;
  // strchr would also match the terminator, which would index one past the
  // table for a NUL that callers never draw but could still reach here.
  if (ch) {
    if (const char *found = strchr(punctuationKeys, ch))
      return punctuation[found - punctuationKeys];
  }
  return blank;
}

void text5(int x, int y, const char *s, uint16_t c, int scale=1) {
  while (*s) {
    const uint8_t *g = glyph(static_cast<char>(toupper(static_cast<unsigned char>(*s++))));
    for (int col=0; col<5; ++col) for (int row=0; row<7; ++row)
      if (g[col] & (1 << row)) for(int yy=0;yy<scale;++yy) for(int xx=0;xx<scale;++xx)
        pixel(x+col*scale+xx, y+row*scale+yy, c);
    x += 6*scale;
  }
}

void restoreMap() {
  if (physicalMapReady && baseMap) memcpy(framebuffer, baseMap, W * H * sizeof(uint16_t));
  else if (W == ASSET_W && H == ASSET_H)
    memcpy_P(framebuffer, MAP_IMAGE, W * H * sizeof(uint16_t));
  else
    // The baked map is 480x480 and does not fit this panel. Clear instead of
    // overrunning the array; OSM tiles replace it once cached anyway.
    memset(framebuffer, 0, W * H * sizeof(uint16_t));
}

double osmWorldX(double longitude, uint8_t zoom) {
  const double size = 256.0 * (1UL << zoom);
  return (longitude + 180.0) / 360.0 * size;
}

double osmWorldY(double latitude, uint8_t zoom) {
  const double clamped = constrain(latitude, -85.05112878, 85.05112878);
  const double radiansLatitude = radians(clamped);
  const double size = 256.0 * (1UL << zoom);
  return (1.0 - log(tan(radiansLatitude) + 1.0 / cos(radiansLatitude)) / PI) * 0.5 * size;
}

uint8_t zoomForRadius() {
  const double diameterMeters = max(1.0, static_cast<double>(queryRadiusNm) * 1852.0 * 2.2);
  const double ratio = 156543.03392 * cos(radians(homeLatitude)) * W / diameterMeters;
  return constrain(static_cast<int>(floor(log(ratio) / log(2.0))), 3, 16);
}

bool mapPoint(float lat, float lon, int &x, int &y) {
  const double size = 256.0 * (1UL << physicalMapZoom);
  double dx = osmWorldX(lon, physicalMapZoom) - osmWorldX(homeLongitude, physicalMapZoom);
  if (dx > size / 2) dx -= size;
  if (dx < -size / 2) dx += size;
  x = lround(dx + W / 2.0);
  y = lround(osmWorldY(lat, physicalMapZoom) - osmWorldY(homeLatitude, physicalMapZoom) + H / 2.0);
  return x >= 0 && x < W && y >= 0 && y < H;
}

float distanceMilesFromHome(float lat, float lon) {
  float dLat = radians(lat - homeLatitude);
  float dLon = radians(lon - homeLongitude);
  float a = sinf(dLat/2)*sinf(dLat/2) + cosf(radians(homeLatitude))*cosf(radians(lat))*sinf(dLon/2)*sinf(dLon/2);
  return 3958.761f * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f-a));
}

float bearingFromHome(float lat, float lon) {
  const float latitude1 = radians(homeLatitude);
  const float latitude2 = radians(lat);
  const float deltaLongitude = radians(lon - homeLongitude);
  const float y = sinf(deltaLongitude) * cosf(latitude2);
  const float x = cosf(latitude1) * sinf(latitude2) -
                  sinf(latitude1) * cosf(latitude2) * cosf(deltaLongitude);
  float bearing = degrees(atan2f(y, x));
  if (bearing < 0.0f) bearing += 360.0f;
  return bearing;
}

int drawPngLine(PNGDRAW *draw) {
  uint16_t pixels[256];
  pngDecoder.getLineAsRGB565(draw, pixels, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  const int destinationY = pngTileScreenY + draw->y;
  if (destinationY < 0 || destinationY >= H) return 1;
  const int sourceX = max(0, -pngTileScreenX);
  const int destinationX = max(0, pngTileScreenX);
  const int count = min(draw->iWidth - sourceX, W - destinationX);
  if (count > 0) memcpy(framebuffer + destinationY * W + destinationX,
                        pixels + sourceX, count * sizeof(uint16_t));
  return 1;
}

// Operator logos.
//
// The screensaver drew three initials in a tinted square because that is all
// the device could produce on its own. The aggregator now caches each
// airline's real logo (see server/app/logos.py), so the panel can fetch one
// once, keep it on the card, and draw it for ever after.
//
// Same machinery as the map tiles deliberately: an HTTPS GET written
// straight to a file, then read back into PSRAM and handed to PNGdec, whose
// line callback writes into the framebuffer. That path is already proven on
// this board. The logos are 128x128 8-bit RGB, non-interlaced and without an
// alpha channel - checked against the live API, not assumed - which is the
// same shape as a map tile, so nothing new has to handle transparency.
constexpr int LOGO_PIXELS = 128;  // must be one of the server's allowed sizes
// One outstanding request at a time, set by the render path and consumed by
// the network task. Deliberately not a queue: the screensaver shows one
// airline at a time, so the next rotation asks for the next logo, and a
// backlog would only mean fetching logos for aircraft that have since left.
volatile bool logoFetchPending = false;
char logoFetchCode[4] = {0};

// The same one-at-a-time arrangement for the type photograph. Separate from
// the logo so a type with no photo does not block that airline's logo, and
// vice versa.
volatile bool photoFetchPending = false;
char photoFetchCode[8] = {0};
constexpr int MAX_PHOTOS_UNAVAILABLE = 24;
char photoUnavailable[MAX_PHOTOS_UNAVAILABLE][8] = {};
int photoUnavailableCount = 0;

bool photoKnownUnavailable(const char *code) {
  for (int i = 0; i < photoUnavailableCount; ++i)
    if (!strcmp(photoUnavailable[i], code)) return true;
  return false;
}

void rememberPhotoUnavailable(const char *code) {
  if (photoKnownUnavailable(code)) return;
  if (photoUnavailableCount >= MAX_PHOTOS_UNAVAILABLE) return;  // the card still remembers
  strncpy(photoUnavailable[photoUnavailableCount], code, 7);
  photoUnavailable[photoUnavailableCount++][7] = 0;
}

// What a fetch attempt actually achieved. The distinction matters: only a
// newly written image justifies a repaint. Treating "already know there is
// no logo" as success repainted the whole screen, which set the tile asking
// again, which repainted again - a spin of full-frame repaints, and a
// full-frame repaint is the exact burst that makes this panel slip.
enum class LogoFetch { Cached, Unavailable, Retry };

// Airlines the aggregator has no logo for, remembered in RAM as well as on
// the card so the render path stops asking at all rather than asking and
// being told no from disk on every rotation.
constexpr int MAX_LOGOS_UNAVAILABLE = 24;
char logoUnavailable[MAX_LOGOS_UNAVAILABLE][4] = {};
int logoUnavailableCount = 0;

bool logoKnownUnavailable(const char *code) {
  for (int i = 0; i < logoUnavailableCount; ++i)
    if (!strcmp(logoUnavailable[i], code)) return true;
  return false;
}

void rememberLogoUnavailable(const char *code) {
  if (logoKnownUnavailable(code)) return;
  if (logoUnavailableCount >= MAX_LOGOS_UNAVAILABLE) return;  // the card still remembers
  memcpy(logoUnavailable[logoUnavailableCount++], code, 4);
}
// A decoded image held in PSRAM, and which key it belongs to.
//
// Decoded once per subject rather than once per repaint. Reading the card and
// running PNGdec on every screensaver repaint - on the bus the panel refills
// its bounce buffer from - is exactly the kind of load this display cannot
// absorb. A staging buffer turns each repaint into a memcpy.
//
// Two of these: the airline logo and the aircraft type photograph. They share
// one decode callback rather than having a near-identical one each, because
// two copies of this drift apart.
struct ImageStage {
  uint16_t *pixels = nullptr;
  int width = 0;
  int height = 0;
  char key[8] = {0};
};

ImageStage logoStage;
ImageStage photoStage;
ImageStage *decodeStageTarget = nullptr;

int decodeStagePngLine(PNGDRAW *draw) {
  ImageStage *stage = decodeStageTarget;
  // A wider row than the stage would run past the end of the line it is
  // writing into. A cache file written by an older build, or a server that
  // starts answering differently, must not be able to corrupt memory here.
  if (!stage || !stage->pixels || draw->iWidth > stage->width) return 0;
  if (draw->y < 0 || draw->y >= stage->height) return 1;  // taller than asked: ignore the rest
  pngDecoder.getLineAsRGB565(draw, stage->pixels + draw->y * stage->width,
                             PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  return 1;
}

// Allocates the staging buffer on first use. PSRAM only: internal RAM is the
// scarce resource the TLS handshake needs, and taking it from there to draw a
// picture would trade a working feed for a prettier panel.
bool ensureStage(ImageStage &stage, int width, int height) {
  if (stage.pixels) return true;
  stage.pixels = static_cast<uint16_t *>(heap_caps_malloc(
      static_cast<size_t>(width) * height * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!stage.pixels) return false;
  stage.width = width;
  stage.height = height;
  return true;
}

// Reads a cached PNG from the card into the stage. Shared by both images.
bool decodeCachedImage(ImageStage &stage, const String &path, const char *key) {
  if (!pngDecoderPtr || !stage.pixels) return false;
  fs::FS &cache = sdMounted ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  File file = cache.open(path, FILE_READ);
  if (!file) return false;
  const size_t bytes = file.size();
  if (!bytes || bytes > 160UL * 1024UL) { file.close(); cache.remove(path); return false; }
  uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!data) data = static_cast<uint8_t *>(malloc(bytes));
  if (!data) { file.close(); return false; }
  const size_t read = file.read(data, bytes);
  file.close();
  if (read != bytes) {
    heap_caps_free(data);
    cache.remove(path);
    return false;
  }

  // Cleared first: an image smaller than the stage would otherwise leave the
  // previous subject's pixels showing around the edges of this one.
  memset(stage.pixels, 0, static_cast<size_t>(stage.width) * stage.height * sizeof(uint16_t));
  stage.key[0] = 0;
  decodeStageTarget = &stage;
  const int opened = pngDecoder.openRAM(data, bytes, decodeStagePngLine);
  const bool success = opened == PNG_SUCCESS && pngDecoder.decode(nullptr, 0) == PNG_SUCCESS;
  if (opened == PNG_SUCCESS) pngDecoder.close();
  decodeStageTarget = nullptr;
  heap_caps_free(data);
  if (!success) {
    // Same reasoning as the map tiles: a corrupt file would otherwise sit
    // there failing to draw for ever.
    cache.remove(path);
    Serial.printf("Removed undecodable cached image %s\n", path.c_str());
    return false;
  }
  strncpy(stage.key, key, sizeof(stage.key) - 1);
  stage.key[sizeof(stage.key) - 1] = 0;
  return true;
}

// Copies a stage to the panel, clipped to the screen. The stage may be
// smaller than the box it is centred in.
void blitStage(const ImageStage &stage, int left, int top) {
  if (!framebuffer || !stage.pixels) return;
  for (int row = 0; row < stage.height; ++row) {
    const int destinationY = top + row;
    if (destinationY < 0 || destinationY >= H) continue;
    const int destinationX = max(0, left);
    const int sourceX = max(0, -left);
    const int count = min(stage.width - sourceX, W - destinationX);
    if (count > 0) memcpy(framebuffer + destinationY * W + destinationX,
                          stage.pixels + row * stage.width + sourceX,
                          count * sizeof(uint16_t));
  }
}

// One decoded copy of the boot screen, on its way from the PNG in flash to
// the panel. Allocated for the decode and freed the moment it has been
// drawn.
//
// Decoding straight to the panel a line at a time needs only 1.6 KB, but it
// paints visibly from the top down over a couple of hundred milliseconds.
// The picture should appear at once, so it is assembled off-screen and
// blitted in one go - and then handed straight back, so the 768 KB is held
// for the decode rather than from boot until the live display starts. Map
// tiles, airline logos and aircraft photographs all want that PSRAM for the
// rest of the run.
uint16_t *bootPixels = nullptr;

int decodeBootPngLine(PNGDRAW *draw) {
  // A row wider than the buffer would run past the end of the line it is
  // writing into. The asset is generated to BOOT_IMAGE_W, but a header from
  // another build must not be able to corrupt memory here.
  if (!bootPixels || draw->iWidth > BOOT_IMAGE_W) return 0;
  if (draw->y < 0 || draw->y >= BOOT_IMAGE_H) return 1;
  pngDecoder.getLineAsRGB565(draw, bootPixels + draw->y * BOOT_IMAGE_W,
                             PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  return 1;
}

// Draws the boot screen from the PNG in flash. PROGMEM on the ESP32-S3 is
// memory-mapped, so PNGdec reads the array in place - nothing is copied out
// of flash first.
bool paintBootImage() {
  if (!pngDecoderPtr) return false;
  const size_t bytes = static_cast<size_t>(BOOT_IMAGE_W) * BOOT_IMAGE_H * sizeof(uint16_t);
  // PSRAM only, with no fallback: 768 KB is not something the internal pool
  // could give up even if it had it, and the caller draws a plain title
  // instead rather than failing to boot.
  bootPixels = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!bootPixels) {
    Serial.println("Boot image: no PSRAM for the decode buffer");
    return false;
  }
  const int opened = pngDecoder.openRAM(const_cast<uint8_t *>(BOOT_IMAGE_PNG),
                                        BOOT_IMAGE_PNG_LEN, decodeBootPngLine);
  const bool success = opened == PNG_SUCCESS && pngDecoder.decode(nullptr, 0) == PNG_SUCCESS;
  if (opened == PNG_SUCCESS) pngDecoder.close();
  // Only on success: half a picture is worse than the fallback title, and a
  // partial decode would otherwise show whatever the rest of the buffer
  // happened to contain.
  if (success) {
    gfx->draw16bitRGBBitmap((W - BOOT_IMAGE_W) / 2, (H - BOOT_IMAGE_H) / 2,
                            bootPixels, BOOT_IMAGE_W, BOOT_IMAGE_H);
  } else {
    Serial.printf("Boot image decode failed (open=%d)\n", opened);
  }
  heap_caps_free(bootPixels);
  bootPixels = nullptr;
  return success;
}

// The ICAO operator prefix of a callsign: RYR2BH is Ryanair. Empty when the
// callsign cannot carry one, which is most GA traffic - those keep the
// initials tile.
bool operatorLogoCode(const char *flight, char *out) {
  out[0] = 0;
  if (!flight) return false;
  // Four characters minimum: three letters and at least one of the flight
  // number. A bare three-letter callsign is not an airline flight.
  if (strlen(flight) < 4) return false;
  for (int i = 0; i < 3; ++i) {
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(flight[i])));
    if (c < 'A' || c > 'Z') return false;
    out[i] = c;
  }
  out[3] = 0;
  return true;
}

String operatorLogoPath(const char *code) {
  return (sdMounted ? "/adsb/logo_" : "/logo_") + String(code) + ".png";
}

// A logo the aggregator has none of. Recorded so the device stops asking;
// without it every screensaver rotation past a cargo or charter operator
// would spend another request for the same 404.
String operatorLogoMissPath(const char *code) {
  return (sdMounted ? "/adsb/logo_" : "/logo_") + String(code) + ".none";
}

bool drawCachedOperatorLogo(int x, int y, int size, const char *code) {
  if (!framebuffer || !ensureStage(logoStage, LOGO_PIXELS, LOGO_PIXELS)) return false;
  if (strcmp(logoStage.key, code) &&
      !decodeCachedImage(logoStage, operatorLogoPath(code), code)) return false;
  // Centred in the tile rather than scaled to it: 128 into 160 leaves an even
  // margin, and a scaler here would be a lot of code for sixteen pixels.
  blitStage(logoStage, x + (size - logoStage.width) / 2, y + (size - logoStage.height) / 2);
  return true;
}

// The aircraft type photograph: a landscape band, drawn in the space the
// departure board leaves below its last row. Same lifecycle as the logo -
// fetched once per type from the aggregator, kept on the card, decoded once
// per type into PSRAM.
constexpr int PHOTO_WIDTH = 160;  // must be one of the server's allowed widths
constexpr int PHOTO_HEIGHT = 96;  // the server crops to 5:3

String typePhotoPath(const char *designator) {
  return (sdMounted ? "/adsb/photo_" : "/photo_") + String(designator) + ".png";
}

String typePhotoMissPath(const char *designator) {
  return (sdMounted ? "/adsb/photo_" : "/photo_") + String(designator) + ".none";
}

// Designators are up to four characters and are used in a filename, so
// anything else is refused rather than sanitised.
bool typePhotoCode(const char *designator, char *out) {
  out[0] = 0;
  if (!designator) return false;
  const size_t length = strlen(designator);
  if (length < 2 || length > 4) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(designator[i])));
    if (!isalnum(static_cast<unsigned char>(c))) return false;
    out[i] = c;
  }
  out[length] = 0;
  return true;
}

// Whether a photograph could be drawn right now, without decoding one.
// The board reserves horizontal space for it before laying out its text, so
// this has to be answerable before the draw.
bool typePhotoAvailable(const char *designator) {
  if (!strcmp(photoStage.key, designator)) return true;  // already decoded
  fs::FS &cache = sdMounted ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  return cache.exists(typePhotoPath(designator));
}

bool drawCachedTypePhoto(int left, int top, const char *designator) {
  if (!framebuffer || !ensureStage(photoStage, PHOTO_WIDTH, PHOTO_HEIGHT)) return false;
  if (strcmp(photoStage.key, designator) &&
      !decodeCachedImage(photoStage, typePhotoPath(designator), designator)) return false;
  blitStage(photoStage, left, top);
  return true;
}

String osmTilePath(uint8_t zoom, int tileX, int tileY) {
  return (sdMounted ? "/adsb/osm_" : "/osm_") + String(zoom) + "_" +
         String(tileX) + "_" + String(tileY) + ".png";
}

uint64_t tileCacheFreeBytes() {
  if (sdMounted) {
    const uint64_t total = SDCARD.totalBytes();
    const uint64_t used = SDCARD.usedBytes();
    return total > used ? total - used : 0;
  }
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();
  return total > used ? total - used : 0;
}

// A position, range or zoom change invalidates every cached tile, and nothing
// previously deleted them. Once the partition filled, cacheOsmTile() failed
// silently and the LCD stayed on the radar fallback for good.
int clearTileCache(bool littleFsOnly = false) {
  const bool useSd = sdMounted && !littleFsOnly;
  fs::FS &cache = useSd ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  const String directory = useSd ? "/adsb" : "/";
  std::vector<String> victims;
  File dir = cache.open(directory.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    String name = entry.name();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.startsWith("osm_") && name.endsWith(".png")) {
      victims.push_back(directory.endsWith("/") ? directory + name
                                                : directory + "/" + name);
    }
    entry.close();
  }
  dir.close();
  int removed = 0;
  for (const String &victim : victims) {
    if (cache.remove(victim)) ++removed;
    delay(0);
  }
  if (removed) Serial.printf("Removed %d cached map tiles from %s\n", removed,
                             useSd ? "SD" : "LittleFS");
  return removed;
}

bool cacheOsmTile(uint8_t zoom, int tileX, int tileY, const String &path) {
  fs::FS &cache = sdMounted ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  if (cache.exists(path)) return true;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("OSM tile %d/%d/%d skipped: WiFi not connected\n", zoom, tileX, tileY);
    return false;
  }
  if (tileCacheFreeBytes() < MIN_TILE_CACHE_FREE_BYTES) {
    clearTileCache();
    if (tileCacheFreeBytes() < MIN_TILE_CACHE_FREE_BYTES) {
      Serial.println("Tile cache storage is full; skipping tile download");
      return false;
    }
  }
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(12000); http.setConnectTimeout(12000);
  const String url = "https://tile.openstreetmap.org/" + String(zoom) + "/" + String(tileX) + "/" + String(tileY) + ".png";
  if (!http.begin(client, url)) {
    Serial.printf("OSM tile %d/%d/%d begin() failed\n", zoom, tileX, tileY);
    return false;
  }
  http.addHeader("User-Agent", userAgent());
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("OSM tile %d/%d/%d HTTP %d\n", zoom, tileX, tileY, code);
    http.end();
    return false;
  }
  const int expected = http.getSize();
  File file = cache.open(path, FILE_WRITE);
  const int written = file ? http.writeToStream(&file) : -1;
  if (file) file.close();
  http.end();
  // A short write used to be committed as a valid cache entry and then decode
  // as half a tile for ever. Verify against Content-Length before keeping it.
  if (written <= 0 || (expected > 0 && written != expected)) {
    Serial.printf("OSM tile %d/%d/%d write failed: %d of %d bytes\n", zoom,
                  tileX, tileY, written, expected);
    cache.remove(path);
    return false;
  }
  return true;
}

// Fetched from our own aggregator rather than logo.dev directly: the logo is
// already cached there for every customer, so this costs the provider
// nothing, needs no token on the device, and works whichever aircraft feed
// the user has selected - the endpoint is deliberately unauthenticated.
constexpr char LOGO_ENDPOINT[] = "https://adsb.2e0lxy.uk/logo/airline/";
// The TLS handshake for this needs a contiguous internal block, and that is
// exactly what the device has least of - it is why the route lookups had to
// move to the server. So a logo is only ever fetched when there is headroom;
// otherwise it waits for a later rotation. Worst case the initials tile
// stays, which is what was there before.
//
// 28 KB from measurement, not from caution. On this board the largest free
// internal block settles at 31,732 bytes, and the aircraft fetch completes a
// TLS handshake at exactly that level every thirty seconds with a 39 KB
// response body. A logo is a fifth of that size and runs on the same task
// between those fetches, so it faces the same conditions the feed already
// survives. The first attempt at this was 48 KB, which is more than this
// board ever has free - the guard could never pass and no logo was ever
// fetched.
constexpr size_t LOGO_MIN_INTERNAL_BLOCK = 28u * 1024u;

LogoFetch cacheOperatorLogo(const char *code) {
  fs::FS &cache = sdMounted ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  const String path = operatorLogoPath(code);
  const String missPath = operatorLogoMissPath(code);
  if (cache.exists(missPath)) return LogoFetch::Unavailable;
  // Present already means the draw that asked for this failed to decode it
  // and removed it, or another pass just fetched it. Either way it is worth
  // one repaint to find out.
  if (cache.exists(path)) return LogoFetch::Cached;
  if (WiFi.status() != WL_CONNECTED) return LogoFetch::Retry;
  if (tileCacheFreeBytes() < MIN_TILE_CACHE_FREE_BYTES) return LogoFetch::Retry;
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (largest < LOGO_MIN_INTERNAL_BLOCK) {
    Serial.printf("Logo %s deferred: largestInternal=%u\n", code, static_cast<unsigned>(largest));
    return LogoFetch::Retry;
  }

  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(12000); http.setConnectTimeout(12000);
  // No theme parameter: the aggregator already requests the dark-background
  // variant for every consumer, which is why these arrive as plain RGB with
  // no alpha channel for this device to composite.
  const String url = String(LOGO_ENDPOINT) + code + ".png?size=" + String(LOGO_PIXELS);
  if (!http.begin(client, url)) return LogoFetch::Retry;
  http.addHeader("User-Agent", userAgent());
  const int status = http.GET();
  if (status == HTTP_CODE_NOT_FOUND) {
    // A real answer, not a failure: the aggregator has no logo for this
    // airline. Remember it so we stop asking.
    http.end();
    File marker = cache.open(missPath, FILE_WRITE);
    if (marker) marker.close();
    Serial.printf("Logo %s: none available\n", code);
    return LogoFetch::Unavailable;
  }
  if (status == 503) {
    // The aggregator is reachable but has no logo token configured. Worth
    // saying plainly rather than as a bare status code, because the fix is
    // on the server and nothing on the device will change until it is made.
    Serial.printf("Logo %s: aggregator has no logo token configured\n", code);
    http.end();
    return LogoFetch::Retry;
  }
  if (status != HTTP_CODE_OK) {
    Serial.printf("Logo %s HTTP %d\n", code, status);
    http.end();
    return LogoFetch::Retry;
  }
  const int expected = http.getSize();
  File file = cache.open(path, FILE_WRITE);
  const int written = file ? http.writeToStream(&file) : -1;
  if (file) file.close();
  http.end();
  // Same short-write check as the map tiles: a truncated PNG committed to
  // the cache would fail to decode for ever after.
  if (written <= 0 || (expected > 0 && written != expected)) {
    Serial.printf("Logo %s write failed: %d of %d bytes\n", code, written, expected);
    cache.remove(path);
    return LogoFetch::Retry;
  }
  Serial.printf("Logo %s cached (%d bytes)\n", code, written);
  return LogoFetch::Cached;
}

constexpr char PHOTO_ENDPOINT[] = "https://adsb.2e0lxy.uk/aircraft-photo/";

// Same guards as the logo, for the same reason: the handshake competes for
// contiguous internal RAM, which is the scarce resource on this board.
LogoFetch cacheTypePhoto(const char *designator) {
  fs::FS &cache = sdMounted ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  const String path = typePhotoPath(designator);
  const String missPath = typePhotoMissPath(designator);
  if (cache.exists(missPath)) return LogoFetch::Unavailable;
  if (cache.exists(path)) return LogoFetch::Cached;
  if (WiFi.status() != WL_CONNECTED) return LogoFetch::Retry;
  if (tileCacheFreeBytes() < MIN_TILE_CACHE_FREE_BYTES) return LogoFetch::Retry;
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (largest < LOGO_MIN_INTERNAL_BLOCK) {
    Serial.printf("Photo %s deferred: largestInternal=%u\n", designator,
                  static_cast<unsigned>(largest));
    return LogoFetch::Retry;
  }

  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(15000); http.setConnectTimeout(15000);
  const String url = String(PHOTO_ENDPOINT) + designator + ".png?size=" + String(PHOTO_WIDTH);
  if (!http.begin(client, url)) return LogoFetch::Retry;
  http.addHeader("User-Agent", userAgent());
  const int status = http.GET();
  if (status == HTTP_CODE_NOT_FOUND) {
    // A real answer: no attribution-free photograph exists for this type.
    // Recorded so we stop asking, which matters more here than for logos -
    // most of the 2,700 designators are types nobody photographs.
    http.end();
    File marker = cache.open(missPath, FILE_WRITE);
    if (marker) marker.close();
    Serial.printf("Photo %s: none available\n", designator);
    return LogoFetch::Unavailable;
  }
  if (status == 503) {
    Serial.printf("Photo %s: aggregator has photographs disabled\n", designator);
    http.end();
    return LogoFetch::Retry;
  }
  if (status != HTTP_CODE_OK) {
    Serial.printf("Photo %s HTTP %d\n", designator, status);
    http.end();
    return LogoFetch::Retry;
  }
  const int expected = http.getSize();
  File file = cache.open(path, FILE_WRITE);
  const int written = file ? http.writeToStream(&file) : -1;
  if (file) file.close();
  http.end();
  if (written <= 0 || (expected > 0 && written != expected)) {
    Serial.printf("Photo %s write failed: %d of %d bytes\n", designator, written, expected);
    cache.remove(path);
    return LogoFetch::Retry;
  }
  Serial.printf("Photo %s cached (%d bytes)\n", designator, written);
  return LogoFetch::Cached;
}

bool drawCachedOsmTile(const String &path, int screenX, int screenY) {
  fs::FS &cache = sdMounted ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  File file = cache.open(path, FILE_READ);
  // cacheOsmTile() trusts cache.exists() alone and never re-fetches a file
  // once it's present, so a tile that's corrupt on disk - filesystem
  // inconsistency (exists() said yes, open() disagrees) or a short read
  // (truncated write, possibly from before the write-length check in
  // cacheOsmTile() existed) - would otherwise report cached=1 drawn=0
  // forever, surviving every reboot and reflash since neither touches the
  // SD card. Evict it here too, not just on an actual PNG decode failure
  // below, so the next attempt re-downloads a fresh copy instead of
  // repeating the same failure indefinitely. A PSRAM allocation failure is
  // deliberately excluded - that's transient memory pressure, not a bad
  // file, and evicting a perfectly good tile over it would just waste
  // bandwidth re-downloading something that was never the problem.
  if (!file) {
    cache.remove(path);
    Serial.printf("Removed unopenable cached tile %s\n", path.c_str());
    return false;
  }
  const size_t size = file.size();
  // An empty cache entry opens fine and then fails every later step silently
  // (malloc(0) returns null), so it would sit there being reported as
  // "cached" and never drawn for ever. Treat it as the corrupt entry it is.
  if (size == 0) {
    file.close();
    cache.remove(path);
    Serial.printf("Removed empty cached tile %s\n", path.c_str());
    return false;
  }
  uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!data) {
    // Not the tile's fault, so it stays cached - but say so, because this
    // path used to fail silently and was indistinguishable in the log from a
    // corrupt tile.
    file.close();
    Serial.printf("Tile %s: no PSRAM for %u bytes (%u free)\n", path.c_str(),
                  static_cast<unsigned>(size),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return false;
  }
  const size_t read = file.read(data, size);
  file.close();
  if (read != size) {
    heap_caps_free(data);
    cache.remove(path);
    Serial.printf("Removed short-read cached tile %s (%u of %u bytes)\n", path.c_str(),
                  static_cast<unsigned>(read), static_cast<unsigned>(size));
    return false;
  }
  pngTileScreenX = screenX;
  pngTileScreenY = screenY;
  // The decoder lives in PSRAM and is allocated at boot; nothing else in
  // this path checks, so guard here rather than dereference a null.
  if (!pngDecoderPtr) return false;
  const int opened = pngDecoder.openRAM(data, size, drawPngLine);
  const bool success = opened == PNG_SUCCESS && pngDecoder.decode(nullptr, 0) == PNG_SUCCESS;
  if (opened == PNG_SUCCESS) pngDecoder.close();
  heap_caps_free(data);
  // A truncated or corrupt tile used to persist for ever and never redraw.
  if (!success) {
    cache.remove(path);
    Serial.printf("Removed undecodable cached tile %s\n", path.c_str());
  }
  return success;
}

void drawLocationFallback() {
  filledRect(0, 0, W, H, rgb(5, 20, 31));
  const int ringStep = layout::radarRadius / 3;
  for (int radius = ringStep; radius <= layout::radarRadius; radius += ringStep) {
    for (int degrees = 0; degrees < 360; ++degrees) {
      const float angle = radians(degrees);
      pixel(layout::centreX + lroundf(cosf(angle) * radius),
            layout::centreY + lroundf(sinf(angle) * radius), rgb(25, 75, 96));
    }
  }
  line(0, layout::centreY, W - 1, layout::centreY, rgb(25, 75, 96));
  line(layout::centreX, 0, layout::centreX, H - 1, rgb(25, 75, 96));
}

bool refreshPhysicalBaseMap() {
  if (!framebuffer || !baseMap || mapRebuildActive) return false;
  mapRebuildActive = true;
  mapRebuildDone = 0;
  mapRebuildTotal = 0;
  // Same contention that broke the aircraft feed: a full rebuild (especially
  // right after a cache clear, which has to re-fetch every tile instead of
  // just the missing ones) is a burst of sequential HTTPS requests that
  // competes with the AIS WebSocket's persistent TLS session for the same
  // scarce internal RAM - a user report of heapMinimum dropping to ~200
  // bytes and the LCD going solid black during exactly this rebuild
  // confirmed it. Pause it for the duration, same as the periodic fetch does.
  const bool pauseAisForMapRebuild = marineProvider == "aisstream" && aisWebSocket.isConnected();
  if (pauseAisForMapRebuild) aisWebSocket.disconnect();
  drawLocationFallback();
  const double centerX = osmWorldX(homeLongitude, physicalMapZoom);
  const double centerY = osmWorldY(homeLatitude, physicalMapZoom);
  const int left = lround(centerX - W / 2.0);
  const int top = lround(centerY - H / 2.0);
  const int firstTileX = static_cast<int>(floor(left / 256.0));
  const int firstTileY = static_cast<int>(floor(top / 256.0));
  const int lastTileX = static_cast<int>(floor((left + W - 1) / 256.0));
  const int lastTileY = static_cast<int>(floor((top + H - 1) / 256.0));
  const int tilesPerAxis = 1 << physicalMapZoom;
  int tilesDrawn = 0;
  mapRebuildTotal = max(0, (lastTileY - firstTileY + 1) * (lastTileX - firstTileX + 1));
  for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
    if (tileY < 0 || tileY >= tilesPerAxis) { mapRebuildDone += lastTileX - firstTileX + 1; continue; }
    for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
      const int wrappedX = (tileX % tilesPerAxis + tilesPerAxis) % tilesPerAxis;
      const String path = osmTilePath(physicalMapZoom, wrappedX, tileY);
      // A single failed fetch/decode used to leave that tile's square showing
      // the dark ring fallback pattern for good; a couple of quick retries
      // clears most transient network blips without a full manual rescan.
      bool cached = false, drawn = false;
      for (int attempt = 0; attempt < 3 && !drawn; ++attempt) {
        cached = cacheOsmTile(physicalMapZoom, wrappedX, tileY, path);
        drawn = cached && drawCachedOsmTile(path, tileX * 256 - left, tileY * 256 - top);
      }
      if (drawn) ++tilesDrawn;
      Serial.printf("tile %d/%d at %d,%d cached=%d drawn=%d\n", wrappedX, tileY,
                    tileX * 256 - left, tileY * 256 - top, cached, drawn);
      ++mapRebuildDone;
      // Each tile is a separate HTTPS round trip. Service the admin interface
      // between them so the UI stays responsive and can show progress.
      if (webServerReady) webServer.handleClient();
      // This loop runs long enough on this core to starve the RGB panel's
      // DMA of PSRAM bandwidth (the tile cache and framebuffer both live in
      // PSRAM, and the panel continuously DMA-reads the framebuffer to
      // refresh the screen) - that's the pre-existing "table/map top rolls
      // to bottom" bug. It isn't a data race dataMutex can fix; the DMA
      // controller is just losing its bus turn to the CPU's own PSRAM
      // traffic. present() already retries this once per frame; nudging it
      // here too gives it a chance to resynchronise mid-loop instead of only
      // once the whole operation is done. See the other call sites of
      // restartAtNextVsync() in the fetch route-lookup loops for the same fix.
      rgbpanel->restartAtNextVsync();
    }
  }
  filledRect(0, H - 15, 17 * 6 + 4, 15, rgb(0, 0, 0));
  text5(3, H - 12, "(C) OPENSTREETMAP", rgb(255, 255, 255));
  // The tile loop above is the single heaviest PSRAM/SD-bus contention
  // window of the whole rebuild - whatever page calls restoreMap() next
  // (right after this function returns, in setup()) is the first thing
  // presented after that window closes, exactly the moment a DMA
  // desync from that contention is most likely to still be in effect.
  // One more nudge here, on top of the per-tile ones above, before that
  // handoff.
  rgbpanel->restartAtNextVsync();
  memcpy(baseMap, framebuffer, W * H * sizeof(uint16_t));
  physicalMapReady = true;
  mapRebuildActive = false;
  mapRebuildMissingTiles = mapRebuildTotal - tilesDrawn;
  Serial.printf("Physical map %d/%d tiles at %.5f, %.5f radius %u nm zoom %u\n",
                tilesDrawn, mapRebuildTotal, homeLatitude,
                homeLongitude, queryRadiusNm, physicalMapZoom);
  // A retry re-fetches every tile in view over HTTPS, not just the missing
  // ones - real network load on top of whatever else (the web server, the
  // AIS socket) is competing for the same scarce internal RAM. Skip it while
  // memory is already tight rather than making a low-memory situation worse;
  // a manual rescan from the admin page still works once things recover.
  const size_t freeInternalHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (mapRebuildMissingTiles > 0 && mapRebuildRetryCount < 3 && freeInternalHeap > 20000) {
    ++mapRebuildRetryCount;
    nextMapRetryAt = millis() + 15000UL;
    Serial.printf("Map rebuild missing %d tiles; retry %u/3 in 15s\n",
                  mapRebuildMissingTiles, mapRebuildRetryCount);
  } else {
    if (mapRebuildMissingTiles > 0)
      Serial.printf("Map rebuild missing %d tiles but free heap is %u; not auto-retrying\n",
                    mapRebuildMissingTiles, static_cast<unsigned>(freeInternalHeap));
    mapRebuildRetryCount = 0;
  }
  if (pauseAisForMapRebuild) connectAisWebSocket();
  return tilesDrawn > 0;
}

// REG_PWM takes 0-255. Writing the raw percentage capped the panel at ~39%
// and meant "100%" from the web UI was dimmer than the boot default.
bool applyBrightness(uint8_t percent) {
  const uint8_t duty = static_cast<uint8_t>(
      (constrain(static_cast<int>(percent), 0, 100) * 255 + 50) / 100);
#if BOARD_HAS_BACKLIGHT_PWM
  return WS_CH32_IO::setPwm(Wire, duty);
#else
  // EXIO2 is a switch, not a PWM output. Report false for any intermediate
  // level so the caller can tell the user the duty was not honoured.
  return WS_CH422G::setPwm(Wire, duty);
#endif
}

void beepAlert() {
#if BOARD_EXPANDER_CH32
  if (!soundAlerts) return;
  WS_CH32_IO::writeRegister(Wire, WS_CH32_IO::REG_OUTPUT,
                            WS_CH32_IO::OUT_DISPLAY_ON | WS_CH32_IO::PIN_BEE_EN);
  delay(200);
  WS_CH32_IO::writeRegister(Wire, WS_CH32_IO::REG_OUTPUT,
                            WS_CH32_IO::OUT_DISPLAY_ON);
#endif  // no buzzer is wired on the CH422G boards
}

const char *compassDirection(float track) {
  static const char *directions[] = {"N","NE","E","SE","S","SW","W","NW"};
  int index = static_cast<int>((track + 22.5f) / 45.0f) & 7;
  return directions[index];
}

String urlEncode(const char *value) {
  String encoded;
  const char hex[] = "0123456789ABCDEF";
  while (*value) {
    uint8_t c = static_cast<uint8_t>(*value++);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

bool requestAccessToken() {
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(8000); http.setConnectTimeout(8000);
  if (!http.begin(client, TOKEN_URL)) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=client_credentials&client_id=" + urlEncode(openSkyClientId.c_str()) +
                "&client_secret=" + urlEncode(openSkyClientSecret.c_str());
  int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    Serial.printf("OpenSky token HTTP %d\n", code);
    http.end();
    return false;
  }
  JsonDocument tokenDoc;
  PsramSink tokenBody;
  http.writeToStream(&tokenBody);
  DeserializationError error =
      deserializeJson(tokenDoc, tokenBody.data(), tokenBody.size());
  http.end();
  if (error || tokenDoc["access_token"].isNull()) {
    Serial.printf("OpenSky token JSON %s\n", error.c_str());
    return false;
  }
  bearerToken = tokenDoc["access_token"].as<String>();
  uint32_t expiresIn = tokenDoc["expires_in"] | 1800;
  uint32_t safeLifetime = expiresIn > 60 ? expiresIn - 60 : expiresIn;
  tokenExpiresAt = millis() + safeLifetime * 1000UL;
  Serial.printf("OpenSky token renewed; expires in %lu seconds\n", static_cast<unsigned long>(expiresIn));
  return true;
}

bool ensureAccessToken() {
  if (bearerToken.length() && static_cast<int32_t>(tokenExpiresAt - millis()) > 0) return true;
  bearerToken = "";
  return requestAccessToken();
}

void normalizeCallsign(const char *input, char output[9]) {
  int n = 0;
  if (input) {
    while (*input && n < 8) {
      if (*input != ' ') output[n++] = toupper(static_cast<unsigned char>(*input));
      ++input;
    }
  }
  output[n] = 0;
}

void airportCode(JsonObject airport, char output[5]) {
  const char *iata = airport["iata_code"] | "";
  const char *icao = airport["icao_code"] | "";
  const char *chosen = strlen(iata) == 3 ? iata : icao;
  strncpy(output, chosen, 4);
  output[4] = 0;
}

void airportName(JsonObject airport, char *output, size_t outSize) {
  const char *name = airport["name"] | "";
  strncpy(output, name, outSize - 1);
  output[outSize - 1] = 0;
}

void airportCity(JsonObject airport, char *output, size_t outSize) {
  const char *city = airport["municipality"] | "";
  strncpy(output, city, outSize - 1);
  output[outSize - 1] = 0;
}

// Departure-board-style abbreviation, e.g. "LONDON" + "London Stansted
// Airport" -> "LON STAN": the first 3 letters of the city plus the first
// word of the airport name that isn't the city itself or a generic suffix
// like "Airport"/"International". Best-effort - adsbdb has no canonical
// short form, and this won't suit every naming convention worldwide.
void abbreviateAirport(const char *cityName, const char *fullAirportName, char *output, size_t outSize) {
  char cityPart[4] = {};
  int ci = 0;
  for (const char *p = cityName; *p && ci < 3; ++p)
    if (isalpha(static_cast<unsigned char>(*p))) cityPart[ci++] = toupper(static_cast<unsigned char>(*p));
  cityPart[ci] = 0;

  char cityUpper[24] = {};
  size_t cu = 0;
  for (const char *p = cityName; *p && cu < sizeof(cityUpper) - 1; ++p) cityUpper[cu++] = toupper(static_cast<unsigned char>(*p));
  cityUpper[cu] = 0;

  static const char *skipWords[] = {"AIRPORT", "INTERNATIONAL", "INTL", "REGIONAL",
                                     "FIELD", "AIRFIELD", "MUNICIPAL", "COUNTY", "AERODROME"};

  char distinguishing[5] = {};
  char word[32] = {};
  size_t wi = 0;
  for (const char *p = fullAirportName;; ++p) {
    char c = *p;
    bool boundary = (c == ' ' || c == '-' || c == 0);
    if (!boundary && wi < sizeof(word) - 1) word[wi++] = toupper(static_cast<unsigned char>(c));
    if (boundary) {
      word[wi] = 0;
      if (wi > 0 && !distinguishing[0] && strcmp(word, cityUpper)) {
        bool skip = false;
        for (const char *s : skipWords) if (!strcmp(word, s)) { skip = true; break; }
        if (!skip) { strncpy(distinguishing, word, 4); distinguishing[4] = 0; }
      }
      wi = 0;
      if (c == 0) break;
    }
  }
  if (distinguishing[0]) snprintf(output, outSize, "%s %s", cityPart, distinguishing);
  else snprintf(output, outSize, "%s", cityPart);
}

// Stores a route the aggregator resolved for us, into the same cache
// routeForCallsign() fills. The server does the adsbdb lookup on behalf of
// every device that can see the flight, which is why this exists: on this
// hardware each lookup cost ~2.2 s of blocked network task and wanted more
// contiguous internal RAM for the TLS handshake than was free, so they were
// throttled to two per refresh and failed intermittently. A route that
// arrives with the aircraft costs nothing.
//
// Marked resolvedAt so the entry ages out normally; if the server later
// stops sending routes (an older deployment), the device falls back to
// looking it up itself once this expires.
// Set the first time the aggregator supplies a route with an aircraft. From
// then on this device stops asking adsbdb itself, because the hardware shows
// what those lookups cost: each is a full TLS handshake that drops the
// largest contiguous internal block from 31732 to 14324 bytes, which is
// where mbedTLS reports "BIGNUM - Memory allocation failed" and the
// certificate bundle reports 0x4290 - an allocation failure inside the
// signature check, not a bad certificate. They also block the network task
// for 2-2.8 seconds a time, and that burst is when the panel slips.
//
// Latched rather than assumed from the provider name: a deployment that has
// not been updated yet sends no route field, and the device must keep
// resolving routes itself until it sees evidence the server will.
bool serverSuppliesRoutes = false;

bool adoptServerRoute(const char *rawCallsign, JsonObjectConst route) {
  if (!routeCache.data) return false;
  if (route.isNull()) return false;
  const char *origin = route["origin"] | "";
  const char *destination = route["destination"] | "";
  if (!origin[0] || !destination[0]) return false;

  char callsign[9];
  normalizeCallsign(rawCallsign, callsign);
  if (strlen(callsign) < 3) return false;

  RouteCacheEntry *slot = nullptr;
  for (auto &entry : routeCache)
    if (entry.occupied && !strcmp(entry.callsign, callsign)) { slot = &entry; break; }
  if (!slot)
    for (auto &entry : routeCache) if (!entry.occupied) { slot = &entry; break; }
  if (!slot) {
    slot = &routeCache[0];
    for (auto &entry : routeCache) if (entry.lastUsed < slot->lastUsed) slot = &entry;
  }
  // Already holding this exact route: just keep it fresh rather than
  // rebuilding the abbreviations on every single fetch.
  if (slot->occupied && !strcmp(slot->callsign, callsign) && slot->hasRoute &&
      !strcmp(slot->origin, origin) && !strcmp(slot->destination, destination)) {
    slot->resolvedAt = slot->lastUsed = millis();
    return true;
  }

  memset(slot, 0, sizeof(*slot));
  strncpy(slot->callsign, callsign, sizeof(slot->callsign) - 1);
  slot->occupied = true;
  slot->resolvedAt = slot->lastUsed = millis();
  strncpy(slot->origin, origin, sizeof(slot->origin) - 1);
  strncpy(slot->destination, destination, sizeof(slot->destination) - 1);
  strncpy(slot->originName, route["origin_name"] | "", sizeof(slot->originName) - 1);
  strncpy(slot->destinationName, route["destination_name"] | "", sizeof(slot->destinationName) - 1);
  strncpy(slot->airline, route["airline"] | "", sizeof(slot->airline) - 1);
  strncpy(slot->originCity, route["origin_city"] | "", sizeof(slot->originCity) - 1);
  strncpy(slot->destinationCity, route["destination_city"] | "", sizeof(slot->destinationCity) - 1);
  if (slot->originCity[0] && slot->originName[0])
    abbreviateAirport(slot->originCity, slot->originName, slot->originAbbrev, sizeof(slot->originAbbrev));
  if (slot->destinationCity[0] && slot->destinationName[0])
    abbreviateAirport(slot->destinationCity, slot->destinationName, slot->destinationAbbrev,
                      sizeof(slot->destinationAbbrev));
  slot->hasRoute = true;
  serverSuppliesRoutes = true;
  return true;
}

RouteCacheEntry *routeForCallsign(const char *rawCallsign, int &lookupsUsed,
                                  WiFiClientSecure &client, HTTPClient &http) {
  if (!routeCache.data) return nullptr;
  char callsign[9];
  normalizeCallsign(rawCallsign, callsign);
  if (strlen(callsign) < 3) return nullptr;

  RouteCacheEntry *slot = nullptr;
  for (auto &entry : routeCache) {
    if (entry.occupied && !strcmp(entry.callsign, callsign)) {
      entry.lastUsed = millis();
      const uint32_t lifetime = entry.hasRoute ? ROUTE_CACHE_MS : ROUTE_RETRY_MS;
      if (millis() - entry.resolvedAt < lifetime) return &entry;
      slot = &entry;
      // Keep the stale entry usable if the refresh budget is already spent,
      // rather than dropping a route we already know.
      if (lookupsUsed >= MAX_ROUTE_LOOKUPS_PER_REFRESH) return &entry;
      break;
    }
  }
  if (!slot) {
    for (auto &entry : routeCache) if (!entry.occupied) { slot = &entry; break; }
  }
  if (!slot) {
    slot = &routeCache[0];
    for (auto &entry : routeCache) if (entry.lastUsed < slot->lastUsed) slot = &entry;
  }
  if (lookupsUsed >= MAX_ROUTE_LOOKUPS_PER_REFRESH) return nullptr;
  ++lookupsUsed;

  memset(slot, 0, sizeof(*slot));
  strncpy(slot->callsign, callsign, sizeof(slot->callsign) - 1);
  slot->occupied = true;
  slot->resolvedAt = slot->lastUsed = millis();

  // client and http are owned by the caller, one fresh pair per lookup - see
  // the caller's comment on why this no longer reuses one keep-alive
  // connection across the whole batch.
  String url = "https://api.adsbdb.com/v0/callsign/" + String(callsign);
  if (!http.begin(client, url)) return slot;
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("User-Agent", userAgent());
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Route %s HTTP %d\n", callsign, code);
    http.end();
    return slot;
  }
  // Every other JSON parse in this file keeps its allocations in PSRAM via
  // psramJsonAllocator/PsramSink - this one didn't, and getString()+a
  // default JsonDocument put both the response body and the whole parsed
  // tree in internal RAM instead. Called on every route lookup (up to
  // MAX_ROUTE_LOOKUPS_PER_REFRESH times per fetch), that's the actual
  // culprit behind the internal-heap fragmentation that permanently breaks
  // the aircraft feed's own TLS connections after the first fetch - not a
  // hardware ceiling, just this one call site allocating in the wrong pool.
  PsramSink body;
  http.writeToStream(&body);
  http.end();
  JsonDocument routeDoc(&psramJsonAllocator);
  if (deserializeJson(routeDoc, body.data(), body.size())) return slot;
  JsonObject route = routeDoc["response"]["flightroute"].as<JsonObject>();
  if (route.isNull()) return slot;
  airportCode(route["origin"].as<JsonObject>(), slot->origin);
  airportCode(route["destination"].as<JsonObject>(), slot->destination);
  airportName(route["origin"].as<JsonObject>(), slot->originName, sizeof(slot->originName));
  airportName(route["destination"].as<JsonObject>(), slot->destinationName, sizeof(slot->destinationName));
  airportCity(route["origin"].as<JsonObject>(), slot->originCity, sizeof(slot->originCity));
  airportCity(route["destination"].as<JsonObject>(), slot->destinationCity, sizeof(slot->destinationCity));
  if (slot->originCity[0] && slot->originName[0])
    abbreviateAirport(slot->originCity, slot->originName, slot->originAbbrev, sizeof(slot->originAbbrev));
  if (slot->destinationCity[0] && slot->destinationName[0])
    abbreviateAirport(slot->destinationCity, slot->destinationName, slot->destinationAbbrev, sizeof(slot->destinationAbbrev));
  slot->hasRoute = slot->origin[0] && slot->destination[0];
  if (slot->hasRoute) Serial.printf("Route %s %s>%s\n", callsign, slot->origin, slot->destination);
  return slot;
}

RouteCacheEntry *cachedRoute(const char *rawCallsign) {
  char callsign[9];
  normalizeCallsign(rawCallsign, callsign);
  for (auto &entry : routeCache) {
    if (entry.occupied && !strcmp(entry.callsign, callsign)) return &entry;
  }
  return nullptr;
}

// Persists the route cache across reboots - the callsigns seen near a fixed
// receiver location repeat daily, so this avoids re-querying adsbdb for
// routes it already resolved last time the device was on. Raw struct dump:
// this file is only ever read back by the exact build that wrote it, so a
// version mismatch (from a firmware update changing RouteCacheEntry) just
// means starting cache-cold again rather than reading garbage.
constexpr uint32_t ROUTE_CACHE_FILE_MAGIC = 0x52435341; // "ASCR"
constexpr uint8_t ROUTE_CACHE_FILE_VERSION = 1;

const char *routeCacheFilePath() {
  return sdMounted ? "/adsb/route_cache.bin" : "/route_cache.bin";
}

void saveRouteCacheToStorage() {
  fs::FS &storage = sdMounted ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  File file = storage.open(routeCacheFilePath(), FILE_WRITE);
  if (!file) return;
  file.write(reinterpret_cast<const uint8_t *>(&ROUTE_CACHE_FILE_MAGIC), sizeof(ROUTE_CACHE_FILE_MAGIC));
  file.write(&ROUTE_CACHE_FILE_VERSION, sizeof(ROUTE_CACHE_FILE_VERSION));
  uint8_t count = 0;
  for (auto &entry : routeCache) if (entry.occupied) ++count;
  file.write(&count, sizeof(count));
  for (auto &entry : routeCache) {
    if (!entry.occupied) continue;
    file.write(reinterpret_cast<const uint8_t *>(&entry), sizeof(entry));
  }
  file.close();
}

void loadRouteCacheFromStorage() {
  fs::FS &storage = sdMounted ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  File file = storage.open(routeCacheFilePath(), FILE_READ);
  if (!file) return;
  uint32_t magic = 0;
  uint8_t version = 0, count = 0;
  bool headerOk = file.read(reinterpret_cast<uint8_t *>(&magic), sizeof(magic)) == sizeof(magic) &&
                  magic == ROUTE_CACHE_FILE_MAGIC &&
                  file.read(&version, sizeof(version)) == sizeof(version) &&
                  version == ROUTE_CACHE_FILE_VERSION &&
                  file.read(&count, sizeof(count)) == sizeof(count);
  int loaded = 0;
  if (headerOk) {
    RouteCacheEntry entry;
    while (loaded < count && loaded < ROUTE_CACHE_SIZE &&
           file.read(reinterpret_cast<uint8_t *>(&entry), sizeof(entry)) == sizeof(entry)) {
      // millis() resets to near-zero at boot, so a persisted timestamp from
      // the previous session would otherwise read as impossibly stale;
      // treat every loaded entry as freshly resolved right now instead.
      entry.resolvedAt = millis();
      entry.lastUsed = millis();
      routeCache[loaded++] = entry;
    }
  }
  file.close();
  if (loaded) Serial.printf("Loaded %d cached routes from %s\n", loaded, routeCacheFilePath());
}

// Airline name from the callsign's ICAO prefix. The feeds' own "ownOp"
// field is empty for most aircraft, so a screensaver that only showed what
// the feed sent displayed a bare callsign like EAG78H and told the viewer
// nothing. The prefix is the one piece of an airline callsign that is
// globally assigned and stable, so it can be resolved on-device.
//
// Deliberately partial: it covers the operators actually seen over the UK
// and Europe plus the major long-haul carriers, and anything unlisted falls
// back to showing the callsign, which is what it did before. Nothing here
// costs RAM - it is const and lives in flash.
struct OperatorName {
  char code[4];
  const char *name;
};

constexpr OperatorName OPERATOR_NAMES[] = {
    // UK and Ireland
    {"BAW", "BRITISH AIRWAYS"},   {"SHT", "BRITISH AIRWAYS SHUTTLE"},
    {"EZY", "EASYJET"},           {"EJU", "EASYJET EUROPE"},
    {"RYR", "RYANAIR"},           {"RUK", "RYANAIR UK"},
    {"EXS", "JET2"},              {"TOM", "TUI AIRWAYS"},
    {"VIR", "VIRGIN ATLANTIC"},   {"LOG", "LOGANAIR"},
    {"EAG", "EMERALD AIRLINES"},  {"EIN", "AER LINGUS"},
    {"BEE", "BLUE ISLANDS"},      {"NPT", "WEST ATLANTIC UK"},
    {"DHK", "DHL AIR UK"},        {"BCS", "DHL EUROPEAN AIR TRANSPORT"},
    // Continental Europe
    {"DLH", "LUFTHANSA"},         {"GEC", "LUFTHANSA CARGO"},
    {"EWG", "EUROWINGS"},         {"CFG", "CONDOR"},
    {"AFR", "AIR FRANCE"},        {"KLM", "KLM"},
    {"SWR", "SWISS"},             {"AUA", "AUSTRIAN AIRLINES"},
    {"BEL", "BRUSSELS AIRLINES"}, {"IBE", "IBERIA"},
    {"VLG", "VUELING"},           {"AEA", "AIR EUROPA"},
    {"TAP", "TAP AIR PORTUGAL"},  {"SAS", "SAS"},
    {"FIN", "FINNAIR"},           {"NAX", "NORWEGIAN"},
    {"WZZ", "WIZZ AIR"},          {"WUK", "WIZZ AIR UK"},
    {"LOT", "LOT POLISH AIRLINES"}, {"CTN", "CROATIA AIRLINES"},
    {"AEE", "AEGEAN AIRLINES"},   {"ICE", "ICELANDAIR"},
    {"BTI", "AIR BALTIC"},        {"CLX", "CARGOLUX"},
    {"SXS", "SUNEXPRESS"},        {"PGT", "PEGASUS"},
    {"THY", "TURKISH AIRLINES"},
    // Middle East, Africa and Asia
    {"UAE", "EMIRATES"},          {"QTR", "QATAR AIRWAYS"},
    {"ETD", "ETIHAD"},            {"SVA", "SAUDIA"},
    {"FDB", "FLYDUBAI"},          {"ABY", "AIR ARABIA"},
    {"GFA", "GULF AIR"},          {"OMA", "OMAN AIR"},
    {"KAC", "KUWAIT AIRWAYS"},    {"MEA", "MIDDLE EAST AIRLINES"},
    {"RJA", "ROYAL JORDANIAN"},   {"ELY", "EL AL"},
    {"MSR", "EGYPTAIR"},          {"RAM", "ROYAL AIR MAROC"},
    {"ETH", "ETHIOPIAN AIRLINES"},{"KQA", "KENYA AIRWAYS"},
    {"AIC", "AIR INDIA"},         {"PIA", "PAKISTAN INTERNATIONAL"},
    {"SIA", "SINGAPORE AIRLINES"},{"CPA", "CATHAY PACIFIC"},
    {"THA", "THAI AIRWAYS"},      {"MAS", "MALAYSIA AIRLINES"},
    {"JAL", "JAPAN AIRLINES"},    {"ANA", "ALL NIPPON AIRWAYS"},
    {"KAL", "KOREAN AIR"},        {"AAR", "ASIANA AIRLINES"},
    {"CCA", "AIR CHINA"},         {"CES", "CHINA EASTERN"},
    {"CSN", "CHINA SOUTHERN"},
    // Americas and Oceania
    {"AAL", "AMERICAN AIRLINES"}, {"UAL", "UNITED AIRLINES"},
    {"DAL", "DELTA AIR LINES"},   {"SWA", "SOUTHWEST AIRLINES"},
    {"JBU", "JETBLUE"},           {"ACA", "AIR CANADA"},
    {"WJA", "WESTJET"},           {"AMX", "AEROMEXICO"},
    {"LAN", "LATAM"},             {"AVA", "AVIANCA"},
    {"QFA", "QANTAS"},            {"ANZ", "AIR NEW ZEALAND"},
    {"FDX", "FEDEX"},             {"UPS", "UPS AIRLINES"},
    {"GTI", "ATLAS AIR"},
    // Business aviation, common overhead and rarely in ownOp
    {"NJE", "NETJETS EUROPE"},    {"EJA", "NETJETS"},
    {"VJT", "VISTAJET"},          {"LXJ", "FLEXJET"},
};

// Only treat a callsign as an airline callsign when it looks like one:
// three letters then a digit. Registrations reach here too - "GBEOY" would
// otherwise match a "GBE" prefix that means nothing.
const char *operatorNameForCallsign(const char *callsign) {
  if (!callsign || strlen(callsign) < 4) return nullptr;
  for (int i = 0; i < 3; ++i)
    if (!isalpha(static_cast<unsigned char>(callsign[i]))) return nullptr;
  if (!isdigit(static_cast<unsigned char>(callsign[3]))) return nullptr;
  char prefix[4] = {};
  for (int i = 0; i < 3; ++i) prefix[i] = toupper(static_cast<unsigned char>(callsign[i]));
  for (const OperatorName &entry : OPERATOR_NAMES)
    if (!strcmp(entry.code, prefix)) return entry.name;
  return nullptr;
}

// What a squawk actually means, where it means anything. The three
// emergency codes are worth interrupting someone for; the conspicuity codes
// explain why half the small aircraft overhead share one number. Everything
// else is a discrete code - a temporary tag a controller issued from their
// local block, carrying no meaning beyond "this sector, today" - so it gets
// no label rather than an invented one.
const char *squawkMeaning(const char *squawk) {
  if (!squawk || !squawk[0]) return nullptr;
  if (!strcmp(squawk, "7700")) return "GENERAL EMERGENCY";
  if (!strcmp(squawk, "7600")) return "RADIO FAILURE";
  if (!strcmp(squawk, "7500")) return "HIJACK";
  if (!strcmp(squawk, "7000")) return "VFR CONSPICUITY";
  if (!strcmp(squawk, "1200")) return "VFR (NORTH AMERICA)";
  if (!strcmp(squawk, "2000")) return "IFR, NO CODE ASSIGNED";
  if (!strcmp(squawk, "7777")) return "MILITARY INTERCEPT";
  if (!strcmp(squawk, "0000")) return "MILITARY / UNASSIGNED";
  return nullptr;
}

uint16_t operatorColour(const char *code) {
  if (!strncmp(code,"BAW",3)) return rgb(20,45,125);
  if (!strncmp(code,"EZY",3)) return rgb(255,85,0);
  if (!strncmp(code,"RYR",3)) return rgb(15,40,125);
  if (!strncmp(code,"EXS",3)) return rgb(210,25,45);
  if (!strncmp(code,"TOM",3)) return rgb(50,160,205);
  if (!strncmp(code,"VIR",3)) return rgb(205,20,45);
  if (!strncmp(code,"LOG",3)) return rgb(35,95,145);
  if (!strncmp(code,"DHK",3) || !strncmp(code,"BCS",3)) return rgb(245,205,20);
  return rgb(0,185,210);
}

void drawMlatPlane(int x, int y, float heading) {
  float a = radians(heading - 90.0f), cs=cosf(a), sn=sinf(a);
  auto tx=[&](float px,float py){return x+lroundf(px*cs-py*sn);};
  auto ty=[&](float px,float py){return y+lroundf(px*sn+py*cs);};
  uint16_t red=rgb(245,30,35), white=rgb(255,255,255);
  disc(x,y,2,red);
  line(tx(12,0),ty(12,0),tx(-9,-5),ty(-9,-5),red);
  line(tx(12,0),ty(12,0),tx(-9,5),ty(-9,5),red);
  line(tx(-9,-5),ty(-9,-5),tx(-5,0),ty(-5,0),red);
  line(tx(-5,0),ty(-5,0),tx(-9,5),ty(-9,5),red);
  pixel(x,y,white);
}

// The square operator tile on the screensaver's departure board, standing in
// for the airline logo in the reference design. Real logos are not shippable
// here - there are thousands of operators, each would need a licensed bitmap,
// and the flash budget is already carrying the map tiles - so the tile is the
// airline's colour with its ICAO prefix reversed out of it, which reads at a
// glance from across a room in the same way the logo does.
void drawOperatorTile(int x, int y, int size, const char *flight, const char *hex) {
  // The real logo when we have one, initials when we do not. Asking for the
  // logo here rather than anywhere else is on purpose: this is the only
  // place that knows an airline is actually being shown to someone, so the
  // device only ever fetches logos it is about to display.
  char airline[4];
  if (size >= LOGO_PIXELS && operatorLogoCode(flight, airline)) {
    if (drawCachedOperatorLogo(x, y, size, airline)) return;
    if (!logoFetchPending && !logoKnownUnavailable(airline)) {
      memcpy(logoFetchCode, airline, 4);
      logoFetchPending = true;
    }
  }

  char code[4] = {'?', '?', '?', 0};
  const char *src = (flight && strlen(flight) >= 3) ? flight : hex;
  for (int i = 0; i < 3 && src && src[i]; ++i) code[i] = toupper(static_cast<unsigned char>(src[i]));
  const uint16_t tint = operatorColour(code);
  filledRect(x, y, size, size, tint);
  // A darker inner border keeps the tile from bleeding into the black
  // background on the colours that are already near-black (BAW, RYR).
  filledRect(x + 3, y + 3, size - 6, 2, rgb(255, 255, 255));
  filledRect(x + 3, y + size - 5, size - 6, 2, rgb(255, 255, 255));
  // Centre the three glyphs: text5 advances 6*scale per character, and the
  // glyph box is 5*scale wide by 7*scale tall.
  const int scale = size / 26 > 1 ? size / 26 : 1;
  const int textW = 3 * 6 * scale - scale;
  text5(x + (size - textW) / 2, y + (size - 7 * scale) / 2, code, rgb(255, 255, 255), scale);
}

void drawOperatorBadge(int x, int y, const char *flight, const char *hex) {
  char code[4] = {'?','?','?',0};
  const char *src = (flight && strlen(flight)>=3) ? flight : hex;
  for (int i=0; i<3 && src && src[i]; ++i) code[i]=toupper(static_cast<unsigned char>(src[i]));
  disc(x,y,11,rgb(5,15,20));
  disc(x,y,10,operatorColour(code));
  text5(x-8,y-3,code,rgb(255,255,255));
}

// --- icon-geometry-begin ---
// Aircraft silhouettes.
//
// Each airframe is one closed outline traced from the nose down the
// starboard side to the tail, in a local frame where +x is the nose and +y
// is starboard, and mirrored about the centreline when it is drawn. Tracing
// a single outline (rather than assembling a few triangles, which is what
// made every icon read as a bare arrowhead) means the fuselage, the swept
// wing, the nacelle line and the tailplane are all part of one silhouette,
// so the shape still looks like an aeroplane at 24 px.
//
// Half-outlines only, so the two sides can never drift apart, and the first
// and last points must sit on the centreline (y == 0) or the mirror will
// leave a notch at the nose or tail.
struct IconPoint {
  float x, y;
};

// Airliner: narrow-body twin - pointed nose, clearly swept wing, swept
// tailplane. The baseline every other jet shape is judged against.
constexpr IconPoint OUTLINE_AIRLINER[] = {
    {13.0f, 0.0f},  {10.5f, 1.8f}, {5.0f, 2.2f},   {-3.0f, 10.5f},
    {-5.5f, 11.0f}, {-2.0f, 2.8f}, {-7.0f, 2.4f},  {-9.5f, 6.0f},
    {-11.5f, 6.2f}, {-11.5f, 2.0f}, {-12.5f, 0.0f},
};

// Wide-body: longer, fatter fuselage and a span half again as wide, which
// is the difference a viewer actually notices between a 737 and a 777.
constexpr IconPoint OUTLINE_HEAVY[] = {
    {16.0f, 0.0f},  {13.0f, 2.4f}, {6.0f, 3.0f},   {-4.0f, 13.5f},
    {-7.5f, 14.0f}, {-2.5f, 3.6f}, {-8.5f, 3.2f},  {-11.5f, 8.0f},
    {-14.0f, 8.2f}, {-14.0f, 2.6f}, {-15.5f, 0.0f},
};

// Regional twin / business jet: short body, only slightly swept wing set
// well forward, so it reads as a smaller machine than the airliner.
constexpr IconPoint OUTLINE_TWIN[] = {
    {11.0f, 0.0f}, {9.0f, 1.8f},  {3.5f, 2.0f},   {1.0f, 9.5f},
    {-1.5f, 9.8f}, {-1.0f, 2.6f}, {-6.5f, 2.2f},  {-8.5f, 6.0f},
    {-10.0f, 6.2f}, {-10.0f, 1.8f}, {-11.0f, 0.0f},
};

// Light single: slim fuselage, unswept wings, generous tailplane. Drawn
// with a propeller arc across the nose (see drawPlaneIcon).
constexpr IconPoint OUTLINE_LIGHT[] = {
    {9.0f, 0.0f},  {7.5f, 1.5f},  {2.5f, 1.6f},  {2.0f, 10.0f},
    {-0.5f, 10.0f}, {-1.0f, 1.6f}, {-6.0f, 1.4f}, {-7.5f, 5.0f},
    {-9.0f, 5.0f}, {-9.0f, 1.4f}, {-9.5f, 0.0f},
};

// Fast jet: cranked delta, sharply swept, narrow span, small all-moving
// tail - deliberately the most aggressive outline in the set.
constexpr IconPoint OUTLINE_FIGHTER[] = {
    {13.0f, 0.0f}, {10.0f, 1.4f}, {5.0f, 1.8f},  {-6.0f, 8.0f},
    {-8.0f, 8.0f}, {-6.0f, 2.6f}, {-9.0f, 2.6f}, {-10.5f, 4.5f},
    {-11.5f, 4.5f}, {-11.0f, 1.8f}, {-11.5f, 0.0f},
};

// Sailplane: the span is the whole signature, so it is nearly twice the
// airliner's on a body barely wider than a line.
constexpr IconPoint OUTLINE_GLIDER[] = {
    {9.0f, 0.0f},  {7.0f, 1.3f},  {2.2f, 1.6f},  {1.0f, 15.0f},
    {-1.2f, 15.0f}, {-1.2f, 1.6f}, {-7.0f, 1.3f}, {-8.5f, 4.5f},
    {-9.5f, 4.5f}, {-9.5f, 1.2f}, {-10.0f, 0.0f},
};

// Unidentified airframe: a plain aeroplane, moderate everything. It has to
// claim nothing about the type while still not looking like an arrow.
constexpr IconPoint OUTLINE_GENERIC[] = {
    {11.0f, 0.0f}, {9.0f, 1.6f},  {4.0f, 2.0f},  {-2.0f, 9.0f},
    {-4.5f, 9.4f}, {-1.5f, 2.4f}, {-6.0f, 2.0f}, {-8.5f, 5.5f},
    {-10.0f, 5.7f}, {-10.0f, 1.6f}, {-11.0f, 0.0f},
};

// Helicopter body: cabin, tapering tail boom, tail fin. The rotor is drawn
// separately because it turns independently of the track.
constexpr IconPoint OUTLINE_HELI[] = {
    {7.5f, 0.0f},  {6.5f, 2.6f},  {3.0f, 4.2f},  {-1.0f, 4.2f},
    {-3.5f, 2.2f}, {-11.0f, 1.3f}, {-11.5f, 4.2f}, {-13.5f, 4.2f},
    {-13.5f, 0.0f},
};

// Fills an arbitrary simple polygon by sorted-scanline crossings. Needed
// because a plane silhouette is concave (wing roots and the tail waist),
// which filledTriangle() cannot express without splitting the outline into
// pieces whose shared edges show as seams.
void fillPolygon(const float *px, const float *py, int n, uint16_t c) {
  if (n < 3) return;
  float minY = py[0], maxY = py[0];
  for (int i = 1; i < n; ++i) {
    if (py[i] < minY) minY = py[i];
    if (py[i] > maxY) maxY = py[i];
  }
  const int y0 = static_cast<int>(floorf(minY)), y1 = static_cast<int>(ceilf(maxY));
  for (int y = y0; y <= y1; ++y) {
    const float sy = y + 0.5f;
    float xs[16];
    int count = 0;
    for (int i = 0; i < n && count < 16; ++i) {
      const int j = (i + 1 == n) ? 0 : i + 1;
      const float ya = py[i], yb = py[j];
      // Half-open test: a vertex counts for the edge below it only, so a
      // scanline through a vertex crosses once, not twice or zero times.
      if ((ya <= sy && yb > sy) || (yb <= sy && ya > sy))
        xs[count++] = px[i] + (sy - ya) * (px[j] - px[i]) / (yb - ya);
    }
    for (int i = 1; i < count; ++i) {
      const float key = xs[i];
      int j = i - 1;
      while (j >= 0 && xs[j] > key) { xs[j + 1] = xs[j]; --j; }
      xs[j + 1] = key;
    }
    for (int i = 0; i + 1 < count; i += 2) {
      const int xa = lroundf(xs[i]), xb = lroundf(xs[i + 1]);
      for (int x = xa; x <= xb; ++x) pixel(x, y, c);
    }
  }
  // Stroke the edges as well. At icon size a wing root or a tail boom is
  // only a pixel or two across, and once the shape is rotated off the axes
  // a pure scanline fill drops those spans entirely - the silhouette comes
  // apart into disconnected blobs. Drawing the outline guarantees every
  // feature stays connected whatever the heading.
  for (int i = 0; i < n; ++i) {
    const int j = (i + 1 == n) ? 0 : i + 1;
    line(lroundf(px[i]), lroundf(py[i]), lroundf(px[j]), lroundf(py[j]), c);
  }
}

// Mirrors a half-outline about the centreline, rotates it onto the screen
// and fills it. Scale lets the same geometry serve a full-size map icon and
// a smaller one without a second table.
template <size_t N>
void fillOutline(int x, int y, float cs, float sn, const IconPoint (&half)[N],
                 uint16_t colour, float scale = 1.0f) {
  constexpr int total = static_cast<int>(N) * 2 - 2;  // both ends are shared
  float px[static_cast<int>(N) * 2 - 2], py[static_cast<int>(N) * 2 - 2];
  int n = 0;
  auto emit = [&](float lx, float ly) {
    lx *= scale;
    ly *= scale;
    px[n] = x + (lx * cs - ly * sn);
    py[n] = y + (lx * sn + ly * cs);
    ++n;
  };
  for (size_t i = 0; i < N; ++i) emit(half[i].x, half[i].y);
  for (int i = static_cast<int>(N) - 2; i >= 1; --i) emit(half[i].x, -half[i].y);
  fillPolygon(px, py, n < total ? n : total, colour);
}

// Aircraft icons: a real top-down silhouette per airframe class, filled
// solid in the altitude colour. The outline tables above carry the shape;
// this function only places, rotates and decorates them (engine nacelles,
// a propeller arc, a turning rotor) and handles the three markers that are
// not aeroplanes and therefore are not rotated at all.
void drawPlaneIcon(int x, int y, float heading, PlaneShape shape, uint16_t colour) {
  const float a = radians(heading - 90.0f), cs = cosf(a), sn = sinf(a);
  auto tx = [&](float px, float py) { return x + lroundf(px * cs - py * sn); };
  auto ty = [&](float px, float py) { return y + lroundf(px * sn + py * cs); };
  const uint16_t white = rgb(255, 255, 255);
  // A nacelle is a stubby block slung under the wing; two per side reads as
  // a four-engine widebody, one per side as a twin.
  auto nacelle = [&](float cx, float cy, float halfLen, float halfWid) {
    const float lx[4] = {cx + halfLen, cx + halfLen, cx - halfLen, cx - halfLen};
    const float ly[4] = {cy - halfWid, cy + halfWid, cy + halfWid, cy - halfWid};
    float px[4], py[4];
    for (int i = 0; i < 4; ++i) {
      px[i] = x + (lx[i] * cs - ly[i] * sn);
      py[i] = y + (lx[i] * sn + ly[i] * cs);
    }
    fillPolygon(px, py, 4, colour);
  };
  switch (shape) {
    case PlaneShape::Helicopter: {
      // The rotor turns regardless of track, so it is animated; the tail
      // boom is what actually shows the heading.
      static float rotorAngle = 0.0f;
      rotorAngle += 35.0f;
      if (rotorAngle >= 360.0f) rotorAngle -= 360.0f;
      // Blades first, body over them, so the rotor reads as passing behind
      // the cabin instead of cutting the machine into a starfish. Thin
      // filled bars rather than line() strokes - a single-pixel blade all
      // but vanishes on the panel.
      const float ra = radians(rotorAngle);
      for (int blade = 0; blade < 2; ++blade) {
        const float ba = ra + blade * 1.5707963f;
        const float bc = cosf(ba) * 13.0f, bs = sinf(ba) * 13.0f;
        const float nx = -sinf(ba), ny = cosf(ba);
        const float bx[4] = {x + bc + nx, x + bc - nx, x - bc - nx, x - bc + nx};
        const float by[4] = {y + bs + ny, y + bs - ny, y - bs - ny, y - bs + ny};
        fillPolygon(bx, by, 4, colour);
      }
      fillOutline(x, y, cs, sn, OUTLINE_HELI, colour);
      disc(x, y, 3, colour);
      pixel(x, y, white);
      break;
    }
    case PlaneShape::Fighter:
      fillOutline(x, y, cs, sn, OUTLINE_FIGHTER, colour);
      pixel(x, y, white);
      break;
    case PlaneShape::LightProp:
      fillOutline(x, y, cs, sn, OUTLINE_LIGHT, colour);
      // Propeller arc: two pixels thick so it survives the panel, and set
      // ahead of the spinner rather than through it.
      line(tx(9.0f, -4.0f), ty(9.0f, -4.0f), tx(9.0f, 4.0f), ty(9.0f, 4.0f), colour);
      line(tx(10.0f, -3.0f), ty(10.0f, -3.0f), tx(10.0f, 3.0f), ty(10.0f, 3.0f), colour);
      pixel(x, y, white);
      break;
    case PlaneShape::Twin:
      fillOutline(x, y, cs, sn, OUTLINE_TWIN, colour);
      // Turboprops and regional jets alike carry their engines out on the
      // wing, which is most of what separates this from the light single.
      nacelle(0.5f, 5.5f, 3.0f, 1.3f);
      nacelle(0.5f, -5.5f, 3.0f, 1.3f);
      pixel(x, y, white);
      break;
    case PlaneShape::HeavyJet:
      fillOutline(x, y, cs, sn, OUTLINE_HEAVY, colour);
      nacelle(-1.0f, 6.5f, 3.2f, 1.5f);
      nacelle(-1.0f, -6.5f, 3.2f, 1.5f);
      nacelle(-3.5f, 10.5f, 2.8f, 1.4f);
      nacelle(-3.5f, -10.5f, 2.8f, 1.4f);
      pixel(x, y, white);
      break;
    case PlaneShape::Glider:
      fillOutline(x, y, cs, sn, OUTLINE_GLIDER, colour);
      pixel(x, y, white);
      break;
    case PlaneShape::Balloon: {
      // Lighter-than-air drifts with the wind, so heading is meaningless
      // here - drawn unrotated, envelope over basket.
      disc(x, y - 4, 7, colour);
      filledRect(x - 2, y + 4, 5, 5, colour);
      line(x - 4, y + 2, x - 2, y + 5, colour);
      line(x + 4, y + 2, x + 2, y + 5, colour);
      pixel(x, y - 4, white);
      break;
    }
    case PlaneShape::Drone: {
      // Quadrotor: an X of arms with a rotor disc on each end, unrotated
      // for the same reason the helicopter's rotor is - at this size the
      // airframe has no meaningful nose.
      for (int i = 0; i < 4; ++i) {
        const int dx = (i & 1) ? 7 : -7, dy = (i & 2) ? 7 : -7;
        line(x, y, x + dx, y + dy, colour);
        line(x, y + 1, x + dx, y + dy + 1, colour);
        disc(x + dx, y + dy, 3, colour);
        disc(x + dx, y + dy, 1, white);
      }
      filledRect(x - 3, y - 3, 7, 7, colour);
      break;
    }
    case PlaneShape::Ground:
      // Surface vehicles and obstacles are not aircraft and should never be
      // mistaken for one at a glance: a plain square, no heading.
      filledRect(x - 5, y - 5, 11, 11, colour);
      filledRect(x - 2, y - 2, 5, 5, white);
      break;
    case PlaneShape::Airliner:
      fillOutline(x, y, cs, sn, OUTLINE_AIRLINER, colour);
      nacelle(0.0f, 5.5f, 3.0f, 1.4f);
      nacelle(0.0f, -5.5f, 3.0f, 1.4f);
      pixel(x, y, white);
      break;
    case PlaneShape::Generic:
    default:
      fillOutline(x, y, cs, sn, OUTLINE_GENERIC, colour);
      pixel(x, y, white);
      break;
  }
}

// --- icon-geometry-end ---

// ---------------------------------------------------------------------------
// Icon colour.
//
// None of this comes off the air - like every other tracker, colour here is
// derived. Altitude drives the base hue on a 16-entry RGB565 ramp indexed by
// alt >> 11 (2,048 ft per step, saturating at the top step), which is a pure
// function needing no lookup beyond the table itself. State then overrides:
// an emergency squawk wins outright, aircraft on the ground go earth-grey,
// and MLAT or stale tracks are dimmed rather than recoloured so that "less
// certain" reads as less prominent without inventing a new colour meaning.
// ---------------------------------------------------------------------------
struct IconRgb {
  uint8_t r, g, b;
};

constexpr IconRgb ALTITUDE_RAMP[16] = {
    {255, 60, 40},   {255, 110, 30},  {255, 160, 25},  {255, 205, 30},
    {240, 240, 40},  {190, 240, 45},  {130, 235, 55},  {70, 225, 90},
    {45, 220, 150},  {40, 215, 200},  {45, 200, 240},  {60, 165, 250},
    {85, 130, 250},  {120, 105, 245}, {160, 95, 240},  {200, 100, 235},
};

uint16_t aircraftIconColour(const AircraftDisplay &a) {
  // An emergency squawk is the one thing that must never be mistaken for a
  // shade of altitude, so it is returned before anything else can dim it.
  const bool emergencySquawk = a.squawk[0] && (!strcmp(a.squawk, "7500") ||
                                               !strcmp(a.squawk, "7600") ||
                                               !strcmp(a.squawk, "7700"));
  const bool emergencyFlag = a.emergency[0] && strcmp(a.emergency, "none") != 0;
  if (emergencySquawk || emergencyFlag) return rgb(255, 0, 0);

  IconRgb colour;
  if (a.onGround || a.altitudeFt < 0) {
    colour = {150, 120, 90};  // earth-grey: on the surface, or no altitude yet
  } else {
    const int step = min(15, a.altitudeFt >> 11);
    colour = ALTITUDE_RAMP[step < 0 ? 0 : step];
  }

  // MLAT is a computed position rather than a reported one, and a track with
  // no recent update is a guess about where something used to be. Both stay
  // on the altitude ramp - just quieter, so certainty reads as brightness.
  uint16_t scale = 100;
  if (a.positionSource == 2) scale = 55;
  if (a.ageSeconds > 60.0f) scale = scale > 60 ? 60 : 45;
  if (scale != 100) {
    colour.r = static_cast<uint8_t>(colour.r * scale / 100);
    colour.g = static_cast<uint8_t>(colour.g * scale / 100);
    colour.b = static_cast<uint8_t>(colour.b * scale / 100);
  }
  return rgb(colour.r, colour.g, colour.b);
}

// Shape says what the aircraft is (type designator first, emitter category
// second - see shapeForAircraft), colour says where it is and how much to
// trust it (see aircraftIconColour). The two are independent on purpose:
// nothing about the airframe should change with altitude, and nothing about
// altitude should change the silhouette.
void drawAircraftIcon(int x, int y, const AircraftDisplay &a) {
  drawPlaneIcon(x, y, a.track, a.iconShape, aircraftIconColour(a));
}

// Records where an icon was just drawn against which entry in latestAircraft,
// so a later tap can be matched back to a specific aircraft. Call sites reset
// iconHitCount to 0 before their draw loop and call this once per icon drawn.
void recordIconHit(int x, int y, int aircraftIndex) {
  if (iconHitCount >= MAX_AIRCRAFT) return;
  iconHits[iconHitCount].x = static_cast<int16_t>(x);
  iconHits[iconHitCount].y = static_cast<int16_t>(y);
  iconHits[iconHitCount].aircraftIndex = static_cast<int16_t>(aircraftIndex);
  ++iconHitCount;
}

int findAircraftIconAt(int x, int y) {
  int best = -1;
  long bestDistSq = 22 * 22;  // generous finger-sized hit radius
  for (int i = 0; i < iconHitCount; ++i) {
    const long dx = iconHits[i].x - x, dy = iconHits[i].y - y;
    const long distSq = dx * dx + dy * dy;
    if (distSq <= bestDistSq) { bestDistSq = distSq; best = iconHits[i].aircraftIndex; }
  }
  return best;
}

void drawRouteLabel(int x, int y, const RouteCacheEntry *route) {
  if (!route || !route->hasRoute) return;
  char label[11];
  snprintf(label, sizeof(label), "%s>%s", route->origin, route->destination);
  int width = strlen(label) * 6;
  int labelX = constrain(x - width / 2, 2, W - width - 2);
  int labelY = constrain(y + 13, 2, H - 11);
  filledRect(labelX - 2, labelY - 2, width + 4, 11, rgb(0,0,0));
  text5(labelX, labelY, label, rgb(255,255,255));
}

void status(const char *label, uint16_t colour) {
  disc(layout::centreX,18,12,rgb(0,0,0)); disc(layout::centreX,18,8,colour);
  int width=strlen(label)*6;
  text5(layout::centreX-width/2,32,label,rgb(255,255,255));
}

void present() {
  // Start the copy at the top of the vertical blanking interval. There is
  // exactly one framebuffer and the panel scans it out continuously, so
  // writing a whole frame at an arbitrary moment races the beam: the
  // display shows part of the old frame and part of the new one, which is
  // the flickering lines and the torn rows. Beginning at vsync keeps the
  // copy ahead of the scan for the rest of the frame - 768 KB of
  // PSRAM-to-PSRAM takes roughly a third of a frame period, against a full
  // frame of scan-out to stay in front of.
  //
  // This is not the same fault as the bandwidth starvation the bounce
  // buffers exist for, which is why changing the pixel clock across its
  // whole range made no difference to it: tearing happens at any clock.
  //
  // Waiting is capped and its result ignored on purpose - if vsync never
  // arrives, drawing a torn frame beats blocking the UI task.
  if (panelDirectDraw) {
    // Already in the panel's buffer; it only needs pushing out of the CPU
    // cache so the LCD DMA reads what was drawn. No vsync wait either -
    // there is no bulk copy to keep ahead of the scan.
    gfx->flush(true);
  } else {
    rgbpanel->waitForVsync(50);
    gfx->draw16bitRGBBitmap(0, 0, framebuffer, W, H);
  }
  // esp_lcd_rgb_panel_restart() returns ESP_ERR_INVALID_STATE unless
  // CONFIG_LCD_RGB_RESTART_IN_VSYNC is set in the sdkconfig, which cannot be
  // changed from platformio.ini with the prebuilt Arduino libraries. The
  // return value used to be discarded, so a permanent no-op was invisible.
  // Log it once at boot; if it reports 0 this call does nothing and the
  // corrected panel timings above are the real fix.
  const bool restarted = rgbpanel->restartAtNextVsync();
  static bool logged = false;
  if (!logged) {
    logged = true;
    Serial.printf("RGB vsync restart supported: %s\n", restarted ? "yes" : "NO");
  }
}

void renderBootScreen(const String &networkLine = "", uint16_t networkColour = RGB565_CYAN) {
  // The asset is generated per panel shape, so it normally fills the screen
  // exactly; clearing first covers a header built for the other board.
  if (BOOT_IMAGE_W != W || BOOT_IMAGE_H != H) gfx->fillScreen(rgb(4, 10, 16));
  if (!paintBootImage()) {
    // No decoder, no PSRAM for the decode buffer, or an asset that will
    // not decode. A plain title beats a blank panel, and the version,
    // credit and status lines below are drawn either way.
    gfx->fillScreen(rgb(4, 10, 16));
    gfx->setTextSize(3);
    gfx->setTextColor(RGB565_CYAN);
    const String title = "ADS-B / MLAT";
    gfx->setCursor(max(4, (W - static_cast<int>(title.length()) * 18) / 2), H / 2 - 40);
    gfx->print(title);
  }
  gfx->setTextWrap(false);
  gfx->setTextSize(2);
  // Top right rather than down with the credit: both crops are dark sky
  // there (measured at mean 14 of 255), the title occupies the middle, and
  // it matches where the screensaver puts the device address.
  gfx->setTextColor(RGB565_WHITE);
  const String version = String("v") + FIRMWARE_VERSION;
  gfx->setCursor(W - static_cast<int>(version.length()) * 12 - 10, 10);
  gfx->print(version);
  const String credit = "Firmware (c) 2E0LXY D.Loxley 2026";
  gfx->setCursor(max(4, (W - static_cast<int>(credit.length()) * 12) / 2), 414);
  gfx->print(credit);
  if (networkLine.length()) {
    gfx->setTextColor(networkColour);
    const int width = networkLine.length() * 12;
    gfx->setCursor(max(8, (W - width) / 2), 448);
    gfx->print(networkLine);
  }
}

// Overview: the map on the left with a compact nearest-aircraft strip down
// the right. Aircraft are clipped to the map pane so markers never spill
// under the table.
constexpr int OVERVIEW_MAP_WIDTH = W * 5 / 8;

// Offsets from panelX, expressed as fractions of W so the wider WS7 panel
// gets proportionally more room instead of the WS4 pixel values overflowing
// or crowding together.
constexpr int OVERVIEW_COL_MILES = W * 66 / 480;
constexpr int OVERVIEW_COL_ALT = W * 108 / 480;
constexpr int OVERVIEW_COL_ROUTE = W * 144 / 480;

void renderOverviewPage() {
  restoreMap();
  iconHitCount = 0;
  for (int i = 0; i < lastCount; ++i) {
    AircraftDisplay &display = latestAircraft[i];
    if (display.x < 0 || display.x >= OVERVIEW_MAP_WIDTH - 10) continue;
    if (display.y < 0 || display.y >= H) continue;
    drawAircraftIcon(display.x, display.y, display);
    recordIconHit(display.x, display.y, i);
  }

  filledRect(OVERVIEW_MAP_WIDTH, 0, W - OVERVIEW_MAP_WIDTH, H, rgb(2, 10, 18));
  line(OVERVIEW_MAP_WIDTH, 0, OVERVIEW_MAP_WIDTH, H - 1, rgb(30, 90, 120));
  const int panelX = OVERVIEW_MAP_WIDTH + 8;
  text5(panelX, 8, "NEAREST", rgb(80, 220, 255));
  text5(panelX + OVERVIEW_COL_MILES, 8, "MI", rgb(120, 170, 200));
  text5(panelX + OVERVIEW_COL_ALT, 8, "ALT", rgb(120, 170, 200));
  text5(panelX + OVERVIEW_COL_ROUTE, 8, "RTE", rgb(120, 170, 200));
  line(panelX, 18, W - 6, 18, rgb(30, 90, 120));

  const int rows = min(lastCount, (H - 46) / 22);
  for (int i = 0; i < rows; ++i) {
    AircraftDisplay &display = latestAircraft[i];
    const int y = 26 + i * 22;
    const char *callsign = strlen(display.flight) ? display.flight : display.hex;
    text5(panelX, y, callsign, rgb(190, 235, 255));
    char miles[8];
    snprintf(miles, sizeof(miles), "%d", static_cast<int>(display.distanceMiles + 0.5f));
    text5(panelX + OVERVIEW_COL_MILES, y, miles, rgb(245, 205, 65));
    char altitude[10];
    if (display.altitudeFt > 0) snprintf(altitude, sizeof(altitude), "%d", display.altitudeFt);
    else snprintf(altitude, sizeof(altitude), "--");
    text5(panelX + OVERVIEW_COL_ALT, y, altitude, rgb(150, 225, 190));
    RouteCacheEntry *route = cachedRoute(display.flight);
    char routeLabel[11];
    // "---" means not looked up yet (still queued); "NO RTE" means adsbdb
    // was asked and had nothing on file, usually a private/GA registration.
    if (route && route->hasRoute) snprintf(routeLabel, sizeof(routeLabel), "%s>%s", route->origin, route->destination);
    else if (route) strcpy(routeLabel, "NO RTE");
    else strcpy(routeLabel, "---");
    text5(panelX + OVERVIEW_COL_ROUTE, y, routeLabel, rgb(130, 210, 255));
  }

  char footer[24];
  snprintf(footer, sizeof(footer), "%d AIRCRAFT", lastCount);
  text5(panelX, H - 16, footer, rgb(90, 190, 230));
  char count[20];
  snprintf(count, sizeof(count), "%d", lastCount);
  status(count, rgb(35, 210, 80));
  present();
}

void renderMapPage() {
  restoreMap();
  iconHitCount = 0;
  for (int i=0; i<lastCount; ++i) {
    AircraftDisplay &display = latestAircraft[i];
    if (display.x < 0 || display.x >= W || display.y < 0 || display.y >= H) continue;
    drawAircraftIcon(display.x, display.y, display);
    recordIconHit(display.x, display.y, i);
    if (display.positionSource != 2) {
      drawRouteLabel(display.x,display.y,cachedRoute(display.flight));
    }
  }
  char count[20];
  if (creditsRemaining >= 0) snprintf(count,sizeof(count),"%d C%ld",lastCount,creditsRemaining);
  else snprintf(count,sizeof(count),"%d",lastCount);
  status(count,rgb(35,210,80));
  present();
}

void drawVesselIcon(int x, int y, float course, uint16_t colour) {
  const float a = radians(course - 90.0f);
  const float cs = cosf(a), sn = sinf(a);
  auto tx = [&](float px, float py) { return x + lroundf(px * cs - py * sn); };
  auto ty = [&](float px, float py) { return y + lroundf(px * sn + py * cs); };
  disc(x, y, 2, colour);
  line(tx(9, 0), ty(9, 0), tx(-6, -4), ty(-6, -4), colour);
  line(tx(9, 0), ty(9, 0), tx(-6, 4), ty(-6, 4), colour);
  line(tx(-6, -4), ty(-6, -4), tx(-3, 0), ty(-3, 0), colour);
  line(tx(-3, 0), ty(-3, 0), tx(-6, 4), ty(-6, 4), colour);
}

void renderMarinePage() {
  restoreMap();
  int plotted = 0;
  for (int i = 0; i < vesselCount; ++i) {
    VesselDisplay &vessel = latestVessels[i];
    int x, y;
    if (!mapPoint(vessel.latitude, vessel.longitude, x, y)) continue;
    drawVesselIcon(x, y, vessel.courseOverGround, rgb(70, 200, 255));
    ++plotted;
  }
  char count[28];
  if (!marineTrackingEnabled) snprintf(count, sizeof(count), "MARINE TRACKING OFF");
  else if (!marineConfigured()) snprintf(count, sizeof(count), "AIS NOT CONFIGURED");
  else snprintf(count, sizeof(count), "%d SHIPS%s", plotted, aisConnected ? "" : " (OFFLINE)");
  status(count, !marineConfigured() ? rgb(150,150,150) : aisConnected ? rgb(35,210,80) : rgb(220,60,60));
  present();
}

// Table column origins, sized to the content they hold at the fixed scale-2
// glyph width (12px/char) rather than scaled to the panel width - a wider
// board should give its extra room to the ROUTE column, not stretch empty
// gaps between narrow columns proportionally. Small columns get slightly
// more even padding than the bare minimum so the row reads as a grid.
constexpr int COL_LOGO = 2;
constexpr int COL_CALLSIGN = 34;
constexpr int COL_MILES = 150;
constexpr int COL_SOURCE = 210;
constexpr int COL_DIR = 240;
constexpr int COL_ALT = 280;
constexpr int COL_ROUTE = 350;

const uint16_t ROW_BAND_DARK = rgb(6, 16, 28);
const uint16_t ROW_BAND_LIGHT = rgb(14, 36, 58);

// Picks the richest origin/destination representation that fits maxChars,
// falling back from full airport names to city names to the departure-board
// abbreviation to raw codes - so a wide panel shows full names while a
// narrow one still gets something readable instead of clipped garbage.
void buildRouteLabel(const RouteCacheEntry *route, char *output, size_t outSize, int maxChars) {
  // No cache entry yet: this callsign hasn't reached the front of the
  // (throttled, two-per-refresh) lookup queue. A resolved entry with
  // hasRoute false means adsbdb was actually asked and had nothing - most
  // often a private/GA registration with no scheduled route on file.
  if (!route) { snprintf(output, outSize, "---"); return; }
  if (!route->hasRoute) { snprintf(output, outSize, maxChars >= 8 ? "NO ROUTE" : "---"); return; }
  struct Option { const char *origin; const char *destination; };
  const Option options[] = {
    {route->originName[0] ? route->originName : nullptr, route->destinationName[0] ? route->destinationName : nullptr},
    {route->originCity[0] ? route->originCity : nullptr, route->destinationCity[0] ? route->destinationCity : nullptr},
    {route->originAbbrev[0] ? route->originAbbrev : nullptr, route->destinationAbbrev[0] ? route->destinationAbbrev : nullptr},
    {route->origin, route->destination},
  };
  for (const Option &opt : options) {
    if (!opt.origin || !opt.destination) continue;
    int len = static_cast<int>(strlen(opt.origin) + 1 + strlen(opt.destination));
    if (len <= maxChars) { snprintf(output, outSize, "%s>%s", opt.origin, opt.destination); return; }
  }
  // Nothing fit - an extremely narrow panel. Show the codes and let the
  // panel edge clip them rather than show nothing.
  snprintf(output, outSize, "%s>%s", route->origin, route->destination);
}

void renderTablePage() {
  filledRect(0,0,W,H,rgb(2,10,18));
  text5(layout::centreX - 96,7,"NEAREST AIRCRAFT",rgb(80,220,255),2);
  text5(COL_LOGO,31,"LOGO",rgb(170,190,205));
  text5(COL_CALLSIGN,31,"CALLSIGN",rgb(170,190,205));
  text5(COL_MILES,31,"MILES",rgb(170,190,205));
  text5(COL_SOURCE,31,"S",rgb(170,190,205));
  text5(COL_DIR,31,"DIR",rgb(170,190,205));
  text5(COL_ALT,31,"ALT FT",rgb(170,190,205));
  text5(COL_ROUTE,31,"FROM TO",rgb(170,190,205));
  line(3,41,W - 4,41,rgb(55,85,105));

  // Clamped here (not just where the scroll gesture changes it) because
  // lastCount shrinks on every fetch as aircraft leave range, which can
  // strand the offset past the end of a now-shorter list.
  tableScrollOffset = constrain(tableScrollOffset, 0, max(0, lastCount - TABLE_VISIBLE_ROWS));
  const int rows = min(lastCount - tableScrollOffset, TABLE_VISIBLE_ROWS);
  for (int i=0; i<rows; ++i) {
    AircraftDisplay &display = latestAircraft[tableScrollOffset + i];
    int y=49+i*40;
    filledRect(0, y-7, W, 40, (i & 1) ? ROW_BAND_LIGHT : ROW_BAND_DARK);
    char distance[6], altitude[7], routeLabel[84];
    snprintf(distance,sizeof(distance),"%d",static_cast<int>(lroundf(display.distanceMiles)));
    if (display.altitudeFt >= 0) snprintf(altitude,sizeof(altitude),"%d",display.altitudeFt);
    else strcpy(altitude,"--");
    const char *identity=display.flight[0] ? display.flight : display.hex;
    const bool isMlat = display.positionSource == 2;
    // MLAT-derived aircraft never get a route lookup (fetchAircraft/
    // fetchAdsbV2Aircraft skip them - it's a multilateration estimate, not a
    // real callsign an ADS-B route API would recognise), so cachedRoute()
    // for one is always empty. Say why instead of showing "---", which reads
    // as a lookup that's still pending or failed.
    if (isMlat) {
      strncpy(routeLabel, "MLAT TRIANGULATION", sizeof(routeLabel) - 1);
      routeLabel[sizeof(routeLabel) - 1] = 0;
    } else {
      RouteCacheEntry *route=cachedRoute(display.flight);
      // Drawn at scale 1 below (6px/char), not the scale-2 used elsewhere in
      // this row - full airport names need roughly double the char budget
      // scale 2 would allow in this column's width.
      const int routeMaxChars = (W - 4 - COL_ROUTE) / 6;
      buildRouteLabel(route, routeLabel, sizeof(routeLabel), routeMaxChars);
    }
    if (isMlat) drawMlatPlane(16,y+7,display.track);
    else drawOperatorBadge(16,y+7,display.flight,display.hex);
    text5(COL_CALLSIGN,y,identity,rgb(255,220,60),2);
    // Registration (tail number) is the airframe's fixed ID, distinct from
    // the callsign above it which can vary flight to flight (e.g. QTR74X
    // flown by A7-AOA, or a squadron callsign like REDARROW on XX221).
    // Scale 1 is a 5x7px glyph - legible in an 800px-wide route label but too
    // small to read as text at normal viewing distance; it reads as a row of
    // dots instead. Scale 2 matches the callsign line above it and still
    // clears the row band (40px tall) with room to spare.
    if (display.registration[0]) text5(COL_CALLSIGN,y+16,display.registration,rgb(150,180,200),2);
    text5(COL_MILES,y,distance,rgb(255,255,255),2);
    text5(COL_SOURCE,y,isMlat ? "M" : "A",isMlat ? rgb(255,65,65) : rgb(60,220,130),2);
    text5(COL_DIR,y,compassDirection(display.track),rgb(255,255,255),2);
    text5(COL_ALT,y,altitude,rgb(255,255,255),2);
    text5(COL_ROUTE,y+4,routeLabel,rgb(255,255,255));
  }
  char footer[72];
  const char *scrollHint =
      (lastCount > TABLE_VISIBLE_ROWS) ? " - SWIPE UP/DOWN, SIDEWAYS FOR PAGE"
                                       : " - SWIPE SIDEWAYS FOR PAGE";
  const int rangeStart = rows > 0 ? tableScrollOffset + 1 : 0;
  if (creditsRemaining >= 0)
    snprintf(footer,sizeof(footer),"%d-%d OF %d  C%ld%s",rangeStart,tableScrollOffset+rows,lastCount,creditsRemaining,scrollHint);
  else
    snprintf(footer,sizeof(footer),"%d-%d OF %d%s",rangeStart,tableScrollOffset+rows,lastCount,scrollHint);
  text5(layout::centreX - 110,layout::footerY,footer,rgb(130,160,180));
  present();
}

void radarRing(int centreX, int centreY, int radius, uint16_t colour) {
  for (int degreesValue = 0; degreesValue < 360; ++degreesValue) {
    const float angle = radians(static_cast<float>(degreesValue));
    pixel(centreX + lroundf(cosf(angle) * radius),
          centreY + lroundf(sinf(angle) * radius), colour);
  }
}

void renderRadarPage() {
  constexpr int centreX = W / 2;
  constexpr int centreY = 245;
  constexpr int outerRadius = layout::radarRadius;
  const uint16_t background = rgb(1, 12, 16);
  const uint16_t grid = rgb(22, 93, 84);
  const uint16_t gridBright = rgb(42, 145, 119);
  const uint16_t sweep = rgb(45, 225, 155);
  filledRect(0, 0, W, H, background);
  text5(12, 9, "LIVE AIRCRAFT RADAR", rgb(90, 235, 185), 2);
  char rangeLabel[22];
  snprintf(rangeLabel, sizeof(rangeLabel), "RANGE %u NM", queryRadiusNm);
  text5(W - 135, 13, rangeLabel, rgb(175, 205, 195));

  for (int ring = 1; ring <= 4; ++ring) {
    radarRing(centreX, centreY, outerRadius * ring / 4,
              ring == 4 ? gridBright : grid);
    char ringLabel[10];
    snprintf(ringLabel, sizeof(ringLabel), "%u", queryRadiusNm * ring / 4);
    text5(centreX + 4, centreY - outerRadius * ring / 4 + 3,
          ringLabel, gridBright);
  }
  line(centreX - outerRadius, centreY, centreX + outerRadius, centreY, grid);
  line(centreX, centreY - outerRadius, centreX, centreY + outerRadius, grid);
  text5(centreX - 3, centreY - outerRadius - 15, "N", rgb(210, 240, 226));
  text5(centreX - 3, centreY + outerRadius + 8, "S", rgb(210, 240, 226));
  text5(centreX + outerRadius + 8, centreY - 3, "E", rgb(210, 240, 226));
  text5(centreX - outerRadius - 14, centreY - 3, "W", rgb(210, 240, 226));

  // A short phosphor-style trail keeps the sweep readable without hiding targets.
  for (int trail = 3; trail >= 0; --trail) {
    const float angle = radians(radarSweepDegrees - trail * 3.0f - 90.0f);
    const int endX = centreX + lroundf(cosf(angle) * outerRadius);
    const int endY = centreY + lroundf(sinf(angle) * outerRadius);
    const uint16_t colour = trail == 0 ? sweep : rgb(12 + trail * 4, 65 + trail * 18, 54 + trail * 12);
    line(centreX, centreY, endX, endY, colour);
  }

  int plotted = 0;
  iconHitCount = 0;
  for (int i = 0; i < lastCount; ++i) {
    AircraftDisplay &aircraft = latestAircraft[i];
    if (!isfinite(aircraft.latitude) || !isfinite(aircraft.longitude)) continue;
    const float distanceNm = aircraft.distanceMiles / 1.15077945f;
    const bool outside = distanceNm > queryRadiusNm;
    const float radius = min(1.0f, distanceNm / max(1.0f, static_cast<float>(queryRadiusNm))) * outerRadius;
    const float bearing = radians(bearingFromHome(aircraft.latitude, aircraft.longitude) - 90.0f);
    const int x = centreX + lroundf(cosf(bearing) * radius);
    const int y = centreY + lroundf(sinf(bearing) * radius);
    if (outside) {
      disc(x, y, 3, rgb(255, 65, 65));
    } else {
      drawAircraftIcon(x, y, aircraft);
      recordIconHit(x, y, i);
    }
    if (outside) continue;
    if (plotted < 10) {
      const char *identity = aircraft.flight[0] ? aircraft.flight : aircraft.hex;
      const int labelX = constrain(x + 12, 2, W - static_cast<int>(strlen(identity)) * 6 - 2);
      const int labelY = constrain(y - 3, 37, H - 12);
      text5(labelX, labelY, identity,
            aircraft.positionSource == 2 ? rgb(255, 75, 75) : rgb(220, 250, 235));
    }
    ++plotted;
  }
  disc(centreX, centreY, 5, rgb(255, 220, 80));
  text5(7, H - 12, "RED RIM TARGETS ARE OUTSIDE RANGE", rgb(170, 195, 188));
  char countLabel[18];
  snprintf(countLabel, sizeof(countLabel), "%d TRACKED", plotted);
  text5(W - 86, H - 12, countLabel, rgb(90, 235, 185));
  present();
}

// A tap that hits a plotted aircraft icon (Overview/Map/Radar) shows this
// instead of advancing the page, giving the touchscreen the same
// "tap a marker for full detail" behaviour the browser map already has.
void renderAircraftDetailCard(int aircraftIndex) {
  if (aircraftIndex < 0 || aircraftIndex >= lastCount) return;
  AircraftDisplay &a = latestAircraft[aircraftIndex];
  const int cardW = min(360, W - 16);
  const int cardH = min(230, H - 16);
  const int cx = (W - cardW) / 2, cy = (H - cardH) / 2;
  const uint16_t frame = rgb(80, 220, 255);
  filledRect(cx, cy, cardW, cardH, rgb(4, 12, 20));
  filledRect(cx, cy, cardW, 2, frame);
  filledRect(cx, cy + cardH - 2, cardW, 2, frame);
  filledRect(cx, cy, 2, cardH, frame);
  filledRect(cx + cardW - 2, cy, 2, cardH, frame);

  const char *identity = a.flight[0] ? a.flight : a.hex;
  text5(cx + 10, cy + 9, identity, rgb(255, 220, 60), 2);
  const bool isMlat = a.positionSource == 2;
  text5(cx + cardW - 66, cy + 12, isMlat ? "MLAT" : "ADS-B",
        isMlat ? rgb(255, 65, 65) : rgb(60, 220, 130));

  int row = cy + 30;
  const int lineHeight = 12;
  auto line5 = [&](const char *text, uint16_t colour) {
    text5(cx + 10, row, text, colour);
    row += lineHeight;
  };
  char buf[64];
  line5(a.operatorName[0] ? a.operatorName : "Unknown operator", rgb(190, 220, 240));
  snprintf(buf, sizeof(buf), "REG %s  HEX %s  %s", a.registration[0] ? a.registration : "--",
           a.hex, a.aircraftType[0] ? a.aircraftType : "TYPE UNKNOWN");
  line5(buf, rgb(200, 210, 220));
  if (a.altitudeFt >= 0)
    snprintf(buf, sizeof(buf), "ALT %d FT  V/S %+d FPM", a.altitudeFt, static_cast<int>(lroundf(a.verticalRateFpm)));
  else
    snprintf(buf, sizeof(buf), "ALT --  V/S %+d FPM", static_cast<int>(lroundf(a.verticalRateFpm)));
  line5(buf, rgb(255, 255, 255));
  snprintf(buf, sizeof(buf), "SPD %d KT  HDG %03d %s", static_cast<int>(lroundf(a.speedKnots)),
           ((static_cast<int>(a.track) % 360) + 360) % 360, compassDirection(a.track));
  line5(buf, rgb(255, 255, 255));
  snprintf(buf, sizeof(buf), "DIST %d MI  SQUAWK %s  CAT %s", static_cast<int>(lroundf(a.distanceMiles)),
           a.squawk[0] ? a.squawk : "--", a.category[0] ? a.category : "--");
  line5(buf, rgb(255, 255, 255));
  snprintf(buf, sizeof(buf), "LAT %.4f  LON %.4f", a.latitude, a.longitude);
  line5(buf, rgb(255, 255, 255));
  RouteCacheEntry *route = cachedRoute(a.flight);
  char routeLabel[40];
  buildRouteLabel(route, routeLabel, sizeof(routeLabel), (cardW - 20) / 6);
  snprintf(buf, sizeof(buf), "ROUTE %s", routeLabel);
  line5(buf, rgb(130, 210, 255));
  if (a.emergency[0] && strcmp(a.emergency, "none")) {
    snprintf(buf, sizeof(buf), "EMERGENCY: %s", a.emergency);
    line5(buf, rgb(255, 65, 65));
  }
  text5(cx + 10, cy + cardH - 13, "TAP ANYWHERE TO CLOSE", rgb(130, 160, 180));
  present();
}

// Rotating single-aircraft display in the style of an LED departure board:
// operator badge, callsign, route and type, plus a phase guess (departing/
// arriving/en route) derived from altitude and vertical rate - the feed has
// no explicit flight-phase field to read instead.
// Truncates in place to what will actually fit, so a long operator name or
// airport pair clips cleanly instead of running off the panel. text5()
// advances 6*scale pixels per character.
void fitTextTo(char *text, int xLeft, int scale, int xRight) {
  const int maxChars = (xRight - xLeft) / (6 * scale);
  if (maxChars > 0 && static_cast<int>(strlen(text)) > maxChars) text[maxChars] = 0;
  else if (maxChars <= 0) text[0] = 0;
}

void fitText(char *text, int xLeft, int scale) {
  fitTextTo(text, xLeft, scale, W - 6);
}

// Overhead only: something you could plausibly see or hear from the
// receiver location, not just anything within the full query radius. Ground
// distance alone isn't enough - an airliner at cruise altitude can be 0
// miles away horizontally (directly above) and still be far too high to see
// or hear, so filter on slant range (distance and altitude combined).
//
// Split out of renderScreensaverPage() so the rotate timer can ask how many
// there are without repainting the screen to find out.
int collectOverheadAircraft(int *matches, int capacity) {
  constexpr float OVERHEAD_MAX_SLANT_MILES = 5.0f;
  int count = 0;
  for (int i = 0; i < lastCount && count < capacity; ++i) {
    const AircraftDisplay &candidate = latestAircraft[i];
    if (candidate.onGround) continue;
    const float altitudeMiles = candidate.altitudeFt > 0 ? candidate.altitudeFt / 5280.0f : 0.0f;
    const float slantMiles =
        sqrtf(candidate.distanceMiles * candidate.distanceMiles + altitudeMiles * altitudeMiles);
    if (slantMiles <= OVERHEAD_MAX_SLANT_MILES) matches[count++] = i;
  }
  return count;
}

// Set when the screensaver is entered, so the first frame after it appears
// is always painted even if the content signature happens to match.
bool screensaverNeedsRedraw = true;

void renderScreensaverPage() {
  int overheadMatches[32];
  const int overheadCount = collectOverheadAircraft(overheadMatches, 32);

  // Repainting clears the whole screen: 768 KB of writes across the same
  // PSRAM bus the panel refills its bounce buffers from, which is the burst
  // that makes the picture slip. Every fetch used to trigger one through
  // renderCurrentPage(), whether or not a single displayed value had
  // changed - and on the no-aircraft page nothing ever changes. Skip the
  // repaint when the frame would be identical.
  //
  // The signature covers everything this page actually draws for the
  // selected aircraft; anything not in it cannot change the picture.

  // The screensaver is the one page with no header, so it is also the one
  // place the admin address cannot be read off the screen. It goes in the
  // top corner of both frames: the empty one, where there is nothing else to
  // read, and the departure board, where the top band above the operator
  // tile is unused anyway.
  //
  // Worked out before the signature below and mixed into it, so a reconnect
  // or a new DHCP lease repaints. Without that this page skips its repaint
  // whenever the frame would be identical and a stale address would sit
  // there indefinitely.
  char address[40];
  if (WiFi.status() == WL_CONNECTED)
    snprintf(address, sizeof(address), "%s", WiFi.localIP().toString().c_str());
  else
    snprintf(address, sizeof(address), "NO WIFI");

  uint32_t signature = 2166136261u;
  auto mix = [&signature](uint32_t value) {
    signature = (signature ^ value) * 16777619u;
  };
  auto mixText = [&mix](const char *text) {
    for (const char *c = text; *c; ++c) mix(static_cast<uint8_t>(*c));
  };
  mix(static_cast<uint32_t>(overheadCount));
  mixText(address);
  if (overheadCount > 0) {
    const AircraftDisplay &shown = latestAircraft[overheadMatches[0]];
    mixText(shown.hex);
    mixText(shown.flight);
    mixText(shown.squawk);
    mix(static_cast<uint32_t>(shown.altitudeFt));
    mix(static_cast<uint32_t>(lroundf(shown.speedKnots)));
    mix(static_cast<uint32_t>(lroundf(shown.track)));
    mix(static_cast<uint32_t>(lroundf(shown.verticalRateFpm)));
    mix(static_cast<uint32_t>(lroundf(shown.distanceMiles * 10.0f)));
    mix(static_cast<uint32_t>(lroundf(shown.signalDb)));
    mix(shown.messages);
    mix(static_cast<uint32_t>(lroundf(shown.ageSeconds)));
    const RouteCacheEntry *route = cachedRoute(shown.flight);
    mixText(route && route->hasRoute ? route->origin : "");
    mixText(route && route->hasRoute ? route->destination : "");
    mixText(route ? route->airline : "");
    mixText(shown.operatorName);
  }
  static uint32_t lastSignature = 0;
  if (!screensaverNeedsRedraw && signature == lastSignature) return;
  lastSignature = signature;
  screensaverNeedsRedraw = false;

  filledRect(0, 0, W, H, rgb(0, 0, 0));

  // Right-aligned in the top corner. Each glyph advances 6*scale with the
  // last column of that advance being the gap to the next character, so the
  // drawn width is one scale unit narrower than the advance total.
  //
  // Sits above the operator tile, which starts at margin + H/24, so it
  // clears the board's own first line on either panel size.
  {
    const int addressScale = W >= 800 ? 2 : 1;
    const int addressWidth = static_cast<int>(strlen(address)) * 6 * addressScale - addressScale;
    const int edge = W / 40;
    text5(W - edge - addressWidth, edge, address, rgb(120, 140, 160), addressScale);
  }

  if (overheadCount == 0) {
    text5(20, H / 2 - 6, "NO OVERHEAD AIRCRAFT", rgb(120, 140, 160), 2);
    text5(20, H / 2 + 24, "TAP OR SWIPE TO RETURN", rgb(70, 90, 110));
    present();
    return;
  }
  // The closest thing overhead, not a rotation through all of them.
  // latestAircraft is sorted by distance (sortAircraftByDistance), so the
  // first match is the nearest, and it changes only when something actually
  // overtakes it - which is a far better trigger for a full-screen repaint
  // than a timer that cycles whether or not the picture would differ.
  AircraftDisplay &a = latestAircraft[overheadMatches[0]];
  RouteCacheEntry *route = cachedRoute(a.flight);
  const bool hasRoute = route && route->hasRoute;

  // Departure-board layout: a colour tile standing in for the airline logo,
  // three identity lines beside it, then the telemetry rows underneath at
  // full width. Everything is derived from W/H so the 480x480 board gets the
  // same design at a smaller scale rather than a clipped copy of this one.
  const int margin = W / 40;
  const int tile = H / 3;
  const int tileY = margin + H / 24;
  const int textX = margin + tile + W / 40;
  const uint16_t white = rgb(255, 255, 255);
  const uint16_t cyan = rgb(120, 205, 255);
  const uint16_t dim = rgb(150, 165, 180);

  drawOperatorTile(margin, tileY, tile, a.flight, a.hex);

  // The type photograph sits opposite the logo, at the top right, so the two
  // pictures frame the identity lines between them. Decided before those
  // lines are laid out because they have to be truncated to make room -
  // which is why availability is answered without decoding anything.
  char designator[8];
  const bool hasDesignator = typePhotoCode(a.aircraftType, designator);
  const int photoLeft = W - margin - PHOTO_WIDTH;
  // Only where there is genuinely room. The route codes are the widest thing
  // on this page and matter more than a photograph, so require space for at
  // least eight of them at their own scale. On the 480x480 board that fails,
  // and it keeps its full text and goes without - checked, not assumed:
  // reserving 160 px there leaves 116, and "BRS-FAO" needs 126.
  const int widestScale = W >= 800 ? 5 : 3;  // routeScale, declared below
  const bool roomForPhoto = photoLeft - 8 - textX >= 8 * 6 * widestScale;
  const bool showPhoto = hasDesignator && roomForPhoto && typePhotoAvailable(designator);
  if (showPhoto) drawCachedTypePhoto(photoLeft, tileY, designator);
  else if (hasDesignator && roomForPhoto && !photoFetchPending &&
           !photoKnownUnavailable(designator)) {
    // Asked for here because this is the only place that knows a type is
    // being shown to someone, so the device only fetches photographs it is
    // about to display.
    memcpy(photoFetchCode, designator, sizeof(designator));
    photoFetchPending = true;
  }
  const int identityRight = showPhoto ? photoLeft - 8 : W - 6;

  // Line 1 - who. The operator name when the feed carries one, otherwise the
  // callsign, which is the most identifying thing left.
  char line[64];
  // Who is flying it, best source first: the operator the feed sent, then
  // the airline the aggregator resolved from the callsign, then the
  // firmware's own prefix table for when neither is available, and only
  // then the bare callsign. Showing "EAG78H" told the viewer nothing when
  // "EMERALD AIRLINES" was derivable from it.
  const char *operatorLabel = a.operatorName[0] ? a.operatorName : nullptr;
  if (!operatorLabel && route && route->airline[0]) operatorLabel = route->airline;
  if (!operatorLabel) operatorLabel = operatorNameForCallsign(a.flight);
  if (!operatorLabel) operatorLabel = a.flight[0] ? a.flight : a.hex;
  snprintf(line, sizeof(line), "%s", operatorLabel);
  const int nameScale = W >= 800 ? 3 : 2;
  fitTextTo(line, textX, nameScale, identityRight);
  text5(textX, tileY, line, white, nameScale);

  // Line 2 - where. Airport codes are the headline; the full names go in a
  // lower row where there is room for them.
  if (hasRoute) snprintf(line, sizeof(line), "%s-%s", route->origin, route->destination);
  else snprintf(line, sizeof(line), "%s", a.flight[0] ? a.flight : a.hex);
  const int routeScale = W >= 800 ? 5 : 3;
  fitTextTo(line, textX, routeScale, identityRight);
  text5(textX, tileY + 10 * nameScale, line, cyan, routeScale);

  // Line 3 - what. The full model where the aggregator resolved one, since
  // "Boeing 737-800" tells a viewer something and "B738" does not; the bare
  // designator otherwise. Registration and callsign alongside, as the
  // callsign is no longer the headline once a route has resolved.
  snprintf(line, sizeof(line), "%s  %s  %s",
           a.typeName[0] ? a.typeName : (a.aircraftType[0] ? a.aircraftType : "UNKNOWN"),
           a.registration[0] ? a.registration : a.hex,
           a.flight[0] ? a.flight : "");
  fitTextTo(line, textX, 2, identityRight);
  text5(textX, tileY + 10 * nameScale + 11 * routeScale, line, dim, 2);

  int y = tileY + tile + H / 16;
  const int rowScale = W >= 800 ? 3 : 2;
  const int rowStep = 11 * rowScale;

  // The two headline telemetry rows, in the reference's own units: thousands
  // of feet, miles per hour, degrees true, and feet per second rather than
  // per minute.
  const float altKft = a.altitudeFt > 0 ? a.altitudeFt / 1000.0f : 0.0f;
  const int speedMph = static_cast<int>(lroundf(a.speedKnots * 1.15078f));
  const int verticalFtPerSec = static_cast<int>(lroundf(a.verticalRateFpm / 60.0f));
  snprintf(line, sizeof(line), "ALT:%.1fKFT, SPD:%dMPH", altKft, speedMph);
  fitText(line, margin, rowScale);
  text5(margin, y, line, white, rowScale);
  y += rowStep;
  snprintf(line, sizeof(line), "TRK:%dDEG, VR:%+dFT/S",
           static_cast<int>(lroundf(a.track)), verticalFtPerSec);
  fitText(line, margin, rowScale);
  text5(margin, y, line, white, rowScale);
  y += rowStep;

  // The radio callsign, which is what would be heard on the air and is
  // rarely the operator's trading name: Jet2 answers to "Channex" and
  // British Airways to "Speedbird". Only when the aggregator resolved one,
  // and only when it differs from the operator name already above.
  if (a.telephony[0] && strcasecmp(a.telephony, a.operatorName)) {
    snprintf(line, sizeof(line), "RADIO:%s", a.telephony);
    fitText(line, margin, rowScale);
    text5(margin, y, line, cyan, rowScale);
    y += rowStep;
  }

  // Everything else the feed gives us for this airframe. Squawk is shown in
  // red when it is one of the three emergency codes, which is the one value
  // on this screen worth interrupting someone for.
  const bool emergencySquawk = a.squawk[0] && (!strcmp(a.squawk, "7500") ||
                                               !strcmp(a.squawk, "7600") ||
                                               !strcmp(a.squawk, "7700"));
  snprintf(line, sizeof(line), "DIST:%.1fMI  SQK:%s  SIG:%.0fDB  MSGS:%lu",
           a.distanceMiles, a.squawk[0] ? a.squawk : "----",
           a.signalDb > -900 ? a.signalDb : 0.0f,
           static_cast<unsigned long>(a.messages));
  fitText(line, margin, 2);
  text5(margin, y, line, emergencySquawk ? rgb(255, 60, 60) : dim, 2);
  y += 24;

  // What the squawk means, where it means anything. A discrete code - a
  // temporary tag issued from a controller's local block - gets no label,
  // because inventing one would be worse than leaving it bare.
  if (const char *meaning = squawkMeaning(a.squawk)) {
    snprintf(line, sizeof(line), "SQUAWK %s: %s", a.squawk, meaning);
    fitText(line, margin, 2);
    text5(margin, y, line, emergencySquawk ? rgb(255, 60, 60) : dim, 2);
  } else if (a.squawk[0]) {
    snprintf(line, sizeof(line), "SQUAWK %s: DISCRETE CODE, ATC ASSIGNED", a.squawk);
    fitText(line, margin, 2);
    text5(margin, y, line, rgb(110, 130, 150), 2);
  }
  y += 24;

  // Full airport names, which is what makes the route mean something to
  // someone who does not read IATA codes.
  if (hasRoute) {
    const char *from = route->originName[0] ? route->originName : route->origin;
    const char *to = route->destinationName[0] ? route->destinationName : route->destination;
    snprintf(line, sizeof(line), "%s > %s", from, to);
  } else {
    snprintf(line, sizeof(line), "%s", route ? "NO SCHEDULED ROUTE" : "LOOKING UP ROUTE...");
  }
  fitText(line, margin, 2);
  text5(margin, y, line, cyan, 2);
  y += 24;

  // Provenance: how the position was derived, how stale it is, and where the
  // aircraft is registered - the details that say how much to trust the rest.
  snprintf(line, sizeof(line), "%s  %s  %.0fS AGO  %s",
           a.hex, a.positionSource == 2 ? "MLAT" : "ADS-B",
           a.ageSeconds >= 0 ? a.ageSeconds : 0.0f,
           a.country[0] ? a.country : "");
  fitText(line, margin, 2);
  text5(margin, y, line, rgb(110, 130, 150), 2);

  y += 24;

  // Position and the barometric/geometric altitude pair. The two altitudes
  // differ by the local pressure error, so showing both is the honest
  // version of a single "altitude" number.
  snprintf(line, sizeof(line), "%.4f %.4f  BARO:%dFT  GEOM:%dFT",
           a.latitude, a.longitude, a.altitudeFt,
           a.geometricAltitudeFt >= 0 ? a.geometricAltitudeFt : a.altitudeFt);
  fitText(line, margin, 2);
  text5(margin, y, line, rgb(110, 130, 150), 2);

  // Which of the overhead aircraft this is, and the way out.
  char footer[48];
  if (overheadCount > 1)
    snprintf(footer, sizeof(footer), "NEAREST OF %d OVERHEAD - TAP OR SWIPE TO RETURN", overheadCount);
  else
    snprintf(footer, sizeof(footer), "OVERHEAD - TAP OR SWIPE TO RETURN");
  text5(margin, H - 14, footer, rgb(70, 90, 110));
  present();
}

const char *displayPageName() {
  if (displayPage == DisplayPage::Overview) return "overview";
  if (displayPage == DisplayPage::Table) return "table";
  if (displayPage == DisplayPage::Radar) return "radar";
  if (displayPage == DisplayPage::Marine) return "marine";
  return "map";
}

void renderCurrentPage() {
  if (screensaverActive) { renderScreensaverPage(); return; }
  if (displayPage == DisplayPage::Overview) renderOverviewPage();
  else if (displayPage == DisplayPage::Table) renderTablePage();
  else if (displayPage == DisplayPage::Radar) renderRadarPage();
  else if (displayPage == DisplayPage::Marine) renderMarinePage();
  else renderMapPage();
}

// Sorting indices and applying one permutation costs at most `lastCount`
// struct moves instead of up to ~31k copies of a ~196 byte record in PSRAM.
void sortAircraftByDistance() {
  if (lastCount < 2) return;
  static uint16_t order[MAX_AIRCRAFT];
  static bool placed[MAX_AIRCRAFT];
  for (int i = 0; i < lastCount; ++i) order[i] = static_cast<uint16_t>(i);
  for (int i = 1; i < lastCount; ++i) {
    const uint16_t key = order[i];
    const float keyDistance = latestAircraft[key].distanceMiles;
    int j = i - 1;
    while (j >= 0 && latestAircraft[order[j]].distanceMiles > keyDistance) {
      order[j + 1] = order[j];
      --j;
    }
    order[j + 1] = key;
  }
  memset(placed, 0, sizeof(placed));
  for (int i = 0; i < lastCount; ++i) {
    if (placed[i] || order[i] == i) { placed[i] = true; continue; }
    AircraftDisplay hold = latestAircraft[i];
    int slot = i;
    while (true) {
      const int source = order[slot];
      placed[slot] = true;
      if (source == i) { latestAircraft[slot] = hold; break; }
      latestAircraft[slot] = latestAircraft[source];
      slot = source;
    }
  }
}

void fetchAdsbV2Aircraft() {
  logHeapDiagnostics("fetch-start");
  // Provider terms, checked directly against each provider's own published
  // docs (also summarised for the user in web_ui.h's providerNotes):
  // - adsb.fi (github.com/adsbfi/opendata): personal, non-commercial use
  //   only; no reselling/redistributing the data; 1 req/s. A single device
  //   run by its owner for their own display is fine. A commercial backend
  //   aggregating this feed for many customers is exactly what these terms
  //   prohibit without adsb.fi's separate written permission.
  // - airplanes.live: blocks cloud/datacenter source IPs outright and asks
  //   that anything beyond personal use go through contact@airplanes.live
  //   first (confirmed firsthand - this project's own aggregator VPS got
  //   blocked with that exact message).
  // - adsb.lol (github.com/adsblol/api): no non-commercial restriction
  //   found; BSD-3-Clause. Docs say a future API key will be earned by
  //   feeding adsb.lol, but none is required yet.
  // None of this blocks a single hobbyist device querying a provider
  // directly for itself - it only matters for a shared backend serving many
  // devices, which is a decision for whatever fetches on the backend's
  // behalf, not this per-device code path.
  String url;
  const String latitude = String(homeLatitude, 5);
  const String longitude = String(homeLongitude, 5);
  const String radius = String(queryRadiusNm);
  if (apiProvider == "aggregator") {
    // Our own backend: polls adsb.fi/airplanes.live/adsb.lol centrally on a
    // shared cache and dedupes by ICAO hex, so many devices share one set
    // of upstream connections instead of each hitting the public APIs
    // directly - see the provider-terms comment above for why that matters
    // at more than a handful of devices. Same {"ac": [...]} response shape
    // as every other provider here, so no parsing changes needed.
    // Requires a per-device key issued from the account dashboard at
    // adsb.2e0lxy.uk/account - the backend rejects requests with no key.
    if (!aggregatorApiKey.length()) {
      finishFeedAttempt("API key required");
      status("KEY", rgb(245,30,35));
      present();
      return;
    }
    url = "https://adsb.2e0lxy.uk/v1/aircraft?lat=" + latitude + "&lon=" + longitude + "&radius=" + radius;
  } else if (apiProvider == "adsbfi") {
    url = "https://opendata.adsb.fi/api/v3/lat/" + latitude + "/lon/" + longitude + "/dist/" + radius;
  } else if (apiProvider == "airplaneslive") {
    url = "https://api.airplanes.live/v2/point/" + latitude + "/" + longitude + "/" + radius;
  } else if (apiProvider == "adsblol") {
    url = "https://api.adsb.lol/v2/point/" + latitude + "/" + longitude + "/" + radius;
  } else if (apiProvider == "adsbone") {
    url = "https://api.adsb.one/v2/point/" + latitude + "/" + longitude + "/" + radius;
  } else if (apiProvider == "adsbx") {
    if (!rapidApiKey.length()) {
      finishFeedAttempt("API key required");
      status("KEY", rgb(245,30,35));
      present();
      return;
    }
    url = "https://adsbexchange-com1.p.rapidapi.com/v2/lat/" + latitude + "/lon/" + longitude + "/dist/" + radius + "/";
  } else if (apiProvider == "flyitalyadsb") {
    // FlyItalyADSB: CC BY-SA 4.0, commercial use explicitly permitted up to
    // 100 requests/minute with attribution - see
    // flyitalyadsb.com/api-documentation. Free key issued instantly by
    // email; sent as X-Api-Key below. Its dist parameter is kilometres, not
    // nautical miles, unlike every other provider here.
    if (!flyItalyApiKey.length()) {
      finishFeedAttempt("API key required");
      status("KEY", rgb(245,30,35));
      present();
      return;
    }
    const String distanceKm = String(queryRadiusNm * 1.852f, 1);
    url = "https://api.flyitalyadsb.com/v2/lat/" + latitude + "/lon/" + longitude + "/dist/" + distanceKm;
  } else {
    finishFeedAttempt("Unknown provider");
    status("FEED", rgb(245,30,35));
    present();
    return;
  }

  bool aircraftAtZeroMiles = false;
  int responseCode = 0;
  // Keep the large provider response scoped so its String and JSON allocations
  // are released before the optional TLS route-enrichment requests.
  {
  JsonDocument filter;
  JsonObject aircraftFilter = filter["ac"][0].to<JsonObject>();
  const char *fields[] = {"lat", "lon", "track", "true_heading", "mag_heading",
                          "alt_baro", "alt_geom", "gs", "baro_rate", "geom_rate",
                          "seen", "rssi", "messages", "flight", "hex", "r", "t",
                          "squawk", "category", "ownOp", "cou", "emergency", "mlat",
                          // Resolved by the aggregator from the offline ICAO
                          // lists. Reading them elsewhere is not enough: a
                          // filter drops anything not named here, so these
                          // arrived and were silently discarded during
                          // parsing, which is why the panel kept showing
                          // "B38M" and no radio callsign.
                          "shape", "type_name", "telephony"};
  for (const char *field : fields) aircraftFilter[field] = true;
  // The aggregator resolves callsign->route server-side and attaches it
  // here, so this device never opens its own connection to adsbdb. A
  // filter drops anything not named, and this is a nested object rather
  // than a scalar, so it needs its own entry.
  JsonObject routeFilter = aircraftFilter["route"].to<JsonObject>();
  for (const char *field : {"origin", "destination", "origin_name",
                            "destination_name", "origin_city", "destination_city",
                            "airline"})
    routeFilter[field] = true;
  JsonDocument doc(&psramJsonAllocator);
  DeserializationError error = DeserializationError::IncompleteInput;
  int code = 0;
  size_t bodySize = 0;
  // A body that stalls partway through (see the force-close comment below)
  // is almost always a one-off transient network hiccup rather than a
  // repeatable failure - confirmed by packet capture to sometimes be plain
  // TCP packet loss on the path, not anything wrong with the request or the
  // server. Retrying once immediately, before giving up and surfacing an
  // error, recovers from exactly that case instead of making every
  // occasional dropped packet count as a failed fetch.
  for (int attempt = 0; attempt < 2; ++attempt) {
    WiFiClientSecure client;
    applyTlsPolicy(client);
    HTTPClient http;
    // HTTPClient::setTimeout(), called here before http.begin() ever connects,
    // is a no-op on this arduino-esp32 version: it only forwards to the live
    // socket once connected() is already true, and even then it lands on
    // Stream::setTimeout() - a member NetworkClientSecure's SO_RCVTIMEO/
    // SO_SNDTIMEO logic never reads. That logic is instead keyed off
    // NetworkClient's own _timeout, which only gets set once, inside
    // connect(), from HTTPClient's separate _connectTimeout (default 5000ms,
    // never previously set here). So every read on this socket has always
    // been bounded by an unconfigured 5s default rather than the timeout this
    // file believed it was setting. setConnectTimeout() below is what actually
    // reaches that value. Confirmed independent of provider (adsb.fi and
    // airplanes.live both hit the same watchdog abort mid-read), so this
    // closes a real gap even though it may not be the sole cause of a stall
    // long enough to still trip the 60s watchdog.
    http.setTimeout(9000); http.setConnectTimeout(9000);
    ++fetchPhases.attempts;
    const uint32_t connectStartedAt = millis();
    if (!http.begin(client, url)) {
      finishFeedAttempt("Connection failed");
      status("API", rgb(245,30,35));
      present();
      return;
    }
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("User-Agent", userAgent());
    if (apiProvider == "adsbx") {
      http.addHeader("X-RapidAPI-Key", rapidApiKey);
      http.addHeader("X-RapidAPI-Host", "adsbexchange-com1.p.rapidapi.com");
    } else if (apiProvider == "aggregator" && aggregatorApiKey.length()) {
      http.addHeader("Authorization", "Bearer " + aggregatorApiKey);
    } else if (apiProvider == "flyitalyadsb" && flyItalyApiKey.length()) {
      http.addHeader("X-Api-Key", flyItalyApiKey);
    }
    // readResponseBody() needs to know how the body is framed, and
    // HTTPClient only keeps response headers it was asked for in advance.
    static const char *bodyFramingHeaders[] = {"Transfer-Encoding", "Content-Encoding"};
    http.collectHeaders(bodyFramingHeaders, 2);
    code = http.GET();
    // Covers DNS, TCP connect, the TLS handshake and the response headers -
    // everything setConnectTimeout(9000) is supposed to bound.
    fetchPhases.connectMs += millis() - connectStartedAt;
    responseCode = code;
    if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
      nextFetchAt = millis() + 60000UL;
      http.end();
      finishFeedAttempt("Rate limited", code);
      status("RATE", rgb(245,30,35));
      present();
      return;
    }
    if (code != HTTP_CODE_OK) {
      Serial.printf("%s HTTP %d\n", apiProvider.c_str(), code);
      http.end();
      finishFeedAttempt("HTTP error", code);
      status("API", rgb(245,30,35));
      present();
      return;
    }

    PsramSink body;
    // Read the body on this core with our own deadline - see the readResponseBody
    // comment for why the old cross-core force-close had to go.
    const uint32_t bodyStartedAt = millis();
    int expected = -1;
    bool chunked = false;
    size_t received = 0;
    const BodyRead bodyOutcome = readResponseBody(http, body, expected, received, chunked);
    const uint32_t bodyMs = millis() - bodyStartedAt;
    fetchPhases.bodyMs += bodyMs;
    doc.clear();
    const uint32_t parseStartedAt = millis();
    error = deserializeJson(doc, body.data(), body.size(), DeserializationOption::Filter(filter));
    fetchPhases.parseMs += millis() - parseStartedAt;
    bodySize = body.size();
    http.end();
    const bool parsed = !error && doc["ac"].is<JsonArray>();
    // One line per attempt with everything needed to tell the failure modes
    // apart without guessing: how much the server said it would send, how
    // much actually arrived, how the read ended, and whether it parsed.
    Serial.printf("%s body %s: %u/%s bytes%s in %lu ms, json %s\n",
                  apiProvider.c_str(), bodyReadName(bodyOutcome),
                  static_cast<unsigned>(received),
                  expected >= 0 ? String(expected).c_str() : "?",
                  chunked ? " (chunked)" : "", static_cast<unsigned long>(bodyMs),
                  parsed ? "ok" : error.c_str());
    if (parsed) break;
    // A stalled or truncated read is a transport problem, not a bad response:
    // retry it once on a fresh connection before surfacing an error. Anything
    // else (a complete body that still won't parse) would fail identically a
    // second time, so it isn't retried.
    const bool transportFailure = bodyOutcome == BodyRead::Stalled ||
                                  bodyOutcome == BodyRead::ClosedEarly ||
                                  (expected >= 0 && received < static_cast<size_t>(expected));
    if (attempt == 0 && transportFailure) {
      Serial.println("Retrying once on a fresh connection after an incomplete body");
      continue;
    }
    finishFeedAttempt("Invalid response", code);
    status("JSON", rgb(245,30,35));
    present();
    return;
  }

  lastCount = 0;
  lastMlat = 0;
  creditsRemaining = -1;
  for (JsonObject aircraft : doc["ac"].as<JsonArray>()) {
    if (lastCount >= MAX_AIRCRAFT || aircraft["lat"].isNull() || aircraft["lon"].isNull()) continue;
    const float latitude = aircraft["lat"].as<float>();
    const float longitude = aircraft["lon"].as<float>();
    AircraftDisplay &display = latestAircraft[lastCount];
    display = AircraftDisplay{};
    display.altitudeFt = -1;
    display.geometricAltitudeFt = -1;
    display.ageSeconds = -1;
    display.signalDb = -999;
    mapPoint(latitude, longitude, display.x, display.y);
    display.latitude = latitude;
    display.longitude = longitude;
    if (!aircraft["track"].isNull()) display.track = aircraft["track"].as<float>();
    else if (!aircraft["true_heading"].isNull()) display.track = aircraft["true_heading"].as<float>();
    else display.track = aircraft["mag_heading"] | 0.0f;
    display.distanceMiles = distanceMilesFromHome(latitude, longitude);
    if (lroundf(display.distanceMiles) == 0) aircraftAtZeroMiles = true;
    JsonVariant altitude = aircraft["alt_baro"];
    if (altitude.is<int>() || altitude.is<float>() || altitude.is<double>()) display.altitudeFt = lroundf(altitude.as<float>());
    else if (!aircraft["alt_geom"].isNull()) display.altitudeFt = lroundf(aircraft["alt_geom"].as<float>());
    if (!aircraft["alt_geom"].isNull()) display.geometricAltitudeFt = lroundf(aircraft["alt_geom"].as<float>());
    display.onGround = altitude.is<const char *>() && !strcmp(altitude.as<const char *>(), "ground");
    display.speedKnots = aircraft["gs"] | 0.0f;
    if (!aircraft["baro_rate"].isNull()) display.verticalRateFpm = aircraft["baro_rate"].as<float>();
    else display.verticalRateFpm = aircraft["geom_rate"] | 0.0f;
    display.ageSeconds = aircraft["seen"] | -1.0f;
    display.signalDb = aircraft["rssi"] | -999.0f;
    display.messages = aircraft["messages"] | 0U;
    const char *flight = aircraft["flight"] | "";
    const char *hex = aircraft["hex"] | "???";
    normalizeCallsign(flight, display.flight);
    strncpy(display.hex, hex, sizeof(display.hex) - 1);
    display.hex[sizeof(display.hex) - 1] = 0;
    strncpy(display.registration, aircraft["r"] | "", sizeof(display.registration) - 1);
    strncpy(display.aircraftType, aircraft["t"] | "", sizeof(display.aircraftType) - 1);
    strncpy(display.squawk, aircraft["squawk"] | "", sizeof(display.squawk) - 1);
    strncpy(display.category, aircraft["category"] | "", sizeof(display.category) - 1);
    strncpy(display.operatorName, aircraft["ownOp"] | "", sizeof(display.operatorName) - 1);
    strncpy(display.typeName, aircraft["type_name"] | "", sizeof(display.typeName) - 1);
    strncpy(display.telephony, aircraft["telephony"] | "", sizeof(display.telephony) - 1);
    strncpy(display.country, aircraft["cou"] | "", sizeof(display.country) - 1);
    strncpy(display.emergency, aircraft["emergency"] | "none", sizeof(display.emergency) - 1);
    // Resolve the silhouette once, here, rather than on every redraw: the
    // type table is a linear scan and these icons are drawn several times a
    // second.
    // The server's silhouette when it sent one, our own guess otherwise.
    // A surface vehicle is still a surface vehicle whatever the type list
    // says, so the on-ground rule is applied over the top of either.
    const PlaneShape served = shapeFromName(aircraft["shape"] | "");
    display.iconShape = served != PlaneShape::Generic
                            ? served
                            : shapeForAircraft(display.aircraftType, display.category, display.onGround);
    if (display.onGround && display.iconShape == PlaneShape::Generic)
      display.iconShape = PlaneShape::Ground;
    adoptServerRoute(display.flight, aircraft["route"].as<JsonObject>());
    JsonArray mlatFields = aircraft["mlat"].as<JsonArray>();
    display.positionSource = !mlatFields.isNull() && mlatFields.size() ? 2 : 0;
    ++lastCount;
  }
  }
  sortAircraftByDistance();
  int routeLookups = 0;
  const uint32_t routeLoopStartedAt = millis();
  for (int i = 0; i < lastCount; ++i) {
    AircraftDisplay &display = latestAircraft[i];
    if (display.positionSource == 2) ++lastMlat;
    else {
      // Each lookup is a fresh TLS handshake to api.adsbdb.com and blocks the
      // single-threaded web server. Service pending admin requests around it
      // so the browser does not fill the listen backlog and get RST.
      if (webServerReady) webServer.handleClient();
      // Nudges the panel to resync mid-loop against PSRAM-DMA starvation -
      // see the tile-rebuild loop's comment on restartAtNextVsync() above.
      rgbpanel->restartAtNextVsync();
      // Nothing to ask: the aggregator already attached the route to this
      // aircraft, or will on a later poll once its own lookup completes.
      if (serverSuppliesRoutes) continue;
      // A prior version kept one keep-alive connection open across every
      // lookup in this loop (HTTPClient::setReuse(true)) to save handshakes.
      // Every watchdog reboot logged after switching provider away from
      // adsb.fi traced back to a hang on the very next fetch cycle's own,
      // completely unrelated connection - always right after this loop had
      // run - and persisted even after explicitly stop()-ing the reused
      // connection at the end of the batch. Whatever state that reuse left
      // behind, closing it afterwards wasn't enough to undo it. Falling back
      // to one fresh connection per lookup, the same pattern every other
      // HTTPS call in this file already uses without issue, trades a little
      // latency for not touching whatever that reuse path corrupts.
      WiFiClientSecure routeClient;
      applyTlsPolicy(routeClient);
      HTTPClient routeHttp;
      routeHttp.setTimeout(6000);
      routeHttp.setConnectTimeout(6000);
      // Per-lookup timing and the internal-heap headroom going into the
      // handshake. The recurring "PK verify failed with error 0x4290" on
      // these lookups decodes as MBEDTLS_ERR_RSA_PUBLIC_FAILED plus
      // MBEDTLS_ERR_MPI_ALLOC_FAILED - an allocation failure inside the
      // certificate signature check, not a bad certificate - so what matters
      // is how much contiguous internal RAM was free at that instant.
      const uint32_t lookupStartedAt = millis();
      const int lookupsBefore = routeLookups;
      routeForCallsign(display.flight, routeLookups, routeClient, routeHttp);
      if (routeLookups > lookupsBefore) {
        const uint32_t lookupMs = millis() - lookupStartedAt;
        fetchPhases.routeLookups = static_cast<uint8_t>(routeLookups);
        if (lookupMs > fetchPhases.routeWorstMs) fetchPhases.routeWorstMs = lookupMs;
        if (lookupMs > 2000)
          Serial.printf("Route %s took %lu ms (largestInternal=%u)\n", display.flight,
                        static_cast<unsigned long>(lookupMs),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
      }
      routeHttp.end();
      routeClient.stop();
      if (webServerReady) webServer.handleClient();
      // Nudges the panel to resync mid-loop against PSRAM-DMA starvation -
      // see the tile-rebuild loop's comment on restartAtNextVsync() above.
      rgbpanel->restartAtNextVsync();
    }
  }
  fetchPhases.routeMs = millis() - routeLoopStartedAt;
  logHeapDiagnostics("fetch-end");
  if (routeLookups > 0) {
    // A flash write disables the instruction cache while it runs, which is
    // also what starves the RGB panel's bounce-buffer refill - so this one is
    // timed both as a fetch cost and as a suspect for the frame roll.
    const uint32_t saveStartedAt = millis();
    saveRouteCacheToStorage();
    fetchPhases.routeSaveMs = millis() - saveStartedAt;
  }
  if (aircraftAtZeroMiles) beepAlert();
  lastFetchCompletedAt = millis();
  finishFeedAttempt("OK", responseCode);
  { const uint32_t renderStartedAt = millis(); renderCurrentPage();
    fetchPhases.renderMs = millis() - renderStartedAt; }
  Serial.printf("Displayed %d aircraft (%d MLAT) from %s\n", lastCount, lastMlat, apiProvider.c_str());
}

void fetchAircraft() {
  fetchPhases.reset();
  logHeapDiagnostics("fetch-start");
  const bool retriedAuth = openSkyAuthRetryPending;
  openSkyAuthRetryPending = false;
  feedRequestStartedAt = millis();
  feedStatus = "Fetching";
  feedHttpCode = 0;
  if (WiFi.status() != WL_CONNECTED) {
    finishFeedAttempt("Wi-Fi unavailable");
    status("WIFI", rgb(245,150,0)); present(); return;
  }
  if (apiProvider != "opensky") {
    fetchAdsbV2Aircraft();
    return;
  }
  const bool useOpenSkyAuthentication = openSkyClientId.length() && openSkyClientSecret.length();
  if (useOpenSkyAuthentication && !ensureAccessToken()) {
    finishFeedAttempt("Authentication failed");
    status("AUTH", rgb(245,30,35)); present(); return;
  }
  bool aircraftAtZeroMiles = false;
  // Release the large OpenSky response and JSON allocation before starting
  // the optional per-callsign HTTPS route lookups.
  {
  WiFiClientSecure client; applyTlsPolicy(client);
  HTTPClient http; http.setTimeout(7000); http.setConnectTimeout(7000);
  const float latDelta = queryRadiusNm / 60.0f;
  const float lonDelta = queryRadiusNm / max(1.0f, 60.0f * cosf(radians(homeLatitude)));
  const String statesUrl = "https://opensky-network.org/api/states/all?lamin=" +
      String(max(-85.0f, homeLatitude - latDelta), 5) + "&lomin=" +
      String(max(-180.0f, homeLongitude - lonDelta), 5) + "&lamax=" +
      String(min(85.0f, homeLatitude + latDelta), 5) + "&lomax=" +
      String(min(180.0f, homeLongitude + lonDelta), 5);
  if (!http.begin(client, statesUrl)) {
    finishFeedAttempt("Connection failed");
    status("API", rgb(245,30,35)); present(); return;
  }
  const char *trackedHeaders[] = {"X-Rate-Limit-Remaining", "X-Rate-Limit-Retry-After-Seconds"};
  http.collectHeaders(trackedHeaders, 2);
  if (useOpenSkyAuthentication) http.addHeader("Authorization", "Bearer " + bearerToken);
  http.addHeader("User-Agent", userAgent());
  http.addHeader("Accept-Encoding", "identity");
  int code=http.GET();
  if (useOpenSkyAuthentication && code == HTTP_CODE_UNAUTHORIZED && !retriedAuth) {
    http.end();
    bearerToken = "";
    // Retry on the next loop pass so this request's TLS context is destroyed
    // first. Recursing here put two mbedTLS sessions in internal RAM at once.
    openSkyAuthRetryPending = true;
    nextFetchAt = millis();
    finishFeedAttempt("Reauthenticating", code);
    status("AUTH", rgb(245,150,0));
    present();
    return;
  }
  if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
    long retrySeconds = http.header("X-Rate-Limit-Retry-After-Seconds").toInt();
    if (retrySeconds < 60) retrySeconds = 3600;
    nextFetchAt = millis() + static_cast<uint32_t>(retrySeconds) * 1000UL;
    Serial.printf("OpenSky rate limit; retry in %ld seconds\n", retrySeconds);
    http.end(); finishFeedAttempt("Rate limited", code); status("RATE",rgb(245,30,35)); present(); return;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("OpenSky HTTP %d\n",code); http.end();
    finishFeedAttempt("HTTP error", code); status("API",rgb(245,30,35)); present(); return;
  }
  String remainingHeader = http.header("X-Rate-Limit-Remaining");
  if (remainingHeader.length()) creditsRemaining = remainingHeader.toInt();
  JsonDocument doc(&psramJsonAllocator);
  PsramSink openSkyBody;
  http.writeToStream(&openSkyBody);
  DeserializationError error =
      deserializeJson(doc, openSkyBody.data(), openSkyBody.size());
  http.end();
  if (error) {
    Serial.printf("JSON %s\n",error.c_str()); finishFeedAttempt("Invalid response", code);
    status("JSON",rgb(245,30,35)); present(); return;
  }
  lastCount=0; lastMlat=0;
  for (JsonVariant item : doc["states"].as<JsonArray>()) {
    JsonArray state = item.as<JsonArray>();
    if (lastCount >= MAX_AIRCRAFT || state[5].isNull() || state[6].isNull()) continue;
    const float latitude = state[6].as<float>();
    const float longitude = state[5].as<float>();
    AircraftDisplay &display = latestAircraft[lastCount];
    display = AircraftDisplay{};
    display.altitudeFt = -1;
    display.geometricAltitudeFt = -1;
    display.ageSeconds = -1;
    display.signalDb = -999;
    mapPoint(latitude,longitude,display.x,display.y);
    display.latitude = latitude;
    display.longitude = longitude;
    display.track=state[10] | 0.0f;
    display.distanceMiles=distanceMilesFromHome(latitude,longitude);
    if (lroundf(display.distanceMiles) == 0) aircraftAtZeroMiles = true;
    if (!state[7].isNull()) display.altitudeFt=lroundf(state[7].as<float>() * 3.28084f);
    else if (!state[13].isNull()) display.altitudeFt=lroundf(state[13].as<float>() * 3.28084f);
    if (!state[13].isNull()) display.geometricAltitudeFt=lroundf(state[13].as<float>() * 3.28084f);
    display.onGround = state[8] | false;
    display.speedKnots = state[9].isNull() ? 0.0f : state[9].as<float>() * 1.943844f;
    display.verticalRateFpm = state[11].isNull() ? 0.0f : state[11].as<float>() * 196.8504f;
    const uint64_t serverTime = doc["time"] | 0ULL;
    const uint64_t lastContact = state[4] | 0ULL;
    if (serverTime && lastContact) display.ageSeconds = max(0.0, static_cast<double>(serverTime) - static_cast<double>(lastContact));
    const char *flight=state[1] | ""; const char *hex=state[0] | "???";
    display.positionSource=state[16] | -1;
    normalizeCallsign(flight, display.flight);
    strncpy(display.hex, hex, sizeof(display.hex)-1);
    display.hex[sizeof(display.hex)-1] = 0;
    strncpy(display.country, state[2] | "", sizeof(display.country) - 1);
    if (!state[14].isNull()) {
      const char *squawk = state[14] | "";
      strncpy(display.squawk, squawk, sizeof(display.squawk) - 1);
    }
    if (!state[17].isNull()) openSkyCategoryToAdsb(state[17].as<int>(), display.category,
                                                   sizeof(display.category));
    strcpy(display.emergency, "none");
    display.iconShape = shapeForAircraft(display.aircraftType, display.category, display.onGround);
    ++lastCount;
  }
  sortAircraftByDistance();
  doc.clear();
  }
  int routeLookups = 0;
  const uint32_t routeLoopStartedAt = millis();
  for (int i=0; i<lastCount; ++i) {
    AircraftDisplay &display = latestAircraft[i];
    if (display.positionSource == 2) {
      ++lastMlat;
    } else {
      if (webServerReady) webServer.handleClient();
      // Nudges the panel to resync mid-loop against PSRAM-DMA starvation -
      // see the tile-rebuild loop's comment on restartAtNextVsync() above.
      rgbpanel->restartAtNextVsync();
      // Nothing to ask: the aggregator already attached the route to this
      // aircraft, or will on a later poll once its own lookup completes.
      if (serverSuppliesRoutes) continue;
      // A prior version kept one keep-alive connection open across every
      // lookup in this loop (HTTPClient::setReuse(true)) to save handshakes.
      // Every watchdog reboot logged after switching provider away from
      // adsb.fi traced back to a hang on the very next fetch cycle's own,
      // completely unrelated connection - always right after this loop had
      // run - and persisted even after explicitly stop()-ing the reused
      // connection at the end of the batch. Whatever state that reuse left
      // behind, closing it afterwards wasn't enough to undo it. Falling back
      // to one fresh connection per lookup, the same pattern every other
      // HTTPS call in this file already uses without issue, trades a little
      // latency for not touching whatever that reuse path corrupts.
      WiFiClientSecure routeClient;
      applyTlsPolicy(routeClient);
      HTTPClient routeHttp;
      routeHttp.setTimeout(6000);
      routeHttp.setConnectTimeout(6000);
      // Per-lookup timing and the internal-heap headroom going into the
      // handshake. The recurring "PK verify failed with error 0x4290" on
      // these lookups decodes as MBEDTLS_ERR_RSA_PUBLIC_FAILED plus
      // MBEDTLS_ERR_MPI_ALLOC_FAILED - an allocation failure inside the
      // certificate signature check, not a bad certificate - so what matters
      // is how much contiguous internal RAM was free at that instant.
      const uint32_t lookupStartedAt = millis();
      const int lookupsBefore = routeLookups;
      routeForCallsign(display.flight, routeLookups, routeClient, routeHttp);
      if (routeLookups > lookupsBefore) {
        const uint32_t lookupMs = millis() - lookupStartedAt;
        fetchPhases.routeLookups = static_cast<uint8_t>(routeLookups);
        if (lookupMs > fetchPhases.routeWorstMs) fetchPhases.routeWorstMs = lookupMs;
        if (lookupMs > 2000)
          Serial.printf("Route %s took %lu ms (largestInternal=%u)\n", display.flight,
                        static_cast<unsigned long>(lookupMs),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
      }
      routeHttp.end();
      routeClient.stop();
      if (webServerReady) webServer.handleClient();
      // Nudges the panel to resync mid-loop against PSRAM-DMA starvation -
      // see the tile-rebuild loop's comment on restartAtNextVsync() above.
      rgbpanel->restartAtNextVsync();
    }
  }
  fetchPhases.routeMs = millis() - routeLoopStartedAt;
  logHeapDiagnostics("fetch-end");
  if (routeLookups > 0) {
    // A flash write disables the instruction cache while it runs, which is
    // also what starves the RGB panel's bounce-buffer refill - so this one is
    // timed both as a fetch cost and as a suspect for the frame roll.
    const uint32_t saveStartedAt = millis();
    saveRouteCacheToStorage();
    fetchPhases.routeSaveMs = millis() - saveStartedAt;
  }
  if (aircraftAtZeroMiles) beepAlert();
  lastFetchCompletedAt = millis();
  finishFeedAttempt("OK", HTTP_CODE_OK);
  { const uint32_t renderStartedAt = millis(); renderCurrentPage();
    fetchPhases.renderMs = millis() - renderStartedAt; }
  Serial.printf("Displayed %d aircraft (%d MLAT), OpenSky credits remaining: %ld\n",lastCount,lastMlat,creditsRemaining);
}

int compareVersions(const String &leftValue, const String &rightValue) {
  String left = leftValue;
  String right = rightValue;
  if (left.startsWith("v") || left.startsWith("V")) left.remove(0, 1);
  if (right.startsWith("v") || right.startsWith("V")) right.remove(0, 1);
  for (int part = 0; part < 3; ++part) {
    const int leftDot = left.indexOf('.');
    const int rightDot = right.indexOf('.');
    String leftSegment = leftDot < 0 ? left : left.substring(0, leftDot);
    String rightSegment = rightDot < 0 ? right : right.substring(0, rightDot);
    // Strip any pre-release suffix ("0-rc1") so it cannot silently read as 0
    // and make a release candidate compare equal to the final release.
    const int leftDash = leftSegment.indexOf('-');
    const int rightDash = rightSegment.indexOf('-');
    const bool leftPre = leftDash >= 0;
    const bool rightPre = rightDash >= 0;
    if (leftPre) leftSegment = leftSegment.substring(0, leftDash);
    if (rightPre) rightSegment = rightSegment.substring(0, rightDash);
    const int leftPart = leftSegment.toInt();
    const int rightPart = rightSegment.toInt();
    if (leftPart == rightPart && leftPre != rightPre) return leftPre ? -1 : 1;
    if (leftPart != rightPart) return leftPart < rightPart ? -1 : 1;
    left = leftDot < 0 ? "" : left.substring(leftDot + 1);
    right = rightDot < 0 ? "" : right.substring(rightDot + 1);
  }
  return 0;
}

// Parses "<64 hex>  <filename>" lines from the release SHA256SUMS.txt asset.
String fetchPublishedSha256(const String &url, const String &wantedName) {
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(12000); http.setConnectTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return "";
  http.addHeader("User-Agent", userAgent());
  String digest;
  if (http.GET() == HTTP_CODE_OK) {
    const String body = http.getString();
    int start = 0;
    while (start < static_cast<int>(body.length())) {
      int end = body.indexOf('\n', start);
      if (end < 0) end = body.length();
      String line = body.substring(start, end);
      line.trim();
      const int gap = line.indexOf(' ');
      if (gap == 64 && line.indexOf(wantedName) > gap) {
        digest = line.substring(0, 64);
        digest.toLowerCase();
        break;
      }
      start = end + 1;
    }
  }
  http.end();
  return digest;
}

bool checkGithubUpdate() {
  githubUpdateStatus = "Checking GitHub";
  githubUpdateAvailable = false;
  githubFirmwareUrl = "";
  githubSignatureUrl = "";
  githubFirmwareSha256 = "";
  githubFirmwareSize = 0;
  if (WiFi.status() != WL_CONNECTED) {
    githubUpdateStatus = "Wi-Fi unavailable";
    return false;
  }
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(12000); http.setConnectTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, GITHUB_RELEASE_API)) {
    githubUpdateStatus = "GitHub connection failed";
    return false;
  }
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.addHeader("User-Agent", userAgent());
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    githubUpdateStatus = "GitHub HTTP " + String(code);
    http.end();
    return false;
  }
  JsonDocument doc;
  PsramSink releaseBody;
  http.writeToStream(&releaseBody);
  const DeserializationError error =
      deserializeJson(doc, releaseBody.data(), releaseBody.size());
  http.end();
  if (error) {
    githubUpdateStatus = "Invalid GitHub response";
    return false;
  }
  githubLatestVersion = doc["tag_name"] | "";
  String checksumsUrl;
  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    String name = asset["name"] | "";
    name.toLowerCase();
    if (name == "sha256sums.txt") {
      checksumsUrl = asset["browser_download_url"] | "";
    } else if (name.endsWith(".bin.sig") && name.indexOf("firmware") >= 0) {
      githubSignatureUrl = asset["browser_download_url"] | "";
    } else if (name.endsWith(".bin") && (name.indexOf("firmware") >= 0 || name.indexOf("adsb-map") >= 0)) {
      githubFirmwareUrl = asset["browser_download_url"] | "";
      githubFirmwareSize = asset["size"] | 0U;
      githubFirmwareSha256 = asset["digest"] | "";
      if (githubFirmwareSha256.startsWith("sha256:")) githubFirmwareSha256.remove(0, 7);
      githubFirmwareSha256.toLowerCase();
    }
  }
  if (!githubLatestVersion.length()) {
    githubUpdateStatus = "Release has no version";
    return false;
  }
  if (compareVersions(FIRMWARE_VERSION, githubLatestVersion) >= 0) {
    githubUpdateStatus = "Firmware is current";
    return true;
  }
  if (!githubFirmwareUrl.length()) {
    githubUpdateStatus = "Release has no firmware binary";
    return false;
  }
  // The GitHub API digest field is not guaranteed. SHA256SUMS.txt is published
  // in every release, so fall back to it rather than blocking updates.
  if (githubFirmwareSha256.length() != 64 && checksumsUrl.length()) {
    githubFirmwareSha256 = fetchPublishedSha256(checksumsUrl, "ESP32-ADSB-firmware.bin");
  }
  if (githubFirmwareSize < 1024 || githubFirmwareSha256.length() != 64 ||
      !githubSignatureUrl.length()) {
    githubUpdateStatus = "Release integrity metadata is missing";
    return false;
  }
  githubUpdateAvailable = true;
  githubUpdateStatus = "Version " + githubLatestVersion + " available";
  Serial.printf("GitHub firmware update available: %s\n", githubLatestVersion.c_str());
  return true;
}

bool downloadGithubUpdateToSd() {
  if (!sdMounted) return false;
  SDCARD.remove(SD_UPDATE_PART);
  File file = SDCARD.open(SD_UPDATE_PART, FILE_WRITE);
  if (!file) {
    githubUpdateStatus = "Unable to create SD staging file";
    return false;
  }
  githubUpdateStatus = "Downloading " + githubLatestVersion + " to SD";
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(15000); http.setConnectTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, githubFirmwareUrl)) {
    file.close();
    SDCARD.remove(SD_UPDATE_PART);
    githubUpdateStatus = "Firmware download failed";
    return false;
  }
  http.addHeader("User-Agent", userAgent());
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    file.close();
    SDCARD.remove(SD_UPDATE_PART);
    githubUpdateStatus = "Firmware HTTP " + String(code);
    return false;
  }
  NetworkClient *stream = http.getStreamPtr();
  uint8_t buffer[4096];
  size_t total = 0;
  uint32_t lastDataAt = millis();
  bool validHeader = false;
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool hashOk = mbedtls_sha256_starts(&context, 0) == 0;
  while (hashOk && (http.connected() || stream->available()) && total < githubFirmwareSize) {
    const size_t available = stream->available();
    if (!available) {
      if (millis() - lastDataAt > 15000) break;
      delay(2);
      continue;
    }
    const size_t count = stream->readBytes(buffer, min(available, sizeof(buffer)));
    if (!count) continue;
    if (!total) validHeader = buffer[0] == 0xE9;
    if (!validHeader || file.write(buffer, count) != count ||
        mbedtls_sha256_update(&context, buffer, count) != 0) break;
    total += count;
    lastDataAt = millis();
    delay(0);
  }
  uint8_t hash[32];
  hashOk = hashOk && mbedtls_sha256_finish(&context, hash) == 0;
  mbedtls_sha256_free(&context);
  http.end();
  file.flush();
  file.close();
  const String actualDigest = hashOk ? bytesToHex(hash, sizeof(hash)) : "";
  if (!validHeader || total != githubFirmwareSize || actualDigest != githubFirmwareSha256 ||
      !verifyFirmwareSignature(actualDigest)) {
    SDCARD.remove(SD_UPDATE_PART);
    githubUpdateStatus = !validHeader ? "Downloaded file is not ESP32 firmware" :
                         total != githubFirmwareSize ? "Firmware download was incomplete" :
                         actualDigest != githubFirmwareSha256 ? "Firmware SHA-256 check failed" :
                         "Firmware release signature check failed";
    return false;
  }
  SDCARD.remove(SD_UPDATE_FILE);
  if (!SDCARD.rename(SD_UPDATE_PART, SD_UPDATE_FILE)) {
    SDCARD.remove(SD_UPDATE_PART);
    githubUpdateStatus = "Unable to finalise SD staging file";
    return false;
  }
  stagedUpdateReady = true;
  stagedUpdateVersion = githubLatestVersion;
  stagedUpdateSha256 = actualDigest;
  stagedUpdateSize = githubFirmwareSize;
  settingsStore.putString("staged-ver", stagedUpdateVersion);
  settingsStore.putString("staged-sha", stagedUpdateSha256);
  settingsStore.putULong("staged-size", stagedUpdateSize);
  sdUsedBytes = SDCARD.usedBytes();
  githubUpdateStatus = "Version " + githubLatestVersion + " verified on SD";
  return true;
}

void discardStagedUpdate() {
  if (sdMounted) SDCARD.remove(SD_UPDATE_FILE);
  stagedUpdateReady = false;
  stagedUpdateVersion = "";
  stagedUpdateSha256 = "";
  stagedUpdateSize = 0;
  settingsStore.remove("staged-ver");
  settingsStore.remove("staged-sha");
  settingsStore.remove("staged-size");
}

bool installStagedUpdate() {
  if (!sdMounted || !stagedUpdateReady) return false;
  // Validate against the persisted metadata, not against RAM that is empty
  // after a reboot. Previously this always failed and leaked the staged image.
  const size_t expectedSize = stagedUpdateSize;
  const String expectedDigest = stagedUpdateSha256;
  if (!expectedSize || expectedDigest.length() != 64) {
    discardStagedUpdate();
    githubUpdateStatus = "Staged firmware metadata is missing";
    return false;
  }
  File file = SDCARD.open(SD_UPDATE_FILE, FILE_READ);
  if (!file || file.size() != expectedSize || file.read() != 0xE9 || !file.seek(0)) {
    if (file) file.close();
    discardStagedUpdate();
    githubUpdateStatus = "Staged firmware is invalid";
    return false;
  }
  file.close();
  String digest;
  if (!sha256File(SDCARD, SD_UPDATE_FILE, digest) || digest != expectedDigest ||
      !verifyFirmwareSignature(digest)) {
    discardStagedUpdate();
    githubUpdateStatus = "Staged firmware SHA-256 check failed";
    return false;
  }
  file = SDCARD.open(SD_UPDATE_FILE, FILE_READ);
  if (!file || !Update.begin(expectedSize, U_FLASH)) {
    if (file) file.close();
    githubUpdateStatus = Update.errorString();
    return false;
  }
  githubUpdateStatus = "Installing verified SD update";
  const size_t written = Update.writeStream(file);
  file.close();
  if (written != expectedSize || !Update.end(false)) {
    githubUpdateStatus = written != expectedSize ? "SD firmware write was incomplete" : Update.errorString();
    Update.abort();
    return false;
  }
  githubUpdateStatus = "Update installed; rebooting";
  delay(300);
  ESP.restart();
  return true;
}

bool installGithubUpdate() {
  if (!githubUpdateAvailable || !githubFirmwareUrl.length()) {
    githubUpdateStatus = "No update is ready";
    return false;
  }
  githubUpdateStatus = "Verifying release signature";
  if (!downloadFirmwareSignature()) {
    githubUpdateStatus = "Firmware release signature is unavailable";
    return false;
  }
  if (sdMounted) {
    if (!downloadGithubUpdateToSd()) return false;
    return installStagedUpdate();
  }
  githubUpdateStatus = "Downloading " + githubLatestVersion;
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(15000); http.setConnectTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, githubFirmwareUrl)) {
    githubUpdateStatus = "Firmware download failed";
    return false;
  }
  http.addHeader("User-Agent", userAgent());
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    githubUpdateStatus = "Firmware HTTP " + String(code);
    http.end();
    return false;
  }
  const int expected = http.getSize();
  if (!Update.begin(expected > 0 ? expected : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    githubUpdateStatus = Update.errorString();
    http.end();
    return false;
  }
  NetworkClient *stream = http.getStreamPtr();
  uint8_t buffer[4096];
  size_t total = 0;
  uint32_t lastDataAt = millis();
  bool validHeader = false;
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool hashOk = mbedtls_sha256_starts(&context, 0) == 0;
  while ((http.connected() || stream->available()) &&
         (expected < 0 || total < static_cast<size_t>(expected)) && hashOk) {
    const size_t available = stream->available();
    if (available) {
      const size_t count = stream->readBytes(buffer, min(available, sizeof(buffer)));
      if (!count) continue;
      if (total == 0) {
        validHeader = buffer[0] == 0xE9;
        if (!validHeader) break;
      }
      if (mbedtls_sha256_update(&context, buffer, count) != 0) {
        hashOk = false;
        break;
      }
      if (Update.write(buffer, count) != count) break;
      total += count;
      lastDataAt = millis();
    } else {
      if (millis() - lastDataAt > 15000) break;
      delay(2);
    }
  }
  uint8_t hash[32];
  hashOk = hashOk && mbedtls_sha256_finish(&context, hash) == 0;
  mbedtls_sha256_free(&context);
  http.end();
  const String actualDigest = hashOk ? bytesToHex(hash, sizeof(hash)) : "";
  String installError;
  if (!validHeader) {
    installError = "Downloaded file is not ESP32 firmware";
  } else if (expected > 0 && total != static_cast<size_t>(expected)) {
    installError = "Firmware download was incomplete";
  } else if (githubFirmwareSize && total != githubFirmwareSize) {
    installError = "Firmware size does not match release metadata";
  } else if (!hashOk || actualDigest != githubFirmwareSha256) {
    installError = "Firmware SHA-256 check failed";
  } else if (!verifyFirmwareSignature(actualDigest)) {
    installError = "Firmware release signature check failed";
  } else if (!Update.end(false)) {
    installError = Update.errorString();
  }
  if (installError.length()) {
    Update.abort();
    githubUpdateStatus = installError;
    return false;
  }
  githubUpdateStatus = "Update installed; rebooting";
  delay(300);
  ESP.restart();
  return true;
}

void sendJson(int statusCode, const String &payload) {
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(statusCode, "application/json", payload);
}

void sendJsonDocument(int statusCode, const JsonDocument &doc) {
  const size_t length = measureJson(doc);
  char *payload = static_cast<char *>(
      heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!payload) {
    String fallback;
    serializeJson(doc, fallback);
    sendJson(statusCode, fallback);
    return;
  }
  serializeJson(doc, payload, length + 1);
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.setContentLength(length);
  webServer.send(statusCode, "application/json", "");
  webServer.client().write(reinterpret_cast<const uint8_t *>(payload), length);
  heap_caps_free(payload);
}

// Every state-changing route requires this token in an X-ADSB-Token header.
// A cross-origin page cannot read /api/status to obtain it, and cannot set a
// custom header without a preflight this server does not answer, so cached
// Basic credentials alone are no longer enough to drive the device.
void generateCsrfToken() {
  char buffer[33];
  for (int i = 0; i < 4; ++i) snprintf(buffer + i * 8, 9, "%08x", esp_random());
  csrfToken = buffer;
}

bool requireWebAuthentication() {
  if (webServer.authenticate(WEB_USERNAME, managementPassword.c_str())) return true;
  webServer.requestAuthentication(BASIC_AUTH, "ADSB Map Control");
  return false;
}

bool requireCsrfToken() {
  if (csrfToken.length() && webServer.header("X-ADSB-Token") == csrfToken) return true;
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(403, "application/json",
                 "{\"message\":\"Request token missing or stale; reload the admin page\"}");
  return false;
}

void sendMessage(int statusCode, const char *message) {
  JsonDocument doc;
  doc["message"] = message;
  String payload;
  serializeJson(doc, payload);
  sendJson(statusCode, payload);
}

void handleStatusApi() {
  if (!requireWebAuthentication()) return;
  JsonDocument doc;
  doc["aircraftTotal"] = lastCount;
  doc["adsb"] = lastCount - lastMlat;
  doc["mlat"] = lastMlat;
  doc["credits"] = creditsRemaining;
  doc["lastRefreshSeconds"] = lastFetchCompletedAt ?
      static_cast<int32_t>((millis() - lastFetchCompletedAt) / 1000UL) : -1;
  doc["feedStatus"] = feedStatus;
  doc["feedHttpCode"] = feedHttpCode;
  doc["feedDurationMs"] = feedRequestDurationMs;
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["uptimeSeconds"] = millis() / 1000UL;
  doc["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  doc["hostname"] = DEVICE_HOSTNAME;
  doc["provider"] = apiProvider;
  doc["hasOpenSkyClientId"] = openSkyClientId.length() > 0;
  doc["hasOpenSkyClientSecret"] = openSkyClientSecret.length() > 0;
  doc["hasRapidApiKey"] = rapidApiKey.length() > 0;
  doc["hasAggregatorApiKey"] = aggregatorApiKey.length() > 0;
  doc["hasFlyItalyApiKey"] = flyItalyApiKey.length() > 0;
  doc["version"] = FIRMWARE_VERSION;
  doc["build"] = String(__DATE__) + " " + __TIME__;
  doc["updateSpace"] = ESP.getFreeSketchSpace();
  doc["heapFree"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  doc["heapMinimum"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  doc["heapLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  doc["loopStackMinimumFree"] = uxTaskGetStackHighWaterMark(nullptr);
  doc["psramFree"] = ESP.getFreePsram();
  doc["psramMinimum"] = ESP.getMinFreePsram();
  doc["psramLargest"] = ESP.getMaxAllocPsram();
  doc["temperatureC"] = temperatureRead();
  doc["aircraftCapacity"] = MAX_AIRCRAFT;
  doc["aircraftStorage"] = "PSRAM";
  doc["marineTrackingEnabled"] = marineTrackingEnabled;
  doc["marineProvider"] = marineProvider;
  doc["aisConfigured"] = marineConfigured();
  doc["hasAisApiKey"] = aisApiKey.length() > 0;
  doc["hasAisHubUsername"] = aisHubUsername.length() > 0;
  doc["hasMyShipTrackingApiKey"] = myShipTrackingApiKey.length() > 0;
  doc["hasDatalasticApiKey"] = datalasticApiKey.length() > 0;
  doc["aisConnected"] = aisConnected;
  doc["vesselCount"] = vesselCount;
  doc["vesselCapacity"] = MAX_VESSELS;
  doc["marineRadiusNm"] = marineRadiusNm;
  doc["aisLastMessageSeconds"] = aisLastMessageAt ?
      static_cast<int32_t>((millis() - aisLastMessageAt) / 1000UL) : -1;
  doc["sdMounted"] = sdMounted;
  doc["sdStatus"] = sdStatus;
  doc["sdType"] = sdCardType;
  doc["sdTotalBytes"] = sdTotalBytes;
  doc["sdUsedBytes"] = sdUsedBytes;
  doc["sdFreeBytes"] = sdTotalBytes >= sdUsedBytes ? sdTotalBytes - sdUsedBytes : 0;
  doc["tileCacheStorage"] = sdMounted ? "SD card" : "LittleFS";
  doc["stagedUpdateReady"] = stagedUpdateReady;
  doc["stagedUpdateVersion"] = stagedUpdateVersion;
  doc["brightness"] = brightnessPercent;
  doc["sound"] = soundAlerts;
  doc["screensaverEnabled"] = screensaverEnabled;
  doc["screensaverIdleMinutes"] = screensaverIdleMinutes;
  doc["pclkKhz"] = panelPclkHz / 1000UL;
  doc["pclkRefreshHz"] = roundf(panelRefreshHz(panelPclkHz) * 10.0f) / 10.0f;
  doc["bounceLines"] = panelBounceLines;
  doc["bounceKb"] = 2UL * panelBounceLines * W * 2UL / 1024UL;
  doc["directDraw"] = panelDirectDraw;
  doc["screensaverActive"] = screensaverActive;
  doc["page"] = displayPageName();
  doc["latitude"] = homeLatitude;
  doc["longitude"] = homeLongitude;
  doc["radiusNm"] = queryRadiusNm;
  doc["mapZoom"] = physicalMapZoom;
  doc["updateAvailable"] = githubUpdateAvailable;
  doc["latestVersion"] = githubLatestVersion;
  doc["updateStatus"] = githubUpdateStatus;
  doc["releaseRepository"] = String(GITHUB_OWNER) + "/" + GITHUB_REPOSITORY;
  doc["csrfToken"] = csrfToken;
  doc["mapRebuildActive"] = mapRebuildActive;
  doc["mapRebuildDone"] = mapRebuildDone;
  doc["mapRebuildTotal"] = mapRebuildTotal;
  sendJsonDocument(200, doc);
}

void handleAircraftApi() {
  if (!requireWebAuthentication()) return;
  JsonDocument doc(&psramJsonAllocator);
  JsonArray aircraft = doc["aircraft"].to<JsonArray>();
  for (int i = 0; i < lastCount; ++i) {
    const AircraftDisplay &display = latestAircraft[i];
    JsonObject item = aircraft.add<JsonObject>();
    item["hex"] = display.hex;
    item["callsign"] = display.flight;
    item["latitude"] = display.latitude;
    item["longitude"] = display.longitude;
    item["distance"] = roundf(display.distanceMiles * 10.0f) / 10.0f;
    item["altitude"] = display.altitudeFt;
    item["geometricAltitude"] = display.geometricAltitudeFt;
    item["speed"] = roundf(display.speedKnots * 10.0f) / 10.0f;
    item["verticalRate"] = lroundf(display.verticalRateFpm);
    item["heading"] = roundf(display.track * 10.0f) / 10.0f;
    item["direction"] = compassDirection(display.track);
    item["source"] = display.positionSource == 2 ? "MLAT" : "ADSB";
    item["registration"] = display.registration;
    item["aircraftType"] = display.aircraftType;
    // Resolved by the aggregator from the ICAO lists: the full model name and
    // the radio callsign. Empty on any other provider, which is why the
    // browser falls back to the designator rather than showing a blank.
    item["typeName"] = display.typeName;
    item["telephony"] = display.telephony;
    item["squawk"] = display.squawk;
    item["category"] = display.category;
    // The browser map draws the same silhouette and colour as the panel, and
    // both come from here rather than being worked out twice: duplicating the
    // type-designator table into JavaScript would be a second copy to keep in
    // step with this one, and it would drift.
    item["shape"] = planeShapeName(display.iconShape);
    const uint16_t iconColour = aircraftIconColour(display);
    char iconColourHex[8];
    snprintf(iconColourHex, sizeof(iconColourHex), "#%02X%02X%02X",
             static_cast<unsigned>((iconColour >> 11) & 0x1F) * 255 / 31,
             static_cast<unsigned>((iconColour >> 5) & 0x3F) * 255 / 63,
             static_cast<unsigned>(iconColour & 0x1F) * 255 / 31);
    item["iconColour"] = iconColourHex;
    item["operator"] = display.operatorName;
    item["country"] = display.country;
    item["emergency"] = display.emergency;
    item["onGround"] = display.onGround;
    item["age"] = display.ageSeconds < 0 ? -1 : roundf(display.ageSeconds * 10.0f) / 10.0f;
    item["messages"] = display.messages;
    item["signal"] = display.signalDb;
    RouteCacheEntry *route = cachedRoute(display.flight);
    if (route && route->hasRoute) {
      item["route"] = String(route->origin) + ">" + route->destination;
      item["routeFull"] = String(route->originName[0] ? route->originName : route->origin) +
                           " -> " + (route->destinationName[0] ? route->destinationName : route->destination);
    } else {
      item["route"] = "";
      item["routeFull"] = "";
    }
  }
  sendJsonDocument(200, doc);
}

void handleWifiScanStart() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  const int state = WiFi.scanComplete();
  if (state == WIFI_SCAN_RUNNING) {
    sendMessage(202, "Wi-Fi scan already running");
    return;
  }
  WiFi.scanDelete();
  if (WiFi.scanNetworks(true, true) == WIFI_SCAN_FAILED) {
    sendMessage(500, "Unable to start Wi-Fi scan");
    return;
  }
  sendMessage(202, "Wi-Fi scan started");
}

void handleWifiScanResults() {
  if (!requireWebAuthentication()) return;
  const int count = WiFi.scanComplete();
  JsonDocument doc;
  if (count == WIFI_SCAN_RUNNING) {
    doc["complete"] = false;
  } else if (count == WIFI_SCAN_FAILED) {
    WiFi.scanDelete();
    sendMessage(500, "Wi-Fi scan failed");
    return;
  } else {
    doc["complete"] = true;
    JsonArray networks = doc["networks"].to<JsonArray>();
    if (count > 0) {
      for (int i = 0; i < count; ++i) {
        const String ssid = WiFi.SSID(i);
        JsonObject network;
        for (JsonObject existing : networks) {
          if (existing["ssid"].as<String>() == ssid) {
            network = existing;
            break;
          }
        }
        if (network.isNull()) {
          network = networks.add<JsonObject>();
          network["ssid"] = ssid;
          network["rssi"] = WiFi.RSSI(i);
          network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        } else if (WiFi.RSSI(i) > network["rssi"].as<int>()) {
          network["rssi"] = WiFi.RSSI(i);
          network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
      }
    }
    WiFi.scanDelete();
  }
  String payload;
  serializeJson(doc, payload);
  sendJson(200, payload);
}

void handleWifiConnect() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  const String ssid = webServer.arg("ssid");
  const String password = webServer.arg("password");
  if (!ssid.length() || ssid.length() > 32 || !validWifiPassword(password)) {
    sendMessage(400, "Use an SSID of 1 to 32 bytes and an empty, 8 to 63 character, or 64-digit hexadecimal password");
    return;
  }
  // Keep the working credentials so a bad SSID or password does not strand the
  // receiver off the LAN until someone power-cycles it into the setup portal.
  previousWifiSsid = WiFi.SSID();
  previousWifiPassword = WiFi.psk();
  wifiRollbackAt = millis() + 45000UL;
  sendMessage(202, "Wi-Fi connection started; reconnect to the device at its new address");
  delay(120);
  WiFi.begin(ssid.c_str(), password.c_str());
}

void handleWifiSavedNetworks() {
  if (!requireWebAuthentication()) return;
  JsonDocument doc;
  savedNetworkNames(doc["saved"].to<JsonArray>());
  doc["max"] = MAX_SAVED_NETWORKS;
  doc["current"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  String payload;
  serializeJson(doc, payload);
  sendJson(200, payload);
}

void handleWifiForgetNetwork() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  const String ssid = webServer.arg("ssid");
  if (!ssid.length()) {
    sendMessage(400, "Name the network to forget");
    return;
  }
  if (!forgetWifiNetwork(ssid)) {
    sendMessage(404, "That network is not saved");
    return;
  }
  // Deliberately does not disconnect. Forgetting the network the receiver
  // is currently on should stop it being rejoined automatically later, not
  // drop the connection the person is using to say so.
  sendMessage(200, "Network forgotten");
}

void handlePageControl() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  const String page = webServer.arg("page");
  if (page != "overview" && page != "map" && page != "table" && page != "radar" && page != "marine") {
    sendMessage(400, "Page must be overview, map, radar, table or marine");
    return;
  }
  displayPage = page == "overview" ? DisplayPage::Overview :
                page == "table" ? DisplayPage::Table :
                page == "radar" ? DisplayPage::Radar :
                page == "marine" ? DisplayPage::Marine : DisplayPage::Map;
  settingsStore.putUChar("display-page", static_cast<uint8_t>(displayPage));
  screensaverActive = false;
  lastInteractionAt = millis();
  { MutexGuard guard(dataMutex); renderCurrentPage(); }
  if (displayPage == DisplayPage::Overview) sendMessage(200, "Overview page selected");
  else if (displayPage == DisplayPage::Table) sendMessage(200, "Table page selected");
  else if (displayPage == DisplayPage::Radar) sendMessage(200, "Radar page selected");
  else if (displayPage == DisplayPage::Marine) sendMessage(200, "Marine page selected");
  else sendMessage(200, "Map page selected");
}

void handleDisplaySettings() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  if (webServer.hasArg("sound")) {
    soundAlerts = webServer.arg("sound") == "1";
    settingsStore.putBool("sound", soundAlerts);
  }
  if (webServer.hasArg("screensaverEnabled")) {
    screensaverEnabled = webServer.arg("screensaverEnabled") == "1";
    settingsStore.putBool("ssaver-on", screensaverEnabled);
    // Turning it off (or back on, resetting the clock) shouldn't leave the
    // panel showing a screensaver from before the setting changed.
    lastInteractionAt = millis();
    if (!screensaverEnabled && screensaverActive) {
      screensaverActive = false;
      MutexGuard guard(dataMutex);
      renderCurrentPage();
    }
  }
  if (webServer.hasArg("screensaverIdleMinutes")) {
    long minutes = 0;
    if (!parseStrictLong(webServer.arg("screensaverIdleMinutes"), minutes) || minutes < 1 || minutes > 120) {
      sendMessage(400, "Screensaver idle time must be 1 to 120 minutes");
      return;
    }
    screensaverIdleMinutes = static_cast<uint16_t>(minutes);
    settingsStore.putUShort("ssaver-min", screensaverIdleMinutes);
    lastInteractionAt = millis();
  }
  if (webServer.hasArg("directDraw")) {
    settingsStore.putBool("direct-draw", webServer.arg("directDraw") == "1");
    // The framebuffer pointer is chosen once at boot, so this needs a
    // restart like the other two panel settings.
    restartPending = true;
    restartAt = millis() + 1500;
    sendMessage(202, "Rendering mode saved - rebooting to apply");
    return;
  }
  if (webServer.hasArg("bounceLines")) {
    long lines = 0;
    bool known = false;
    if (parseStrictLong(webServer.arg("bounceLines"), lines))
      for (uint16_t choice : PANEL_BOUNCE_CHOICES)
        if (choice == static_cast<uint16_t>(lines)) { known = true; break; }
    if (!known) {
      sendMessage(400, "Unsupported bounce buffer size");
      return;
    }
    settingsStore.putUShort("bounce-lines", static_cast<uint16_t>(lines));
    // Allocated when esp_lcd creates the panel, so like the pixel clock it
    // only takes effect on the next boot.
    restartPending = true;
    restartAt = millis() + 1500;
    sendMessage(202, "Bounce buffer saved - rebooting to apply");
    return;
  }
  if (webServer.hasArg("pclkKhz")) {
    long khz = 0;
    bool known = false;
    if (parseStrictLong(webServer.arg("pclkKhz"), khz))
      for (uint32_t choice : PANEL_PCLK_CHOICES)
        if (choice == static_cast<uint32_t>(khz) * 1000UL) { known = true; break; }
    if (!known) {
      sendMessage(400, "Unsupported pixel clock");
      return;
    }
    settingsStore.putULong("pclk-khz", static_cast<uint32_t>(khz));
    // The clock is latched when esp_lcd initialises the panel, so it cannot
    // be changed on a running display - save it and restart. Long enough a
    // delay for this response to reach the browser first.
    restartPending = true;
    restartAt = millis() + 1500;
    sendMessage(202, "Pixel clock saved - rebooting to apply");
    return;
  }
  // The brightness slider is gone: the CH422G drives the backlight enable as a
  // plain switch with no PWM channel, so any value between 10 and 100 looked
  // identical on the 800x480 boards. The backlight is still switched on at
  // boot via applyBrightness().
  sendMessage(200, "Display settings saved");
}

void handleLocationSettings() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  if (!webServer.hasArg("latitude") || !webServer.hasArg("longitude") ||
      !webServer.hasArg("radius")) {
    sendMessage(400, "Latitude, longitude and radius are required");
    return;
  }
  double latitude = 0;
  double longitude = 0;
  long radius = 0;
  long requestedZoom = 0;
  const bool hasRequestedZoom = webServer.hasArg("zoom");
  if (!parseStrictDouble(webServer.arg("latitude"), latitude) ||
      !parseStrictDouble(webServer.arg("longitude"), longitude) ||
      !parseStrictLong(webServer.arg("radius"), radius) ||
      (hasRequestedZoom && !parseStrictLong(webServer.arg("zoom"), requestedZoom)) ||
      latitude < -85.0 || latitude > 85.0 ||
      longitude < -180.0 || longitude > 180.0 ||
      radius < 5 || radius > 250 ||
      (hasRequestedZoom && (requestedZoom < 3 || requestedZoom > 16))) {
    sendMessage(400, "Use latitude -85 to 85, longitude -180 to 180, radius 5 to 250 nm, and zoom 3 to 16");
    return;
  }
  homeLatitude = latitude;
  homeLongitude = longitude;
  queryRadiusNm = static_cast<uint16_t>(radius);
  physicalMapZoom = hasRequestedZoom ? static_cast<uint8_t>(requestedZoom) : zoomForRadius();
  settingsStore.putFloat("home-lat", homeLatitude);
  settingsStore.putFloat("home-lon", homeLongitude);
  settingsStore.putUShort("radius-nm", queryRadiusNm);
  settingsStore.putUChar("map-zoom", physicalMapZoom);
  physicalMapReady = false;
  physicalMapRefreshPending = true;
  nextFetchAt = 0;
  clearTileCache();
  sendMessage(202, "Location saved; rebuilding both maps and refreshing aircraft");
}

void handleGithubUpdateCheck() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  githubCheckPending = true;
  sendMessage(202, "GitHub update check requested");
}

void handleGithubUpdateInstall() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  if (firmwareUploadStarted || githubInstallPending || Update.isRunning()) {
    sendMessage(409, "Another firmware operation is already in progress");
    return;
  }
  if (!githubUpdateAvailable || !githubFirmwareUrl.length()) {
    sendMessage(409, "No newer GitHub firmware release is ready");
    return;
  }
  githubInstallPending = true;
  sendMessage(202, "GitHub firmware download and installation started");
}

void handleSdRescan() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  if (githubInstallPending || Update.isRunning()) {
    sendMessage(409, "SD card cannot be rescanned during a firmware update");
    return;
  }
  if (mapRebuildActive) {
    sendMessage(409, "SD card cannot be rescanned while the LCD map is rebuilding");
    return;
  }
  const bool mounted = mountSdCard();
  // Mounting a card moves the cache prefix from /osm_ to /adsb/osm_, orphaning
  // everything already written to LittleFS.
  if (mounted) clearTileCache(true);
  physicalMapReady = false;
  physicalMapRefreshPending = true;
  sendMessage(200, mounted ? "SD card mounted; map cache moved to SD" :
                           "No readable SD card detected; LittleFS remains active");
}

void handlePasswordChange() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  const String password = webServer.arg("password");
  if (password.length() < 8 || password.length() > 63) {
    sendMessage(400, "Management password must be 8 to 63 characters");
    return;
  }
  managementPassword = password;
  settingsStore.putString("password", managementPassword);
  sendMessage(200, "Management password changed");
}

void handleProviderSettings() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  const String provider = webServer.arg("provider");
  if (provider != "opensky" && provider != "adsbfi" &&
      provider != "airplaneslive" && provider != "adsblol" &&
      provider != "adsbone" && provider != "adsbx" &&
      provider != "aggregator" && provider != "flyitalyadsb") {
    sendMessage(400, "Unknown aircraft data provider");
    return;
  }
  if (webServer.arg("clientId").length() > 128 ||
      webServer.arg("clientSecret").length() > 256 ||
      webServer.arg("rapidApiKey").length() > 256 ||
      webServer.arg("aggregatorApiKey").length() > 128 ||
      webServer.arg("flyItalyApiKey").length() > 128) {
    sendMessage(400, "API credential fields are too long");
    return;
  }
  if (webServer.arg("clear") == "1") {
    openSkyClientId = "";
    openSkyClientSecret = "";
    rapidApiKey = "";
    aggregatorApiKey = "";
    flyItalyApiKey = "";
    settingsStore.putString("os-client", "");
    settingsStore.putString("os-secret", "");
    settingsStore.putString("rapid-key", "");
    settingsStore.putString("agg-key", "");
    settingsStore.putString("flyitaly-key", "");
  } else {
    if (webServer.hasArg("clientId") && webServer.arg("clientId").length()) {
      openSkyClientId = webServer.arg("clientId");
      settingsStore.putString("os-client", openSkyClientId);
    }
    if (webServer.hasArg("clientSecret") && webServer.arg("clientSecret").length()) {
      openSkyClientSecret = webServer.arg("clientSecret");
      settingsStore.putString("os-secret", openSkyClientSecret);
    }
    if (webServer.hasArg("rapidApiKey") && webServer.arg("rapidApiKey").length()) {
      rapidApiKey = webServer.arg("rapidApiKey");
      settingsStore.putString("rapid-key", rapidApiKey);
    }
    if (webServer.hasArg("aggregatorApiKey") && webServer.arg("aggregatorApiKey").length()) {
      aggregatorApiKey = webServer.arg("aggregatorApiKey");
      settingsStore.putString("agg-key", aggregatorApiKey);
    }
    if (webServer.hasArg("flyItalyApiKey") && webServer.arg("flyItalyApiKey").length()) {
      flyItalyApiKey = webServer.arg("flyItalyApiKey");
      settingsStore.putString("flyitaly-key", flyItalyApiKey);
    }
  }
  apiProvider = provider;
  settingsStore.putString("provider", apiProvider);
  bearerToken = "";
  tokenExpiresAt = 0;
  nextFetchAt = 0;
  sendMessage(200, "Aircraft data provider settings saved");
}

void handleMarineCredentials() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  const String provider = webServer.hasArg("provider") ? webServer.arg("provider") : marineProvider;
  if (provider != "aisstream" && provider != "aishub" &&
      provider != "myshiptracking" && provider != "datalastic") {
    sendMessage(400, "Unknown marine data provider");
    return;
  }
  if (webServer.arg("credential").length() > 128) {
    sendMessage(400, "Marine credential is too long");
    return;
  }
  String *credentialField = provider == "aishub" ? &aisHubUsername :
                             provider == "myshiptracking" ? &myShipTrackingApiKey :
                             provider == "datalastic" ? &datalasticApiKey : &aisApiKey;
  const char *storeKey = provider == "aishub" ? "aishub-user" :
                         provider == "myshiptracking" ? "mst-key" :
                         provider == "datalastic" ? "datalastic-key" : "ais-key";
  if (webServer.arg("clear") == "1") {
    *credentialField = "";
    settingsStore.putString(storeKey, "");
  } else if (webServer.hasArg("credential") && webServer.arg("credential").length()) {
    *credentialField = webServer.arg("credential");
    settingsStore.putString(storeKey, *credentialField);
  }
  if (webServer.hasArg("radius")) {
    const long radius = webServer.arg("radius").toInt();
    if (radius < 5 || radius > 250) {
      sendMessage(400, "Marine radius must be 5 to 250 nautical miles");
      return;
    }
    marineRadiusNm = static_cast<uint16_t>(radius);
    settingsStore.putUShort("marine-radius", marineRadiusNm);
  }
  marineProvider = provider;
  settingsStore.putString("marine-provider", marineProvider);
  if (webServer.hasArg("enabled")) {
    marineTrackingEnabled = webServer.arg("enabled") == "1";
    settingsStore.putBool("marine-enabled", marineTrackingEnabled);
    // Mutually exclusive with the aircraft feed: switching this on should
    // stop competing with it for the same TLS/heap budget, and switching it
    // off should let the aircraft feed resume immediately rather than wait
    // out whatever fetch interval was already in flight.
    if (!marineTrackingEnabled) nextFetchAt = 0;
  }
  // Any provider, key, radius, or enabled change needs a clean slate: the
  // old vessels came from a different source/area/state and would
  // otherwise linger stale on the map until MARINE_STALE_MS drops them.
  aisWebSocket.disconnect();
  aisConnected = false;
  vesselCount = 0;
  nextMarineFetchAt = 0;
  if (marineTrackingEnabled && marineProvider == "aisstream" && aisApiKey.length()) connectAisWebSocket();
  sendMessage(200, "Marine settings saved");
}

void handleMarineVessels() {
  if (!requireWebAuthentication()) return;
  JsonDocument doc(&psramJsonAllocator);
  JsonArray vessels = doc["vessels"].to<JsonArray>();
  for (int i = 0; i < vesselCount; ++i) {
    VesselDisplay &vessel = latestVessels[i];
    JsonObject item = vessels.add<JsonObject>();
    item["mmsi"] = vessel.mmsi;
    item["name"] = vessel.name[0] ? vessel.name : String(vessel.mmsi);
    item["latitude"] = vessel.latitude;
    item["longitude"] = vessel.longitude;
    item["speed"] = roundf(vessel.speedKnots * 10.0f) / 10.0f;
    item["course"] = roundf(vessel.courseOverGround * 10.0f) / 10.0f;
    item["heading"] = vessel.heading;
    item["distance"] = roundf(vessel.distanceMiles * 10.0f) / 10.0f;
    item["navStatus"] = vessel.navStatus[0] ? vessel.navStatus : "UNKNOWN";
    item["shipType"] = vessel.shipType[0] ? vessel.shipType : "UNKNOWN";
    item["age"] = roundf((millis() - vessel.lastUpdateMs) / 100.0f) / 10.0f;
  }
  sendJsonDocument(200, doc);
}

void handleFirmwareUpload() {
  if (!webServer.authenticate(WEB_USERNAME, managementPassword.c_str())) return;
  if (!csrfToken.length() || webServer.header("X-ADSB-Token") != csrfToken) {
    firmwareUploadError = "Request token missing or stale; reload the admin page";
    firmwareUploadStarted = false;
    firmwareUploadComplete = false;
    firmwareUploadBytes = 0;
    return;
  }
  const String contentType = webServer.header("Content-Type");
  if (webServer.arg("upload") != "1" || !contentType.startsWith("multipart/")) {
    firmwareUploadError = "Firmware uploads require a multipart file request";
    firmwareUploadStarted = false;
    firmwareUploadComplete = false;
    firmwareUploadBytes = 0;
    return;
  }
  HTTPUpload &upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    firmwareUploadError = "";
    firmwareUploadStarted = true;
    firmwareUploadComplete = false;
    firmwareUploadBytes = 0;
    if (githubInstallPending || Update.isRunning()) {
      firmwareUploadError = "Another firmware operation is already in progress";
      return;
    }
    String filename = upload.filename;
    filename.toLowerCase();
    if (!filename.endsWith(".bin")) {
      firmwareUploadError = "Firmware filename must end in .bin";
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      firmwareUploadError = Update.errorString();
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (firmwareUploadError.length()) return;
    if (firmwareUploadBytes == 0 && upload.currentSize > 0 && upload.buf[0] != 0xE9) {
      Update.abort();
      firmwareUploadError = "Selected file is not an ESP32 application image";
      return;
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      firmwareUploadError = Update.errorString();
      Update.abort();
    } else {
      firmwareUploadBytes += upload.currentSize;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!firmwareUploadError.length() && !Update.end(true)) {
      firmwareUploadError = Update.errorString();
    }
    firmwareUploadComplete = firmwareUploadBytes >= 1024 &&
                             !firmwareUploadError.length() && !Update.hasError();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    firmwareUploadError = "Firmware upload was cancelled";
    firmwareUploadComplete = false;
    firmwareUploadBytes = 0;
  }
}

void handleFirmwareResult() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  if (webServer.arg("upload") != "1" || !firmwareUploadStarted ||
      !firmwareUploadComplete || firmwareUploadBytes < 1024) {
    const String error = firmwareUploadError.length() ? firmwareUploadError : "No complete firmware image was uploaded";
    JsonDocument doc;
    doc["message"] = error;
    String payload;
    serializeJson(doc, payload);
    sendJson(400, payload);
    firmwareUploadStarted = false;
    firmwareUploadComplete = false;
    firmwareUploadBytes = 0;
    return;
  }
  if (firmwareUploadError.length() || Update.hasError()) {
    const String error = firmwareUploadError.length() ? firmwareUploadError : Update.errorString();
    JsonDocument doc;
    doc["message"] = error;
    String payload;
    serializeJson(doc, payload);
    sendJson(400, payload);
    firmwareUploadStarted = false;
    firmwareUploadComplete = false;
    firmwareUploadBytes = 0;
    return;
  }
  sendMessage(200, "Firmware validated; device is rebooting");
  firmwareUploadStarted = false;
  firmwareUploadComplete = false;
  firmwareUploadBytes = 0;
  restartPending = true;
  restartAt = millis() + 1500;
}

void beginWebControl() {
  if (!webServerReady) {
    webServer.on("/", HTTP_GET, []() {
      if (!requireWebAuthentication()) return;
      webServer.sendHeader("Cache-Control", "no-store");
      webServer.send_P(200, "text/html", WEB_UI);
    });
    webServer.on("/api/status", HTTP_GET, handleStatusApi);
    webServer.on("/api/aircraft", HTTP_GET, handleAircraftApi);
    webServer.on("/api/page", HTTP_POST, handlePageControl);
    webServer.on("/api/settings", HTTP_POST, handleDisplaySettings);
    webServer.on("/api/location", HTTP_POST, handleLocationSettings);
    webServer.on("/api/update/check", HTTP_POST, handleGithubUpdateCheck);
    webServer.on("/api/update/github", HTTP_POST, handleGithubUpdateInstall);
    webServer.on("/api/sd/rescan", HTTP_POST, handleSdRescan);
    webServer.on("/api/refresh", HTTP_POST, []() {
      if (!requireWebAuthentication()) return;
      if (!requireCsrfToken()) return;
      nextFetchAt = 0;
      sendMessage(202, "Aircraft refresh requested");
    });
    webServer.on("/api/wifi/scan", HTTP_POST, handleWifiScanStart);
    webServer.on("/api/wifi/results", HTTP_GET, handleWifiScanResults);
    webServer.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
    webServer.on("/api/wifi/saved", HTTP_GET, handleWifiSavedNetworks);
    webServer.on("/api/wifi/forget", HTTP_POST, handleWifiForgetNetwork);
    webServer.on("/api/password", HTTP_POST, handlePasswordChange);
    webServer.on("/api/provider", HTTP_POST, handleProviderSettings);
    webServer.on("/api/marine/credentials", HTTP_POST, handleMarineCredentials);
    webServer.on("/api/marine/vessels", HTTP_GET, handleMarineVessels);
    webServer.on("/api/firmware", HTTP_POST, handleFirmwareResult, handleFirmwareUpload);
    webServer.on("/api/reboot", HTTP_POST, []() {
      if (!requireWebAuthentication()) return;
      if (!requireCsrfToken()) return;
      sendMessage(202, "Device is rebooting");
      restartPending = true;
      restartAt = millis() + 800;
    });
    webServer.on("/api/portal", HTTP_POST, []() {
      if (!requireWebAuthentication()) return;
      if (!requireCsrfToken()) return;
      sendMessage(202, "Setup portal will start as ADSB_WIFI");
      setupPortalPending = true;
    });
    webServer.onNotFound([]() {
      if (!requireWebAuthentication()) return;
      sendMessage(404, "Not found");
    });
    const char *trackedRequestHeaders[] = {"Content-Type", "X-ADSB-Token"};
    webServer.collectHeaders(trackedRequestHeaders, 2);
    webServerReady = true;
  }
  webServer.begin();
  if (WiFi.status() == WL_CONNECTED) {
    MDNS.end();
    if (MDNS.begin(DEVICE_HOSTNAME)) MDNS.addService("http", "tcp", 80);
  }
  Serial.printf("Web control: http://%s/ or http://%s.local/\n",
                WiFi.localIP().toString().c_str(), DEVICE_HOSTNAME);
}

// AIS ship-type codes are a large ITU-defined table; this groups the ranges
// that matter for a receiver display rather than reproducing it in full.
const char *shipTypeName(int type) {
  if (type == 30) return "FISHING";
  if (type == 36 || type == 37) return "PLEASURE/SAIL";
  if (type >= 40 && type <= 49) return "HIGH SPEED";
  if (type == 50) return "PILOT";
  if (type == 51) return "SAR";
  if (type == 52) return "TUG";
  if (type >= 60 && type <= 69) return "PASSENGER";
  if (type >= 70 && type <= 79) return "CARGO";
  if (type >= 80 && type <= 89) return "TANKER";
  if (type >= 90 && type <= 99) return "OTHER";
  return "UNKNOWN";
}

const char *navStatusName(int status) {
  switch (status) {
    case 0: return "UNDERWAY";
    case 1: return "AT ANCHOR";
    case 2: return "NOT UNDER CMD";
    case 3: return "RESTRICTED MANOEUVRE";
    case 4: return "CONSTRAINED DRAUGHT";
    case 5: return "MOORED";
    case 6: return "AGROUND";
    case 7: return "FISHING";
    case 8: return "SAILING";
    case 14: return "AIS-SART";
    default: return "UNKNOWN";
  }
}

VesselDisplay *findOrCreateVessel(uint32_t mmsi) {
  for (int i = 0; i < vesselCount; ++i) {
    if (latestVessels[i].mmsi == mmsi) return &latestVessels[i];
  }
  VesselDisplay *slot;
  if (vesselCount < MAX_VESSELS) {
    slot = &latestVessels[vesselCount++];
  } else {
    // Full: evict the longest-untouched vessel rather than dropping this one.
    slot = &latestVessels[0];
    for (int i = 1; i < vesselCount; ++i)
      if (latestVessels[i].lastUpdateMs < slot->lastUpdateMs) slot = &latestVessels[i];
  }
  memset(slot, 0, sizeof(*slot));
  slot->mmsi = mmsi;
  slot->heading = -1;
  return slot;
}

void pruneStaleVessels() {
  const uint32_t now = millis();
  int kept = 0;
  for (int i = 0; i < vesselCount; ++i) {
    if (now - latestVessels[i].lastUpdateMs <= MARINE_STALE_MS) {
      if (kept != i) latestVessels[kept] = latestVessels[i];
      ++kept;
    }
  }
  vesselCount = kept;
}

bool marineDataDirty = false;

// Shared by every REST provider below: writes name/type/nav-status text
// fields and marks the vessel touched, so each fetch function only has to
// pull its provider-specific field names into these common slots.
void applyVesselTextFields(VesselDisplay *vessel, const char *name, int shipType, int navStatus) {
  if (name && name[0]) {
    strncpy(vessel->name, name, sizeof(vessel->name) - 1);
    vessel->name[sizeof(vessel->name) - 1] = 0;
  }
  const char *typeName = shipTypeName(shipType);
  strncpy(vessel->shipType, typeName, sizeof(vessel->shipType) - 1);
  vessel->shipType[sizeof(vessel->shipType) - 1] = 0;
  const char *statusName = navStatusName(navStatus);
  strncpy(vessel->navStatus, statusName, sizeof(vessel->navStatus) - 1);
  vessel->navStatus[sizeof(vessel->navStatus) - 1] = 0;
}

void applyVesselPosition(VesselDisplay *vessel, double lat, double lon) {
  if (lat == 0.0 && lon == 0.0) return;
  vessel->latitude = lat;
  vessel->longitude = lon;
  vessel->distanceMiles = distanceMilesFromHome(lat, lon);
}

// data.aishub.net: https://www.aishub.net/api - a member-contributed AIS
// exchange. Requires an AISHub account that shares your own receiver's data
// with their network; the username alone (without a contributing receiver)
// may return no data. Do not query more than once a minute - AISHub's
// service returns nothing if called more frequently.
void fetchAisHubVessels() {
  if (!aisHubUsername.length()) return;
  const double latRadius = marineRadiusNm / 60.0;
  const double lonRadius = marineRadiusNm / (60.0 * max(0.1, cos(radians(homeLatitude))));
  const String url = "https://data.aishub.net/ws.php?username=" + aisHubUsername +
      "&format=1&output=json&compress=0" +
      "&latmin=" + String(homeLatitude - latRadius, 5) +
      "&latmax=" + String(homeLatitude + latRadius, 5) +
      "&lonmin=" + String(homeLongitude - lonRadius, 5) +
      "&lonmax=" + String(homeLongitude + lonRadius, 5);
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(9000); http.setConnectTimeout(9000);
  if (!http.begin(client, url)) return;
  http.addHeader("User-Agent", userAgent());
  const int code = http.GET();
  if (code == HTTP_CODE_OK) {
    JsonDocument doc(&psramJsonAllocator);
    PsramSink body;
    http.writeToStream(&body);
    // AISHub's own success envelope is a 2-element array: [{meta}, [vessels]].
    if (!deserializeJson(doc, body.data(), body.size()) && doc[0]["ERROR"] == false) {
      for (JsonObject v : doc[1].as<JsonArray>()) {
        const uint32_t mmsi = v["MMSI"] | 0;
        if (!mmsi) continue;
        VesselDisplay *vessel = findOrCreateVessel(mmsi);
        vessel->lastUpdateMs = millis();
        applyVesselPosition(vessel, v["LATITUDE"] | 0.0, v["LONGITUDE"] | 0.0);
        vessel->speedKnots = v["SOG"] | vessel->speedKnots;
        vessel->courseOverGround = v["COG"] | vessel->courseOverGround;
        const float heading = v["HEADING"] | 511.0f;
        vessel->heading = heading < 360.0f ? heading : -1;
        applyVesselTextFields(vessel, v["NAME"] | "", v["TYPE"] | -1, v["NAVSTAT"] | -1);
      }
      aisConnected = true;
      aisLastMessageAt = millis();
    } else {
      aisConnected = false;
    }
  } else {
    aisConnected = false;
  }
  http.end();
}

// api.myshiptracking.com/api/v2/vessel/zone - freemium REST, Bearer auth.
void fetchMyShipTrackingVessels() {
  if (!myShipTrackingApiKey.length()) return;
  const double latRadius = marineRadiusNm / 60.0;
  const double lonRadius = marineRadiusNm / (60.0 * max(0.1, cos(radians(homeLatitude))));
  const String url = "https://api.myshiptracking.com/api/v2/vessel/zone?response=simple" +
      String("&minlat=") + String(homeLatitude - latRadius, 5) +
      "&maxlat=" + String(homeLatitude + latRadius, 5) +
      "&minlon=" + String(homeLongitude - lonRadius, 5) +
      "&maxlon=" + String(homeLongitude + lonRadius, 5);
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(9000); http.setConnectTimeout(9000);
  if (!http.begin(client, url)) return;
  http.addHeader("User-Agent", userAgent());
  http.addHeader("Authorization", "Bearer " + myShipTrackingApiKey);
  const int code = http.GET();
  if (code == HTTP_CODE_OK) {
    JsonDocument doc(&psramJsonAllocator);
    PsramSink body;
    http.writeToStream(&body);
    if (!deserializeJson(doc, body.data(), body.size()) && doc["status"] == "success") {
      for (JsonObject v : doc["data"].as<JsonArray>()) {
        const uint32_t mmsi = v["mmsi"] | 0;
        if (!mmsi) continue;
        VesselDisplay *vessel = findOrCreateVessel(mmsi);
        vessel->lastUpdateMs = millis();
        applyVesselPosition(vessel, v["lat"] | 0.0, v["lng"] | 0.0);
        vessel->speedKnots = v["speed"] | vessel->speedKnots;
        vessel->courseOverGround = v["course"] | vessel->courseOverGround;
        // This endpoint doesn't report true heading separately from course.
        vessel->heading = -1;
        applyVesselTextFields(vessel, v["vessel_name"] | "", v["vtype"] | -1, v["nav_status"] | -1);
      }
      aisConnected = true;
      aisLastMessageAt = millis();
    } else {
      aisConnected = false;
    }
  } else {
    aisConnected = false;
  }
  http.end();
}

// api.datalastic.com/api/v0/vessel_inradius - freemium REST, API-key query
// param. Radius is capped at 50 nm by the API itself, tighter than the
// 250 nm ceiling on the other providers' bounding boxes.
void fetchDatalasticVessels() {
  if (!datalasticApiKey.length()) return;
  const uint16_t radius = min<uint16_t>(marineRadiusNm, 50);
  const String url = "https://api.datalastic.com/api/v0/vessel_inradius?api-key=" + datalasticApiKey +
      "&lat=" + String(homeLatitude, 5) +
      "&lon=" + String(homeLongitude, 5) +
      "&radius=" + String(radius);
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(9000); http.setConnectTimeout(9000);
  if (!http.begin(client, url)) return;
  http.addHeader("User-Agent", userAgent());
  const int code = http.GET();
  if (code == HTTP_CODE_OK) {
    JsonDocument doc(&psramJsonAllocator);
    PsramSink body;
    http.writeToStream(&body);
    if (!deserializeJson(doc, body.data(), body.size()) && !doc["data"].isNull()) {
      for (JsonObject v : doc["data"]["vessels"].as<JsonArray>()) {
        // Datalastic returns mmsi as a string field, not a number.
        const uint32_t mmsi = atol(v["mmsi"] | "");
        if (!mmsi) continue;
        VesselDisplay *vessel = findOrCreateVessel(mmsi);
        vessel->lastUpdateMs = millis();
        applyVesselPosition(vessel, v["lat"] | 0.0, v["lon"] | 0.0);
        vessel->speedKnots = v["speed"] | vessel->speedKnots;
        vessel->courseOverGround = v["course"] | vessel->courseOverGround;
        const float heading = v["heading"] | 511.0f;
        vessel->heading = heading < 360.0f ? heading : -1;
        // Datalastic's "type" is a text label (e.g. "Tanker"), not a numeric
        // AIS code, so it's copied directly instead of going through
        // shipTypeName()'s numeric-range lookup.
        const char *typeText = v["type"] | "UNKNOWN";
        strncpy(vessel->shipType, typeText, sizeof(vessel->shipType) - 1);
        vessel->shipType[sizeof(vessel->shipType) - 1] = 0;
        strcpy(vessel->navStatus, "UNKNOWN");
      }
      aisConnected = true;
      aisLastMessageAt = millis();
    } else {
      aisConnected = false;
    }
  } else {
    aisConnected = false;
  }
  http.end();
}

void fetchMarineRest() {
  if (marineProvider == "aishub") fetchAisHubVessels();
  else if (marineProvider == "myshiptracking") fetchMyShipTrackingVessels();
  else if (marineProvider == "datalastic") fetchDatalasticVessels();
}

// AISstream.io pushes one JSON object per WebSocket text frame - a
// PositionReport (course/speed/heading) or ShipStaticData (name/type),
// keyed by MMSI. There is no polling: this is the entire live feed.
void onAisEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      aisConnected = true;
      aisConsecutiveFailures = 0;
      Serial.println("AIS WebSocket connected; subscribing");
      const double latRadius = marineRadiusNm / 60.0;
      const double lonRadius = marineRadiusNm / (60.0 * max(0.1, cos(radians(homeLatitude))));
      JsonDocument sub(&psramJsonAllocator);
      sub["APIKey"] = aisApiKey;
      JsonArray boxes = sub["BoundingBoxes"].to<JsonArray>();
      JsonArray box = boxes.add<JsonArray>();
      JsonArray corner1 = box.add<JsonArray>();
      corner1.add(homeLatitude - latRadius);
      corner1.add(homeLongitude - lonRadius);
      JsonArray corner2 = box.add<JsonArray>();
      corner2.add(homeLatitude + latRadius);
      corner2.add(homeLongitude + lonRadius);
      JsonArray filters = sub["FilterMessageTypes"].to<JsonArray>();
      filters.add("PositionReport");
      filters.add("ShipStaticData");
      String message;
      serializeJson(sub, message);
      aisWebSocket.sendTXT(message);
      break;
    }
    case WStype_DISCONNECTED:
      aisConnected = false;
      if (aisIntentionalDisconnect) {
        aisIntentionalDisconnect = false;
        Serial.println("AIS WebSocket paused for aircraft fetch");
      } else {
        // A real failure (usually the SSL alloc error logged just above by
        // the library) - back off instead of retrying every few seconds
        // and hammering an already-tight heap.
        const uint32_t backoffMs = min<uint32_t>(60000UL, 8000UL << min<uint8_t>(aisConsecutiveFailures, 3));
        aisNextRetryAt = millis() + backoffMs;
        if (aisConsecutiveFailures < 250) ++aisConsecutiveFailures;
        Serial.printf("AIS WebSocket disconnected; retrying in %lu ms (failure #%u)\n",
                      static_cast<unsigned long>(backoffMs), aisConsecutiveFailures);
      }
      break;
    // AISstream.io documents that it always sends binary frames whose
    // payload happens to be UTF-8 JSON, not text frames - handle both the
    // same way rather than silently dropping every real message.
    case WStype_TEXT:
    case WStype_BIN: {
      aisLastMessageAt = millis();
      // Every position report parses here, often several times a second in
      // busy waters - unlike a one-off request, this allocator choice runs
      // hot, so it must come from PSRAM like every other large JSON parse in
      // this file rather than fragmenting the scarce internal heap.
      JsonDocument doc(&psramJsonAllocator);
      if (deserializeJson(doc, payload, length)) return;
      const char *messageType = doc["MessageType"] | "";
      JsonObject meta = doc["MetaData"].as<JsonObject>();
      if (meta.isNull()) return;
      const uint32_t mmsi = meta["MMSI"] | 0;
      if (!mmsi) return;
      VesselDisplay *vessel = findOrCreateVessel(mmsi);
      vessel->lastUpdateMs = millis();
      const char *name = meta["ShipName"] | "";
      if (name[0]) {
        strncpy(vessel->name, name, sizeof(vessel->name) - 1);
        vessel->name[sizeof(vessel->name) - 1] = 0;
      }
      if (!strcmp(messageType, "PositionReport")) {
        JsonObject report = doc["Message"]["PositionReport"].as<JsonObject>();
        if (!report.isNull()) {
          // Despite the docs' inline example showing Latitude/Longitude on
          // MetaData, AISstream's own example code (github.com/aisstream/
          // example) reads position from the PositionReport message itself
          // - MetaData's copy is unreliable and was silently leaving every
          // vessel at 0,0.
          const double lat = report["Latitude"] | 0.0;
          const double lon = report["Longitude"] | 0.0;
          if (lat != 0.0 || lon != 0.0) {
            vessel->latitude = lat;
            vessel->longitude = lon;
            vessel->distanceMiles = distanceMilesFromHome(lat, lon);
          }
          vessel->speedKnots = report["Sog"] | vessel->speedKnots;
          vessel->courseOverGround = report["Cog"] | vessel->courseOverGround;
          const float trueHeading = report["TrueHeading"] | 511.0f;
          vessel->heading = trueHeading < 360.0f ? trueHeading : -1;
          const int navStatus = report["NavigationalStatus"] | -1;
          const char *statusName = navStatusName(navStatus);
          strncpy(vessel->navStatus, statusName, sizeof(vessel->navStatus) - 1);
          vessel->navStatus[sizeof(vessel->navStatus) - 1] = 0;
        }
      } else if (!strcmp(messageType, "ShipStaticData")) {
        JsonObject staticData = doc["Message"]["ShipStaticData"].as<JsonObject>();
        if (!staticData.isNull()) {
          const int shipType = staticData["Type"] | -1;
          const char *typeName = shipTypeName(shipType);
          strncpy(vessel->shipType, typeName, sizeof(vessel->shipType) - 1);
          vessel->shipType[sizeof(vessel->shipType) - 1] = 0;
        }
      }
      if (displayPage == DisplayPage::Marine) marineDataDirty = true;
      break;
    }
    default:
      break;
  }
}

// AISstream.io negotiates permessage-deflate (RFC 7692) when the client
// requests it; the links2004/WebSockets library used here does not
// implement that extension, so this connection runs uncompressed. Per
// AISstream's own documentation, uncompressed connections become subject to
// per-user bandwidth limits (with excess messages dropped) starting
// September 2026 - if vessels start silently going stale after that date,
// this is the first thing to check.
void connectAisWebSocket() {
  if (!marineTrackingEnabled || marineProvider != "aisstream" || !aisApiKey.length()) return;
#if ADSB_TLS_INSECURE
  aisWebSocket.beginSSL("stream.aisstream.io", 443, "/v0/stream");
#else
  aisWebSocket.beginSslWithBundle(
      "stream.aisstream.io", 443, "/v0/stream",
      rootca_crt_bundle_start,
      static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
#endif
  aisWebSocket.onEvent(onAisEvent);
  // Reconnect timing is driven explicitly (aisNextRetryAt, in networkTask)
  // so our own exponential backoff actually controls retry frequency;
  // leave the library's own timer effectively disabled rather than have it
  // race a second reconnect attempt against ours.
  aisWebSocket.setReconnectInterval(3600000UL);
}

}

// Everything that talks to the network or the SD/flash filesystem for admin
// housekeeping lives here, running on its own core so a slow or failing
// fetch never blocks touch input or rendering in loop(). Calls that touch
// the buffers loop() also reads (aircraft/vessel data, the physical map) are
// wrapped in dataMutex; see its declaration for the full rationale.
void networkTask(void *) {
  for (;;) {
    // Several admin API handlers (handleAircraftApi in particular) build a
    // sizeable PSRAM-backed JSON document from latestAircraft on every call -
    // real, non-trivial PSRAM traffic that ran unguarded here despite
    // present()'s full-frame PSRAM->DMA copy needing the same mutex. The two
    // running at once is exactly the condition that can starve the RGB
    // panel's DMA and roll a frame (see the vsync-restart comment in
    // present()); serialize all HTTP handling against rendering the same way
    // the fetches already are, not just the ones known to touch PSRAM.
    { MutexGuard guard(dataMutex); webServer.handleClient(); }
    if (githubInstallPending) {
      githubInstallPending = false;
      installGithubUpdate();
    }
    // Only ever checks when the admin page's "Check for updates" button asks
    // for it (githubCheckPending) - this used to also run automatically
    // every UPDATE_CHECK_MS, one more background HTTPS/TLS session this
    // board didn't need adding to its already-tight memory budget.
    if (githubCheckPending) {
      githubCheckPending = false;
      checkGithubUpdate();
    }
    if (physicalMapRefreshPending) {
      physicalMapRefreshPending = false;
      { MutexGuard guard(dataMutex); refreshPhysicalBaseMap(); }
      needsRedraw = true;
    }
    if (nextMapRetryAt && static_cast<int32_t>(millis() - nextMapRetryAt) >= 0) {
      nextMapRetryAt = 0;
      physicalMapRefreshPending = true;
    }
    if (setupPortalPending) {
      setupPortalPending = false;
      delay(250);
      webServer.stop();
      MDNS.end();
      WiFiManager wm;
      wm.setWiFiAPChannel(6);
      wm.setConfigPortalTimeout(900);
      wm.startConfigPortal("ADSB_WIFI");
      beginWebControl();
    }
    if (pageSavePending && static_cast<int32_t>(millis() - pageSaveAt) >= 0) {
      pageSavePending = false;
      settingsStore.putUChar("display-page", static_cast<uint8_t>(displayPage));
    }
    if (wifiRollbackAt && static_cast<int32_t>(millis() - wifiRollbackAt) >= 0) {
      wifiRollbackAt = 0;
      if (WiFi.status() != WL_CONNECTED && previousWifiSsid.length()) {
        Serial.println("New Wi-Fi credentials failed; restoring the previous network");
        WiFi.begin(previousWifiSsid.c_str(), previousWifiPassword.c_str());
      }
      previousWifiSsid = "";
      previousWifiPassword = "";
    }
    // A logo the screensaver has just asked for. On the network task, so the
    // TLS handshake and the SD write never happen on the core that drives
    // the panel. At most one per pass, and the render path only asks for one
    // at a time, so this cannot turn into a burst of HTTPS requests.
    if (logoFetchPending) {
      // Under the lock: the render task writes the code and the flag, and a
      // three-byte copy read while it was being written would fetch some
      // other airline's logo.
      char code[4];
      { MutexGuard guard(dataMutex); memcpy(code, logoFetchCode, 4); }
      const LogoFetch outcome = cacheOperatorLogo(code);
      MutexGuard guard(dataMutex);
      if (outcome == LogoFetch::Cached) {
        // Repaint so the logo replaces the initials now rather than at the
        // next rotation. The screensaver skips repaints whose content has
        // not changed, and the logo is not part of that signature, so
        // without this the tile it was fetched for would already be gone.
        screensaverNeedsRedraw = true;
      } else if (outcome == LogoFetch::Unavailable) {
        rememberLogoUnavailable(code);
      }
      // Cleared whatever happened: a Retry is worth another go, but on a
      // later rotation rather than immediately, and clearing it here stops
      // one unreachable logo blocking every other airline's.
      logoFetchPending = false;
    }

    // And the type photograph, on the same terms. Separate from the logo so
    // one missing image cannot block the other, and after it so a first
    // sighting fetches the logo first - it is the smaller download and the
    // more identifying picture.
    if (photoFetchPending && !logoFetchPending) {
      char code[8];
      { MutexGuard guard(dataMutex); memcpy(code, photoFetchCode, sizeof(code)); }
      const LogoFetch outcome = cacheTypePhoto(code);
      MutexGuard guard(dataMutex);
      if (outcome == LogoFetch::Cached) screensaverNeedsRedraw = true;
      else if (outcome == LogoFetch::Unavailable) rememberPhotoUnavailable(code);
      photoFetchPending = false;
    }

    // One place to record a network that works, whichever route got us
    // there: the boot autoConnect, the setup portal, or the Wi-Fi page. The
    // alternative was remembering at each of those call sites and missing
    // one.
    if (WiFi.status() == WL_CONNECTED) {
      wifiDisconnectedSince = 0;
      static String lastRemembered;
      const String current = WiFi.SSID();
      if (current.length() && current != lastRemembered) {
        lastRemembered = current;
        rememberWifiNetwork(current, WiFi.psk());
      }
    } else if (!wifiRollbackAt) {
      // Not while a rollback is pending - that already has a network in
      // mind and walking the list would fight it.
      const uint32_t now = millis();
      // Zero means "not currently disconnected", so the one tick per ~49
      // days where millis() is genuinely 0 borrows the next millisecond.
      if (!wifiDisconnectedSince) wifiDisconnectedSince = now ? now : 1;
      else if (now - wifiDisconnectedSince >= WIFI_RETRY_AFTER_MS) {
        wifiDisconnectedSince = 0;
        connectSavedWifiNetwork();
      }
    }
    if (restartPending && static_cast<int32_t>(millis() - restartAt) >= 0) {
      delay(100);
      ESP.restart();
    }
    // Marine tracking and the aircraft feed are mutually exclusive - both
    // need a persistent TLS session or frequent HTTPS fetches, and running
    // both at once was the root of the recurring SSL alloc failures. Only
    // one of these two blocks ever does anything at a time.
    if (marineTrackingEnabled) {
      if (marineProvider == "aisstream") {
        if (aisApiKey.length()) {
          MutexGuard guard(dataMutex);
          aisWebSocket.loop();
          if (!aisWebSocket.isConnected() && static_cast<int32_t>(millis() - aisNextRetryAt) >= 0) {
            connectAisWebSocket();
          }
        }
      } else if (marineConfigured() && static_cast<int32_t>(millis() - nextMarineFetchAt) >= 0) {
        { MutexGuard guard(dataMutex); fetchMarineRest(); }
        nextMarineFetchAt = millis() + MARINE_REST_REFRESH_MS;
        marineDataDirty = true;
      }
      if (static_cast<int32_t>(millis() - nextMarinePruneAt) >= 0) {
        { MutexGuard guard(dataMutex); pruneStaleVessels(); }
        nextMarinePruneAt = millis() + 60000UL;
      }
    } else if (static_cast<int32_t>(millis() - nextFetchAt) >= 0) {
      const uint32_t fetchStartedAt = millis();
      // The AIS WebSocket's persistent TLS session and this fetch's own TLS
      // session compete for the same scarce internal RAM on this board - with
      // both open at once, heapMinimum fell to a few hundred bytes and every
      // aircraft/route request failed. Pausing the socket for the fetch's
      // duration is the difference between the feed working at all and not;
      // AISstream tolerates the brief reconnect (it re-subscribes on connect).
      const bool pauseAis = marineProvider == "aisstream" && aisWebSocket.isConnected();
      const uint32_t aisPauseStartedAt = millis();
      if (pauseAis) { aisIntentionalDisconnect = true; aisWebSocket.disconnect(); }
      const uint32_t aisPauseMs = millis() - aisPauseStartedAt;
      { MutexGuard guard(dataMutex); fetchAircraft(); }
      // fetchAircraft() resets the phase counters, so this has to be folded
      // in afterwards rather than before.
      fetchPhases.aisPauseMs = aisPauseMs;
      // This was a known-good, already-connected session we paused ourselves,
      // not a failure - reconnect immediately rather than waiting on the
      // failure backoff, which doesn't apply here.
      if (pauseAis) connectAisWebSocket();
      const uint32_t blockedMs = millis() - fetchStartedAt;
      Serial.printf("fetchAircraft blocked the network task for %lu ms\n",
                    static_cast<unsigned long>(blockedMs));
      // Every blocking call inside a fetch is separately bounded (9s connect,
      // 6s per route lookup, an explicit deadline on the body read), so a
      // cycle in the tens of seconds means one of those bounds is not
      // holding. Print where the time went so the next long cycle names the
      // culprit instead of leaving it to inference. Anything the phases do
      // not account for shows up as "other" - which is itself the answer if
      // it is the large number.
      if (blockedMs > FETCH_PHASE_REPORT_MS) {
        const uint32_t accounted = fetchPhases.aisPauseMs + fetchPhases.connectMs +
                                   fetchPhases.bodyMs + fetchPhases.parseMs +
                                   fetchPhases.routeMs + fetchPhases.routeSaveMs +
                                   fetchPhases.renderMs;
        Serial.printf(
            "  slow fetch breakdown: ais=%lu connect=%lu(x%u) body=%lu parse=%lu "
            "routes=%lu(x%u worst=%lu) save=%lu render=%lu other=%lu\n",
            static_cast<unsigned long>(fetchPhases.aisPauseMs),
            static_cast<unsigned long>(fetchPhases.connectMs), fetchPhases.attempts,
            static_cast<unsigned long>(fetchPhases.bodyMs),
            static_cast<unsigned long>(fetchPhases.parseMs),
            static_cast<unsigned long>(fetchPhases.routeMs), fetchPhases.routeLookups,
            static_cast<unsigned long>(fetchPhases.routeWorstMs),
            static_cast<unsigned long>(fetchPhases.routeSaveMs),
            static_cast<unsigned long>(fetchPhases.renderMs),
            static_cast<unsigned long>(blockedMs > accounted ? blockedMs - accounted : 0));
      }
      if (openSkyAuthRetryPending) nextFetchAt = millis() + 1000UL;
      else if (static_cast<int32_t>(millis() - nextFetchAt) >= 0) nextFetchAt = millis() + REFRESH_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  // First thing on the wire, before anything can fail: which binary is
  // actually running. Several rounds of debugging were spent on symptoms that
  // turned out to be a stale build or the wrong checkout being flashed, and
  // nothing in the old boot log distinguished one image from another.
  delay(50);
#if defined(ADSB_BOARD_WS7)
  constexpr char boardName[] = "WS7 800x480";
#else
  constexpr char boardName[] = "WS4 480x480";
#endif
  Serial.printf("\n=== ESP32 ADS-B v%s | built %s %s | %s | panel %dx%d ===\n",
                FIRMWARE_VERSION, __DATE__, __TIME__, boardName, W, H);
  // The default Task Watchdog Timer (5s, watching the idle task on both
  // cores) reboots the whole chip if any task occupies a core without
  // yielding for that long. Pinning network I/O to its own core means the
  // various already-deliberate HTTPClient timeouts in this file (up to
  // 15000ms, e.g. the firmware download path) can now legitimately exceed
  // that 5s window on a slow response or a weak Wi-Fi link - a live test
  // reproduced exactly this, crashing on an ordinary slow adsb.fi fetch, not
  // a real hang. Widen it well past the longest configured timeout instead
  // of shortening those timeouts (they were sized for real, already-observed
  // slow-network conditions on this board); a genuinely stuck task still
  // gets caught and rebooted, just with more headroom for legitimate waits.
  //
  // 30s wasn't enough either: a later live test hit this same abort on
  // ordinary aircraft fetches (http.setTimeout is only 9000ms) every single
  // boot. HTTPClient::writeToStreamDataBlock has no overall deadline of its
  // own - each individual read is capped at 9s, but if the far end keeps
  // trickling a few bytes through just before each of those caps, the loop
  // never gives up and never yields long enough for the idle task to run,
  // so several such reads in a row can add up past whatever this is set to
  // without any single call ever looking "stuck". Doubled to 60s to buy more
  // margin; this doesn't fix that unbounded retry loop (nothing in this
  // file's control can, short of vendoring a patched HTTPClient), so a
  // connection degraded enough could still trip it.
  esp_task_wdt_config_t watchdogConfig = {
      .timeout_ms = 60000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&watchdogConfig);
  dataMutex = xSemaphoreCreateRecursiveMutex();
  if (!LittleFS.begin(true)) Serial.println("LittleFS map cache unavailable");
  settingsStore.begin("adsb-web", false);
  managementPassword = settingsStore.getString("password", "aircraft");
  apiProvider = settingsStore.getString("provider", "opensky");
  if (apiProvider != "opensky" && apiProvider != "adsbfi" &&
      apiProvider != "airplaneslive" && apiProvider != "adsblol" &&
      apiProvider != "adsbone" && apiProvider != "adsbx" &&
      apiProvider != "aggregator" && apiProvider != "flyitalyadsb") apiProvider = "opensky";
  // Compiled-in credentials are opt-in. Without this flag a locally built
  // image carries no secret that `strings firmware.bin` could recover.
#ifdef ADSB_BAKE_CREDENTIALS
  openSkyClientId = settingsStore.getString("os-client", OPENSKY_CLIENT_ID);
  openSkyClientSecret = settingsStore.getString("os-secret", OPENSKY_CLIENT_SECRET);
#else
  openSkyClientId = settingsStore.getString("os-client", "");
  openSkyClientSecret = settingsStore.getString("os-secret", "");
#endif
  rapidApiKey = settingsStore.getString("rapid-key", "");
  aggregatorApiKey = settingsStore.getString("agg-key", "");
  flyItalyApiKey = settingsStore.getString("flyitaly-key", "");
  aisApiKey = settingsStore.getString("ais-key", "");
  aisHubUsername = settingsStore.getString("aishub-user", "");
  myShipTrackingApiKey = settingsStore.getString("mst-key", "");
  datalasticApiKey = settingsStore.getString("datalastic-key", "");
  marineTrackingEnabled = settingsStore.getBool("marine-enabled", false);
  marineProvider = settingsStore.getString("marine-provider", "aisstream");
  if (marineProvider != "aisstream" && marineProvider != "aishub" &&
      marineProvider != "myshiptracking" && marineProvider != "datalastic") marineProvider = "aisstream";
  marineRadiusNm = constrain(settingsStore.getUShort("marine-radius", DEFAULT_MARINE_RADIUS_NM), 5, 250);
  homeLatitude = settingsStore.getFloat("home-lat", DEFAULT_HOME_LAT);
  homeLongitude = settingsStore.getFloat("home-lon", DEFAULT_HOME_LON);
  queryRadiusNm = constrain(settingsStore.getUShort("radius-nm", DEFAULT_RADIUS_NM), 5, 250);
  if (!isfinite(homeLatitude) || homeLatitude < -85.0f || homeLatitude > 85.0f) homeLatitude = DEFAULT_HOME_LAT;
  if (!isfinite(homeLongitude) || homeLongitude < -180.0f || homeLongitude > 180.0f) homeLongitude = DEFAULT_HOME_LON;
  physicalMapZoom = constrain(settingsStore.getUChar("map-zoom", zoomForRadius()), 3, 16);
  displayPage = static_cast<DisplayPage>(constrain(settingsStore.getUChar("display-page", 0), 0, DISPLAY_PAGE_COUNT - 1));
  soundAlerts = settingsStore.getBool("sound", true);
  screensaverEnabled = settingsStore.getBool("ssaver-on", false);
  screensaverIdleMinutes = constrain(settingsStore.getUShort("ssaver-min", 5), 1, 120);
  brightnessPercent = settingsStore.getUChar("brightness", 100);
  brightnessPercent = constrain(brightnessPercent, 10, 100);
  generateCsrfToken();
#if BOARD_HAS_BOOT_BUTTON
  // Only claim GPIO 0 on boards where it is actually a free button. On the
  // 800x480 panels it is the G3 data line, so even the pinMode() call - which
  // used to run unconditionally here - was reconfiguring a pin the RGB
  // peripheral owns, ahead of panel init reclaiming it.
  pinMode(0, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(0), onBootButtonFalling, FALLING);
#endif
  delay(300);
#if BOARD_EXPANDER_CH32
  if (!WS_CH32_IO::begin(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL,
                         WS_CH32_IO::DEFAULT_I2C_FREQ, &Serial)) {
#else
  if (!WS_CH422G::begin(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, 400000, &Serial)) {
#endif
    Serial.println("Rev4 display helper unavailable");
  }
  applyBrightness(brightnessPercent);
  // Read before the panel exists: esp_lcd latches the pixel clock at init, so
  // a change only takes effect on the next boot. An unknown stored value (a
  // downgrade, a corrupted key) falls back to the board default rather than
  // initialising the panel with something it cannot drive.
  {
    const uint32_t storedKhz = settingsStore.getULong("pclk-khz", PANEL_PCLK_HZ / 1000UL);
    const uint32_t storedHz = storedKhz * 1000UL;
    panelPclkHz = PANEL_PCLK_HZ;
    for (uint32_t choice : PANEL_PCLK_CHOICES)
      if (choice == storedHz) { panelPclkHz = storedHz; break; }
  }
  {
    const uint16_t buildDefault = Arduino_ESP32RGBPanel::bounceBufferLines();
    const uint16_t storedLines = settingsStore.getUShort("bounce-lines", buildDefault);
    panelBounceLines = buildDefault;
    for (uint16_t choice : PANEL_BOUNCE_CHOICES)
      if (choice == storedLines) { panelBounceLines = storedLines; break; }
    Arduino_ESP32RGBPanel::setBounceBufferLines(panelBounceLines);
  }
  createDisplay(panelPclkHz);
  Serial.printf("Panel pixel clock: %.1f MHz (~%.1f Hz refresh), bounce buffer %u lines (%u KB internal)\n",
                panelPclkHz / 1000000.0f, panelRefreshHz(panelPclkHz), panelBounceLines,
                static_cast<unsigned>(2UL * panelBounceLines * W * 2UL / 1024UL));
  if (!gfx->begin()) {
    Serial.println("Display initialization failed");
    while (true) delay(1000);
  }
  touchReady = ADSB_ENABLE_TOUCH ? beginTouch() : false;
#ifdef DISPLAY_DIAGNOSTIC
  // Keep this test independent of PSRAM, Wi-Fi, the map and OpenSky.
  gfx->fillScreen(RGB565_RED);
  delay(1500);
  gfx->fillScreen(RGB565_GREEN);
  delay(1500);
  gfx->fillScreen(RGB565_BLUE);
  delay(1500);
  gfx->fillScreen(RGB565_WHITE);
  gfx->setCursor(55, 220);
  gfx->setTextSize(4);
  gfx->setTextColor(RGB565_BLACK);
  gfx->println("DISPLAY OK");
  return;
#endif
  // Both of these used to be plain globals in internal DRAM - 44.5 KB for
  // the PNG decoder and 8.4 KB for the route cache - competing with the RGB
  // bounce buffers and every mbedTLS handshake for the scarcest memory on
  // the board. Neither needs the speed.
  //
  // The decoder is set up before the first boot screen rather than after it,
  // because the boot screen is now a PNG and needs it. It only wants a PSRAM
  // allocation, so there is nothing here that has to wait.
  panelDirectDraw = settingsStore.getBool("direct-draw", false);
  initPngDecoder();
  if (!pngDecoderPtr) Serial.println("PNG decoder allocation failed - images unavailable");
  renderBootScreen();
  delay(2800);
  initRouteCache();
  if (!routeCache.data) Serial.println("Route cache allocation failed - routes unavailable");
  // In direct mode every pixel() lands in the panel's own buffer, so there
  // is no shadow to allocate and present() has nothing to copy.
  framebuffer = panelDirectDraw
                    ? gfx->getFramebuffer()
                    : (uint16_t *)heap_caps_malloc(W * H * sizeof(uint16_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  Serial.printf("Rendering: %s\n", panelDirectDraw
                                        ? "direct to panel framebuffer"
                                        : "shadow buffer, copied on present()");
  baseMap=(uint16_t*)heap_caps_malloc(W*H*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  latestAircraft = static_cast<AircraftDisplay *>(heap_caps_calloc(
      MAX_AIRCRAFT, sizeof(AircraftDisplay), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  latestVessels = static_cast<VesselDisplay *>(heap_caps_calloc(
      MAX_VESSELS, sizeof(VesselDisplay), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!framebuffer || !baseMap || !latestAircraft || !latestVessels) {
    Serial.println("PSRAM display/aircraft buffers unavailable");
    while(true) delay(1000);
  }
  mountSdCard();
  loadRouteCacheFromStorage();
  restoreMap(); status("SETUP",rgb(245,150,0)); present();
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setWiFiAPChannel(6);
  wm.setConfigPortalTimeout(900);
  wm.setAPCallback([](WiFiManager *) {
    renderBootScreen("AP access: 192.168.4.1", rgb(245, 180, 35));
  });
  renderBootScreen("Wi-Fi connecting - please wait", rgb(53,169,244));
  if (!wm.autoConnect("ADSB_WIFI")) {
    // autoConnect only knows the one network the ESP32 itself remembers.
    // Before declaring setup required, try the others this receiver has
    // successfully used - which is the whole point of keeping them.
    renderBootScreen("Trying saved networks", rgb(53,169,244));
    if (!connectSavedWifiNetwork()) {
      renderBootScreen("Wi-Fi failed - setup required", rgb(255,65,65));
      delay(5000);
      restoreMap(); status("WIFI",rgb(245,30,35)); present();
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    renderBootScreen("Wi-Fi connected: " + WiFi.localIP().toString(), rgb(55,215,110));
    delay(5000);
    { MutexGuard guard(dataMutex); refreshPhysicalBaseMap(); }
    restoreMap();
    status("MAP", rgb(53,169,244));
    present();
  }
  beginWebControl();
  Serial.printf("Web login username: %s\n", WEB_USERNAME);
  { MutexGuard guard(dataMutex); fetchAircraft(); }
  nextFetchAt=millis()+REFRESH_MS;
  connectAisWebSocket();
  // Everything network-bound (aircraft/marine fetches, the map tile rebuild,
  // the AIS socket, the admin web server, OTA/Wi-Fi/SD housekeeping) now runs
  // on its own task on the other core, so a slow fetch can't freeze touch
  // input and rendering in loop() below. See the dataMutex comment above for
  // how the two tasks share the aircraft/vessel/map buffers safely, and the
  // NETWORK_TASK_STACK_BYTES comment for why its stack has to be internal
  // RAM rather than PSRAM despite the extra pressure that puts on mbedTLS/AIS.
  xTaskCreatePinnedToCore(networkTask, "network", NETWORK_TASK_STACK_BYTES, nullptr, 1, nullptr, 0);
}

void loop() {
#ifdef DISPLAY_DIAGNOSTIC
  static uint8_t colour = 0;
  static uint32_t nextChange = 0;
  if (millis() >= nextChange) {
    const uint16_t colours[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_WHITE};
    gfx->fillScreen(colours[colour++ & 3]);
    nextChange = millis() + 2000;
  }
  delay(20);
  return;
#endif
  // The cross-core force-close that used to live here is gone: it was the
  // cause of the corrupted-heap failures (cert verification, BIGNUM/SSL
  // allocation) and watchdog aborts that followed every stall, not a cure for
  // them. readResponseBody() now bounds the read on the core that owns the
  // TLS session, so nothing here needs to reach into another core's socket.
  // Everything network-bound now lives in networkTask() on the other core.
  // This loop only ever touches shared data through dataMutex, and even then
  // just for the length of a render call (milliseconds), never for the
  // length of a network request - that's what keeps touch and rendering
  // responsive regardless of what the network is doing.
  if (needsRedraw) {
    needsRedraw = false;
    MutexGuard guard(dataMutex);
    renderCurrentPage();
  }
  // Evaluate both: short-circuiting used to leave bootButtonPending set, which
  // advanced the page twice on the following pass.
  const TouchGesture gesture = touchGesture();
  const bool pressed = bootButtonTapped();
  if (gesture != TouchGesture::None || pressed) {
    lastInteractionAt = millis();
    if (screensaverActive) {
      // Consume this touch as a dismissal only - it shouldn't also advance
      // the page it's returning to, same as the detail-card swallow below.
      screensaverActive = false;
      MutexGuard guard(dataMutex);
      renderCurrentPage();
      delay(15);
      return;
    }
  }
  int pageStep = 0;
  // Swipe right advances Overview -> Table -> Map -> Radar -> Marine and
  // wraps; swipe left walks back. A tap either opens a detail card over the
  // aircraft icon it hit, or - if it missed every icon - advances the page,
  // same as before. The boot button also advances. All of that is swallowed
  // while a detail card is showing: any touch just dismisses it back to
  // whichever page was already active, and it times out on its own too, so
  // it can't be left open indefinitely if nobody taps again.
  if (detailAircraftIndex >= 0) {
    if (gesture != TouchGesture::None || pressed || millis() - detailShownAt > 8000) {
      detailAircraftIndex = -1;
      MutexGuard guard(dataMutex);
      renderCurrentPage();
    }
  } else if (gesture == TouchGesture::SwipeRight) {
    pageStep = 1;
  } else if (gesture == TouchGesture::SwipeLeft) {
    pageStep = -1;
  } else if (displayPage == DisplayPage::Table &&
             (gesture == TouchGesture::SwipeUp || gesture == TouchGesture::SwipeDown)) {
    // Content follows the finger: dragging up brings later rows into view
    // (scroll forward through the list), dragging down goes back toward the
    // nearest aircraft.
    tableScrollOffset += gesture == TouchGesture::SwipeUp ? TABLE_VISIBLE_ROWS : -TABLE_VISIBLE_ROWS;
    tableScrollOffset = constrain(tableScrollOffset, 0, max(0, lastCount - TABLE_VISIBLE_ROWS));
    { MutexGuard guard(dataMutex); renderCurrentPage(); }
  } else if (gesture == TouchGesture::Tap) {
    const int hitIndex = findAircraftIconAt(lastTapX, lastTapY);
    if (hitIndex >= 0) {
      detailAircraftIndex = hitIndex;
      detailShownAt = millis();
      MutexGuard guard(dataMutex);
      renderAircraftDetailCard(hitIndex);
    } else if (displayPage != DisplayPage::Table) {
      pageStep = 1;
    }
    // The Table page plots no icons, so every tap on it missed one and
    // advanced the page - including the near-taps left over from a scroll
    // attempt that did not quite qualify as a swipe. On a page whose whole
    // purpose is to be read and scrolled, that made it feel like the
    // display changed page at random. Taps there now do nothing; the swipes
    // still work and the footer says so.
  } else if (pressed) {
    pageStep = 1;
  }
  if (pageStep) {
    const int pageCount = DISPLAY_PAGE_COUNT;
    displayPage = static_cast<DisplayPage>(
        (static_cast<int>(displayPage) + pageStep + pageCount) % pageCount);
    tableScrollOffset = 0;
    pageSavePending = true;
    pageSaveAt = millis() + 5000UL;
    { MutexGuard guard(dataMutex); renderCurrentPage(); }
    Serial.printf("Page: %s (%s)\n", displayPageName(),
                  gesture == TouchGesture::SwipeLeft    ? "swipe left"
                  : gesture == TouchGesture::SwipeRight ? "swipe right"
                  : gesture == TouchGesture::Tap        ? "tap"
                                                        : "button");
  }
  // Both periodic repaints below check screensaverActive. Without it the
  // radar sweep - which repaints unconditionally every 750 ms - painted
  // straight over the screensaver the moment it appeared, so on the Radar
  // page the screensaver flashed up and vanished, over and over, instead of
  // staying put. The marine page had the same hole on its own timer.
  if (displayPage == DisplayPage::Radar && !screensaverActive && detailAircraftIndex < 0 &&
      static_cast<int32_t>(millis() - nextRadarFrameAt) >= 0) {
    // Full-screen PSRAM copies faster than this can starve the RGB DMA and
    // momentarily wrap the bottom scan lines to the top of the panel.
    radarSweepDegrees += 18.0f;
    if (radarSweepDegrees >= 360.0f) radarSweepDegrees -= 360.0f;
    { MutexGuard guard(dataMutex); renderRadarPage(); }
    nextRadarFrameAt = millis() + 750;
  }
  if (displayPage == DisplayPage::Marine && marineDataDirty && !screensaverActive &&
      detailAircraftIndex < 0 &&
      static_cast<int32_t>(millis() - nextMarineRenderAt) >= 0) {
    marineDataDirty = false;
    { MutexGuard guard(dataMutex); renderMarinePage(); }
    nextMarineRenderAt = millis() + 2000;
  }
  // Screensaver: activate after the configured idle time (no touch/button/
  // web-driven interaction - see lastInteractionAt's updates elsewhere), then
  // rotate which aircraft it shows every few seconds. Interaction handling
  // above already dismisses it and returns early, so reaching here means
  // nothing has touched the panel this pass.
  if (screensaverEnabled && !screensaverActive && detailAircraftIndex < 0 &&
      millis() - lastInteractionAt > screensaverIdleMinutes * 60000UL) {
    screensaverActive = true;
    screensaverNeedsRedraw = true;
    screensaverRefreshAt = millis() + SCREENSAVER_REFRESH_MS;
    MutexGuard guard(dataMutex);
    renderCurrentPage();
  } else if (screensaverActive && static_cast<int32_t>(millis() - screensaverRefreshAt) >= 0) {
    screensaverRefreshAt = millis() + SCREENSAVER_REFRESH_MS;
    // Every repaint clears the full screen - 768 KB of writes across the bus
    // the panel refills its bounce buffers from - so it is a burst of
    // exactly the kind that makes the picture slip. Hence a deliberately
    // slow cadence, and renderScreensaverPage() still compares a signature
    // of everything it draws and returns without touching the framebuffer
    // when the frame would be identical. So this tick costs nothing on a
    // quiet sky and repaints only when a value on screen has actually
    // moved, or when a closer aircraft has taken over the display.
    MutexGuard guard(dataMutex);
    renderCurrentPage();
  }
  delay(15);
}
