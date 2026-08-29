// Board configuration: Waveshare ESP32-S3-Touch-LCD-7 and -4.3 (800x480).
//
// Both boards ship a byte-identical panel definition in the vendor demo, so
// one config covers them. Pins and horizontal timings are taken from
// BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h.
//
// VERTICAL BLANKING IS NOT THE VENDOR VALUE. The header specifies
// VFP 8 / VPW 4 / VBP 8 for a 500 line total, but the panel clocks out ~529
// lines per frame, so 29 lines of the following frame appeared at the bottom
// of the display. Measured on hardware against a labelled row ruler and
// corrected to 49 lines of blanking. Horizontal (820 total) was already right.
#pragma once

#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-7"

// ---- Panel -----------------------------------------------------------------
#define PANEL_WIDTH 800
#define PANEL_HEIGHT 480
#define PANEL_ROTATION 0

#define PANEL_PIN_DE 5
#define PANEL_PIN_VSYNC 3
#define PANEL_PIN_HSYNC 46
#define PANEL_PIN_PCLK 7
#define PANEL_PINS_R 1, 2, 42, 41, 40
#define PANEL_PINS_G 39, 0, 45, 48, 47, 21
#define PANEL_PINS_B 14, 38, 18, 17, 10

#define PANEL_HSYNC_POLARITY 1
#define PANEL_HSYNC_FRONT_PORCH 8
#define PANEL_HSYNC_PULSE_WIDTH 4
#define PANEL_HSYNC_BACK_PORCH 8
#define PANEL_VSYNC_POLARITY 1
#define PANEL_VSYNC_FRONT_PORCH 22
#define PANEL_VSYNC_PULSE_WIDTH 4
#define PANEL_VSYNC_BACK_PORCH 23
#define PANEL_PCLK_ACTIVE_NEG 1
#define PANEL_PCLK_HZ 16000000L  // 820 x 529 -> 36.9 Hz

// ST7262 is a plain RGB driver with no configuration bus.
#define PANEL_NEEDS_SPI_INIT 0

// ---- Expander, backlight, input --------------------------------------------
#define BOARD_EXPANDER_CH32 0
#define BOARD_EXPANDER_CH422G 1
#define BOARD_I2C_SDA 8
#define BOARD_I2C_SCL 9

// EXIO2 drives the MP3302 enable through the expander. It is a switch, not a
// PWM output, so the brightness slider degrades to a toggle on this board.
#define BOARD_HAS_BACKLIGHT_PWM 0
#define BOARD_BACKLIGHT_EXIO 2

// GPIO 0 is the G3 data line here, so the BOOT button cannot be used for page
// cycling and the GT911 is the only input available.
#define BOARD_HAS_TOUCH 1
#define BOARD_HAS_BOOT_BUTTON 0
#define BOARD_TOUCH_INT 4
#define BOARD_TOUCH_RST_EXIO 1

// ---- SD card ---------------------------------------------------------------
#define BOARD_SD_SDMMC 0
#define BOARD_SD_SPI 1
#define BOARD_SD_MOSI 11
#define BOARD_SD_SCK 12
#define BOARD_SD_MISO 13
#define BOARD_SD_CS_EXIO 4
