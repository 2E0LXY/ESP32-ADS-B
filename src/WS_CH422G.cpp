#include "WS_CH422G.h"

namespace WS_CH422G {
namespace {

uint8_t ioShadow = IO_DEFAULT;

bool writeRegister(TwoWire &wire, uint8_t address, uint8_t value) {
  wire.beginTransmission(address);
  wire.write(value);
  return wire.endTransmission() == 0;
}

}  // namespace

uint8_t shadow() { return ioShadow; }

bool begin(TwoWire &wire, int sda, int scl, uint32_t frequency, Print *log) {
  if (sda >= 0 && scl >= 0) {
    wire.begin(sda, scl);
  } else {
    wire.begin();
  }
  wire.setClock(frequency);
  delay(20);

  if (!writeRegister(wire, ADDR_WR_SET, SET_IO_OUTPUT_ENABLE)) {
    if (log) log->println("CH422G: output enable failed");
    return false;
  }
  ioShadow = IO_DEFAULT;
  if (!writeRegister(wire, ADDR_WR_IO, ioShadow)) {
    if (log) log->println("CH422G: initial IO write failed");
    return false;
  }
  if (log) log->printf("CH422G ready, IO 0x%02X\n", ioShadow);
  return true;
}

bool writePin(uint8_t pin, bool high, TwoWire &wire) {
  if (pin > 7) return false;
  const uint8_t mask = static_cast<uint8_t>(1u << pin);
  const uint8_t next = high ? (ioShadow | mask) : (ioShadow & ~mask);
  if (next == ioShadow) return true;
  if (!writeRegister(wire, ADDR_WR_IO, next)) return false;
  ioShadow = next;
  return true;
}

bool readPins(uint8_t *value, TwoWire &wire) {
  if (!value) return false;
  if (wire.requestFrom(static_cast<uint8_t>(ADDR_RD_IO), static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  *value = wire.read();
  return true;
}

bool setBacklight(bool on, TwoWire &wire) {
#ifdef BOARD_BACKLIGHT_EXIO
  return writePin(BOARD_BACKLIGHT_EXIO, on, wire);
#else
  return writePin(2, on, wire);
#endif
}

bool setPwm(TwoWire &wire, uint8_t value) {
  // No PWM path exists on this board; treat any non-zero level as "on" and
  // report that the requested duty could not be honoured so the caller can
  // surface the limitation rather than silently ignoring it.
  const bool applied = setBacklight(value > 0, wire);
  return applied && (value == 0 || value == 255);
}

}  // namespace WS_CH422G
