#!/usr/bin/env python3
"""
Converts a raw BGRX8888 framebuffer dump (from --capture-about or
--preview-browse) into a viewable PNG, using only the stdlib (zlib) — no
Pillow/ImageMagick in the environment this was written in, same constraint
capture_about_gif.py's from-scratch GIF writer already documents.

Usage: python3 tools/raw_to_png.py <in.raw> <width> <height> <out.png>
"""
import sys, struct, zlib


def raw_to_png(raw_path, w, h, out_path):
    with open(raw_path, "rb") as f:
        data = f.read()
    expected = w * h * 4
    if len(data) != expected:
        raise SystemExit(f"{raw_path}: expected {expected} bytes, got {len(data)}")

    # Each pixel is a little-endian uint32 0x00RRGGBB (fb_fill_rect_alpha
    # packs it that way) — in memory that's bytes [B, G, R, 0x00].
    scanlines = bytearray()
    for y in range(h):
        scanlines.append(0)  # filter type 0 (none)
        row = data[y * w * 4:(y + 1) * w * 4]
        for x in range(w):
            b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            scanlines += bytes((r, g, b))

    compressed = zlib.compress(bytes(scanlines), 9)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8-bit RGB truecolor
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", compressed) + chunk(b"IEND", b""))
    with open(out_path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    raw_to_png(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4])
