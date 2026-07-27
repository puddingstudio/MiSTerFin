#!/bin/sh
# Runs MiSTerFin on a desktop Linux box instead of on the MiSTer, so UI work
# doesn't need a flash-and-look cycle for every change.
#
# There's no /dev/fb0 to draw into here, so the app is pointed at a plain
# malloc'd buffer of the same size (MISTERFIN_FB, see fb.c) and reads its
# input from the terminal instead of /dev/input/eventN (MISTERFIN_STDIN /
# MISTERFIN_KEYS, see main.c). Every fb_flip dumps the visible buffer to a
# raw file, which tools/raw_to_png.py turns into a viewable PNG using only
# the Python stdlib.
#
# Needs a ./jellyfin.conf pointing at a real server (gitignored) — the same
# file --preview-browse already expects. Video playback is NOT usable here:
# mplayer writes straight into a real framebuffer, which a malloc'd buffer
# can't stand in for.
#
# Usage:
#   tools/run-local.sh                              interactive (arrows, Enter, Esc, Tab, q to quit)
#   tools/run-local.sh -k "right,right,a"           scripted, screenshot the result
#   tools/run-local.sh -k "down:200,down:200" -o shot.png
#   tools/run-local.sh --ntsc -k "a"                240-line NTSC geometry instead of PAL's 288
#
# Key names for -k: up down left right a b select start l r wait
# Each may carry a ":<ms>" delay, e.g. "a:1500" to wait 1.5s after pressing A.

set -e

cd "$(dirname "$0")/.."

WIDTH=640
HEIGHT=288
KEYS=""
OUT="/tmp/misterfin_shot.png"
HOLD=""

while [ $# -gt 0 ]; do
    case "$1" in
        --ntsc)        HEIGHT=240 ;;
        --pal)         HEIGHT=288 ;;
        -k|--keys)     KEYS="$2"; shift ;;
        -o|--out)      OUT="$2"; shift ;;
        --hold)        HOLD=1 ;;
        -h|--help)     sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)             echo "unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

RAW="/tmp/misterfin_frame.raw"

make --no-print-directory

export MISTERFIN_FB="${WIDTH}x${HEIGHT}"
export MISTERFIN_FRAME_OUT="$RAW"

if [ -n "$KEYS" ]; then
    export MISTERFIN_KEYS="$KEYS"
    [ -n "$HOLD" ] && export MISTERFIN_KEYS_HOLD=1
else
    export MISTERFIN_STDIN=1
fi

rm -f "$RAW"
./misterfin || true

if [ ! -f "$RAW" ]; then
    echo "no frame was rendered (did startup fail? check jellyfin.conf)" >&2
    exit 1
fi

python3 tools/raw_to_png.py "$RAW" "$WIDTH" "$HEIGHT" "$OUT"
echo "$OUT"
