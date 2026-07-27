#!/usr/bin/env python3
"""
Generate a smaller mplayer bitmap font for SUBTITLES only (loaded via
mplayer's -subfont, separate from the OSD's -font — see gen_font.py, which
this is a variant of).

The OSD font (gen_font.py) renders font8x8_basic 1:1 scaled (16px). This
one supersamples each glyph at a much higher internal resolution and
box-downsamples it to a smaller target size (13px) for a better-placed
edge than naive nearest-neighbor scaling would give — but still thresholds
to hard 0/255 alpha, same as the OSD font: confirmed on hardware that this
mplayer build's font renderer has no real alpha blending (any nonzero
alpha renders fully opaque), so an actual smooth/antialiased gradient just
fills the whole glyph cell in solid white instead of looking soft.

Run from project root: python3 tools/gen_subfont.py
"""

import re, struct, os

# ASCII printable + Latin-1 Supplement (accented Latin, U+00A0-U+00FF), so
# subtitles show accents instead of missing glyphs — matches the two ranges
# the on-screen UI font now covers (src/font8x8.h: basic + ext_latin).
CODES     = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))
NUM_CHARS = len(CODES)   # 95 + 96 = 191

SUPERSAMPLE  = 8          # internal render scale before downsampling
TARGET_H     = 13         # final glyph height in px (vs. 16px for the OSD font)
TARGET_W     = TARGET_H   # font8x8 glyphs are square
COVER_THRESH = 128        # confirmed on hardware: mplayer's font renderer has no
                           # alpha blending — ANY nonzero alpha renders fully
                           # opaque, so a smooth gradient here just fills in the
                           # whole glyph cell solid white. Threshold to hard
                           # 0/255 instead (supersampling still helps by letting
                           # the threshold land on a better-antialiased edge
                           # position than naive nearest-neighbor would).

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC    = os.path.join(ROOT, "src", "font8x8.h")
OUTDIR = os.path.join(ROOT, "assets", "subfont")

IMG_W = NUM_CHARS * TARGET_W
IMG_H = TARGET_H

# ---------------------------------------------------------------------------

def parse_font8x8(path):
    with open(path) as f:
        text = f.read()
    entries = re.findall(r'\{\s*((?:0x[0-9a-fA-F]+,?\s*){8})\}', text)
    font = []
    for e in entries:
        vals = [int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]+', e)]
        font.append(vals)
    # font8x8.h now holds font8x8_basic (128, U+0000-U+007F) followed by
    # font8x8_ext_latin (96, U+00A0-U+00FF).
    assert len(font) == 224, f"Expected 224 glyphs (128 basic + 96 ext_latin), got {len(font)}"
    return font

def glyph_for_code(font, code):
    """Map a Unicode code point to its glyph row in the parsed font8x8.h."""
    if code < 0x80:
        return font[code]
    return font[128 + (code - 0xA0)]   # ext_latin block: index 128 == U+00A0

def make_raw(pixels, w, h):
    """Encode pixel array as mhwanh indexed raw file (same format gen_font.py uses)."""
    header = (b'mhwanh'
              + b'\x00\x00'
              + struct.pack('>H', w)
              + struct.pack('>H', h)
              + struct.pack('>H', 256)
              + b'\x00' * 18)
    assert len(header) == 32
    palette = bytes(v for i in range(256) for v in [i, i, i])
    return header + palette + bytes(pixels)

def glyph_coverage(glyph):
    """
    Supersample this 8x8 bit glyph to (8*SUPERSAMPLE)^2, then box-downsample
    to TARGET_W x TARGET_H, returning a coverage value 0-255 per output
    pixel (255 = fully inside a lit source pixel, 0 = fully outside,
    in-between at edges — the antialiasing).
    """
    ss = SUPERSAMPLE
    src_dim = 8 * ss
    # supersampled 1-bit canvas as a flat bytearray of 0/1
    super_px = bytearray(src_dim * src_dim)
    for row in range(8):
        bits = glyph[row]
        for col in range(8):
            if not ((bits >> col) & 1):
                continue
            for dy in range(ss):
                for dx in range(ss):
                    super_px[(row * ss + dy) * src_dim + (col * ss + dx)] = 1

    out = bytearray(TARGET_W * TARGET_H)
    # box filter: each output pixel averages a (src_dim/TARGET) block
    bw = src_dim / TARGET_W
    bh = src_dim / TARGET_H
    for oy in range(TARGET_H):
        y0 = int(oy * bh)
        y1 = max(y0 + 1, int((oy + 1) * bh))
        for ox in range(TARGET_W):
            x0 = int(ox * bw)
            x1 = max(x0 + 1, int((ox + 1) * bw))
            total = 0
            count = 0
            for sy in range(y0, y1):
                base = sy * src_dim
                for sx in range(x0, x1):
                    total += super_px[base + sx]
                    count += 1
            coverage = round(255 * total / count) if count else 0
            out[oy * TARGET_W + ox] = 255 if coverage >= COVER_THRESH else 0
    return out

def render(font):
    alpha  = bytearray(IMG_W * IMG_H)
    bitmap = bytearray(IMG_W * IMG_H)

    # mplayer's actual blend per gen_font.py's own comment is ADDITIVE, not
    # a real blend: dst = (video * srca >> 8) + src. That means a nonzero
    # "src" (bitmap) value gets added on top REGARDLESS of alpha — so a
    # bitmap of 255 written across the WHOLE glyph cell (background
    # included, as this loop did before) saturates every pixel to white no
    # matter what alpha says. gen_font.py never had this bug because it
    # only ever touches pixels that are actually part of a glyph stroke —
    # background pixels are left at the bytearray default of 0/0
    # (untouched). Match that here: only write alpha/bitmap for foreground
    # (coverage-thresholded) pixels.
    for ci, code in enumerate(CODES):
        glyph  = glyph_for_code(font, code)
        cov    = glyph_coverage(glyph)
        x_base = ci * TARGET_W
        for gy in range(TARGET_H):
            for gx in range(TARGET_W):
                if not cov[gy * TARGET_W + gx]:
                    continue
                idx = gy * IMG_W + (x_base + gx)
                alpha[idx]  = 255
                bitmap[idx] = 255

    return alpha, bitmap

def make_desc():
    lines = [
        "[info]",
        "name MiSTerFin-sub",
        f"spacewidth {TARGET_W}",
        "charspace 1",
        f"height {TARGET_H}",
        "",
        "[files]",
        "alpha font-alpha.raw",
        "bitmap font-bitmap.raw",
        "",
        "[characters]",
    ]
    for ci, code in enumerate(CODES):
        start = ci * TARGET_W
        end   = start + TARGET_W - 1
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

    print(f"Generated assets/subfont/ ({IMG_W}x{IMG_H} px, {NUM_CHARS} chars, "
          f"{TARGET_H}px glyph height, {SUPERSAMPLE}x supersampled AA)")

if __name__ == "__main__":
    main()
