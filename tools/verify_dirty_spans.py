#!/usr/bin/env python3
"""Proves the dirty-span renderer draws the same picture as a full-frame one.

The firmware cannot be built or run here, and the fault this change targets -
the RGB panel losing DMA sync and scanning out permanently offset - only
shows on the hardware. What *can* be checked from here is the part that would
be a bug rather than a tuning question: that pushing only the spans that
changed leaves the panel pixel-identical to copying the whole frame.

So this extracts markRow(), pixel(), restoreMap() and present() verbatim out
of src/main.cpp, wraps them in a host harness with a stub panel, and runs
them against a reference renderer that copies everything. Extracted rather
than re-typed: a transcribed copy would test the transcription.

It covers a map rebuild part-way through (baseMap changing under everything),
the map being dropped and coming back, and the span buffers failing to
allocate - which has to fall back to whole-frame copies rather than draw
nothing.

Usage:
    python3 tools/verify_dirty_spans.py
"""

import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
SOURCE = REPO / "src" / "main.cpp"


def extract(source: str, start: str, end: str) -> str:
    """The text between two markers, so the harness tests the real code."""
    try:
        begin = source.index(start)
        finish = source.index(end, begin)
    except ValueError:
        sys.exit(f"could not find {start!r} .. {end!r} in main.cpp - "
                 "the renderer has moved and this tool needs updating")
    return source[begin:finish]


def build_harness(source: str) -> str:
    spans = extract(source, "int16_t *paintedLeft = nullptr;", "inline void invalidateWholeFrame")
    spans += "inline void invalidateWholeFrame() { fullFrameNeeded = true; }\n"
    pixel = extract(source, "void pixel(int x, int y, uint16_t c) {", "\n}\n") + "\n}\n"
    restore = extract(source, "void restoreMap() {", "\n}\n") + "\n}\n"
    present = extract(source, "  rgbpanel->waitForVsync(50);\n  if (spansReady()) {",
                      "  // Realign the scan after the copy.")
    present = (present
               .replace("gfx->draw16bitRGBBitmap(x0, y, framebuffer + y * W + x0, x1 - x0, 1);",
                        "panelBlit(x0, y, framebuffer + y * W + x0, x1 - x0, 1);")
               .replace("gfx->draw16bitRGBBitmap(0, 0, framebuffer, W, H);",
                        "panelBlit(0, 0, framebuffer, W, H);")
               .replace("rgbpanel->waitForVsync(50);", ""))
    return HARNESS.replace("@SPANS@", spans).replace("@PIXEL@", pixel) \
                  .replace("@RESTORE@", restore).replace("@PRESENT@", present)


HARNESS = r"""
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cassert>

constexpr int W = 800, H = 480;
constexpr int ASSET_W = 480, ASSET_H = 480;
static uint16_t MAP_IMAGE[1];
#define memcpy_P memcpy
struct Serial_ { void println(const char*) {} } Serial;

uint16_t *framebuffer = nullptr;
uint16_t *baseMap = nullptr;
uint16_t *panel = nullptr;
bool physicalMapReady = false;

@SPANS@

// Stands in for gfx->draw16bitRGBBitmap at rotation 0: a row-wise copy into
// the panel's framebuffer, which is what the library does.
void panelBlit(int x, int y, const uint16_t *bitmap, int w, int h) {
  for (int row = 0; row < h; ++row)
    memcpy(panel + (y + row) * W + x, bitmap + row * w, (size_t)w * sizeof(uint16_t));
}

@PIXEL@
@RESTORE@

void present() {
@PRESENT@
}

// --- the reference: whole-frame restore and whole-frame present ----------
static std::vector<uint16_t> refShadow(W * H), refPanel(W * H);
void refRestore() {
  if (physicalMapReady && baseMap) memcpy(refShadow.data(), baseMap, W * H * 2);
  else memset(refShadow.data(), 0, W * H * 2);
}
void refPixel(int x, int y, uint16_t c) {
  if ((unsigned)x < W && (unsigned)y < H) refShadow[y * W + x] = c;
}
void refPresent() { memcpy(refPanel.data(), refShadow.data(), W * H * 2); }

static unsigned seed = 12345;
unsigned rnd() { seed = seed * 1103515245u + 12345u; return (seed >> 8); }

struct Op { int x, y, w, h; uint16_t colour; };
std::vector<Op> randomFrame() {
  std::vector<Op> ops;
  const int count = 1 + rnd() % 12;
  for (int n = 0; n < count; ++n) {
    Op op;
    op.w = 1 + rnd() % 60;
    op.h = 1 + rnd() % 24;
    op.x = rnd() % W;
    op.y = rnd() % H;
    op.colour = (uint16_t)rnd();
    ops.push_back(op);
  }
  return ops;
}

// The shape of the real Overview page: the aircraft strip is text down the
// right-hand 300 px, redrawn every render, and the markers are scattered
// over the map on the left. Worth measuring separately - random rectangles
// flatter a span-based renderer.
std::vector<Op> overviewFrame() {
  std::vector<Op> ops;
  for (int line = 0; line < 24; ++line)              // the strip: 24 rows of text
    ops.push_back({505, 8 + line * 19, 285, 16, (uint16_t)rnd()});
  for (int marker = 0; marker < 30; ++marker)        // aircraft over the map
    ops.push_back({(int)(rnd() % 470), (int)(rnd() % 460), 14, 14, (uint16_t)rnd()});
  ops.push_back({4, 4, 120, 16, (uint16_t)rnd()});   // the status badge
  return ops;
}

int main() {
  framebuffer = (uint16_t *)malloc(W * H * 2);
  baseMap = (uint16_t *)malloc(W * H * 2);
  panel = (uint16_t *)malloc(W * H * 2);
  int16_t *spans = (int16_t *)malloc(4 * H * sizeof(int16_t));
  paintedLeft = spans; paintedRight = spans + H;
  pendingLeft = spans + 2 * H; pendingRight = spans + 3 * H;
  spanReset(paintedLeft, paintedRight);
  spanReset(pendingLeft, pendingRight);

  for (int i = 0; i < W * H; ++i) {
    const uint16_t v = (uint16_t)rnd();
    baseMap[i] = v;
    framebuffer[i] = 0xdead;          // whatever malloc gave us
    panel[i] = 0xbeef;                // the boot screen, as far as we know
    refShadow[i] = 0xdead;
    refPanel[i] = 0xbeef;
  }
  physicalMapReady = true;

  long long spanPixels = 0, fullPixels = 0;
  for (int frame = 0; frame < 400; ++frame) {
    // A map rebuild every so often, and a settings change that drops the map.
    if (frame == 120) invalidateWholeFrame();
    if (frame == 200) { physicalMapReady = false; invalidateWholeFrame(); }
    if (frame == 210) { physicalMapReady = true; invalidateWholeFrame(); }

    restoreMap();
    refRestore();
    for (const Op &op : randomFrame())
      for (int dy = 0; dy < op.h; ++dy)
        for (int dx = 0; dx < op.w; ++dx) {
          pixel(op.x + dx, op.y + dy, op.colour);
          refPixel(op.x + dx, op.y + dy, op.colour);
        }
    present();
    refPresent();
    spanPixels += lastPresentPixels;
    fullPixels += (long long)W * H;

    if (memcmp(framebuffer, refShadow.data(), W * H * 2) != 0) {
      printf("FRAME %d: shadow buffer diverged from the reference\n", frame);
      return 1;
    }
    if (memcmp(panel, refPanel.data(), W * H * 2) != 0) {
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
          if (panel[y * W + x] != refPanel[y * W + x]) {
            printf("FRAME %d: panel differs first at x=%d y=%d (%04x vs %04x)\n",
                   frame, x, y, panel[y * W + x], refPanel[y * W + x]);
            return 1;
          }
    }
  }
  printf("400 frames: panel identical to a full-frame renderer every frame\n");
  printf("pixels pushed: %lld vs %lld full-frame (%.1f%%)\n",
         spanPixels, fullPixels, 100.0 * spanPixels / fullPixels);

  // And with the allocation failed, it must behave exactly as before.
  paintedLeft = paintedRight = pendingLeft = pendingRight = nullptr;
  fullFrameNeeded = true;
  for (int frame = 0; frame < 20; ++frame) {
    restoreMap(); refRestore();
    for (const Op &op : randomFrame())
      for (int dy = 0; dy < op.h; ++dy)
        for (int dx = 0; dx < op.w; ++dx) {
          pixel(op.x + dx, op.y + dy, op.colour);
          refPixel(op.x + dx, op.y + dy, op.colour);
        }
    present(); refPresent();
    if (memcmp(panel, refPanel.data(), W * H * 2) != 0) {
      printf("fallback frame %d diverged\n", frame);
      return 1;
    }
  }
  printf("no span buffers: 20 frames identical, full-frame fallback intact\n");

  // Again with an Overview-shaped frame, and counting both copies this time.
  paintedLeft = spans; paintedRight = spans + H;
  pendingLeft = spans + 2 * H; pendingRight = spans + 3 * H;
  spanReset(paintedLeft, paintedRight);
  spanReset(pendingLeft, pendingRight);
  fullFrameNeeded = true;
  physicalMapReady = true;
  long long pushed = 0, restored = 0;
  for (int frame = 0; frame < 200; ++frame) {
    long long before = 0;
    for (int y = 0; y < H; ++y)
      if (paintedRight[y] > paintedLeft[y]) before += paintedRight[y] - paintedLeft[y];
    restoreMap();
    refRestore();
    restored += before;
    for (const Op &op : overviewFrame())
      for (int dy = 0; dy < op.h; ++dy)
        for (int dx = 0; dx < op.w; ++dx) {
          pixel(op.x + dx, op.y + dy, op.colour);
          refPixel(op.x + dx, op.y + dy, op.colour);
        }
    present();
    refPresent();
    pushed += lastPresentPixels;
    if (memcmp(panel, refPanel.data(), W * H * 2) != 0) {
      printf("overview frame %d diverged\n", frame);
      return 1;
    }
  }
  const double full = (double)W * H * 200;
  printf("overview-shaped load, 200 frames, all identical:\n");
  printf("  restoreMap reads  %.0f KB, was %.0f KB  (%.1f%%)\n",
         restored * 2 / 1024.0, full * 2 / 1024.0, 100.0 * restored / full);
  printf("  present pushes    %.0f KB, was %.0f KB  (%.1f%%)\n",
         pushed * 2 / 1024.0, full * 2 / 1024.0, 100.0 * pushed / full);
  printf("  per render        %.0f KB, was 1536 KB\n",
         (restored + pushed) * 2 / 1024.0 / 200);
  return 0;
}
"""


def main() -> int:
    harness = build_harness(SOURCE.read_text(errors="surrogateescape"))
    with tempfile.TemporaryDirectory() as scratch:
        source = pathlib.Path(scratch) / "sim.cpp"
        binary = pathlib.Path(scratch) / "sim"
        source.write_text(harness)
        compile_result = subprocess.run(
            ["g++", "-O1", "-std=c++17", "-o", str(binary), str(source)],
            capture_output=True, text=True)
        if compile_result.returncode != 0:
            print(compile_result.stderr)
            return compile_result.returncode
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="")
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())
