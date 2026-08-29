// CH422G I2C IO expander, as fitted to the Waveshare ESP32-S3-Touch-LCD-7
// and -4.3. Presented with the same surface as WS_CH32_IO so the application
// can call either behind the board config.
//
// The CH422G uses distinct I2C addresses as register selectors rather than a
// register byte, and offers no read-back of the output latch. Every write to
// WR_IO therefore drives all eight pins at once, so a shadow copy is kept
// here. EXIO0, EXIO5, EXIO6 and EXIO7 are undocumented on these boards and
// are held at their power-on state; driving them blind once dropped the board
// off USB entirely.
#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace WS_CH422G {

// 7-bit addresses. The datasheet quotes 8-bit values (0x48, 0x46, 0x70, 0x4D).
constexpr uint8_t ADDR_WR_SET = 0x24;
constexpr uint8_t ADDR_WR_OC = 0x23;
constexpr uint8_t ADDR_WR_IO = 0x38;
constexpr uint8_t ADDR_RD_IO = 0x26;

constexpr uint8_t SET_IO_OUTPUT_ENABLE = 1 << 0;
constexpr uint8_t SET_OPEN_DRAIN_ENABLE = 1 << 2;

// Power-on state of the output latch. Releases TP_RST and LCD_RST, enables the
// backlight, deasserts SD_CS and leaves USB_SEL as the ROM bootloader set it.
constexpr uint8_t IO_DEFAULT = 0xFF;

bool begin(TwoWire &wire = Wire, int sda = -1, int scl = -1,
           uint32_t frequency = 400000, Print *log = nullptr);

// Set or clear a single EXIO pin (0-7) without disturbing the others.
bool writePin(uint8_t pin, bool high, TwoWire &wire = Wire);
bool readPins(uint8_t *value, TwoWire &wire = Wire);

// Present for interface parity with WS_CH32_IO. This board's backlight is a
// switch on EXIO2, so anything above zero turns it on. Returns false if a
// genuine duty was requested, letting the caller report the limitation.
bool setPwm(TwoWire &wire, uint8_t value);
bool setBacklight(bool on, TwoWire &wire = Wire);

uint8_t shadow();

}  // namespace WS_CH422G
