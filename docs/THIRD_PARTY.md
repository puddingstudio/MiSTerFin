# Third-party code and components

Everything MiSTerFin builds on that wasn't written for this project, with
where it lives and under what terms. MiSTerFin's own code is licensed
[CC BY-NC 4.0](../LICENSE); the components below keep their own licences.

## Bundled in this repository

- **[stb_image](https://github.com/nothings/stb) v2.30** — Sean Barrett and
  contributors. Image decoding (covers, backdrops, PNG/JPEG/GIF).
  Vendored verbatim as `src/stb_image.h`. Dual-licensed MIT / public
  domain; used here as public domain.

- **[font8x8](https://github.com/dhepper/font8x8)** — Daniel Hepper, public
  domain, based on the IBM VGA font via Marcel Sondaar. The app's entire
  bitmap font: vendored as `src/font8x8.h` (extended in-project with the
  Latin-1 range), and the mplayer OSD/subtitle font atlases in `assets/`
  are generated from it by `tools/gen_font.py` / `tools/gen_subfont.py`.

- **[MPlayer](https://mplayerhq.hu) 1.5** — the MPlayer team, GPL-2.0-or-later.
  All video/audio playback. Release packages ship `mplayer-arm` built from
  unmodified upstream source by `docker/build-mplayer.sh`, with one
  modified file: `docker/vo_fbdev.c` is MPlayer's framebuffer video driver
  (© 2001 Szabolcs Berecz and MPlayer contributors) carrying this
  project's vsync-wait and page-flip patch. That file — and the shipped
  `mplayer-arm` binary — remain under the GPL; corresponding source is the
  upstream 1.5 release plus that one file in this repository.

## External components MiSTerFin talks to

- **[Zaparoo Project](https://zaparoo.org)** — the dual-mode menu core
  (`menu_zaparoo.rbf`, a GPL fork of Menu_MiSTer, branch
  `feat/dual-mode-native-fb`). `src/ddr.c` is this project's own
  implementation of its DDR native-video protocol; when that core is
  installed, playback uses it automatically. See the note in `src/ddr.h`.

- **[Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer)** — the
  MiSTer-devel main firmware, GPL. `src/fb.c`'s FPGA SPI protocol (the
  vsync wait and the framebuffer page flip — register addresses, command
  words, handshake sequence) is this project's own implementation of the
  protocol as defined by Main_MiSTer's source; no code was copied.

- **[Interlaced Menu core](https://github.com/iwalton3/Menu_MiSTer/releases/tag/v0.0.1)**
  — Izzie Walton's GPL fork of Menu_MiSTer adding true 576i/480i scanout,
  downloaded separately by users who want interlaced output (see the
  [display compatibility guide](DISPLAY_COMPATIBILITY.md#interlaced-output)).

## Reference and inspiration (no code copied)

- **[jellyfin-apiclient-python](https://github.com/jellyfin/jellyfin-apiclient-python)**
  — reference for the exact shape of the MediaBrowser `Authorization`
  header a real Jellyfin client sends.

- **Ryan Geiss** — the Nebula music visualizer is an independent
  implementation inspired by his classic feedback-based visualizers.
