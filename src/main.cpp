#include <Arduino.h>

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
constexpr int ROUTE_CACHE_SIZE = 48;
constexpr int MAX_ROUTE_LOOKUPS_PER_REFRESH = 4;
constexpr uint32_t ROUTE_CACHE_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t ROUTE_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr char FIRMWARE_VERSION[] = "2.5.1";
constexpr char DEVICE_HOSTNAME[] = "adsb-map";
constexpr char WEB_USERNAME[] = "admin";
constexpr char GITHUB_OWNER[] = "2E0LXY";
constexpr char GITHUB_REPOSITORY[] = "ESP32-ADS-B";
constexpr char GITHUB_RELEASE_API[] = "https://api.github.com/repos/2E0LXY/ESP32-ADS-B/releases/latest";
constexpr uint32_t UPDATE_CHECK_MS = 6UL * 60UL * 60UL * 1000UL;
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

struct RouteCacheEntry {
  char callsign[9] = {};
  char origin[5] = {};
  char destination[5] = {};
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
enum class DisplayPage : uint8_t { Map = 0, Table = 1, Radar = 2 };
DisplayPage displayPage = DisplayPage::Map;
float radarSweepDegrees = 0.0f;
uint32_t nextRadarFrameAt = 0;
bool touchReady = false;
uint8_t touchAddress = 0;
uint32_t lastTouchAt = 0;
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
uint32_t nextGithubCheckAt = 0;
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
bool openSkyAuthRetryPending = false;
bool pageSavePending = false;
uint32_t pageSaveAt = 0;
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
  http.setTimeout(12000);
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
#else
  // On the CH422G boards the GT911 reset line sits on the expander. The
  // controller is present at 0x5D but the driver is not ported yet, so the
  // reset is issued and the probe deliberately reports no touch.
  WS_CH422G::writePin(BOARD_TOUCH_RST_EXIO, false);
  delay(10);
  WS_CH422G::writePin(BOARD_TOUCH_RST_EXIO, true);
  delay(60);
  return false;
#endif
}

bool touchTapped() {
  if (!touchReady) return false;
  uint8_t statusByte = 0;
  if (!touchReadRegister(0x814E, &statusByte, 1)) return false;
  if ((statusByte & 0x80) == 0) return false;
  const bool hasPoint = (statusByte & 0x0F) > 0;
  touchWriteRegister(0x814E, 0);
  if (!hasPoint || millis() - lastTouchAt < 350) return false;
  lastTouchAt = millis();
  return true;
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
  if (WiFi.status() != WL_CONNECTED) return false;
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
  http.setTimeout(12000);
  const String url = "https://tile.openstreetmap.org/" + String(zoom) + "/" + String(tileX) + "/" + String(tileY) + ".png";
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", userAgent());
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("OSM tile %d/%d/%d HTTP %d\n", zoom, tileX, tileY, code);
    http.end();
    return false;
  }
  File file = cache.open(path, FILE_WRITE);
  const int written = file ? http.writeToStream(&file) : -1;
  if (file) file.close();
  http.end();
  if (written <= 0) {
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
  bool drewTile = false;
  mapRebuildTotal = max(0, (lastTileY - firstTileY + 1) * (lastTileX - firstTileX + 1));
  for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
    if (tileY < 0 || tileY >= tilesPerAxis) { mapRebuildDone += lastTileX - firstTileX + 1; continue; }
    for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
      const int wrappedX = (tileX % tilesPerAxis + tilesPerAxis) % tilesPerAxis;
      const String path = osmTilePath(physicalMapZoom, wrappedX, tileY);
      if (cacheOsmTile(physicalMapZoom, wrappedX, tileY, path)) {
        drewTile |= drawCachedOsmTile(path, tileX * 256 - left, tileY * 256 - top);
      }
      ++mapRebuildDone;
      // Each tile is a separate HTTPS round trip. Service the admin interface
      // between them so the UI stays responsive and can show progress.
      if (webServerReady) webServer.handleClient();
    }
  }
  filledRect(0, H - 15, 17 * 6 + 4, 15, rgb(0, 0, 0));
  text5(3, H - 12, "(C) OPENSTREETMAP", rgb(255, 255, 255));
  memcpy(baseMap, framebuffer, W * H * sizeof(uint16_t));
  physicalMapReady = true;
  mapRebuildActive = false;
  Serial.printf("Physical map %s at %.5f, %.5f radius %u nm zoom %u\n",
                drewTile ? "ready" : "using radar fallback", homeLatitude,
                homeLongitude, queryRadiusNm, physicalMapZoom);
  return drewTile;
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
  http.setTimeout(8000);
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
  DeserializationError error = deserializeJson(tokenDoc, http.getStream());
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

RouteCacheEntry *routeForCallsign(const char *rawCallsign, int &lookupsUsed) {
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

  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient http;
  http.setTimeout(6000);
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
  String payload = http.getString();
  http.end();
  JsonDocument routeDoc;
  if (deserializeJson(routeDoc, payload)) return slot;
  JsonObject route = routeDoc["response"]["flightroute"].as<JsonObject>();
  if (route.isNull()) return slot;
  airportCode(route["origin"].as<JsonObject>(), slot->origin);
  airportCode(route["destination"].as<JsonObject>(), slot->destination);
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

void drawAdsbLogo(int x, int y, float heading, const char *flight, const char *hex) {
  char code[4] = {'?','?','?',0};
  const char *src = (flight && strlen(flight)>=3) ? flight : hex;
  for (int i=0;i<3 && src && src[i];++i) code[i]=toupper(static_cast<unsigned char>(src[i]));
  uint16_t brand=operatorColour(code), white=rgb(255,255,255), dark=rgb(5,15,20);
  disc(x,y,11,dark); disc(x,y,10,brand);
  text5(x-8,y-3,code,white,1);
  float a=radians(heading-90.0f);
  int nx=x+lroundf(cosf(a)*15), ny=y+lroundf(sinf(a)*15);
  line(x,y,nx,ny,white);
  disc(nx,ny,2,white);
}

void drawOperatorBadge(int x, int y, const char *flight, const char *hex) {
  char code[4] = {'?','?','?',0};
  const char *src = (flight && strlen(flight)>=3) ? flight : hex;
  for (int i=0; i<3 && src && src[i]; ++i) code[i]=toupper(static_cast<unsigned char>(src[i]));
  disc(x,y,11,rgb(5,15,20));
  disc(x,y,10,operatorColour(code));
  text5(x-8,y-3,code,rgb(255,255,255));
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

void renderMapPage() {
  restoreMap();
  for (int i=0; i<lastCount; ++i) {
    AircraftDisplay &display = latestAircraft[i];
    if (display.x < 0 || display.x >= W || display.y < 0 || display.y >= H) continue;
    if (display.positionSource == 2) {
      drawMlatPlane(display.x,display.y,display.track);
    } else {
      drawAdsbLogo(display.x,display.y,display.track,display.flight,display.hex);
      drawRouteLabel(display.x,display.y,cachedRoute(display.flight));
    }
  }
  char count[20];
  if (creditsRemaining >= 0) snprintf(count,sizeof(count),"%d C%ld",lastCount,creditsRemaining);
  else snprintf(count,sizeof(count),"%d",lastCount);
  status(count,rgb(35,210,80));
  present();
}

// Table column origins, expressed against the panel width. The 480 design
// used 2/34/132/184/244/280/354; those ratios are preserved.
constexpr int COL_LOGO = W * 2 / 480;
constexpr int COL_CALLSIGN = W * 34 / 480;
constexpr int COL_MILES = W * 132 / 480;
constexpr int COL_SOURCE = W * 184 / 480;
constexpr int COL_DIR = W * 244 / 480;
constexpr int COL_ALT = W * 280 / 480;
constexpr int COL_ROUTE = W * 354 / 480;

void renderTablePage() {
  filledRect(0,0,W,H,rgb(2,10,18));
  // Columns are fractions of the panel width so the 800px board spreads the
  // table out instead of crowding it into the leftmost 480px.
  text5(layout::centreX - 96,7,"NEAREST AIRCRAFT",rgb(80,220,255),2);
  text5(COL_LOGO,31,"LOGO",rgb(170,190,205));
  text5(COL_CALLSIGN,31,"CALLSIGN",rgb(170,190,205));
  text5(COL_MILES,31,"MILES",rgb(170,190,205));
  text5(COL_SOURCE,31,"SOURCE",rgb(170,190,205));
  text5(COL_DIR,31,"DIR",rgb(170,190,205));
  text5(COL_ALT,31,"ALT FT",rgb(170,190,205));
  text5(COL_ROUTE,31,"FROM TO",rgb(170,190,205));
  line(3,41,W - 4,41,rgb(55,85,105));

  int rows = min(lastCount, 10);
  for (int i=0; i<rows; ++i) {
    AircraftDisplay &display = latestAircraft[i];
    int y=49+i*40;
    char distance[6], altitude[7], routeLabel[11];
    snprintf(distance,sizeof(distance),"%d",static_cast<int>(lroundf(display.distanceMiles)));
    if (display.altitudeFt >= 0) snprintf(altitude,sizeof(altitude),"%d",display.altitudeFt);
    else strcpy(altitude,"--");
    RouteCacheEntry *route=cachedRoute(display.flight);
    if (route && route->hasRoute) snprintf(routeLabel,sizeof(routeLabel),"%s>%s",route->origin,route->destination);
    else strcpy(routeLabel,"---");
    const char *identity=display.flight[0] ? display.flight : display.hex;
    if (display.positionSource == 2) drawMlatPlane(16,y+7,display.track);
    else drawOperatorBadge(16,y+7,display.flight,display.hex);
    text5(COL_CALLSIGN,y,identity,rgb(255,255,255),2);
    text5(COL_MILES,y,distance,rgb(255,220,80),2);
    text5(COL_SOURCE,y,display.positionSource==2 ? "MLAT" : "ADSB",display.positionSource==2 ? rgb(255,65,65) : rgb(60,220,130),2);
    text5(COL_DIR,y,compassDirection(display.track),rgb(255,255,255),2);
    text5(COL_ALT,y,altitude,rgb(255,255,255),2);
    text5(COL_ROUTE,y,routeLabel,rgb(130,210,255),2);
    line(3,y+25,476,y+25,rgb(25,45,60));
  }
  char footer[24];
  if (creditsRemaining >= 0)
    snprintf(footer,sizeof(footer),"%d AIRCRAFT  C%ld",lastCount,creditsRemaining);
  else
    snprintf(footer,sizeof(footer),"%d AIRCRAFT",lastCount);
  text5(layout::centreX - 70,layout::footerY,footer,rgb(130,160,180));
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
    } else if (aircraft.positionSource == 2) {
      drawMlatPlane(x, y, aircraft.track);
    } else {
      drawAdsbLogo(x, y, aircraft.track, aircraft.flight, aircraft.hex);
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

const char *displayPageName() {
  if (displayPage == DisplayPage::Table) return "table";
  if (displayPage == DisplayPage::Radar) return "radar";
  return "map";
}

void renderCurrentPage() {
  if (displayPage == DisplayPage::Table) renderTablePage();
  else if (displayPage == DisplayPage::Radar) renderRadarPage();
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
  http.setTimeout(9000);
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
  const DeserializationError error = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error || !doc["ac"].is<JsonArray>()) {
    Serial.printf("%s JSON %s\n", apiProvider.c_str(), error.c_str());
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
    else routeForCallsign(display.flight, routeLookups);
  }
  if (aircraftAtZeroMiles) beepAlert();
  lastFetchCompletedAt = millis();
  finishFeedAttempt("OK", responseCode);
  renderCurrentPage();
  Serial.printf("Displayed %d aircraft (%d MLAT) from %s\n", lastCount, lastMlat, apiProvider.c_str());
}

void fetchAircraft() {
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
  HTTPClient http; http.setTimeout(7000);
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
  DeserializationError error=deserializeJson(doc,http.getStream());
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
      routeForCallsign(display.flight, routeLookups);
    }
  }
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
  http.setTimeout(12000);
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
  http.setTimeout(12000);
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
  const DeserializationError error = deserializeJson(doc, http.getStream());
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
  http.setTimeout(15000);
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
  http.setTimeout(15000);
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
    if (route && route->hasRoute) item["route"] = String(route->origin) + ">" + route->destination;
    else item["route"] = "";
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
  if (page != "map" && page != "table" && page != "radar") {
    sendMessage(400, "Page must be map, radar or table");
    return;
  }
  displayPage = page == "table" ? DisplayPage::Table :
                page == "radar" ? DisplayPage::Radar : DisplayPage::Map;
  settingsStore.putUChar("display-page", static_cast<uint8_t>(displayPage));
  renderCurrentPage();
  if (displayPage == DisplayPage::Table) sendMessage(200, "Table page selected");
  else if (displayPage == DisplayPage::Radar) sendMessage(200, "Radar page selected");
  else sendMessage(200, "Map page selected");
}

void handleDisplaySettings() {
  if (!requireWebAuthentication()) return;
  if (!requireCsrfToken()) return;
  if (webServer.hasArg("sound")) {
    soundAlerts = webServer.arg("sound") == "1";
    settingsStore.putBool("sound", soundAlerts);
  }
  if (webServer.hasArg("brightness")) {
    long brightness = 0;
    if (!parseStrictLong(webServer.arg("brightness"), brightness) || brightness < 10 || brightness > 100) {
      sendMessage(400, "Brightness must be a whole number from 10 to 100");
      return;
    }
    brightnessPercent = static_cast<uint8_t>(brightness);
    settingsStore.putUChar("brightness", brightnessPercent);
    applyBrightness(brightnessPercent);
  }
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
}

void setup() {
  Serial.begin(115200);
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
  homeLatitude = settingsStore.getFloat("home-lat", DEFAULT_HOME_LAT);
  homeLongitude = settingsStore.getFloat("home-lon", DEFAULT_HOME_LON);
  queryRadiusNm = constrain(settingsStore.getUShort("radius-nm", DEFAULT_RADIUS_NM), 5, 250);
  if (!isfinite(homeLatitude) || homeLatitude < -85.0f || homeLatitude > 85.0f) homeLatitude = DEFAULT_HOME_LAT;
  if (!isfinite(homeLongitude) || homeLongitude < -180.0f || homeLongitude > 180.0f) homeLongitude = DEFAULT_HOME_LON;
  physicalMapZoom = constrain(settingsStore.getUChar("map-zoom", zoomForRadius()), 3, 16);
  displayPage = static_cast<DisplayPage>(constrain(settingsStore.getUChar("display-page", 0), 0, 2));
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
  if (!framebuffer || !baseMap || !latestAircraft) {
    Serial.println("PSRAM display/aircraft buffers unavailable");
    while(true) delay(1000);
  }
  mountSdCard();
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
    refreshPhysicalBaseMap();
    restoreMap();
    status("MAP", rgb(53,169,244));
    present();
  }
  beginWebControl();
  Serial.printf("Web login username: %s\n", WEB_USERNAME);
  fetchAircraft();
  nextFetchAt=millis()+REFRESH_MS;
  nextGithubCheckAt=millis()+15000UL;
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
  webServer.handleClient();
  if (githubInstallPending) {
    githubInstallPending = false;
    installGithubUpdate();
  }
  if (githubCheckPending || static_cast<int32_t>(millis() - nextGithubCheckAt) >= 0) {
    githubCheckPending = false;
    checkGithubUpdate();
    nextGithubCheckAt = millis() + UPDATE_CHECK_MS;
  }
  if (physicalMapRefreshPending) {
    physicalMapRefreshPending = false;
    refreshPhysicalBaseMap();
    renderCurrentPage();
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
  // Evaluate both: short-circuiting used to leave bootButtonPending set, which
  // advanced the page twice on the following pass.
  const bool touched = touchTapped();
  const bool pressed = bootButtonTapped();
  if (touched || pressed) {
    displayPage = static_cast<DisplayPage>((static_cast<uint8_t>(displayPage) + 1) % 3);
    pageSavePending = true;
    pageSaveAt = millis() + 5000UL;
    renderCurrentPage();
    Serial.printf("Page: %s\n", displayPageName());
  }
  if (displayPage == DisplayPage::Radar && static_cast<int32_t>(millis() - nextRadarFrameAt) >= 0) {
    // Full-screen PSRAM copies faster than this can starve the RGB DMA and
    // momentarily wrap the bottom scan lines to the top of the panel.
    radarSweepDegrees += 18.0f;
    if (radarSweepDegrees >= 360.0f) radarSweepDegrees -= 360.0f;
    renderRadarPage();
    nextRadarFrameAt = millis() + 750;
  }
  if (static_cast<int32_t>(millis()-nextFetchAt) >= 0) {
    fetchAircraft();
    if (openSkyAuthRetryPending) nextFetchAt = millis() + 1000UL;
    else if (static_cast<int32_t>(millis()-nextFetchAt) >= 0) nextFetchAt=millis()+REFRESH_MS;
  }
  delay(50);
}
