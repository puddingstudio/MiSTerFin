#!/usr/bin/env python3
"""
Generate mplayer bitmap font from font8x8_basic data.

Reads src/font8x8.h, outputs assets/font/:
  font.desc       — mplayer font description
  font-alpha.raw  — alpha map (mhwanh indexed format)
  font-bitmap.raw — bitmap map (mhwanh indexed format)

Run from project root: python3 tools/gen_font.py
"""

import re, struct, os, sys

# Two targets: the normal 2x (16x16, square — this platform's usual
# non-square pixels happen to read fine at a plain uniform scale here,
# established for a long time) glyph for progressive 288/240-line
# framebuffers, and an ASYMMETRIC one for the interlaced full-frame
# 576/480-line modes. mplayer draws its OSD (VSync/seek flash) straight
# into the physical framebuffer, bypassing the app's own line-doubling
# (that only applies to what the APP itself draws) — confirmed on hardware
# in two steps: a uniform 4x read vertically squished (about half the
# height it should be relative to the doubled physical raster), and a
# uniform 3x read too WIDE/stretched once height was fixed — the doubled
# vertical resolution alone (same physical screen, 2x the scanlines) makes
# a uniformly-scaled square glyph visibly non-square here, unlike the
# progressive resolutions. Scale width and height separately instead.
TARGETS = [(2, 2, "font"), (2, 3, "font2x")]
SCALE_X   = 2
SCALE_Y   = 2
CODES     = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))
NUM_CHARS = len(CODES)   # 95 + 96 = 191
CHAR_W    = 8 * SCALE_X         # glyph width in px
CHAR_H    = 8 * SCALE_Y         # glyph height in px
IMG_W     = NUM_CHARS * CHAR_W  # total atlas width
IMG_H     = CHAR_H              # total atlas height

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC    = os.path.join(ROOT, "src", "font8x8.h")
OUTDIR = os.path.join(ROOT, "assets", "font")

def set_target(scale_x, scale_y, dirname):
    global SCALE_X, SCALE_Y, CHAR_W, CHAR_H, IMG_W, IMG_H, OUTDIR
    SCALE_X = scale_x
    SCALE_Y = scale_y
    CHAR_W  = 8 * SCALE_X
    CHAR_H  = 8 * SCALE_Y
    IMG_W   = NUM_CHARS * CHAR_W
    IMG_H   = CHAR_H
    OUTDIR  = os.path.join(ROOT, "assets", dirname)

# ---------------------------------------------------------------------------

def parse_font8x8(path):
    with open(path) as f:
        text = f.read()
    entries = re.findall(r'\{\s*((?:0x[0-9a-fA-F]+,?\s*){8})\}', text)
    font = []
    for e in entries:
        vals = [int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]+', e)]
        font.append(vals)
    assert len(font) == 224, f"Expected 224 glyphs (128 basic + 96 ext_latin), got {len(font)}"
    return font

def glyph_for_code(font, code):
    if code < 0x80:
        return font[code]
    return font[128 + (code - 0xA0)]   # ext_latin block: index 128 == U+00A0

def make_raw(pixels, w, h):
    """Encode pixel array as mhwanh indexed raw file."""
    header = (b'mhwanh'
              + b'\x00\x00'
              + struct.pack('>H', w)
              + struct.pack('>H', h)
              + struct.pack('>H', 256)   # 256 palette entries
              + b'\x00' * 18)           # padding → 32 bytes total
    assert len(header) == 32
    palette = bytes(v for i in range(256) for v in [i, i, i])  # grayscale
    return header + palette + bytes(pixels)

def render(font):
    """
    Return (alpha_pixels, bitmap_pixels) for the full glyph strip.

    Alpha:  0 = opaque (text pixel), 255 = transparent (background).
    Bitmap: 0 = foreground colour,   255 = background.

    mplayer resamples alpha with factor=1.0:
      text pixel  alpha=0   → stored alpha=0  (opaque)
      bg pixel    alpha=255 → stored alpha=1  (transparent)
    """
    alpha  = bytearray(IMG_W * IMG_H)   # default 0 = transparent (skip draw)
    bitmap = bytearray(IMG_W * IMG_H)   # default 0 (won't be drawn anyway)

    for ci, code in enumerate(CODES):
        glyph  = glyph_for_code(font, code)   # 8 bytes, one per row, LSB-first
        x_base = ci * CHAR_W

        for row in range(8):
            bits = glyph[row]
            for col in range(8):
                if not ((bits >> col) & 1):
                    continue
                for dy in range(SCALE_Y):
                    for dx in range(SCALE_X):
                        px = x_base + col * SCALE_X + dx
                        py = row * SCALE_Y + dy
                        idx = py * IMG_W + px
                        # mplayer blend: dst_new = (video * srca >> 8) + src
                        # srca=0  → if(srca) is false → TRANSPARENT (skip)
                        # srca=255 → after resampling stored=1 → dst=0+src=src
                        # So: text pixels need alpha=255 (→ stored=1 → draw),
                        #     background needs alpha=0 (→ stored=0 → transparent).
                        alpha[idx]  = 255   # text pixel → will be drawn
                        bitmap[idx] = 255   # brightness added = 255 → white text

    return alpha, bitmap

def make_desc():
    lines = [
        "[info]",
        "name MiSTerDVD",
        f"spacewidth {CHAR_W}",
        "charspace 1",
        f"height {CHAR_H}",
        "",
        "[files]",
        "alpha font-alpha.raw",
        "bitmap font-bitmap.raw",
        "",
        "[characters]",
    ]
    for ci, code in enumerate(CODES):
        start = ci * CHAR_W
        end   = start + CHAR_W - 1
        lines.append(f"{code} {start} {end}")
    return "\n".join(lines) + "\n"

# ---------------------------------------------------------------------------

def main():
    font = parse_font8x8(SRC)
    for scale_x, scale_y, dirname in TARGETS:
        set_target(scale_x, scale_y, dirname)
        alpha, bitmap = render(font)
        os.makedirs(OUTDIR, exist_ok=True)

        with open(os.path.join(OUTDIR, "font.desc"), "w") as f:
            f.write(make_desc())

        with open(os.path.join(OUTDIR, "font-alpha.raw"), "wb") as f:
            f.write(make_raw(alpha, IMG_W, IMG_H))

        with open(os.path.join(OUTDIR, "font-bitmap.raw"), "wb") as f:
            f.write(make_raw(bitmap, IMG_W, IMG_H))

        print(f"Generated assets/{dirname}/ ({IMG_W}x{IMG_H} px, {NUM_CHARS} chars, scale {SCALE_X}x{SCALE_Y})")

if __name__ == "__main__":
    main()
