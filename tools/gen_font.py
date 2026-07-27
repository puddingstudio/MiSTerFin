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

SCALE     = 2
# ASCII printable + Latin-1 Supplement (accented Latin, U+00A0-U+00FF) so
# the OSD font matches the UI/subtitle coverage (src/font8x8.h basic + ext_latin).
CODES     = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))
NUM_CHARS = len(CODES)   # 95 + 96 = 191
CHAR_W    = 8 * SCALE          # 16 px wide per glyph
CHAR_H    = 8 * SCALE          # 16 px tall per glyph
IMG_W     = NUM_CHARS * CHAR_W # 1520 px total width
IMG_H     = CHAR_H             # 16 px total height

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC    = os.path.join(ROOT, "src", "font8x8.h")
OUTDIR = os.path.join(ROOT, "assets", "font")

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
                for dy in range(SCALE):
                    for dx in range(SCALE):
                        px = x_base + col * SCALE + dx
                        py = row * SCALE + dy
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
    alpha, bitmap = render(font)
    os.makedirs(OUTDIR, exist_ok=True)

    with open(os.path.join(OUTDIR, "font.desc"), "w") as f:
        f.write(make_desc())

    with open(os.path.join(OUTDIR, "font-alpha.raw"), "wb") as f:
        f.write(make_raw(alpha, IMG_W, IMG_H))

    with open(os.path.join(OUTDIR, "font-bitmap.raw"), "wb") as f:
        f.write(make_raw(bitmap, IMG_W, IMG_H))

    print(f"Generated assets/font/ ({IMG_W}x{IMG_H} px, {NUM_CHARS} chars, scale {SCALE}x)")
    print(f"  font.desc       {os.path.getsize(os.path.join(OUTDIR, 'font.desc'))} B")
    print(f"  font-alpha.raw  {os.path.getsize(os.path.join(OUTDIR, 'font-alpha.raw'))} B")
    print(f"  font-bitmap.raw {os.path.getsize(os.path.join(OUTDIR, 'font-bitmap.raw'))} B")

if __name__ == "__main__":
    main()
