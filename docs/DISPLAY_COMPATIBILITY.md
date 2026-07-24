# Display Compatibility

MiSTerFin writes directly to the standard MiSTer framebuffer (`/dev/fb0`) — it doesn't know or care what's physically connected downstream. Whether it actually shows up correctly depends entirely on **MiSTer's own analog/HDMI output configuration** (`MiSTer.ini`) and the exact chain of cables/adapters between the MiSTer and the display. Most "I can't get it on my CRT" reports come down to an `MiSTer.ini` setting or an unsupported adapter combo, not a MiSTerFin bug.

This doc tracks combos we've actually verified, plus the exact `MiSTer.ini` keys that matter for each. **Confirmed = tested on real hardware by us.** Untested combos aren't necessarily broken — we just haven't verified them yet.

**Quick sanity check before anything else:** whatever input mode you select *on the display itself* (RGB vs Component/YPbPr) has to match `ypbpr` in `MiSTer.ini`. Confirmed on real hardware — RGB ini (`ypbpr=0`) + display set to RGB input = correct picture; same RGB ini + display switched to its Component input = solid pink, nothing usable. This is an easy thing to get out of sync (especially on displays that auto-label inputs or default to component) and looks like a much scarier problem than it is.

---

## ✅ Confirmed: Analog I/O board (VGA) → SCART → CRT TV

**Chain:** MiSTer main board → official MiSTer Analog I/O board (VGA-style DB15 output) → VGA-to-SCART cable → a classic consumer CRT TV, Sony Trinitron, SCART (RGB) input.

**Audio:** the VGA-to-SCART cable needs a separate 3.5mm jack input (alongside the VGA connector) to carry audio into the SCART's audio pins — video-only VGA-to-SCART cables exist and won't give you sound.

**Relevant `MiSTer.ini` settings:**

```ini
forced_scandoubler=0   ; leave scandoubler as the core/menu default, don't force it
ypbpr=0                ; RGB, not component — this combo is RGB over SCART
composite_sync=1       ; composite sync on the HSync line — what a VGA-to-SCART cable expects
vga_scaler=0           ; VGA port outputs the native/direct signal, not the HDMI scaler's output
```

**Plus this `[Menu]`-scoped override, further down the file — confirmed load-bearing, not optional:**

```ini
[Menu]
vga_scaler=1
video_mode=640,26,60,74,288,0,4,20,12587
```

MiSTerFin (like any MiSTer Script) runs under the "Menu" core context, so this section's `video_mode` is what actually sets the live framebuffer resolution it draws into — confirmed via `fbset -s` on real hardware: **with this section active, `/dev/fb0` is 640x288**; remove/comment it out and the framebuffer falls back to 640x480 instead. We initially assumed this section only affected the HDMI/scaler path and documented it as irrelevant here — that was wrong, verified by directly testing both states on the device. If MiSTerFin looks correctly proportioned on your CRT, this `[Menu]` block (or an equivalent custom video_mode tuned to a similar low interlaced-ish resolution) is very likely why.

`video_mode_pal`/`video_mode_ntsc` (the top-level ones, not `[Menu]`-scoped) are for `vsync_adjust` mode-switching and don't matter for this combo.

**Status:** this is our own daily dev/test setup — confirmed working throughout MiSTerFin's development.

---

## ✅ Confirmed: Analog I/O board (VGA) → Component (YPbPr) → professional monitor (PAL)

**Chain:** MiSTer main board → official MiSTer Analog I/O board (VGA-style DB15 output) → VGA-to-3xRCA component breakout cable → Sony LMD-1410 (professional LCD monitor), component input, PAL.

**Audio:** the VGA-to-component cable needs its own 3.5mm jack input feeding a separate 2xRCA stereo pair (not part of the 3 component video RCAs) — same idea as the SCART combo, just a stereo RCA pair instead of SCART's built-in audio pins.

| Connector on MiSTer | Connector on display | Picture |
|---|---|---|
| ![MiSTer Analog I/O board VGA connector](images/mister-connector.jpg) | ![Component breakout cable connected to the monitor's BNC inputs](images/tv-connector.jpg) | ![MiSTerFin displayed correctly on the LMD-1410](images/tv-picture.jpg) |

**Relevant `MiSTer.ini` settings:**

```ini
forced_scandoubler=0   ; same as the SCART combo
ypbpr=1                ; component, not RGB
composite_sync=0       ; do NOT force sync onto the HSync line for component — real YPbPr
                        ; expects sync embedded in the Y/luma channel itself. Leaving this at
                        ; the SCART combo's value of 1 (copied over without changing it) was
                        ; the actual bug the first time we tried this: the display synced fine
                        ; (correctly identified the signal as PAL 576i) but rendered everything
                        ; with a strong green cast — a classic symptom of the sync signal
                        ; fighting with the Y channel. composite_sync=0 fixed it completely.
vga_scaler=0            ; same as the SCART combo
```

The same `[Menu]`-scoped override from the SCART combo above (`vga_scaler=1` + the custom `video_mode=640,26,60,74,288,0,4,20,12587` line) applies here too — component output goes through the same Analog I/O board VGA port, same framebuffer resolution story.

**Note:** a *passive* VGA-to-component adapter/cable (no official Analog I/O board) almost certainly won't work at all — see the "does NOT work" entry below. This confirmed combo specifically used the real Analog I/O board, which has the actual DAC hardware to generate true YPbPr levels.

**Status:** confirmed working, including MiSTerFin itself (not just a color-bars-level check) — 2026-07-24.

---

## Untested / reported problem combos

*(to fill in as we verify or get reports)*

- HDMI direct to a modern TV/monitor
- HDMI → Blackmagic-style capture card (see the aspect-ratio caveat below — MiSTer stretches a Script's framebuffer to fill the target `video_mode` canvas with no aspect correction, unlike FPGA cores)
- Component (YPbPr) via a passive VGA-to-component adapter, i.e. **without** the official Analog I/O board (**likely does NOT work** — these adapters just rewire pins, they don't do the RGB→YPbPr color-space conversion; confirmed this produces "no sync" on at least one display we tried)
- NTSC on either SCART or component (not yet tested — the `[Menu]` custom `video_mode` line above is untested for whether it needs different values for NTSC)
- Direct Video Adapter (HDMI-to-VGA DAC) for VGA/component output
