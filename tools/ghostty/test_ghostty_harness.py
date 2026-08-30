#!/usr/bin/env python3
"""Unit tests for the Ghostty framebuffer presenter."""

from __future__ import annotations

import base64
import io
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


MODULE_PATH = Path(__file__).with_name("ghostty_harness.py")
SPEC = importlib.util.spec_from_file_location("ghostty_harness", MODULE_PATH)
assert SPEC and SPEC.loader
HARNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HARNESS)


class ConversionTests(unittest.TestCase):
    def test_bgrx_to_rgb_reorders_pixels(self):
        frame = bytes((3, 2, 1, 0, 30, 20, 10, 255))
        self.assertEqual(
            HARNESS.bgrx_to_rgb(frame, 2, 1),
            bytes((1, 2, 3, 10, 20, 30)),
        )

    def test_bgrx_to_rgb_rejects_wrong_size(self):
        with self.assertRaisesRegex(ValueError, "expected 8 frame bytes"):
            HARNESS.bgrx_to_rgb(b"short", 2, 1)


class ProtocolTests(unittest.TestCase):
    def test_empty_command_has_no_continuation_field(self):
        chunks = list(HARNESS.kitty_chunks("a=d,d=i,i=1"))
        self.assertEqual(chunks, [b"\x1b_Ga=d,d=i,i=1;\x1b\\"])

    def test_payload_is_split_and_reassembles(self):
        payload = bytes(range(256)) * 40
        chunks = list(HARNESS.kitty_chunks("a=T,f=24", payload))

        self.assertGreater(len(chunks), 1)
        self.assertIn(b"a=T,f=24,m=1;", chunks[0])
        self.assertIn(b"m=0;", chunks[-1])

        encoded = b"".join(chunk.split(b";", 1)[1][:-2] for chunk in chunks)
        self.assertEqual(base64.b64decode(encoded), payload)

    def test_second_frame_is_placed_before_the_old_image_is_deleted(self):
        tty = io.BytesIO()
        presenter = HARNESS.GhosttyPresenter(tty, 1, 1)
        original_geometry = HARNESS.terminal_geometry
        HARNESS.terminal_geometry = lambda _tty: (80, 24, 0, 0)
        try:
            presenter.show(bytes((3, 2, 1, 0)))
            first_output = tty.getvalue()
            tty.seek(0)
            tty.truncate()

            presenter.show(bytes((6, 5, 4, 0)))
            second_output = tty.getvalue()
        finally:
            HARNESS.terminal_geometry = original_geometry

        self.assertIn(b"a=T", first_output)
        self.assertNotIn(b"a=d", first_output)
        self.assertIn(b"a=t", second_output)
        self.assertIn(b"a=p", second_output)
        self.assertIn(b"a=d", second_output)
        self.assertLess(second_output.index(b"a=p"), second_output.index(b"a=d"))

    def test_placement_enforces_the_four_by_three_cell_rectangle(self):
        tty = io.BytesIO()
        presenter = HARNESS.GhosttyPresenter(tty, 640, 288)
        original_geometry = HARNESS.terminal_geometry
        HARNESS.terminal_geometry = lambda _tty: (80, 24, 0, 0)
        try:
            presenter.show(bytes(640 * 288 * 4))
        finally:
            HARNESS.terminal_geometry = original_geometry

        control = tty.getvalue().split(b";", 1)[0]
        self.assertIn(b",c=64,r=24,", control)


class GeometryTests(unittest.TestCase):
    def test_four_by_three_image_fits_typical_terminal_cells(self):
        self.assertEqual(HARNESS.display_cells(80, 24), (64, 24))

    def test_image_is_limited_by_terminal_height(self):
        self.assertEqual(HARNESS.display_cells(120, 10), (27, 10))

    def test_reported_pixel_geometry_is_used(self):
        self.assertEqual(
            HARNESS.display_cells(100, 40, 1000, 800),
            (100, 38),
        )


class FrameReadTests(unittest.TestCase):
    def test_complete_frame_is_returned(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "frame.raw"
            path.write_bytes(b"frame")
            self.assertEqual(HARNESS.read_complete_frame(path, 5), b"frame")

    def test_missing_and_short_frames_are_ignored(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "frame.raw"
            self.assertIsNone(HARNESS.read_complete_frame(path, 5))
            path.write_bytes(b"bad")
            self.assertIsNone(HARNESS.read_complete_frame(path, 5))


class ChildEnvironmentTests(unittest.TestCase):
    def test_desktop_cache_root_is_set_by_default(self):
        with patch.dict("os.environ", {}, clear=True):
            env = HARNESS.child_environment(640, 288, Path("/tmp/frame.raw"))
        self.assertEqual(env["MISTERFIN_CACHE_ROOT"], "/tmp/misterfin-cache")

    def test_explicit_cache_root_is_preserved(self):
        with patch.dict("os.environ", {"MISTERFIN_CACHE_ROOT": "/tmp/custom-cache"}, clear=True):
            env = HARNESS.child_environment(640, 240, Path("/tmp/frame.raw"))
        self.assertEqual(env["MISTERFIN_CACHE_ROOT"], "/tmp/custom-cache")


if __name__ == "__main__":
    unittest.main()
