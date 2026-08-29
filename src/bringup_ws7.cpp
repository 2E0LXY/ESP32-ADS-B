// Panel bring-up / timing sweep for Waveshare ESP32-S3-Touch-LCD-7 and -4.3.
//
// Press the RESET button to advance to the next configuration. The active
// config index survives reset in RTC memory, so a full power cycle returns to
// config 0. The current config is drawn large at the top of the panel so it is
// legible in a photograph even when the geometry is wrong.
//
// Build: pio run -e ws_lcd_7_bringup

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

namespace {

constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;

constexpr uint8_t CH422G_WR_SET = 0x48 >> 1;  // 0x24
constexpr uint8_t CH422G_WR_IO = 0x70 >> 1;   // 0x38
constexpr uint8_t CH422G_IO_OE = 1 << 0;
constexpr uint8_t EXIO_BACKLIGHT_OFF = 0xFB;
constexpr uint8_t EXIO_BACKLIGHT_ON = 0xFF;

constexpr int PANEL_W = 800;
constexpr int PANEL_H = 480;

struct PanelConfig {
  int32_t pclkHz;
  uint16_t pclkActiveNeg;
  uint16_t hfp, hpw, hbp;
  uint16_t vfp, vpw, vbp;
};

// Config 0 is the vendor value from BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h.
// The rest vary one axis at a time: first the pixel clock (bandwidth), then
// the clock edge, then larger blanking intervals typical of 7" RGB panels.
const PanelConfig CONFIGS[] = {
    // Measured from the panel: it clocks out ~529 lines per frame, but the
    // vendor header only supplies 500 (480 + 8/4/8), so 29 lines of the next
    // frame appear at the bottom. Horizontal geometry was already correct, so
    // H stays at the vendor 820 and only the vertical blanking grows to 49.
    {16000000L, 1, 8, 4, 8, 22, 4, 23},      // 0 measured fix, 36.9 Hz
};
constexpr int CONFIG_COUNT = sizeof(CONFIGS) / sizeof(CONFIGS[0]);

int configIndex = -1;  // RESET clears RTC memory on this chip

Arduino_ESP32RGBPanel *rgbpanel = nullptr;
Arduino_RGB_Display *gfx = nullptr;

bool writeExpander(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t waitUntil = millis() + 1500;
  while (!Serial && millis() < waitUntil) delay(10);

  configIndex = (configIndex + 1) % CONFIG_COUNT;
  const PanelConfig &cfg = CONFIGS[configIndex];

  const long hTotal = PANEL_W + cfg.hfp + cfg.hpw + cfg.hbp;
  const long vTotal = PANEL_H + cfg.vfp + cfg.vpw + cfg.vbp;
  const double refresh = (double)cfg.pclkHz / (double)(hTotal * vTotal);

  Serial.println();
  Serial.printf("=== config %d of %d ===\n", configIndex, CONFIG_COUNT - 1);
  Serial.printf("PCLK %ld Hz, active_neg %u\n", (long)cfg.pclkHz, cfg.pclkActiveNeg);
  Serial.printf("H %d + %u/%u/%u = %ld   V %d + %u/%u/%u = %ld\n", PANEL_W,
                cfg.hfp, cfg.hpw, cfg.hbp, hTotal, PANEL_H, cfg.vfp, cfg.vpw,
                cfg.vbp, vTotal);
  Serial.printf("refresh %.1f Hz\n", refresh);
  Serial.println("Press RESET to advance to the next config.");

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(20);
  writeExpander(CH422G_WR_SET, CH422G_IO_OE);
  writeExpander(CH422G_WR_IO, EXIO_BACKLIGHT_OFF);
  delay(120);

  rgbpanel = new Arduino_ESP32RGBPanel(
      5 /*DE*/, 3 /*VSYNC*/, 46 /*HSYNC*/, 7 /*PCLK*/,
      1, 2, 42, 41, 40,       // R0-R4
      39, 0, 45, 48, 47, 21,  // G0-G5
      14, 38, 18, 17, 10,     // B0-B4
      1, cfg.hfp, cfg.hpw, cfg.hbp,
      1, cfg.vfp, cfg.vpw, cfg.vbp,
      cfg.pclkActiveNeg, cfg.pclkHz);
  gfx = new Arduino_RGB_Display(PANEL_W, PANEL_H, rgbpanel, 0, true);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() FAILED");
    return;
  }
  Serial.println("gfx->begin() ok");

  gfx->fillScreen(RGB565_BLACK);

  // Colour bars, rows 0-99. Order proves the RGB data pin mapping.
  const uint16_t bars[] = {RGB565_RED,    RGB565_GREEN,  RGB565_BLUE,
                           RGB565_WHITE,  RGB565_YELLOW, RGB565_CYAN,
                           RGB565_MAGENTA, RGB565_BLACK};
  for (int i = 0; i < 8; ++i) {
    gfx->fillRect(i * (PANEL_W / 8), 0, PANEL_W / 8, 100, bars[i]);
  }

  // Labelled rulers. If the panel geometry is correct each label sits on its
  // line and ROW 479 is the very last visible row.
  const int rows[] = {120, 240, 360, 479};
  for (int i = 0; i < 4; ++i) {
    gfx->drawFastHLine(0, rows[i], PANEL_W, RGB565_WHITE);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(PANEL_W - 150, rows[i] - 20);
    gfx->printf("ROW %d", rows[i]);
  }

  gfx->drawRect(0, 0, PANEL_W, PANEL_H, RGB565_WHITE);
  gfx->fillRect(0, PANEL_H - 40, 40, 40, RGB565_GREEN);
  gfx->fillRect(PANEL_W - 40, PANEL_H - 40, 40, 40, RGB565_GREEN);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(4);
  gfx->setCursor(20, 140);
  gfx->printf("CONFIG %d / %d", configIndex, CONFIG_COUNT - 1);
  gfx->setTextSize(3);
  gfx->setCursor(20, 190);
  gfx->printf("PCLK %ld MHz  NEG %u", (long)(cfg.pclkHz / 1000000), cfg.pclkActiveNeg);
  gfx->setCursor(20, 230);
  gfx->printf("H %ld  V %ld  %.1f Hz", hTotal, vTotal, refresh);
  gfx->setTextSize(2);
  gfx->setCursor(20, 280);
  gfx->print("Press RESET for next config");
  gfx->setCursor(20, 310);
  gfx->print("Good = ROW 479 line at very bottom,");
  gfx->setCursor(20, 335);
  gfx->print("green squares in bottom corners, no repeat");

  delay(400);
  writeExpander(CH422G_WR_IO, EXIO_BACKLIGHT_ON);
  Serial.println("Backlight on, pattern drawn.");
}

void loop() {
  static uint32_t next = 0;
  if (millis() >= next) {
    next = millis() + 5000;
    Serial.printf("config %d alive %lus  heap %u\n", configIndex,
                  millis() / 1000, ESP.getFreeHeap());
  }
  delay(50);
}
