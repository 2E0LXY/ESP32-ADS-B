// Panel bring-up for the 800x480 Waveshare boards, now driven entirely from
// src/board_config.h and the WS_CH422G driver. Nothing here hardcodes a pin,
// a timing or an expander register: if this renders correctly then the board
// abstraction is correct on hardware, not just in a build.
//
// Build: pio run -e ws_lcd_7_bringup

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

#include "WS_CH422G.h"
#include "board_config.h"

namespace {

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    PANEL_PIN_DE, PANEL_PIN_VSYNC, PANEL_PIN_HSYNC, PANEL_PIN_PCLK,
    PANEL_PINS_R, PANEL_PINS_G, PANEL_PINS_B,
    PANEL_HSYNC_POLARITY, PANEL_HSYNC_FRONT_PORCH, PANEL_HSYNC_PULSE_WIDTH,
    PANEL_HSYNC_BACK_PORCH,
    PANEL_VSYNC_POLARITY, PANEL_VSYNC_FRONT_PORCH, PANEL_VSYNC_PULSE_WIDTH,
    PANEL_VSYNC_BACK_PORCH,
    PANEL_PCLK_ACTIVE_NEG, PANEL_PCLK_HZ);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    layout::W, layout::H, rgbpanel, PANEL_ROTATION, true);

void scanI2C() {
  int found = 0;
  Serial.println("I2C scan:");
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X\n", address);
      ++found;
    }
  }
  Serial.printf("  %d device(s)\n", found);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t waitUntil = millis() + 1500;
  while (!Serial && millis() < waitUntil) delay(10);

  const long hTotal = layout::W + PANEL_HSYNC_FRONT_PORCH +
                      PANEL_HSYNC_PULSE_WIDTH + PANEL_HSYNC_BACK_PORCH;
  const long vTotal = layout::H + PANEL_VSYNC_FRONT_PORCH +
                      PANEL_VSYNC_PULSE_WIDTH + PANEL_VSYNC_BACK_PORCH;
  const double refresh = (double)PANEL_PCLK_HZ / (double)(hTotal * vTotal);

  Serial.println();
  Serial.printf("=== %s ===\n", BOARD_NAME);
  Serial.printf("Panel %dx%d  H %ld  V %ld  %.1f Hz\n", layout::W, layout::H,
                hTotal, vTotal, refresh);
  Serial.printf("layout: centre %d,%d  scale %.3f  radarRadius %d  footerY %d\n",
                layout::centreX, layout::centreY, layout::scale,
                layout::radarRadius, layout::footerY);
  Serial.printf("backlight PWM capable: %s\n",
                BOARD_HAS_BACKLIGHT_PWM ? "yes" : "no (switch only)");

  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL, 400000);
  delay(20);
  scanI2C();

  const bool expanderReady = WS_CH422G::begin(Wire, BOARD_I2C_SDA,
                                              BOARD_I2C_SCL, 400000, &Serial);
  Serial.printf("CH422G begin: %s, shadow 0x%02X\n",
                expanderReady ? "ok" : "FAILED", WS_CH422G::shadow());

  // Prove single-pin control leaves the undocumented pins untouched.
  WS_CH422G::setBacklight(false);
  Serial.printf("backlight off, shadow 0x%02X\n", WS_CH422G::shadow());
  delay(120);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() FAILED");
    return;
  }
  Serial.println("gfx->begin() ok");

  gfx->fillScreen(RGB565_BLACK);

  const uint16_t bars[] = {RGB565_RED,     RGB565_GREEN,  RGB565_BLUE,
                           RGB565_WHITE,   RGB565_YELLOW, RGB565_CYAN,
                           RGB565_MAGENTA, RGB565_BLACK};
  for (int i = 0; i < 8; ++i) {
    gfx->fillRect(i * (layout::W / 8), 0, layout::W / 8, layout::scaled(100),
                  bars[i]);
  }

  // Derived furniture. If the layout namespace is right these land sensibly on
  // any panel size without a second set of coordinates.
  gfx->drawCircle(layout::centreX, layout::centreY, layout::radarRadius,
                  RGB565_DARKGREY);
  gfx->drawCircle(layout::centreX, layout::centreY, layout::radarRadius / 2,
                  RGB565_DARKGREY);
  gfx->drawFastHLine(0, layout::footerY, layout::W, RGB565_WHITE);
  gfx->drawRect(0, 0, layout::W, layout::H, RGB565_WHITE);
  gfx->fillRect(0, layout::H - 40, 40, 40, RGB565_GREEN);
  gfx->fillRect(layout::W - 40, layout::H - 40, 40, 40, RGB565_GREEN);
  gfx->drawFastHLine(0, layout::H - 1, layout::W, RGB565_WHITE);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(layout::margin * 2, layout::scaled(130));
  gfx->print(BOARD_NAME);
  gfx->setTextSize(2);
  gfx->setCursor(layout::margin * 2, layout::scaled(175));
  gfx->printf("%dx%d  H %ld  V %ld  %.1f Hz", layout::W, layout::H, hTotal,
              vTotal, refresh);
  gfx->setCursor(layout::margin * 2, layout::scaled(200));
  gfx->printf("board_config.h + WS_CH422G driver");
  gfx->setCursor(layout::margin * 2, layout::scaled(225));
  gfx->printf("centre %d,%d  scale %.2f  radius %d", layout::centreX,
              layout::centreY, layout::scale, layout::radarRadius);
  gfx->setCursor(layout::margin * 2, layout::scaled(250));
  gfx->printf("expander %s  backlight %s", expanderReady ? "ok" : "FAILED",
              BOARD_HAS_BACKLIGHT_PWM ? "PWM" : "switch");

  delay(300);
  WS_CH422G::setBacklight(true);
  Serial.printf("backlight on, shadow 0x%02X\n", WS_CH422G::shadow());
  Serial.println("Pattern drawn from board_config.h derived layout.");
}

void loop() {
  static uint32_t next = 0;
  if (millis() >= next) {
    next = millis() + 5000;
    Serial.printf("alive %lus  heap %u  psram %u\n", millis() / 1000,
                  ESP.getFreeHeap(), ESP.getFreePsram());
  }
  delay(50);
}
