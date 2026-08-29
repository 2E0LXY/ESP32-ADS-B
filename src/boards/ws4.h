// Board configuration: Waveshare ESP32-S3-Touch-LCD-4 Rev 4.0 (480x480).
//
// Timings corrected against the vendor reference retained at
// docs/hardware-reference-rev3-ST7701.h. The original firmware transposed
// HBP and HFP and passed no pixel clock, defaulting to 12 MHz / 42 Hz.
#pragma once

#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-4"

// ---- Panel -----------------------------------------------------------------
#define PANEL_WIDTH 480
#define PANEL_HEIGHT 480
#define PANEL_ROTATION 2  // physically mounted 180 degrees

#define PANEL_PIN_DE 40
#define PANEL_PIN_VSYNC 39
#define PANEL_PIN_HSYNC 38
#define PANEL_PIN_PCLK 41
#define PANEL_PINS_R 46, 3, 8, 18, 17
#define PANEL_PINS_G 14, 13, 12, 11, 10, 9
#define PANEL_PINS_B 5, 45, 48, 47, 21

#define PANEL_HSYNC_POLARITY 1
#define PANEL_HSYNC_FRONT_PORCH 50
#define PANEL_HSYNC_PULSE_WIDTH 8
#define PANEL_HSYNC_BACK_PORCH 10
#define PANEL_VSYNC_POLARITY 1
#define PANEL_VSYNC_FRONT_PORCH 8
#define PANEL_VSYNC_PULSE_WIDTH 2
#define PANEL_VSYNC_BACK_PORCH 18
#define PANEL_PCLK_ACTIVE_NEG 0
#define PANEL_PCLK_HZ 16500000L

// The ST7701 needs an SPI configuration sequence before the RGB interface
// works. GPIO 1 and 2 are shared with SDMMC below; the panel must be fully
// initialised before the card is mounted, and never re-initialised after.
#define PANEL_NEEDS_SPI_INIT 1
#define PANEL_SPI_CS 42
#define PANEL_SPI_SCK 2
#define PANEL_SPI_MOSI 1

// ---- Expander, backlight, input --------------------------------------------
#define BOARD_EXPANDER_CH32 1
#define BOARD_EXPANDER_CH422G 0
#define BOARD_I2C_SDA 15
#define BOARD_I2C_SCL 7

#define BOARD_HAS_BACKLIGHT_PWM 1  // CH32 REG_PWM, 0-255 duty
#define BOARD_HAS_TOUCH 0          // no touch controller fitted
#define BOARD_HAS_BOOT_BUTTON 1    // GPIO 0 is free for page cycling

// ---- SD card ---------------------------------------------------------------
#define BOARD_SD_SDMMC 1
#define BOARD_SD_SPI 0
#define BOARD_SD_CLK 2
#define BOARD_SD_CMD 1
#define BOARD_SD_D0 4
