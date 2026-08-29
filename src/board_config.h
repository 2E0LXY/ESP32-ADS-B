// Board selection. Define exactly one of ADSB_BOARD_WS4 or ADSB_BOARD_WS7 in
// platformio.ini build_flags; WS4 is assumed if neither is given so existing
// build commands keep working.
#pragma once

#if defined(ADSB_BOARD_WS7)
#include "boards/ws7.h"
#elif defined(ADSB_BOARD_WS4)
#include "boards/ws4.h"
#else
#warning "No ADSB_BOARD_* defined; defaulting to ADSB_BOARD_WS4"
#include "boards/ws4.h"
#endif

#if defined(ADSB_BOARD_WS4) && defined(ADSB_BOARD_WS7)
#error "Define only one ADSB_BOARD_* target"
#endif

// Derived geometry. Every layout coordinate in the render code must be
// expressed against these rather than as a literal, so a board with a
// different panel lays out correctly without a second set of drawing code.
namespace layout {

constexpr int W = PANEL_WIDTH;
constexpr int H = PANEL_HEIGHT;

constexpr int centreX = W / 2;
constexpr int centreY = H / 2;

// The 480x480 design was built on a 480 pixel reference. Scaling against the
// shorter axis keeps circular elements circular on a wider panel.
constexpr float scale = (H < W ? H : W) / 480.0f;

constexpr int scaled(int reference) {
  return static_cast<int>(reference * scale + 0.5f);
}

// Common furniture, previously hard-coded.
constexpr int margin = scaled(8);
constexpr int headerHeight = scaled(28);
constexpr int footerY = H - scaled(21);
constexpr int radarRadius = (H < W ? H : W) / 2 - scaled(24);

}  // namespace layout
