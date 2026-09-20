// Board configuration: Guition JC8012P4A1 (ESP32-P4 + ESP32-C6, 10.1in).
//
// Unlike the Waveshare boards this is a two-chip design. The ESP32-P4 has no
// radio at all; Wi-Fi is an ESP32-C6-MINI-1U reached over SDIO running
// ESP-Hosted, so WiFi.begin() is an RPC to a second processor rather than a
// local peripheral. Host and slave firmware versions must match or requests
// time out - and the C6 is reflashable only via the CN5 header, which is
// inside the case.
//
// THE PANEL IS NATIVE PORTRAIT. 800x1280, scanned out that way by a JD9365
// over 2-lane MIPI-DSI. The application draws 1280x800 landscape and px()
// transposes into the panel buffer; nothing else in the renderer changes.
// See PANEL_TRANSPOSE below.
//
// Batch matters. The rear label reads SKU:10153001-V2 (2631); 2628 and above
// ship a revised panel needing different JD9365 init timing. The wrong init
// gives persistent horizontal lines or no boot at all.
#pragma once

#define BOARD_NAME "Guition JC8012P4A1"
#define BOARD_JC8012P4A1 1
#define BOARD_PANEL_BATCH 2631

// ---- Panel -----------------------------------------------------------------
// Timings below are CONFIRMED against ESPHome's merged JC8012P4A1 driver
// (esphome/components/mipi_dsi/models/guition.py, PR #13241) and match the
// vendor profile that works on this panel. Do not substitute the generic
// JD9365 timings from the ESP32-P4 Function-EV-Board examples: those are what
// produce the well-known "backlight on, screen black, repeated DSI/DPI
// underrun" failure on this exact panel (esp-brookesia issue #72). The panel
// answers its ID correctly either way, so a valid ID read proves nothing.
//
// Note the driver declares swap_xy false and width/height as the PHYSICAL
// 800x1280 - rotation is the application's job, not the panel's.
// Logical geometry the renderer works in.
#define PANEL_WIDTH 1280
#define PANEL_HEIGHT 800
#define PANEL_ROTATION 0
// Physical geometry the DSI controller scans out.
#define PANEL_PHYS_WIDTH 800
#define PANEL_PHYS_HEIGHT 1280
#define PANEL_TRANSPOSE 1

#define PANEL_IS_MIPI_DSI 1
#define PANEL_DSI_LANES 2
#define PANEL_DSI_LANE_RATE_MBPS 1000
#define PANEL_PCLK_HZ 60000000L

#define PANEL_HSYNC_BACK_PORCH 20
#define PANEL_HSYNC_PULSE_WIDTH 20
#define PANEL_HSYNC_FRONT_PORCH 40
#define PANEL_VSYNC_BACK_PORCH 8
#define PANEL_VSYNC_PULSE_WIDTH 4
#define PANEL_VSYNC_FRONT_PORCH 20

#define PANEL_PIN_RESET 27
#define PANEL_NEEDS_SPI_INIT 0  // JD9365 is configured over DSI, not SPI
#define PANEL_COLOR_ORDER_RGB 1
#define PANEL_DSI_SWAP_XY 0

// The ~220-entry JD9365 register init table is NOT reproduced here. Use the
// esp_lcd_jd9365 component's own vendor sequence and supply the timings
// above; the reported black-screen failures trace to wrong timings rather
// than a missing table. If a board-specific table does turn out to be needed,
// take it from ESPHome's guition.py rather than retyping it - and note that
// file is GPLv3, which has consequences for this repo's licence.

// ---- Backlight, input ------------------------------------------------------
#define BOARD_EXPANDER_CH32 0
#define BOARD_EXPANDER_CH422G 0

// Real PWM on a real GPIO, active high - the brightness slider works properly
// here, unlike the expander-driven toggle on the Waveshare boards.
#define BOARD_HAS_BACKLIGHT_PWM 1
#define BOARD_BACKLIGHT_PIN 23

// I2C is shared by touch, the RX8025T RTC, the ES8311 codec, the camera
// connector and the CN4/EXTEND headers. R43 and R44 are 2.2k pull-ups on the
// board, so the internal pull-ups must stay OFF: two sets in parallel drag
// the bus out of spec and it stops answering.
#define BOARD_I2C_SDA 7
#define BOARD_I2C_SCL 8
#define BOARD_I2C_FREQ 400000
#define BOARD_I2C_INTERNAL_PULLUPS 0

// GSL3680, driver-compatible with the GSL3670. Fixed address 0x40.
#define BOARD_HAS_TOUCH 1
#define BOARD_TOUCH_GSL3680 1
#define BOARD_TOUCH_ADDR 0x40
#define BOARD_TOUCH_INT 21
#define BOARD_TOUCH_RST 22
// The touch matrix is etched to the panel's native portrait grid, so the same
// transform applied to px() has to be applied to touch, or taps land on the
// wrong widget. Must match PANEL_TRANSPOSE's direction.
#define BOARD_TOUCH_SWAP_XY 1
#define BOARD_TOUCH_MIRROR_X 1
#define BOARD_TOUCH_MIRROR_Y 0

#define BOARD_HAS_BOOT_BUTTON 1

// ---- Wi-Fi co-processor (ESP-Hosted over SDIO) -----------------------------
// 51k pull-ups on CLK, CMD and all four data lines are on the board. Keep the
// bus at 20 MHz; higher is reported unstable on this hardware.
#define BOARD_WIFI_HOSTED 1
#define BOARD_HOSTED_SDIO_CLK 18
#define BOARD_HOSTED_SDIO_CMD 19
#define BOARD_HOSTED_SDIO_D0 14
#define BOARD_HOSTED_SDIO_D1 15
#define BOARD_HOSTED_SDIO_D2 16
#define BOARD_HOSTED_SDIO_D3 17
#define BOARD_HOSTED_SDIO_FREQ_KHZ 20000
#define BOARD_HOSTED_C6_RESET 54
#define BOARD_HOSTED_C6_WAKEUP 6  // C6 IO2 -> P4 GPIO6, co-processor wakes host
// D3 must stay pulled high even in 1-line debug mode; letting it float makes
// the C6 negotiate down to SPI and throughput collapses.
// The C6 ships with ESP-Hosted slave v0.0.6. If the host library is newer the
// RPCs time out - reflash via CN5 (EN/TXD/RXD/GND/IO0; do NOT connect VDD),
// with the P4 held in bootloader mode so it stops driving the SDIO bus.

// ---- Battery ---------------------------------------------------------------
// 68k series / 100k to ground on GPIO52: Vadc = Vbat * 0.595. Empirically
// ~1.8V reads critically low and ~2.43V reads full.
#define BOARD_HAS_BATTERY 1
#define BOARD_BATTERY_ADC_PIN 52
#define BOARD_BATTERY_DIVIDER 1.68f

// ---- RTC -------------------------------------------------------------------
// RX8025T with a CR1220 cell, so the clock survives power loss and the first
// render after boot can show a real time before NTP has answered.
#define BOARD_HAS_RTC 1
#define BOARD_RTC_RX8025T 1

// ---- SD card ---------------------------------------------------------------
// SDMMC Slot 0 on the ESP32-P4 has fixed GPIOs - these are not a choice, and
// changing them does not move the peripheral. All six lines carry 5.1k
// pull-ups on the board. 4-bit mode by default.
#define BOARD_SD_SDMMC 1
#define BOARD_SD_SPI 0
#define BOARD_SD_BUS_WIDTH 4
#define BOARD_SD_CLK 43
#define BOARD_SD_CMD 44
#define BOARD_SD_D0 39
#define BOARD_SD_D1 40
#define BOARD_SD_D2 41
#define BOARD_SD_D3 42
// In 1-line mode D1/D2 go unused, but the card's D3 must STILL be pulled up
// or the card negotiates down to SPI protocol mode.

// ---- Audio (unused by this firmware, recorded so the pins are not reused) --
// ES8311 codec over I2S, NS4150B class-D amp. PA_CTRL must be held low during
// boot and I2S init or the speaker pops.
#define BOARD_I2S_MCLK 13
#define BOARD_I2S_SCLK 12
#define BOARD_I2S_SDOUT 11
#define BOARD_I2S_LRCK 10
#define BOARD_I2S_DSDIN 9
#define BOARD_AUDIO_PA_CTRL 20

// ---- Local ADS-B receiver (UART module) ------------------------------------
// Reserved for the GNS5892 / ADSBee m1090 route. Pins are free choice from the
// EXTEND headers; fill in once the module is chosen.
#define BOARD_HAS_LOCAL_ADSB 0
// #define BOARD_ADSB_UART_RX ??
// #define BOARD_ADSB_UART_TX ??
// #define BOARD_ADSB_BAUD 921600
