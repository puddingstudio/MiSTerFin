# MiSTerFin

<p align="center"><img src="docs/about.gif" width="100%" alt="MiSTerFin about screen"></p>

A [Jellyfin](https://jellyfin.org) client for the [MiSTer FPGA](https://misterfpga.org) platform. Browse your Movies/TV/Music library, see cover art and overview, and play back on a CRT — video is server-side transcoded and letterboxed to PAL or NTSC, with client-side subtitles, full pause/seek/resume support, and a proper music player with a now-playing screen. Works with whatever analog output your MiSTer is already set up for (SCART, composite, component, ...) — MiSTerFin just writes to the standard framebuffer, same as any other MiSTer app.

Having trouble getting it onto a CRT? Check [docs/DISPLAY_COMPATIBILITY.md](docs/DISPLAY_COMPATIBILITY.md) for confirmed working display/cable/`MiSTer.ini` combinations.

The UI is currently tuned for PAL/NTSC-resolution CRT output (288p/240p) — it isn't optimized for higher resolutions yet (that may come later). That said, a regular multisync VGA CRT monitor can still be made to show a genuine 240p picture via an `MiSTer.ini` trick documented in the compatibility guide above — in effect, "simulating" a CRT TV's picture on an ordinary computer monitor, for a PVM/BVM-like look without needing broadcast-video hardware.

---

## Features

- **Quick Connect sign-in** — no API key, no admin dashboard, no password typed on a gamepad. First launch shows a code; approve it from any device already signed into Jellyfin and the login is saved. A revoked login is detected and re-requested automatically. API keys still work for existing setups
- **Continue Watching and Next Up** appear as the first two cards on the home screen, ahead of the libraries, and only when they have something in them. Episodes there show which series and which episode they are, since a row called "Episode 03" out of context identifies nothing
- **Home screen** is a horizontal library carousel — name + item count per library, the active one centered, with a dimmed cover-art mosaic from that library filling the background. Every library gets its own slot along the strip (extras simply run off-screen rather than being hidden), and LEFT/RIGHT slides between them with the background fading through black. SELECT swaps to a classic list view instead, if you prefer that; either way, the selected library is remembered when you back out of one. B opens an exit-confirm dialog instead of quitting immediately, so a stray press can't silently close the app
- **Browsing within a library** (movies/series/albums/episodes/tracks) uses a list with cover art per item, watched/resume badges, a live clock, and a scrolling marquee for titles too long to fit (e.g. "Artist / Album", "Series / Season"). Albums show year + track count, artists show album count, and series show season + episode count
- **Info screen** with cover art, description, year, and status
- **Video playback** is server-side transcoded with correct letterbox/pillarbox scaling for any source aspect ratio
- **Pause menu** with a live progress bar, VSync ON/OFF toggle, resume/stop
- **Subtitles** are rendered client-side (instant toggle/switch, no re-buffering) for text-based tracks, with a picker menu and live sync fine-tuning; image-based tracks (PGS/VobSub — no text to hand back client-side) fall back to a server-side burn-in automatically instead of silently failing to show. ASS/SSA subtitles are cleaned up on the way in — inline override codes like `{\an8}` and `{\i1}` are stripped rather than drawn on screen as literal text
- **Alternate audio tracks** — a second tab in the same SELECT menu lists every audio stream (language, codec, channel count, and whatever else the server puts in its display title, so a commentary track is distinguishable from the main mix). Switching restarts the stream at the current position, since the server transcodes one chosen track into what it sends
- **Music library**: browse Artists → Albums → Tracks, direct-play audio (no server transcode needed for a plain FLAC/MP3 file), a now-playing screen with a live clock, cover art (falls back to the album's cover for a track with no embedded art of its own), a real audio-reactive VU meter pair (reads mplayer's own live PCM export, not a decorative animation), a SELECT-cycled background effect — starfield, rain, or a faithful port of [MiSTer-Toasty-Squadron](https://github.com/puddingstudio/MiSTer-Toasty-Squadron)'s own flying-toaster screensaver (same flight paths, sizes, and moon, right down to its biggest sprites flying over the cover art) — seek within a track, and prev/next-track navigation that auto-advances at the end of each track
- **Sync**: resume position and watched status are read from and reported back to Jellyfin, so they stay in sync with your other Jellyfin clients
- **About screen** with a GitHub-releases update check and one-button in-app update (A installs, applied on next launch); the same animated starfield background also shows on the setup screen if `jellyfin.conf` is missing/misconfigured

## Scope (v1)

- Movies, TV shows, and Music — no photo libraries
- Server-side transcode for video (the MiSTer's ARM Cortex-A9 can't decode arbitrary HEVC/4K sources locally) to a CRT-sized stream, then letterboxed/pillarboxed client-side to exactly fill the PAL/NTSC frame
- Audio plays back directly (`static=true`, no server transcode) — this mplayer build decodes FLAC/MP3 natively, and there's no letterboxing concern for audio the way there is for video

---

## Requirements

- MiSTer FPGA (standard Linux image, standard `menu.rbf` — no special core required)
- A reachable Jellyfin server + an API key
- `curl` on the MiSTer (included in the standard MiSTer Linux image)

Grab the latest release zip from the [Releases](../../releases) page and skip straight to **Installation** below, or build from source if you'd rather. Everything needed to run MiSTerFin (including its own `mplayer-arm`) is built from this repo either way; you don't need a separate mplayer install or any other MiSTer app already set up.

Prefer updating through a MiSTer Downloader? There's also a community-maintained [MiSTerFin database](https://github.com/theypsilon/MultiDatabases_MiSTer/tree/main/misterfin) for [Downloader](https://github.com/MiSTer-devel/Downloader_MiSTer) — install it once and future updates get picked up automatically alongside your other MiSTer Downloader-managed content, as an alternative to the in-app updater above.

Video output goes through the standard MiSTer framebuffer path (`mplayer -vo fbdev:/dev/fb0`) and works on any menu core.

---

## Building from Source

Requires [Zig](https://ziglang.org) (for ARM cross-compilation of the app itself), [Docker](https://www.docker.com) (only for building `mplayer-arm`, MiSTerFin's own fbdev-patched mplayer — a one-time step, not needed again unless you want to rebuild it), and `sshpass` (only for `make deploy`, which copies everything over SSH — skip it and copy files manually if you don't want it installed).

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

`make deploy` copies `misterfin-arm`, `mplayer-arm`, `assets/font/`, `assets/subfont/`, `assets/toasty/`, `assets/about.png`, and the launcher script to the right places on the MiSTer — see the `deploy` target in `Makefile` if you want to do it manually instead.

`make deploy` targets `mister.local` by default. This works as-is if your MiSTer is visible under that hostname on your network (its stock image advertises itself via mDNS) and you haven't changed the default `root` login. If `mister.local` doesn't resolve for you, override it with your MiSTer's IP instead: `make deploy MISTER_HOST=192.168.x.x`.

If you'd rather not build `mplayer-arm` yourself, MPlayer 1.5 built with `--enable-fbdev --enable-alsa` and the vsync patch in `docker/vo_fbdev.c` applied will work — that's exactly what `docker/build-mplayer.sh` automates.

### Running it on a desktop

`tools/run-local.sh` runs the real app on an ordinary Linux box, so UI and API work doesn't need a flash-and-look cycle for every change. There's no `/dev/fb0` to draw into, so the app is pointed at a plain malloc'd buffer of the same size and reads keys from the terminal instead of `/dev/input/eventN`; every frame is dumped and converted to a PNG by `tools/raw_to_png.py` (stdlib `zlib` only — no image library needed).

```bash
tools/run-local.sh                            # interactive: arrows, Enter, Esc, Tab, q to quit
tools/run-local.sh -k "right,right,a"         # scripted, then screenshot the result
tools/run-local.sh -k "down:200,a:1500" -o shot.png
tools/run-local.sh --ntsc -k "a"              # 240-line NTSC geometry instead of PAL's 288
```

It needs a `./jellyfin.conf` in the repo root pointing at a real server (gitignored, same file `--preview-browse` expects). **Video playback is not usable this way** — mplayer writes straight into a real framebuffer, which a malloc'd buffer can't stand in for; everything else (browsing, info screens, music metadata, menus, auth) works normally.

The underlying switches are plain environment variables if you'd rather drive them yourself: `MISTERFIN_FB=640x288` picks the headless buffer size, `MISTERFIN_FRAME_OUT=<path>` dumps each frame, `MISTERFIN_STDIN=1` reads the terminal, and `MISTERFIN_KEYS="right,a:800"` plays a scripted sequence (`MISTERFIN_KEYS_HOLD=1` to stay running afterwards instead of quitting).

---

## Installation

1. Copy these files (from a downloaded release zip, or your own build) to `/media/fat/misterfin/` on your MiSTer:

   ```
   misterfin-arm
   mplayer-arm
   font/
   subfont/
   toasty/
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

Create `/media/fat/misterfin/jellyfin.conf`. The only line you actually need is your server:

```
http://<your-jellyfin-ip>:8096
PAL
```

On first launch MiSTerFin shows a **Quick Connect** code. Open Jellyfin on any device where you're already signed in, go to your user menu → Quick Connect, and type the code in. That's it — the resulting login is saved to `token.conf` next to the config, so it's a one-time step. If the login is ever revoked, the app notices and asks again by itself.

Two small files get written next to `jellyfin.conf` and don't need creating yourself: `token.conf` (the saved login) and `device.conf` (a random GUID identifying this install to the server). Delete `token.conf` to sign out. Don't copy `device.conf` between two MiSTers that use the same Jellyfin account — Jellyfin logs out any existing session sharing a device id, so they'd take turns signing each other out.

Quick Connect must be enabled server-side (Dashboard → General → Quick Connect); it's on by default on most installs. If it's off, MiSTerFin says so and tells you the alternative.

**TV mode** is `PAL` or `NTSC`, optional, defaults to `PAL`. It's recognised wherever it appears in the file, so you don't have to pad the lines above it.

**Transcode profile** is also optional, and likewise recognised wherever it appears — `WxH` or `WxH@BITRATE`, e.g. `640x480@12000000`. It sets what the server is asked to transcode video down to before mplayer scales it back up to fill the screen; the default is `480x270@8000000`. The active profile is shown on the pause screen so you can confirm which one is in effect.

Raising it is the main lever on picture quality, and the main way to run out of CPU. Total pixel count is what costs decode time on this hardware — bitrate is close to free (2 Mbps and 8 Mbps both measured ~34% CPU with no A/V drift), while `720x576` was confirmed unsustainable: A/V desync grew continuously and the CPU saturated. `480x270` is the known-good default. Anything in between is unmeasured, so if you raise it, watch for audio drifting out of sync — that's the symptom that appears first.

### Using an API key instead

Still supported, and unchanged if you already have one set up:

```
http://<your-jellyfin-ip>:8096
your-api-key-here
your-username-here
PAL
```

1. **Server URL** — no trailing slash.
2. **API key** — Jellyfin admin dashboard → Advanced → API Keys → **+**. Name it something recognizable like "MiSTerFin" when you create it — Jellyfin's Dashboard → Devices/Sessions shows the *key's own registered name* as the client, permanently fixed at creation time, not something MiSTerFin can override later (its actual version number still shows up correctly regardless). A generically-named key (or one created before you'd settled on a name) will just show that name instead — harmless, but confusing to look at later.
3. **Username** — your plain Jellyfin username. MiSTerFin resolves it to the real user id via `GET /Users` at startup.

Quick Connect is the better option where you have the choice. An API key isn't scoped to a user — it authenticates as the *server*, and which user's watch state gets written is decided entirely by the `userId` the client sends, resolved from the username on line 3. That works fine, but it's honour-system: the same key can read and write any account on the server, so it's a much broader credential than the job needs. It also requires admin dashboard access to create in the first place, and it's why playback progress has to be reported through a per-user endpoint rather than the normal session one (see `report_user_data` — `Sessions/Playing/Progress` silently does nothing without a real user session).

A Quick Connect token *is* the user, so none of that applies: nothing to resolve, nothing to trust, and no access beyond that one account.

A saved Quick Connect login takes precedence over an API key in the file, so re-authenticating once sticks even if an old key is left behind.

There is no on-screen keyboard for the server URL — edit the file over SSH (using the standard MiSTer root password) or by pulling the SD card.

---

## Controls

Button labels below follow Xbox-style naming (bottom face button = A, right face button = B) — this matches most controllers, including generic/8BitDo pads in Xbox mode. Nintendo/SNES-style controllers are the notable exception: their A/B (and X/Y) positions are swapped relative to Xbox, so on those pads the button positions are reversed from the labels here.

### Browser
| Button | Keyboard | Action |
|--------|----------|--------|
| Left / Right | Left / Right | Navigate the home screen's library carousel |
| Up / Down | Up / Down | Navigate (home screen in list mode, or anywhere below it) |
| SELECT | Tab | Home screen only: swap between the carousel and the classic list |
| B | Enter / X | Open / drill in (library → series → season → episode, or library → artist → album → track) |
| A | Esc / Backspace / Z | Back (opens an exit-confirm dialog from the top-level library screen — A cancels, B confirms) |
| START | Pause / Home | About screen |

### About screen (START)
| Button | Keyboard | Action |
|--------|----------|--------|
| B | Enter / X | Install the update, if one's available (applied on next launch) |
| A | Esc / Backspace / Z | Back |

### Info screen (movies/episodes)
| Button | Keyboard | Action |
|--------|----------|--------|
| B | Enter / X | Play (resumes automatically if a resume position exists) |
| SELECT | Tab | Restart from the beginning (only shown if a resume position exists) |
| A | Esc / Backspace / Z | Back to browser |

### During video playback
| Button | Keyboard | Action |
|--------|----------|--------|
| B | Enter / X | Pause / resume |
| Left / Right | Left / Right | Seek back/forward 30s (or adjust subtitle sync, in the subtitle menu) |
| SELECT | Tab | Open the audio/subtitle track picker |
| L | PageUp | VSync ON |
| R | PageDown | VSync OFF |
| A | Esc / Backspace / Z | Stop, back to browser |

### Track picker (SELECT during video playback)

Two tabs — **AUDIO** and **SUBTITLES** — switched with the shoulder buttons.

| Button | Keyboard | Action |
|--------|----------|--------|
| L / R | PageUp / PageDown | Switch between the AUDIO and SUBTITLES tabs |
| Up / Down | Up / Down | Select a track (SUBTITLES also has an "Off" entry) |
| Left / Right | Left / Right | Adjust subtitle sync offset (SUBTITLES tab only) |
| B | Enter / X | Apply |
| A / SELECT | Esc / Backspace / Z / Tab | Cancel |

A `>` marks the track currently playing. Changing the **subtitle** track is instant for text-based tracks (rendered client-side). Changing the **audio** track always restarts the stream at the current position — Jellyfin transcodes one chosen audio stream into what it sends, so there's no way to switch it client-side the way a subtitle can be. Changing both at once costs a single restart, not two.

VSync is ON by default (tear-free) — turn it OFF if you'd rather trade tearing for a bit more decode headroom.

### Now playing (music) — selecting a track plays it immediately, no separate info screen
| Button | Keyboard | Action |
|--------|----------|--------|
| B | Enter / X | Pause / resume |
| Left / Right | Left / Right | Seek back/forward 10s within the track |
| Up / Down | Up / Down | Previous / next track in the current album/list |
| SELECT | Tab | Cycle the background effect (starfield / rain / Toasty Squadron sprites) |
| A | Esc / Backspace / Z | Stop, back to browser |

Reaching the end of a track auto-advances to the next one in the same list, same as any normal music player. Audio is direct-played, so seeking is a real in-place seek (no stop/restart the way video's seek needs).

A keyboard works standalone, with no gamepad attached.

---

## Known limitations

Verified against a real Jellyfin 10.11 server: auth, browsing (views/items, including Music), resume position, cover art, subtitles, video playback (transcoded over TS), and music playback (direct-played FLAC/MP3) all confirmed working end-to-end on real MiSTer hardware.

- **`playSessionId` is required on the video stream URL.** Without it, Jellyfin can silently serve back a stale cached transcode from an earlier request instead of honoring the current `maxWidth`/`maxHeight`/`videoBitRate` — confirmed on a real server. Already handled in `jf_stream_url()`.
- **Hardware-accelerated server transcoding (QSV/NVENC/VAAPI) may be broken on the user's server and is outside this client's control.** If `/Videos/{id}/stream` returns HTTP 500, check the Jellyfin server log (`/System/Logs`) for `FfmpegException` — if the ffmpeg command line shows `h264_qsv`/`h264_nvenc`/`vaapi`, the fix is server-side: Dashboard → Playback → Transcoding → Hardware acceleration → None (or fix the GPU driver).
- **No NEON-accelerated colorspace conversion in this mplayer/ffmpeg build.** Total pixel count (resolution) is what actually costs CPU, not bitrate — confirmed on hardware that doubling `videoBitRate` at a fixed resolution moved average CPU usage by only ~1 percentage point, while trying native PAL resolution (720x576) instead of the default 480x270 caused continuously growing A/V desync and pegged the CPU. Keep the transcode resolution small and spend bitrate freely on quality instead; let mplayer's own `-vf` scale it back up, which is cheap relative to decode.
- **mplayer's native MPEG-TS demuxer misses the video track** on Jellyfin's transcoded TS output — fixed by forcing `-demuxer lavf` in `play()`. If you ever see audio-only playback, this is the first thing to check.
- **Letterbox/pillarbox requires `dsize` as the last `-vf` stage, or mplayer overrides your sizing.** See the `-vf` chain in `play()` if you're touching this.
- **mplayer's `-af export` header fields (nch/sz) don't match this build's actual export file size** — the now-playing VU meter reads however many samples are actually present after the 8-byte header instead of trusting those fields. See `read_af_samples()` if you're touching the visualizer.
- **Any mplayer slave command sent while paused silently resumes playback unless prefixed with `pausing_keep`.** This isn't Jellyfin/MiSTerFin-specific, just an mplayer slave-mode quirk — but it's the reason pause-state commands (subtitle visibility, seeking while paused) are all prefixed that way throughout `main.c`.
- **Server-side subtitle burn-in (image-based tracks only) forces a full stream restart, and seeking with it active re-decodes from the true start of the file up to the seek target** — a multi-minute stall on a deep seek. Text-based tracks are unaffected (rendered client-side, no restart). See `jf_stream_url()`'s `burn_in_sub_index` comment.
- **`MediaSourceId` is omitted** from stream/progress/subtitle requests rather than guessed — works for direct single-version items; multi-version items (multiple cuts/qualities of the same title) may not resolve to the version you expect.
- **The on-screen font is ASCII-only**, so non-ASCII titles are transliterated rather than drawn as-is (`Amélie` → `Amelie`, `Zoë & Co.` → `Zoe & Co.`, smart quotes and em-dashes normalised). Accented Latin folds to its base letter; anything with no ASCII form — CJK, Cyrillic, Greek — collapses to a single `?`. See `jf_text_to_display()`. Fixing this properly means a larger font atlas, not a parsing change.
- **No on-screen keyboard** for server setup — `jellyfin.conf` must be edited manually (SSH or SD card).
- Silent failure if `mplayer-arm` can't open the stream (bad URL, server down, transcode rejected) — you're dropped back to the browser with no error message.

---

## Changelog

### v0.9.3
- Redesigned info screen — full-height backdrop art, bigger logo, star rating
- Home screen loads faster — library covers now cache to the SD card and prefetch in the background
- No more black screen on startup — shows a loading indicator instead
- Button hints now use Xbox-style A/B/X/Y labels to match most modern controllers
- Jellyfin dashboard now correctly shows device name and app version
- Display compatibility guide now also covers PAL 288p@100Hz on VGA CRT monitors

### v0.9.2
- Fixed video corruption (comb/tearing artifacts) on NTSC displays
- Various NTSC UI polish — margins, list size, music player cover
- Display compatibility guide now also covers VGA CRT monitors (240p@120Hz)

### v0.9.1
- NTSC display support — UI now adapts correctly instead of looking stretched
- New display compatibility guide for CRT/SCART/component setups having trouble getting picture

### v0.9
- First public preview release

---

## Licence

[CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/) — free to use, share, and modify, non-commercial only.

---

| <a href="https://pudding.studio"><img src=".github/images/pudding.gif" width="100"></a> | *made over the weekends at pudding*<br>https://pudding.studio |
|:---:|:---|
