#!/usr/bin/env python3
"""
Assemble the raw BGRX framebuffer dumps produced by
`misterfin-arm --capture-about N` (see run_capture_about() in src/main.c)
into a single looping animated GIF for the repo's README/about.md.

No PIL/ImageMagick/ffmpeg available in the environment this was written in,
so this implements GIF89a writing (global color table + per-frame LZW image
data + Netscape looping extension) from scratch, including a matching
decoder used purely to self-check each frame's LZW stream before trusting
the output file (there's no way to open the result in an image viewer from
here to eyeball it).

Usage: python3 tools/capture_about_gif.py <frames_dir> <width> <height> <out.gif>
  frames_dir  directory of about_frame_NNN.raw files, 4 bytes/pixel BGRX
  width/height  from the "W H STRIDE" line --capture-about prints
"""
import sys, os, glob, array, struct
from collections import Counter

DELAY_CS = 4          # centiseconds per frame (40ms, matches the capture's own usleep)
MAX_COLORS = 256

def load_frames(frames_dir, w, h):
    paths = sorted(glob.glob(os.path.join(frames_dir, "about_frame_*.raw")))
    if not paths:
        raise SystemExit(f"no frames found in {frames_dir}")
    frames = []
    for p in paths:
        data = open(p, "rb").read()
        assert len(data) == w * h * 4, f"{p}: expected {w*h*4} bytes, got {len(data)}"
        arr = array.array('I')
        arr.frombytes(data)
        # each uint32 is 0x00RRGGBB already (B,G,R,0 in memory, little-endian host)
        frames.append(arr)
    return frames

def resize_vertical(arr, w, src_h, dst_h):
    """The app's own framebuffer (640x288 here) uses non-square pixels —
    MiSTer stretches it 5/3 vertically to fill a real 4:3 CRT (same ratio
    draw_about()'s own blit_fit_centered() image-sizing math already
    accounts for). A GIF has no pixel-aspect metadata, so dumping the raw
    buffer 1:1 renders squashed/stretched in any normal viewer — confirmed
    by the user ("jako rasiren" / stretched). Pre-stretching each frame to
    640x480 (exactly 5/3 taller) here bakes in the correct 4:3 look.

    Nearest-neighbor row duplication, not blending — keeps the bitmap
    font/pixel-art edges crisp instead of blurring them, matching how the
    real CRT scaler (and the app's own chunky look) reads (user preference)."""
    out = array.array('I', bytes(4 * w * dst_h))
    for dy in range(dst_h):
        sy = round(dy * (src_h - 1) / (dst_h - 1))
        row = sy * w
        out[dy * w:(dy + 1) * w] = arr[row:row + w]
    return out

def build_palette(colors_counter, max_colors):
    """Weighted median-cut quantization down to <= max_colors entries.
    Returns a list of (r,g,b) tuples."""
    items = [((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, cnt)
             for c, cnt in colors_counter.items()]
    buckets = [items]

    def channel_range(bucket, ch):
        vals = [p[ch] for p in bucket]
        return max(vals) - min(vals)

    while len(buckets) < max_colors:
        # split the bucket with the largest weighted spread on its widest channel
        best_idx, best_ch, best_range = -1, 0, -1
        for i, b in enumerate(buckets):
            if len(b) <= 1:
                continue
            for ch in range(3):
                r = channel_range(b, ch)
                if r > best_range:
                    best_range, best_idx, best_ch = r, i, ch
        if best_idx < 0:
            break
        bucket = buckets.pop(best_idx)
        bucket.sort(key=lambda p: p[best_ch])
        total = sum(p[3] for p in bucket)
        acc, split = 0, 0
        for i, p in enumerate(bucket):
            acc += p[3]
            if acc >= total / 2:
                split = i + 1
                break
        split = max(1, min(split, len(bucket) - 1))
        buckets.append(bucket[:split])
        buckets.append(bucket[split:])

    palette = []
    for b in buckets:
        total = sum(p[3] for p in b)
        r = sum(p[0] * p[3] for p in b) / total
        g = sum(p[1] * p[3] for p in b) / total
        bl = sum(p[2] * p[3] for p in b) / total
        palette.append((round(r), round(g), round(bl)))
    return palette

def nearest_index(rgb, palette):
    r, g, b = rgb
    best_i, best_d = 0, None
    for i, (pr, pg, pb) in enumerate(palette):
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if best_d is None or d < best_d:
            best_d, best_i = d, i
    return best_i

def build_color_map(colors_counter, palette):
    return {c: nearest_index(((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF), palette)
            for c in colors_counter}

# ---------------------------------------------------------------------------
# GIF LZW (encoder + matching decoder, used only for self-verification)

class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.nbits = 0
    def write(self, code, width):
        self.acc |= code << self.nbits
        self.nbits += width
        while self.nbits >= 8:
            self.buf.append(self.acc & 0xFF)
            self.acc >>= 8
            self.nbits -= 8
    def flush(self):
        if self.nbits > 0:
            self.buf.append(self.acc & 0xFF)
            self.acc = 0
            self.nbits = 0
        return self.buf

def lzw_encode(indices, min_code_size):
    clear_code = 1 << min_code_size
    end_code = clear_code + 1
    bw = BitWriter()

    def reset_table():
        table = {(i,): i for i in range(clear_code)}
        return table, end_code + 1, min_code_size + 1

    table, next_code, code_size = reset_table()
    bw.write(clear_code, code_size)

    prefix = None
    for c in indices:
        if prefix is None:
            prefix = (c,)
            continue
        cand = prefix + (c,)
        if cand in table:
            prefix = cand
        else:
            bw.write(table[prefix], code_size)
            if next_code < 4096:
                table[cand] = next_code
                next_code += 1
                if next_code > (1 << code_size) and code_size < 12:
                    code_size += 1
            else:
                bw.write(clear_code, code_size)
                table, next_code, code_size = reset_table()
            prefix = (c,)
    if prefix is not None:
        bw.write(table[prefix], code_size)
    bw.write(end_code, code_size)
    return bw.flush()

class BitReader:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.acc = 0
        self.nbits = 0
    def read(self, width):
        while self.nbits < width:
            self.acc |= self.data[self.pos] << self.nbits
            self.pos += 1
            self.nbits += 8
        val = self.acc & ((1 << width) - 1)
        self.acc >>= width
        self.nbits -= width
        return val

def lzw_decode(data, min_code_size):
    clear_code = 1 << min_code_size
    end_code = clear_code + 1
    br = BitReader(data)

    def reset_table():
        table = {i: (i,) for i in range(clear_code)}
        return table, end_code + 1, min_code_size + 1

    table, next_code, code_size = reset_table()
    out = []
    prev = None
    while True:
        code = br.read(code_size)
        if code == clear_code:
            table, next_code, code_size = reset_table()
            prev = None
            continue
        if code == end_code:
            break
        if code in table:
            entry = table[code]
        else:
            entry = prev + (prev[0],)
        out.extend(entry)
        # The decoder's table is permanently one entry behind the encoder's
        # (it can't form prev+entry[0] until it has decoded BOTH prev and
        # the current entry, one code later than when the encoder formed
        # the same pair) — so its code-size bump must fire one code EARLIER
        # than the encoder's own threshold to land on the same code_size by
        # the time it reads the code that needs the extra bit. Checking the
        # bump against next_code (pre-increment) == (1<<code_size)-1, not
        # next_code (post-increment) > (1<<code_size) as the encoder does,
        # is exactly that one-code compensation — confirmed by a mismatch
        # assertion (encoder/decoder code_size trace disagreed) before this
        # fix, and by the round-trip self-check in main() after it.
        if prev is not None and next_code < 4096:
            table[next_code] = prev + (entry[0],)
            if next_code == (1 << code_size) - 1 and code_size < 12:
                code_size += 1
            next_code += 1
        prev = entry
    return out

# ---------------------------------------------------------------------------

def sub_blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out.extend(chunk)
    out.append(0)
    return bytes(out)

def write_gif(path, w, h, palette, frame_indices, min_code_size, delay_cs):
    pal_size_field = 7  # 2^(7+1) = 256 entries
    full_palette = list(palette) + [(0, 0, 0)] * (256 - len(palette))

    out = bytearray()
    out += b"GIF89a"
    out += struct.pack("<HH", w, h)
    out.append(0b10000111)   # global color table present, 8-bit color res, table size=256
    out.append(0)             # background color index
    out.append(0)             # pixel aspect ratio
    for r, g, b in full_palette:
        out += bytes((r, g, b))

    # Netscape looping extension — 0 = loop forever
    out += bytes((0x21, 0xFF, 0x0B)) + b"NETSCAPE2.0" + bytes((0x03, 0x01, 0, 0, 0x00))

    for indices in frame_indices:
        out += bytes((0x21, 0xF9, 0x04, 0x00))
        out += struct.pack("<H", delay_cs)
        out += bytes((0x00, 0x00))
        out += bytes((0x2C,))
        out += struct.pack("<HHHH", 0, 0, w, h)
        out.append(0x00)   # no local color table, no interlace
        out.append(min_code_size)
        lzw = lzw_encode(indices, min_code_size)
        out += sub_blocks(bytes(lzw))

    out.append(0x3B)
    with open(path, "wb") as f:
        f.write(out)

def main():
    if len(sys.argv) != 5:
        raise SystemExit("usage: capture_about_gif.py <frames_dir> <w> <h> <out.gif>")
    frames_dir, w, h, out_path = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]

    raw_frames = load_frames(frames_dir, w, h)
    print(f"loaded {len(raw_frames)} frames of {w}x{h}")

    dst_h = round(h * 5 / 3)   # undo the app's 5:3 non-square-pixel PAL buffer, see resize_vertical()
    frames = [resize_vertical(fr, w, h, dst_h) for fr in raw_frames]
    h = dst_h
    print(f"resized to {w}x{h} (4:3)")

    # Vertical interpolation can blend in colors that didn't exist in any
    # single raw frame, and which row lands on a blend boundary shifts as
    # the stars move — so unlike the raw capture, the palette must be built
    # from every frame's colors, not just a sample.
    all_colors = Counter()
    for fr in frames:
        all_colors.update(fr)
    print(f"{len(all_colors)} distinct colors across all frames")

    palette = build_palette(all_colors, MAX_COLORS)
    print(f"quantized to {len(palette)} palette entries")

    color_map = {}
    for c in all_colors:
        r, g, b = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
        color_map[c] = nearest_index((r, g, b), palette)
    print(f"resolved index map for {len(color_map)} distinct colors across all frames")

    frame_indices = []
    for i, fr in enumerate(frames):
        idx = [color_map[c] for c in fr]
        frame_indices.append(idx)
        print(f"frame {i}: indexed", end="\r")
    print()

    min_code_size = 8  # matches the 256-entry global color table
    print("self-checking LZW round-trip on frame 0...")
    encoded = lzw_encode(frame_indices[0], min_code_size)
    decoded = lzw_decode(encoded, min_code_size)
    assert decoded == list(frame_indices[0]), "LZW round-trip mismatch on frame 0!"
    print("round-trip OK")

    write_gif(out_path, w, h, palette, frame_indices, min_code_size, DELAY_CS)
    size = os.path.getsize(out_path)
    print(f"wrote {out_path} ({size/1024:.1f} KB)")

if __name__ == "__main__":
    main()
