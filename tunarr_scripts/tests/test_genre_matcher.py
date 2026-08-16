#!/usr/bin/env python3
"""Tests for genre_matcher.py — pure logic, no network, no filesystem
beyond a disposable fixture file this test writes itself."""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from testkit import Checker
import genre_matcher as gm

SAMPLE_CONFIG = """\
# comment line, ignored
Action: Action
Sci-Fi: Science Fiction
Comedy-Horror: Comedy, Horror

blank line above, ignored
"""


def run():
    c = Checker()

    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "channel_genres.txt"
        path.write_text(SAMPLE_CONFIG)
        allowlist = gm.load_channel_genres(path)

        c.eq(
            [name for name, _ in allowlist],
            ["Action", "Sci-Fi", "Comedy-Horror"],
            "load_channel_genres preserves file order",
        )
        c.eq(
            dict(allowlist)["Comedy-Horror"],
            ["Comedy", "Horror"],
            "load_channel_genres splits multi-genre line",
        )

        missing = Path(d) / "does_not_exist.txt"
        try:
            gm.load_channel_genres(missing)
            c.check(False, "load_channel_genres raises on missing file")
        except SystemExit:
            c.check(True, "load_channel_genres raises on missing file")

    allowlist = [
        ("Action", ["Action"]),
        ("Sci-Fi", ["Science Fiction"]),
        ("Comedy", ["Comedy"]),
        ("Drama", ["Drama"]),
    ]

    c.eq(
        gm.genres_for_movie(["Action", "Thriller"], allowlist),
        ["Action"],
        "single matching genre",
    )
    c.eq(
        gm.genres_for_movie(["Comedy", "action"], allowlist),
        ["Action", "Comedy"],
        "case-insensitive match, allow-list order preserved not input order",
    )
    c.eq(
        gm.genres_for_movie(["Action", "Comedy", "Drama"], allowlist, cap=2),
        ["Action", "Comedy"],
        "capped at 2 matches even with 3 eligible genres",
    )
    c.eq(
        gm.genres_for_movie(["Documentary"], allowlist),
        [],
        "genre not in allow-list is skipped, no channel",
    )
    c.eq(
        gm.genres_for_movie(["Action", "Comedy"], allowlist, cap=1),
        ["Action"],
        "cap=1 keeps only the first allow-list match",
    )

    print(f"test_genre_matcher: {c.checks} checks, {c.failures} failures")
    return c.checks, c.failures


if __name__ == "__main__":
    _, failures = run()
    sys.exit(1 if failures else 0)
