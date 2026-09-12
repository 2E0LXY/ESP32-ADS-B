#!/usr/bin/env python3
"""Turns a boot screen image into src/boot_asset.h.

The old asset was a raw RGB565 array: 480x480 pixels as 460,800 bytes of
flash, written out as a 1.6 MB C header. That only filled the 480x480
board's panel - on the 800x480 boards it was centred with black bars down
both sides - and a new image meant regenerating a header nobody could
review.

This stores a PNG instead and lets the firmware decode it at boot with
PNGdec, which is already a dependency (map tiles, airline logos and
aircraft photographs all arrive as PNGs). Two things fall out of that:

* Less flash for more pixels. A 256-colour PNG of the 800x480 image is
  about 187 KB against 768,000 bytes raw - and less than half what the old
  460,800-byte 480x480 asset cost, for 2.7 times the pixels.
* One source image, both panels. Each board gets a crop that suits its
  shape rather than one square asset letterboxed onto the other.

256 colours with Floyd-Steinberg dithering rather than truecolour: the
panel is RGB565 and cannot show more than 65k colours anyway, truecolour
would be 600 KB, and the measured error after dithering is a couple of
levels per channel on an image that is mostly sky.

Usage:
    python3 tools/make_boot_asset.py assets/boot-source.png

Writes src/boot_asset.h, plus assets/boot-800x480.png and
assets/boot-480x480.png so the crops can be looked at before flashing.
"""

import argparse
import os
import pathlib
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover - a developer machine without Pillow
    sys.exit("This needs Pillow: python3 -m pip install Pillow")

REPO = pathlib.Path(__file__).resolve().parent.parent
# The 800x480 boards get the full panel. The 480x480 board gets the same
# crop letterboxed, not a square crop of the middle: the title spans nearly
# the whole width, so a square crop would cut the ends off both words.
PANELS = ((800, 480), (480, 480))
COLOURS = 256
# renderBootScreen() prints the firmware credit at y=414 and the network
# status at y=448, both in white at text size 2 (16 px tall). On the old
# asset those fell on dark sky; on a full-bleed image whose lower half is
# moonlit cloud they would be unreadable. So the bottom of the picture is
# shaded towards the night sky before quantising - baked in, rather than a
# bar drawn at runtime, so the preview PNG shows exactly what the panel
# will show.
#
# The shading ramps in over RAMP pixels and then holds, rather than
# deepening all the way down: the brightest cloud here is around 210 of
# 255, so the first text line needs the full strength already, and a
# gradient that only reached it at the very bottom left that line sitting
# on cloud at 210. A soft edge above the text is what keeps it from looking
# like a panel pasted over the photograph.
TEXT_BAND_TOP = 384
SCRIM_RAMP = 30
SCRIM_STRENGTH = 0.86


def _darkest(image: Image.Image) -> tuple[int, int, int]:
    """The image's own darkest tone, used for scrims and letterbox bars so
    they read as more night sky rather than as pure black."""
    small = image.convert("RGB").resize((32, 32))
    getter = getattr(small, "get_flattened_data", small.getdata)
    return min(list(getter()), key=sum)


def _fit(source: Image.Image, width: int, height: int) -> Image.Image:
    """Centre-crops to the panel's aspect ratio, then scales to it."""
    source_ratio = source.width / source.height
    target_ratio = width / height
    if source_ratio > target_ratio:
        crop_width = int(round(source.height * target_ratio))
        left = (source.width - crop_width) // 2
        box = (left, 0, left + crop_width, source.height)
    else:
        crop_height = int(round(source.width / target_ratio))
        top = (source.height - crop_height) // 2
        box = (0, top, source.width, top + crop_height)
    return _shade_text_band(source.crop(box).resize((width, height), Image.LANCZOS))


def _shade_text_band(image: Image.Image) -> Image.Image:
    """Fades the bottom of the image towards its own darkest tone."""
    if image.height <= TEXT_BAND_TOP:
        return image
    shaded = image.copy()
    pixels = shaded.load()
    dark = _darkest(image)
    for y in range(TEXT_BAND_TOP, image.height):
        # Ramps in over SCRIM_RAMP pixels, then holds - see the note above.
        mix = SCRIM_STRENGTH * min(1.0, (y - TEXT_BAND_TOP) / SCRIM_RAMP)
        for x in range(image.width):
            r, g, b = pixels[x, y]
            pixels[x, y] = (
                int(r + (dark[0] - r) * mix),
                int(g + (dark[1] - g) * mix),
                int(b + (dark[2] - b) * mix),
            )
    return shaded


def _letterbox(source: Image.Image, width: int, height: int) -> Image.Image:
    """Scales the whole image to fit and centres it on the darkest tone.

    Used for the square panel. The bars are filled from the image's own
    darkest colour rather than pure black so they read as more night sky,
    and the lower bar is where renderBootScreen puts its two lines of
    text - which on the old asset sat on top of the picture.
    """
    scaled = source.copy()
    scaled.thumbnail((width, height), Image.LANCZOS)
    background = _darkest(source)
    canvas = Image.new("RGB", (width, height), background)
    canvas.paste(scaled, ((width - scaled.width) // 2, (height - scaled.height) // 2))
    return canvas


def _as_png_bytes(image: Image.Image, path: pathlib.Path) -> bytes:
    # Floyd-Steinberg (Pillow's default for this conversion) rather than no
    # dithering: the sky is a smooth gradient and 256 flat colours band it
    # visibly.
    quantised = image.convert("P", palette=Image.ADAPTIVE, colors=COLOURS)
    quantised.save(path, optimize=True)
    return path.read_bytes()


def _header(entries: list[tuple[int, int, bytes]]) -> str:
    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "// Generated by tools/make_boot_asset.py - do not edit by hand.",
        "//",
        "// The boot screen as a 256-colour PNG, decoded at boot by PNGdec",
        "// (already used for map tiles, airline logos and aircraft photos).",
        "// It was a raw RGB565 array, which cost 460,800 bytes of flash for a",
        "// 480x480 image that the 800x480 panels could only show centred",
        "// between black bars. One crop per panel shape, and less flash than",
        "// the single raw asset used.",
        "",
    ]
    for width, height, blob in entries:
        guard = "#if PANEL_WIDTH == 800" if width == 800 else "#else"
        lines.append(guard)
        lines.append(f"constexpr int BOOT_IMAGE_W = {width};")
        lines.append(f"constexpr int BOOT_IMAGE_H = {height};")
        lines.append(f"// {width}x{height}, {len(blob):,} bytes")
        lines.append("static const uint8_t BOOT_IMAGE_PNG[] PROGMEM = {")
        for start in range(0, len(blob), 16):
            chunk = blob[start:start + 16]
            lines.append("  " + ",".join(f"0x{byte:02x}" for byte in chunk) + ",")
        lines.append("};")
    lines.append("#endif")
    lines.append("constexpr size_t BOOT_IMAGE_PNG_LEN = sizeof(BOOT_IMAGE_PNG);")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="the boot screen image")
    parser.add_argument("--out", default=str(REPO / "src" / "boot_asset.h"))
    arguments = parser.parse_args()

    source = Image.open(arguments.source).convert("RGB")
    print(f"source: {arguments.source} {source.width}x{source.height}")

    entries = []
    for width, height in PANELS:
        # A panel as wide as it is tall gets the whole picture letterboxed;
        # anything wider gets a crop that fills it.
        fitted = (_letterbox(source, width, height) if width == height
                  else _fit(source, width, height))
        preview = REPO / "assets" / f"boot-{width}x{height}.png"
        blob = _as_png_bytes(fitted, preview)
        entries.append((width, height, blob))
        print(f"  {width}x{height}: {len(blob):,} bytes PNG "
              f"(raw RGB565 would be {width * height * 2:,}) -> {preview.name}")

    header = _header(entries)
    pathlib.Path(arguments.out).write_text(header)
    print(f"wrote {arguments.out} ({os.path.getsize(arguments.out):,} bytes of source)")
    print(f"flash cost: {sum(len(blob) for _w, _h, blob in entries):,} bytes total, "
          f"but only one panel's array is compiled in")
    return 0


if __name__ == "__main__":
    sys.exit(main())
