#!/usr/bin/env python3
"""
Drives MiSTerFin with a synthetic gamepad to test held-direction auto-repeat.

Auto-repeat is the one input behaviour the headless harness can't reach on
its own: it's driven by evdev press/release pairs from a real device, not by
the terminal (which does its own key repeat) or by scripted key playback
(which only ever emits discrete presses). So this creates a virtual pad via
/dev/uinput, holds a direction down for real, and reports how far the cursor
actually travelled.

Needs write access to /dev/uinput (group `input` on most distros, or run
under sudo). Stdlib only — the uinput ioctls are issued directly via ctypes.

Usage:
    python3 tools/mock-jellyfin.py 18096 &      # or point at any server
    python3 tools/test-autorepeat.py
"""

import ctypes
import fcntl
import os
import struct
import subprocess
import sys
import time

# ── uinput / evdev constants (linux/input-event-codes.h, linux/uinput.h) ────
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BTN_SOUTH, BTN_EAST = 0x130, 0x131
BTN_TL, BTN_TR = 0x136, 0x137
BTN_SELECT, BTN_START = 0x13a, 0x13b
ABS_HAT0X, ABS_HAT0Y = 0x10, 0x11
ABS_CNT = 64

UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502

BUTTONS = [BTN_SOUTH, BTN_EAST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START]


class VirtualPad:
    """A uinput gamepad with a hat-switch D-pad — the same shape the client's
    EV_ABS handling expects from a real pad."""

    def __init__(self, name=b"MiSTerFin Test Pad"):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)

        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_SYN)
        for btn in BUTTONS:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, btn)
        for axis in (ABS_HAT0X, ABS_HAT0Y):
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, axis)

        # struct uinput_user_dev: name[80], input_id{4 u16}, u32 ff_effects_max,
        # then absmax/absmin/absfuzz/absflat, each s32[ABS_CNT].
        absmax = [0] * ABS_CNT
        absmin = [0] * ABS_CNT
        absmax[ABS_HAT0X] = absmax[ABS_HAT0Y] = 1
        absmin[ABS_HAT0X] = absmin[ABS_HAT0Y] = -1
        payload = (name.ljust(80, b"\0") +
                   struct.pack("HHHH", 3, 0x1234, 0x5678, 1) +
                   struct.pack("I", 0) +
                   struct.pack("%di" % ABS_CNT, *absmax) +
                   struct.pack("%di" % ABS_CNT, *absmin) +
                   struct.pack("%di" % ABS_CNT, *([0] * ABS_CNT)) +
                   struct.pack("%di" % ABS_CNT, *([0] * ABS_CNT)))
        os.write(self.fd, payload)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        time.sleep(0.5)   # let udev create the /dev/input/eventN node

    def _emit(self, ev_type, code, value):
        # struct input_event: struct timeval{long,long}, u16 type, u16 code, s32 value
        os.write(self.fd, struct.pack("llHHi", 0, 0, ev_type, code, value))

    def _sync(self):
        self._emit(EV_SYN, SYN_REPORT, 0)

    def tap(self, code, hold=0.05):
        self._emit(EV_KEY, code, 1)
        self._sync()
        time.sleep(hold)
        self._emit(EV_KEY, code, 0)
        self._sync()
        time.sleep(0.1)

    def hold_hat(self, axis, value, seconds):
        """Press a D-pad direction, keep it down for `seconds`, then centre."""
        self.hat_down(axis, value)
        time.sleep(seconds)
        self.hat_up(axis)
        time.sleep(0.2)

    def hat_down(self, axis, value):
        self._emit(EV_ABS, axis, value)
        self._sync()

    def hat_up(self, axis):
        self._emit(EV_ABS, axis, 0)
        self._sync()

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)


def snapshot(raw_path, out_png, width, height):
    tmp = raw_path + ".snap"
    with open(raw_path, "rb") as src, open(tmp, "wb") as dst:
        dst.write(src.read())
    subprocess.run([sys.executable, "tools/raw_to_png.py", tmp,
                    str(width), str(height), out_png], check=True)
    os.unlink(tmp)
    return out_png


def main():
    width, height = 640, 288
    raw = "/tmp/misterfin_frame.raw"
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "/tmp"

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    subprocess.run(["make", "--no-print-directory"], check=True)

    env = dict(os.environ,
               MISTERFIN_FB=f"{width}x{height}",
               MISTERFIN_FRAME_OUT=raw)
    # No MISTERFIN_STDIN / MISTERFIN_KEYS: this test drives the real evdev
    # path exclusively, which is the entire point.
    if os.path.exists(raw):
        os.unlink(raw)

    app = subprocess.Popen(["./misterfin"], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pad = VirtualPad()
    results = {}
    try:
        time.sleep(4.0)                       # startup fetch + first draw

        # A completed press anywhere teaches the client this device reports
        # releases, which is what unlocks auto-repeat (see input_seen_release).
        # It also drills into the first library, giving us a long list.
        pad.tap(BTN_EAST)
        time.sleep(2.5)
        snapshot(raw, os.path.join(out_dir, "repeat_before.png"), width, height)

        # A single tap should move exactly one row — repeat must not kick in
        # for a quick press.
        pad.hold_hat(ABS_HAT0Y, 1, 0.05)
        time.sleep(0.5)
        snapshot(raw, os.path.join(out_dir, "repeat_tap.png"), width, height)

        # Now hold it down for real.
        pad.hold_hat(ABS_HAT0Y, 1, 2.0)
        time.sleep(0.5)
        snapshot(raw, os.path.join(out_dir, "repeat_hold.png"), width, height)

        # And confirm it actually STOPS on release rather than running away.
        time.sleep(1.5)
        snapshot(raw, os.path.join(out_dir, "repeat_after_release.png"), width, height)

        # Regression case: hold a direction ACROSS screen changes and back.
        # The app drains pending input on every transition, so a release
        # arriving afterwards has no matching press left to cancel it — which
        # is exactly how repeat got stuck on permanently. The round trip ends
        # back on the list, where DOWN genuinely scrolls, so a stuck repeat
        # would be plainly visible as a moved cursor.
        snapshot(raw, os.path.join(out_dir, "screen_00_before.png"), width, height)
        pad.hat_down(ABS_HAT0Y, 1)            # hold DOWN, and keep holding
        time.sleep(0.15)                      # a press, but not long enough to repeat
        pad.tap(BTN_EAST)                     # into the info screen (drains input)
        time.sleep(2.0)
        pad.tap(BTN_SOUTH)                    # back to the list (drains again)
        time.sleep(2.0)                       # STILL holding: must not scroll
        snapshot(raw, os.path.join(out_dir, "screen_01_held.png"), width, height)
        pad.hat_up(ABS_HAT0Y)                 # release at last
        time.sleep(1.5)
        snapshot(raw, os.path.join(out_dir, "screen_02_released.png"), width, height)
        results["ok"] = True
    finally:
        pad.close()
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()

    print("wrote repeat_before/tap/hold/after_release .png to", out_dir)
    print("expected: tap moves 1 row; 2s hold moves many; after-release count "
          "is unchanged from hold")
    return 0 if results.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
