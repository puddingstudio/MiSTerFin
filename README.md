# MiSTerFin

<p align="center"><img src="docs/about.gif" width="100%" alt="MiSTerFin about screen"></p>

A [Jellyfin](https://jellyfin.org) client for the [MiSTer FPGA](https://misterfpga.org) platform. Browse your Movies/TV library, see poster art and overview, and play back on a CRT — video is server-side transcoded and letterboxed to PAL or NTSC, with client-side subtitles and full pause/seek/resume support. Works with whatever analog output your MiSTer is already set up for (SCART, composite, component, ...) — MiSTerFin just writes to the standard framebuffer, same as any other MiSTer app.

---

## Features

- Browse menu with library list, cover art per item, and watched/resume badges
- Info screen with cover art, description, year, and status
- Server-side transcoded playback with correct letterbox/pillarbox scaling for any source aspect ratio
- Pause menu with a live progress bar, VSync ON/OFF toggle, resume/stop
- Subtitles rendered client-side (instant toggle/switch, no re-buffering), with a picker menu and live sync fine-tuning
- Resume position and watched status read from and reported back to Jellyfin, so they stay in sync with your other Jellyfin clients
- About screen with a GitHub-releases update check

## Scope (v1)

- Movies and TV shows (video) only — no music/photo libraries
- Server-side transcode (the MiSTer's ARM Cortex-A9 can't decode arbitrary HEVC/4K sources locally) to a CRT-sized stream, then letterboxed/pillarboxed client-side to exactly fill the PAL/NTSC frame

---

## Requirements

- MiSTer FPGA (standard Linux image, standard `menu.rbf` — no special core required)
- A reachable Jellyfin server + an API key
- `curl` on the MiSTer (included in the standard MiSTer Linux image)

There is no prebuilt release yet — see **Building from Source** below. Everything needed to run MiSTerFin (including its own `mplayer-arm`) is built from this repo; you don't need a separate mplayer install or any other MiSTer app already set up.

Video output goes through the standard MiSTer framebuffer path (`mplayer -vo fbdev:/dev/fb0`) and works on any menu core.

---

## Building from Source

Requires [Zig](https://ziglang.org) (for ARM cross-compilation of the app itself) and [Docker](https://www.docker.com) (only for building `mplayer-arm`, MiSTerFin's own fbdev-patched mplayer — a one-time step, not needed again unless you want to rebuild it).

```bash
# 1. Build mplayer-arm (one-time; needs Docker)
cd docker
docker build -t misterfin-mplayer .
docker run --name misterfin-mplayer-build misterfin-mplayer
docker cp misterfin-mplayer-build:/build/mplayer-arm ../mplayer-arm
docker rm misterfin-mplayer-build
cd ..

# 2. Build the app itself
make arm

# 3. Deploy everything over SSH (uses the standard MiSTer root password)
make deploy
```

`make deploy` copies `misterfin-arm`, `mplayer-arm`, `assets/font/`, `assets/subfont/`, `assets/about.png`, and the launcher script to the right places on the MiSTer — see the `deploy` target in `Makefile` if you want to do it manually instead.

If you'd rather not build `mplayer-arm` yourself, MPlayer 1.5 built with `--enable-fbdev --enable-alsa` and the vsync patch in `docker/vo_fbdev.c` applied will work — that's exactly what `docker/build-mplayer.sh` automates.

---

## Installation

1. Copy these files to `/media/fat/misterfin/` on your MiSTer:

   ```
   misterfin-arm
   mplayer-arm
   font/
   subfont/
   about.png
   jellyfin.conf      (see below)
   ```

2. Copy the launcher script:

   ```
   tools/MiSTerFin.sh  →  /media/fat/Scripts/MiSTerFin.sh
   ```

3. Launch from the MiSTer **Scripts** menu.

(`make deploy` does steps 1–2 for you, minus `jellyfin.conf` itself.)

---

## Configuring `jellyfin.conf`

Create `/media/fat/misterfin/jellyfin.conf` — 4 lines (see `jellyfin.conf.example`):

```
http://192.168.2.10:8096
your-api-key-here
your-username-here
PAL
```

1. **Server URL** — no trailing slash.
2. **API key** — Jellyfin admin dashboard → Advanced → API Keys → **+**.
3. **Username** — your plain Jellyfin username. MiSTerFin resolves it to the real user id via `GET /Users` at startup (the raw id isn't easily findable anywhere in the Jellyfin UI — it only shows up buried in a dashboard URL — but everyone already knows their own username).
4. **TV mode** — `PAL` or `NTSC`. Optional, defaults to `PAL`.

There is no on-screen setup keyboard in v1 — edit the file over SSH (using the standard MiSTer root password) or by pulling the SD card.

---

## Controls

### Browser
| Button | Action |
|--------|--------|
| Up / Down | Navigate |
| A | Open / drill in (library → series → season → episode) |
| B | Back (exits the app from the top-level library list) |
| START | About screen |

### Info screen
| Button | Action |
|--------|--------|
| A | Play (resumes automatically if a resume position exists) |
| SELECT | Restart from the beginning (only shown if a resume position exists) |
| B | Back to browser |

### During playback
| Button | Action |
|--------|--------|
| A | Pause / resume |
| Left / Right | Seek back/forward 30s (or adjust subtitle sync, in the subtitle menu) |
| SELECT | Open the subtitle menu |
| L | VSync ON |
| R | VSync OFF |
| B | Stop, back to browser |

### Subtitle menu (SELECT during playback)
| Button | Action |
|--------|--------|
| Up / Down | Select subtitle track (or off) |
| Left / Right | Adjust subtitle sync offset |
| A | Apply |
| B / SELECT | Cancel |

VSync is ON by default (tear-free) — turn it OFF if you'd rather trade tearing for a bit more decode headroom.

---

## Known limitations

Verified against a real Jellyfin 10.11 server: auth, browsing (views/items), resume position, poster images, subtitles, and playback (video + audio, transcoded over TS) all confirmed working end-to-end on real MiSTer hardware.

- **`playSessionId` is required on the stream URL.** Without it, Jellyfin can silently serve back a stale cached transcode from an earlier request instead of honoring the current `maxWidth`/`maxHeight`/`videoBitRate` — confirmed on a real server. Already handled in `jf_stream_url()`.
- **Hardware-accelerated server transcoding (QSV/NVENC/VAAPI) may be broken on the user's server and is outside this client's control.** If `/Videos/{id}/stream` returns HTTP 500, check the Jellyfin server log (`/System/Logs`) for `FfmpegException` — if the ffmpeg command line shows `h264_qsv`/`h264_nvenc`/`vaapi`, the fix is server-side: Dashboard → Playback → Transcoding → Hardware acceleration → None (or fix the GPU driver).
- **No NEON-accelerated colorspace conversion in this mplayer/ffmpeg build.** Total pixel count dominates playback smoothness far more than codec/bitrate choice — keep the requested transcode resolution small and let mplayer's own `-vf` scale it back up, which is cheap relative to decode.
- **mplayer's native MPEG-TS demuxer misses the video track** on Jellyfin's transcoded TS output — fixed by forcing `-demuxer lavf` in `play()`. If you ever see audio-only playback, this is the first thing to check.
- **Letterbox/pillarbox requires `dsize` as the last `-vf` stage, or mplayer overrides your sizing.** See the `-vf` chain in `play()` if you're touching this.
- **`MediaSourceId` is omitted** from stream/progress/subtitle requests rather than guessed — works for direct single-version items; multi-version items (multiple cuts/qualities of the same title) may not resolve to the version you expect.
- **No on-screen keyboard** for server setup — `jellyfin.conf` must be edited manually (SSH or SD card).
- Silent failure if `mplayer-arm` can't open the stream (bad URL, server down, transcode rejected) — you're dropped back to the browser with no error message.

---

## Licence

MIT
