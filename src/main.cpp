#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

// Board capability macros. Must come before anything that tests them,
// notably the SD backend selection below.
#include "board_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
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

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    PANEL_PIN_DE, PANEL_PIN_VSYNC, PANEL_PIN_HSYNC, PANEL_PIN_PCLK,
    PANEL_PINS_R, PANEL_PINS_G, PANEL_PINS_B,
    PANEL_HSYNC_POLARITY, PANEL_HSYNC_FRONT_PORCH, PANEL_HSYNC_PULSE_WIDTH,
    PANEL_HSYNC_BACK_PORCH,
    PANEL_VSYNC_POLARITY, PANEL_VSYNC_FRONT_PORCH, PANEL_VSYNC_PULSE_WIDTH,
    PANEL_VSYNC_BACK_PORCH,
    PANEL_PCLK_ACTIVE_NEG, PANEL_PCLK_HZ);

#if PANEL_NEEDS_SPI_INIT
// The ST7701 needs an SPI register sequence before its RGB interface works.
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    PANEL_WIDTH, PANEL_HEIGHT, rgbpanel, PANEL_ROTATION, true,
    bus, GFX_NOT_DEFINED, st7701_type1_init_operations,
    sizeof(st7701_type1_init_operations));
#else
// The ST7262 is a plain RGB driver with no configuration bus.
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    PANEL_WIDTH, PANEL_HEIGHT, rgbpanel, PANEL_ROTATION, true);
#endif

#if !ADSB_TLS_INSECURE
// Declared at global scope on purpose: an unnamed namespace would give these
// internal linkage and the asm-labelled bundle symbols would not resolve.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");
#endif

namespace {
// boot_asset.h and map_asset.h are generated at 480x480.
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
constexpr char FIRMWARE_VERSION[] = "2.5.1";
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
  uint32_t resolvedAt = 0;
  uint32_t lastUsed = 0;
  bool occupied = false;
  bool hasRoute = false;
};

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
PNG pngDecoder;
int pngTileScreenX = 0;
int pngTileScreenY = 0;
uint32_t nextFetchAt = 0;
uint32_t tokenExpiresAt = 0;
String bearerToken;
int lastCount = 0;
int lastMlat = 0;
long creditsRemaining = -1;
RouteCacheEntry routeCache[ROUTE_CACHE_SIZE];
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
bool soundAlerts = true;
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
// HTTPClient::writeToStreamDataBlock() (arduino-esp32 3.3.11) has no overall
// deadline of its own once headers are in: its body-read loop is just
// `while (connected()) { if (available()) read-and-copy; else delay(1); }`
// forever, with no millis()-based timeout unlike the header-reading loop
// above it. If a provider accepts the connection, sends a partial response,
// then stalls without closing the socket - a dead NAT/firewall state is
// enough, no cooperation from the far end required - that loop spins on
// core 0 until the task watchdog panics and reboots the whole board. That
// is the exact, source-confirmed cause of every "IDLE0 starved on CPU 0:
// network" reboot logged in this project: every backtrace bottoms out in
// this one loop, just caught at whichever inner call happened to be running
// when the watchdog sampled it. It can't be fixed by any HTTPClient/
// WiFiClientSecure timeout setter - none of them are consulted here - and
// rewriting the body read ourselves would lose HTTPClient's own chunked-
// transfer decoding (see PsramSink's comment above on why that matters).
// The only lever available from outside the vendored library is closing
// the underlying socket out from under it: loop() on core 1 is never
// blocked by this (every crash log shows CPU 1/IDLE1 running fine), so it
// polls activeFetchDeadlineMs and force-stops activeFetchClient if a body
// read has made no progress in that long, which unblocks the stuck read
// with a clean error instead of a 60-second hang ending in a full reboot.
// The deadline is an idle timeout, not a cap on the whole transfer: it is
// pushed forward every time activeFetchBody's size actually grows, so a
// big-but-healthy response (more aircraft, more route lookups already
// cached) isn't punished for simply taking a while - only a transfer that
// goes completely quiet gets force-closed. An earlier version used a flat
// deadline for the entire read and ended up killing normal slow transfers
// on almost every cycle, which is worse than the rare genuine stall it was
// meant to catch.
NetworkClientSecure *volatile activeFetchClient = nullptr;
PsramSink *volatile activeFetchBody = nullptr;
volatile size_t activeFetchLastSeenSize = 0;
volatile uint32_t activeFetchDeadlineMs = 0;
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
constexpr int SWIPE_MIN_PIXELS = 70;
constexpr uint32_t SWIPE_MAX_MS = 700;

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

  int x = 0, y = 0;
  const bool contact = touchPoint(x, y);

  if (contact) {
    if (!down) {
      down = true;
      startX = lastX = x;
      startY = lastY = y;
      startedAt = millis();
    } else {
      lastX = x;
      lastY = y;
    }
    return TouchGesture::None;
  }

  if (!down) return TouchGesture::None;
  down = false;
  const uint32_t heldFor = millis() - startedAt;
  const int deltaX = lastX - startX;
  const int deltaY = lastY - startY;
  if (millis() - lastTouchAt < 350) return TouchGesture::None;
  lastTouchAt = millis();
  if (heldFor <= SWIPE_MAX_MS && abs(deltaX) >= SWIPE_MIN_PIXELS &&
      abs(deltaX) > abs(deltaY) * 2) {
    return deltaX < 0 ? TouchGesture::SwipeLeft : TouchGesture::SwipeRight;
  }
  // A vertical drag that missed a clean horizontal swipe used to fall all
  // the way through to a tap, which - on any page where the release point
  // didn't land on an aircraft icon - advanced the page exactly like a
  // horizontal swipe would. Table scrolling needs this recognised as its
  // own gesture instead.
  if (heldFor <= SWIPE_MAX_MS && abs(deltaY) >= SWIPE_MIN_PIXELS &&
      abs(deltaY) > abs(deltaX) * 2) {
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
  static const uint8_t blank[5] = {};
  static const uint8_t greater[5] = {0x41,0x22,0x14,0x08,0x00};
  static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
  if (ch >= '0' && ch <= '9') return chars[ch-'0'];
  if (ch >= 'A' && ch <= 'Z') return chars[10+ch-'A'];
  if (ch == '>') return greater;
  if (ch == '-') return dash;
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

bool drawCachedOsmTile(const String &path, int screenX, int screenY) {
  fs::FS &cache = sdMounted ? static_cast<fs::FS &>(SDCARD) : static_cast<fs::FS &>(LittleFS);
  File file = cache.open(path, FILE_READ);
  if (!file) return false;
  const size_t size = file.size();
  uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!data) { file.close(); return false; }
  const size_t read = file.read(data, size);
  file.close();
  if (read != size) { heap_caps_free(data); return false; }
  pngTileScreenX = screenX;
  pngTileScreenY = screenY;
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

RouteCacheEntry *routeForCallsign(const char *rawCallsign, int &lookupsUsed,
                                  WiFiClientSecure &client, HTTPClient &http) {
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

void drawOperatorBadge(int x, int y, const char *flight, const char *hex) {
  char code[4] = {'?','?','?',0};
  const char *src = (flight && strlen(flight)>=3) ? flight : hex;
  for (int i=0; i<3 && src && src[i]; ++i) code[i]=toupper(static_cast<unsigned char>(src[i]));
  disc(x,y,11,rgb(5,15,20));
  disc(x,y,10,operatorColour(code));
  text5(x-8,y-3,code,rgb(255,255,255));
}

// The ADS-B emitter category (A0-A7/B0-B7/C0-C7) picks a recognisable
// silhouette instead of every aircraft drawing as the same blob; an empty or
// unrecognised category (common on feeds that omit it) falls back to the
// airliner shape since that's the majority of what's actually in the air.
enum class PlaneShape : uint8_t { Jet, Light, Helicopter, Military };

PlaneShape planeShapeForCategory(const char *category) {
  if (!category || !category[0]) return PlaneShape::Jet;
  if (!strcmp(category, "A7")) return PlaneShape::Helicopter;
  if (!strcmp(category, "A6")) return PlaneShape::Military;
  if (!strcmp(category, "A1") || !strcmp(category, "A2") ||
      !strcmp(category, "B1") || !strcmp(category, "B2") ||
      !strcmp(category, "B3") || !strcmp(category, "B4")) return PlaneShape::Light;
  return PlaneShape::Jet;
}

// Every shape is built from filledTriangle()/disc() - solid, not outlined -
// so it actually reads as a plane silhouette on the panel instead of a faint
// wireframe; a thin line() outline (the original version of this function)
// all but disappeared against the map at normal viewing distance.
void drawPlaneIcon(int x, int y, float heading, PlaneShape shape, uint16_t colour) {
  const float a = radians(heading - 90.0f), cs = cosf(a), sn = sinf(a);
  auto tx = [&](float px, float py) { return x + lroundf(px * cs - py * sn); };
  auto ty = [&](float px, float py) { return y + lroundf(px * sn + py * cs); };
  const uint16_t white = rgb(255, 255, 255);
  switch (shape) {
    case PlaneShape::Helicopter: {
      // The rotor spins independent of track; a short tail boom still shows
      // which way the aircraft is actually heading. A real rotor blade is
      // thin, so this is the one shape that stays as lines (doubled for
      // weight) rather than a fill.
      static float rotorAngle = 0.0f;
      rotorAngle += 35.0f;
      if (rotorAngle >= 360.0f) rotorAngle -= 360.0f;
      const float ra = radians(rotorAngle);
      const int r1x = x + lroundf(cosf(ra) * 13), r1y = y + lroundf(sinf(ra) * 13);
      const int r2x = x - lroundf(cosf(ra) * 13), r2y = y - lroundf(sinf(ra) * 13);
      disc(x, y, 4, colour);
      line(r1x, r1y, r2x, r2y, colour);
      line(r1x, r1y + 1, r2x, r2y + 1, colour);
      line(x, y, tx(-11, 0), ty(-11, 0), colour);
      pixel(x, y, white);
      break;
    }
    case PlaneShape::Military:
      // Narrower and more sharply swept than the standard jet silhouette.
      filledTriangle(tx(13,0), ty(13,0), tx(-6,-4), ty(-6,-4), tx(-10,0), ty(-10,0), colour);
      filledTriangle(tx(13,0), ty(13,0), tx(-10,0), ty(-10,0), tx(-6,4), ty(-6,4), colour);
      pixel(x, y, white);
      break;
    case PlaneShape::Light:
      // Straight, unswept wings crossing a slim fuselage - a small prop
      // aircraft, not the swept dart shape used for everything else.
      filledTriangle(tx(10,0), ty(10,0), tx(-10,-2), ty(-10,-2), tx(-10,2), ty(-10,2), colour);
      filledTriangle(tx(2,0), ty(2,0), tx(0,-8), ty(0,-8), tx(-2,0), ty(-2,0), colour);
      filledTriangle(tx(2,0), ty(2,0), tx(-2,0), ty(-2,0), tx(0,8), ty(0,8), colour);
      pixel(x, y, white);
      break;
    case PlaneShape::Jet:
    default:
      // Swept-wing airliner dart - the same outline every icon used to draw,
      // now filled solid via its two diagonal-split triangles.
      filledTriangle(tx(12,0), ty(12,0), tx(-9,-5), ty(-9,-5), tx(-5,0), ty(-5,0), colour);
      filledTriangle(tx(12,0), ty(12,0), tx(-5,0), ty(-5,0), tx(-9,5), ty(-9,5), colour);
      pixel(x, y, white);
      break;
  }
}

// Replaces the old drawAdsbLogo/drawMlatPlane pair: shape identifies the
// aircraft type from its ADS-B category, colour flags MLAT (estimated,
// non-ADS-B) position in red the same way the table's A/M source column
// already does, and falls back to the operator's brand colour otherwise.
void drawAircraftIcon(int x, int y, float heading, const char *flight,
                       const char *hex, const char *category, bool isMlat) {
  char code[4] = {'?','?','?',0};
  const char *src = (flight && strlen(flight) >= 3) ? flight : hex;
  for (int i = 0; i < 3 && src && src[i]; ++i) code[i] = toupper(static_cast<unsigned char>(src[i]));
  const uint16_t colour = isMlat ? rgb(245, 30, 35) : operatorColour(code);
  drawPlaneIcon(x, y, heading, planeShapeForCategory(category), colour);
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
  gfx->draw16bitRGBBitmap(0, 0, framebuffer, W, H);
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
  if (W == ASSET_W && H == ASSET_H) {
    gfx->draw16bitRGBBitmap(0, 0, const_cast<uint16_t *>(BOOT_IMAGE), W, H);
  } else {
    // Centre the 480x480 splash rather than overrunning the array.
    gfx->fillScreen(rgb(4, 10, 16));
    gfx->draw16bitRGBBitmap((W - ASSET_W) / 2, (H - ASSET_H) / 2,
                            const_cast<uint16_t *>(BOOT_IMAGE), ASSET_W, ASSET_H);
  }
  gfx->setTextWrap(false);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
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
    drawAircraftIcon(display.x, display.y, display.track, display.flight, display.hex,
                      display.category, display.positionSource == 2);
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
    drawAircraftIcon(display.x, display.y, display.track, display.flight, display.hex,
                      display.category, display.positionSource == 2);
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
  char footer[36];
  const char *scrollHint = (lastCount > TABLE_VISIBLE_ROWS) ? " - SWIPE UP/DOWN" : "";
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
      drawAircraftIcon(x, y, aircraft.track, aircraft.flight, aircraft.hex,
                        aircraft.category, aircraft.positionSource == 2);
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

const char *displayPageName() {
  if (displayPage == DisplayPage::Overview) return "overview";
  if (displayPage == DisplayPage::Table) return "table";
  if (displayPage == DisplayPage::Radar) return "radar";
  if (displayPage == DisplayPage::Marine) return "marine";
  return "map";
}

void renderCurrentPage() {
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
  String url;
  const String latitude = String(homeLatitude, 5);
  const String longitude = String(homeLongitude, 5);
  const String radius = String(queryRadiusNm);
  if (apiProvider == "adsbfi") {
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
  }
  const int code = http.GET();
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

  JsonDocument filter;
  JsonObject aircraftFilter = filter["ac"][0].to<JsonObject>();
  const char *fields[] = {"lat", "lon", "track", "true_heading", "mag_heading",
                          "alt_baro", "alt_geom", "gs", "baro_rate", "geom_rate",
                          "seen", "rssi", "messages", "flight", "hex", "r", "t",
                          "squawk", "category", "ownOp", "cou", "emergency", "mlat"};
  for (const char *field : fields) aircraftFilter[field] = true;
  JsonDocument doc(&psramJsonAllocator);
  PsramSink body;
  // See the activeFetchClient/activeFetchDeadlineMs comment above
  // feedStatus's declaration: this is the one call in the whole file
  // confirmed to have caused every watchdog reboot logged so far, so it's
  // the one wrapped in the external force-stop deadline.
  activeFetchLastSeenSize = 0;
  activeFetchBody = &body;
  activeFetchDeadlineMs = millis() + 15000UL;
  activeFetchClient = &client;
  http.writeToStream(&body);
  activeFetchClient = nullptr;
  activeFetchBody = nullptr;
  const DeserializationError error = deserializeJson(
      doc, body.data(), body.size(), DeserializationOption::Filter(filter));
  http.end();
  if (error || !doc["ac"].is<JsonArray>()) {
    Serial.printf("%s JSON %s (%u bytes)\n", apiProvider.c_str(), error.c_str(),
                  static_cast<unsigned>(body.size()));
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
    strncpy(display.country, aircraft["cou"] | "", sizeof(display.country) - 1);
    strncpy(display.emergency, aircraft["emergency"] | "none", sizeof(display.emergency) - 1);
    JsonArray mlatFields = aircraft["mlat"].as<JsonArray>();
    display.positionSource = !mlatFields.isNull() && mlatFields.size() ? 2 : 0;
    ++lastCount;
  }
  }
  sortAircraftByDistance();
  int routeLookups = 0;
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
      routeForCallsign(display.flight, routeLookups, routeClient, routeHttp);
      routeHttp.end();
      routeClient.stop();
      if (webServerReady) webServer.handleClient();
      // Nudges the panel to resync mid-loop against PSRAM-DMA starvation -
      // see the tile-rebuild loop's comment on restartAtNextVsync() above.
      rgbpanel->restartAtNextVsync();
    }
  }
  logHeapDiagnostics("fetch-end");
  if (routeLookups > 0) saveRouteCacheToStorage();
  if (aircraftAtZeroMiles) beepAlert();
  lastFetchCompletedAt = millis();
  finishFeedAttempt("OK", responseCode);
  renderCurrentPage();
  Serial.printf("Displayed %d aircraft (%d MLAT) from %s\n", lastCount, lastMlat, apiProvider.c_str());
}

void fetchAircraft() {
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
    if (!state[17].isNull()) snprintf(display.category, sizeof(display.category), "C%d", state[17].as<int>());
    strcpy(display.emergency, "none");
    ++lastCount;
  }
  sortAircraftByDistance();
  doc.clear();
  }
  int routeLookups = 0;
  for (int i=0; i<lastCount; ++i) {
    AircraftDisplay &display = latestAircraft[i];
    if (display.positionSource == 2) {
      ++lastMlat;
    } else {
      if (webServerReady) webServer.handleClient();
      // Nudges the panel to resync mid-loop against PSRAM-DMA starvation -
      // see the tile-rebuild loop's comment on restartAtNextVsync() above.
      rgbpanel->restartAtNextVsync();
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
      routeForCallsign(display.flight, routeLookups, routeClient, routeHttp);
      routeHttp.end();
      routeClient.stop();
      if (webServerReady) webServer.handleClient();
      // Nudges the panel to resync mid-loop against PSRAM-DMA starvation -
      // see the tile-rebuild loop's comment on restartAtNextVsync() above.
      rgbpanel->restartAtNextVsync();
    }
  }
  logHeapDiagnostics("fetch-end");
  if (routeLookups > 0) saveRouteCacheToStorage();
  if (aircraftAtZeroMiles) beepAlert();
  lastFetchCompletedAt = millis();
  finishFeedAttempt("OK", HTTP_CODE_OK);
  renderCurrentPage();
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
    item["squawk"] = display.squawk;
    item["category"] = display.category;
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
      provider != "adsbone" && provider != "adsbx") {
    sendMessage(400, "Unknown aircraft data provider");
    return;
  }
  if (webServer.arg("clientId").length() > 128 ||
      webServer.arg("clientSecret").length() > 256 ||
      webServer.arg("rapidApiKey").length() > 256) {
    sendMessage(400, "API credential fields are too long");
    return;
  }
  if (webServer.arg("clear") == "1") {
    openSkyClientId = "";
    openSkyClientSecret = "";
    rapidApiKey = "";
    settingsStore.putString("os-client", "");
    settingsStore.putString("os-secret", "");
    settingsStore.putString("rapid-key", "");
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
    if (webServer.arg("upload") != "1") {
      firmwareUploadError = "Firmware upload request is missing its upload marker";
      return;
    }
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
      sendMessage(202, "Setup portal will start as ADSBMAP");
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
      wm.startConfigPortal("ADSBMAP", "aircraft");
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
      if (pauseAis) { aisIntentionalDisconnect = true; aisWebSocket.disconnect(); }
      { MutexGuard guard(dataMutex); fetchAircraft(); }
      // This was a known-good, already-connected session we paused ourselves,
      // not a failure - reconnect immediately rather than waiting on the
      // failure backoff, which doesn't apply here.
      if (pauseAis) connectAisWebSocket();
      Serial.printf("fetchAircraft blocked the network task for %lu ms\n",
                    static_cast<unsigned long>(millis() - fetchStartedAt));
      if (openSkyAuthRetryPending) nextFetchAt = millis() + 1000UL;
      else if (static_cast<int32_t>(millis() - nextFetchAt) >= 0) nextFetchAt = millis() + REFRESH_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
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
      apiProvider != "adsbone" && apiProvider != "adsbx") apiProvider = "opensky";
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
  brightnessPercent = settingsStore.getUChar("brightness", 100);
  brightnessPercent = constrain(brightnessPercent, 10, 100);
  generateCsrfToken();
  pinMode(0, INPUT_PULLUP);
#if BOARD_HAS_BOOT_BUTTON
  attachInterrupt(digitalPinToInterrupt(0), onBootButtonFalling, FALLING);
#endif  // GPIO 0 is an RGB data line on the 800x480 boards
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
  renderBootScreen();
  delay(2800);
  framebuffer=(uint16_t*)heap_caps_malloc(W*H*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
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
  if (!wm.autoConnect("ADSBMAP", "aircraft")) {
    renderBootScreen("Wi-Fi failed - setup required", rgb(255,65,65));
    delay(5000);
    restoreMap(); status("WIFI",rgb(245,30,35)); present();
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
  // Last-resort rescue for the writeToStreamDataBlock() stall documented by
  // activeFetchClient's declaration: networkTask() on core 0 can be stuck
  // spinning inside that vendored loop with no way to feed the watchdog
  // itself, but this loop on core 1 is never blocked by it, so it's the one
  // place that can still notice and act. Deliberately does not take
  // dataMutex - networkTask holds it for the entire stuck fetch, so waiting
  // for it here would just add a second stuck task.
  if (activeFetchClient) {
    // Any growth in the buffered body is forward progress - a big response
    // taking a while is not the same failure as one that has gone silent.
    const size_t currentSize = activeFetchBody ? activeFetchBody->size() : 0;
    if (currentSize != activeFetchLastSeenSize) {
      activeFetchLastSeenSize = currentSize;
      activeFetchDeadlineMs = millis() + 15000UL;
    } else if (static_cast<int32_t>(millis() - activeFetchDeadlineMs) >= 0) {
      Serial.println("Aircraft fetch body read stalled with no new data for 15s; force-closing the socket");
      activeFetchClient->stop();
      activeFetchDeadlineMs = millis() + 15000UL;
    }
  }
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
    } else {
      pageStep = 1;
    }
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
  if (displayPage == DisplayPage::Radar && detailAircraftIndex < 0 &&
      static_cast<int32_t>(millis() - nextRadarFrameAt) >= 0) {
    // Full-screen PSRAM copies faster than this can starve the RGB DMA and
    // momentarily wrap the bottom scan lines to the top of the panel.
    radarSweepDegrees += 18.0f;
    if (radarSweepDegrees >= 360.0f) radarSweepDegrees -= 360.0f;
    { MutexGuard guard(dataMutex); renderRadarPage(); }
    nextRadarFrameAt = millis() + 750;
  }
  if (displayPage == DisplayPage::Marine && marineDataDirty && detailAircraftIndex < 0 &&
      static_cast<int32_t>(millis() - nextMarineRenderAt) >= 0) {
    marineDataDirty = false;
    { MutexGuard guard(dataMutex); renderMarinePage(); }
    nextMarineRenderAt = millis() + 2000;
  }
  delay(15);
}
