# Ghostty interactive harness

This helper presents MiSTerFin's existing desktop framebuffer inside Ghostty. MiSTerFin still reads the terminal directly, so the helper does not translate or intercept input.

From the repository root, run:

```bash
python3 tools/ghostty/ghostty_harness.py --ntsc
```

Use `--pal` for the 640x288 layout. PAL is the default. The helper builds the host binary before launch. Pass `--no-build` to use the existing binary.

Keys match the desktop harness:

- Arrow keys navigate.
- `B`, Enter, or `X` confirms, matching the on-screen B label.
- `A`, Escape, Backspace, or `Z` goes back, matching the on-screen A label.
- Tab is Select.
- Home or `P` is Start.
- Page Up or `[` is the left shoulder button.
- Page Down or `]` is the right shoulder button.
- `Q` exits.

The helper writes MiSTerFin's stdout and stderr to `/tmp/misterfin-ghostty.log` so terminal output cannot corrupt the image. Pass `--log PATH` to choose another location.

Artwork is cached under `/tmp/misterfin-cache` by default. Set `MISTERFIN_CACHE_ROOT` before launching the helper to use another location.

Ghostty must report `TERM=xterm-ghostty`. The `--force` option permits another terminal that implements the Kitty graphics protocol.

The viewer double-buffers terminal images to avoid flicker. It uploads a complete frame under an alternate image ID, places the new frame over the current frame, and only then deletes the old frame. MiSTerFin's 640x240 and 640x288 framebuffers use non-square CRT pixels, so the viewer fits them into a physical 4:3 rectangle using the terminal's cell geometry. The viewer caps presentation at 20 FPS by default and skips duplicate frames. Change the cap with `--fps NUMBER`. This cap only affects the terminal preview. It does not change MiSTerFin's own frame loop.

Video playback remains unavailable in the desktop harness because `mplayer` opens `/dev/fb0` directly. Browsing, artwork, menus, setup, and metadata use the headless framebuffer and are visible.

Run the helper tests with:

```bash
python3 -m unittest tools/ghostty/test_ghostty_harness.py
```
