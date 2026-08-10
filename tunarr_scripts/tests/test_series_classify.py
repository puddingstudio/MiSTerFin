#!/usr/bin/env python3
"""Tests for series_classify.py — pure logic, no network. Operates on the
normalized {duration_ms, season, episode} shape (see
tunarr_client.normalize_episode), not Tunarr's raw nested API response."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from testkit import Checker
import series_classify as sc


def _ep(season, episode, minutes):
    return {"season": season, "episode": episode, "duration_ms": minutes * 60 * 1000}


def run():
    c = Checker()

    # 19h boundary: exactly at, just under, just over.
    exactly_19h = [_ep(1, i, 60) for i in range(19)]  # 19 * 60min = 19h exactly
    c.eq(sc.series_total_runtime_hours(exactly_19h), 19.0, "19h exactly sums correctly")
    c.check(sc.is_channel_worthy(19.0), "19.0h is channel-worthy (>= threshold)")

    just_under = [_ep(1, i, 60) for i in range(18)] + [_ep(1, 19, 59)]  # 18h59m
    hours = sc.series_total_runtime_hours(just_under)
    c.check(not sc.is_channel_worthy(hours), f"18h59m ({hours:.4f}h) is NOT channel-worthy")

    just_over = exactly_19h + [_ep(1, 20, 1)]  # 19h + 1min
    hours = sc.series_total_runtime_hours(just_over)
    c.check(sc.is_channel_worthy(hours), f"19h1m ({hours:.4f}h) is channel-worthy")

    # Missing duration_ms doesn't crash, counts as 0.
    c.eq(sc.series_total_runtime_hours([{"season": 1, "episode": 1}]), 0.0,
         "missing duration_ms treated as 0, no crash")

    # group_by_season / bucket_seasons.
    episodes = (
        [_ep(1, i, 30) for i in range(1, 6)]      # season 1, 5 eps
        + [_ep(2, i, 30) for i in range(1, 26)]   # season 2, 25 eps (> 20 threshold)
        + [_ep(0, 1, 10)]                          # a special, season 0
    )
    grouped = sc.group_by_season(episodes)
    c.eq(sorted(grouped.keys()), [0, 1, 2], "group_by_season finds seasons 0,1,2")
    c.eq(len(grouped[1]), 5, "season 1 has 5 episodes")
    c.eq(len(grouped[2]), 25, "season 2 has 25 episodes")
    c.eq([e["episode"] for e in grouped[1]], [1, 2, 3, 4, 5], "season 1 episode order preserved")

    buckets = sc.bucket_seasons(episodes, threshold=20)
    # season 0 (1 ep) + season 1 (5 eps) + season 2 split into 20+5 = 4 buckets total
    c.eq(len(buckets), 4, "oversized season 2 (25 eps) splits into 2 buckets at threshold=20")
    bucket_sizes = sorted(len(b) for b in buckets)
    c.eq(bucket_sizes, [1, 5, 5, 20], "bucket sizes: special(1), season1(5), season2 split (20+5)")

    print(f"test_series_classify: {c.checks} checks, {c.failures} failures")
    return c.checks, c.failures


if __name__ == "__main__":
    _, failures = run()
    sys.exit(1 if failures else 0)
