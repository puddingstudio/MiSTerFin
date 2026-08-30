#!/usr/bin/env python3
"""Run MiSTerFin's desktop harness as an interactive image in Ghostty."""

from __future__ import annotations

import argparse
import base64
import fcntl
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
from typing import BinaryIO, Iterable


ESC = b"\x1b"
ST = ESC + b"\\"
IMAGE_IDS = (0x4D465001, 0x4D465002)
PLACEMENT_ID = 1
DEFAULT_FPS = 20.0
DISPLAY_ASPECT = 4.0 / 3.0
REPO_ROOT = Path(__file__).resolve().parents[2]


def bgrx_to_rgb(frame: bytes, width: int, height: int) -> bytes:
    """Convert MiSTerFin's little-endian BGRX8888 buffer to packed RGB."""
    expected = width * height * 4
    if len(frame) != expected:
        raise ValueError(f"expected {expected} frame bytes, got {len(frame)}")

    rgb = bytearray(width * height * 3)
    rgb[0::3] = frame[2::4]
    rgb[1::3] = frame[1::4]
    rgb[2::3] = frame[0::4]
    return bytes(rgb)


def kitty_chunks(control: str, payload: bytes = b"") -> Iterable[bytes]:
    """Encode one Kitty graphics command, splitting large payloads safely."""
    encoded = base64.b64encode(payload)
    if not encoded:
        yield ESC + b"_G" + control.encode("ascii") + b";" + ST
        return

    chunks = [encoded[i:i + 4096] for i in range(0, len(encoded), 4096)]
    for index, chunk in enumerate(chunks):
        more = int(index + 1 < len(chunks))
        prefix = f"{control},m={more}" if index == 0 else f"m={more}"
        yield ESC + b"_G" + prefix.encode("ascii") + b";" + chunk + ST


def display_cells(
    columns: int,
    rows: int,
    pixel_width: int = 0,
    pixel_height: int = 0,
) -> tuple[int, int]:
    """Fit MiSTerFin's non-square pixels into a physical 4:3 rectangle."""
    columns = max(columns, 1)
    rows = max(rows, 1)

    if pixel_width > 0 and pixel_height > 0:
        cell_width = pixel_width / columns
        cell_height = pixel_height / rows
    else:
        # A typical terminal cell is about twice as tall as it is wide.
        cell_width = 1.0
        cell_height = 2.0

    width_at_full_columns = columns * cell_width
    image_height_pixels = width_at_full_columns / DISPLAY_ASPECT
    fitted_rows = max(1, round(image_height_pixels / cell_height))

    if fitted_rows <= rows:
        return columns, fitted_rows

    height_at_full_rows = rows * cell_height
    image_width_pixels = height_at_full_rows * DISPLAY_ASPECT
    fitted_columns = max(1, round(image_width_pixels / cell_width))
    return min(fitted_columns, columns), rows


def terminal_geometry(tty: BinaryIO) -> tuple[int, int, int, int]:
    packed = fcntl.ioctl(tty.fileno(), termios.TIOCGWINSZ, b"\0" * 8)
    rows, columns, pixel_width, pixel_height = struct.unpack("HHHH", packed)
    fallback = shutil.get_terminal_size((80, 24))
    return columns or fallback.columns, rows or fallback.lines, pixel_width, pixel_height


def read_complete_frame(path: Path, expected_size: int) -> bytes | None:
    """Read a frame only if the producer did not change it during the read."""
    try:
        with path.open("rb") as source:
            before = os.fstat(source.fileno())
            if before.st_size != expected_size:
                return None
            frame = source.read()
            after = os.fstat(source.fileno())
    except FileNotFoundError:
        return None

    if len(frame) != expected_size:
        return None
    if before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns:
        return None
    return frame


class GhosttyPresenter:
    def __init__(self, tty: BinaryIO, width: int, height: int):
        self.tty = tty
        self.width = width
        self.height = height
        self.image_id: int | None = None

    def write(self, data: bytes) -> None:
        self.tty.write(data)

    def enter(self) -> None:
        self.write(b"\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H")

    def leave(self) -> None:
        self.delete_image()
        self.write(b"\x1b[?25h\x1b[?1049l")

    def delete_image(self, image_id: int | None = None) -> None:
        image_id = self.image_id if image_id is None else image_id
        if image_id is None:
            return
        for chunk in kitty_chunks(f"a=d,d=I,i={image_id},q=2"):
            self.write(chunk)
        if image_id == self.image_id:
            self.image_id = None

    def create_image(self, image_id: int, rgb: bytes, columns: int, rows: int) -> None:
        self.write(b"\x1b[H")
        control = (
            f"a=T,f=24,t=d,s={self.width},v={self.height},i={image_id},"
            f"p={PLACEMENT_ID},c={columns},r={rows},C=1,q=2"
        )
        for chunk in kitty_chunks(control, rgb):
            self.write(chunk)
        self.image_id = image_id

    def replace_image(self, image_id: int, rgb: bytes, columns: int, rows: int) -> None:
        # Upload without displaying, place the complete new image over the old
        # one, then release the old image. There is never a blank placement.
        control = f"a=t,f=24,t=d,s={self.width},v={self.height},i={image_id},q=2"
        for chunk in kitty_chunks(control, rgb):
            self.write(chunk)

        self.write(b"\x1b[H")
        placement = (
            f"a=p,i={image_id},p={PLACEMENT_ID},c={columns},r={rows},C=1,q=2"
        )
        for chunk in kitty_chunks(placement):
            self.write(chunk)

        old_image_id = self.image_id
        self.image_id = image_id
        self.delete_image(old_image_id)

    def show(self, frame: bytes) -> None:
        columns, rows, pixel_width, pixel_height = terminal_geometry(self.tty)
        image_columns, image_rows = display_cells(
            columns,
            rows,
            pixel_width,
            pixel_height,
        )
        rgb = bgrx_to_rgb(frame, self.width, self.height)

        if self.image_id is None:
            self.create_image(IMAGE_IDS[0], rgb, image_columns, image_rows)
        else:
            next_image_id = IMAGE_IDS[1] if self.image_id == IMAGE_IDS[0] else IMAGE_IDS[0]
            self.replace_image(next_image_id, rgb, image_columns, image_rows)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Navigate MiSTerFin inside Ghostty using the desktop harness."
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--ntsc", action="store_true", help="use the 640x240 layout")
    mode.add_argument("--pal", action="store_true", help="use the 640x288 layout (default)")
    parser.add_argument(
        "--fps",
        type=float,
        default=DEFAULT_FPS,
        help=f"maximum terminal presentation rate (default: {DEFAULT_FPS:g})",
    )
    parser.add_argument(
        "--binary",
        type=Path,
        default=REPO_ROOT / "misterfin",
        help="MiSTerFin host binary to run",
    )
    parser.add_argument("--no-build", action="store_true", help="do not run make first")
    parser.add_argument(
        "--log",
        type=Path,
        default=Path("/tmp/misterfin-ghostty.log"),
        help="child stdout and stderr log",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="run even when TERM does not identify Ghostty",
    )
    args = parser.parse_args(argv)
    if args.fps <= 0:
        parser.error("--fps must be greater than zero")
    return args


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def child_environment(width: int, height: int, frame_path: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["MISTERFIN_FB"] = f"{width}x{height}"
    env["MISTERFIN_FRAME_OUT"] = str(frame_path)
    env["MISTERFIN_STDIN"] = "1"
    env.setdefault("MISTERFIN_CACHE_ROOT", "/tmp/misterfin-cache")
    env.pop("MISTERFIN_KEYS", None)
    env.pop("MISTERFIN_KEYS_HOLD", None)
    return env


def run(args: argparse.Namespace) -> int:
    if "ghostty" not in os.environ.get("TERM", "").lower() and not args.force:
        print("This viewer requires Ghostty (expected TERM=xterm-ghostty).", file=sys.stderr)
        print("Use --force only with another Kitty-graphics-compatible terminal.", file=sys.stderr)
        return 2

    if not sys.stdin.isatty():
        print("Interactive MiSTerFin input requires a terminal on stdin.", file=sys.stderr)
        return 2

    if not args.no_build:
        completed = subprocess.run(["make", "--no-print-directory"], cwd=REPO_ROOT)
        if completed.returncode:
            return completed.returncode

    binary = args.binary.resolve()
    if not binary.is_file():
        print(f"MiSTerFin host binary not found: {binary}", file=sys.stderr)
        return 2

    width = 640
    height = 240 if args.ntsc else 288
    frame_size = width * height * 4
    frame_interval = 1.0 / args.fps
    args.log.parent.mkdir(parents=True, exist_ok=True)

    process: subprocess.Popen[bytes] | None = None
    stopping = False

    def request_stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    previous_handlers = {
        signum: signal.signal(signum, request_stop)
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    }

    try:
        with tempfile.TemporaryDirectory(prefix="misterfin-ghostty-") as temp_dir:
            frame_path = Path(temp_dir) / "frame.raw"
            env = child_environment(width, height, frame_path)

            with args.log.open("wb") as log, open("/dev/tty", "wb", buffering=0) as tty:
                presenter = GhosttyPresenter(tty, width, height)
                presenter.enter()
                try:
                    process = subprocess.Popen(
                        [str(binary)],
                        cwd=REPO_ROOT,
                        env=env,
                        stdin=None,
                        stdout=log,
                        stderr=log,
                    )
                    previous_frame: bytes | None = None
                    next_frame_at = 0.0

                    while process.poll() is None and not stopping:
                        now = time.monotonic()
                        if now < next_frame_at:
                            time.sleep(min(next_frame_at - now, 0.01))
                            continue

                        frame = read_complete_frame(frame_path, frame_size)
                        if frame is not None and frame != previous_frame:
                            presenter.show(frame)
                            previous_frame = frame
                        next_frame_at = time.monotonic() + frame_interval
                finally:
                    if process is not None:
                        stop_process(process)
                    presenter.leave()
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)

    assert process is not None
    if process.returncode not in (0, -signal.SIGTERM):
        print(f"MiSTerFin exited with status {process.returncode}. See {args.log}.", file=sys.stderr)
        return process.returncode or 1
    return 0


def main() -> int:
    return run(parse_args(sys.argv[1:]))


if __name__ == "__main__":
    raise SystemExit(main())
