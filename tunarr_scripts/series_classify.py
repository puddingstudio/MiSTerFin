#!/usr/bin/env python3
"""Pure logic, no network: classifies a series as "channel-worthy" (gets its
own dedicated channel) or "small" (routed to a genre mix channel instead),
and buckets a series' episodes into schedule slots. Used by
create_series_channels.py and create_mix_channels.py.

Operates on the normalized {duration_ms, season, episode, id} shape (see
tunarr_client.normalize_episode) rather than Tunarr's verbose nested API
response — keeps this module's tests simple fixture dicts instead of full
API payloads.

See WORKFLOW.md for why these thresholds exist and what they mean for the
channels you end up with.
"""

from typing import Dict, List

# A series' combined runtime across ALL its seasons, at or above this many
# hours, gets its own dedicated channel. Below it, the series is meant to be
# routed to a tv-<genre>-mix channel instead (create_mix_channels.py) rather
# than given a one-off channel for a handful of episodes.
SMALL_SERIES_RUNTIME_THRESHOLD_HOURS = 19.0

# A season longer than this many episodes gets split into multiple schedule
# slots instead of one — keeps any single slot's program pool a manageable
# size for Tunarr's random-schedule picker.
SEASON_BUCKET_THRESHOLD = 20


def series_total_runtime_hours(episodes: List[dict]) -> float:
    """episodes: normalized dicts with duration_ms (see normalize_episode)."""
    total_ms = sum(ep.get("duration_ms") or 0 for ep in episodes)
    return total_ms / 1000 / 3600


def is_channel_worthy(total_hours: float, threshold: float = SMALL_SERIES_RUNTIME_THRESHOLD_HOURS) -> bool:
    return total_hours >= threshold


def group_by_season(episodes: List[dict]) -> Dict[int, List[dict]]:
    """Groups episodes by season number, preserving episode-number order
    within each season. Episodes with no season number (specials) are
    grouped under 0."""
    seasons: Dict[int, List[dict]] = {}
    for ep in episodes:
        season_num = ep.get("season") or 0
        seasons.setdefault(season_num, []).append(ep)
    for eps in seasons.values():
        eps.sort(key=lambda e: e.get("episode") or 0)
    return seasons


def bucket_seasons(episodes: List[dict], threshold: int = SEASON_BUCKET_THRESHOLD) -> List[List[dict]]:
    """One schedule slot per season, except a season longer than `threshold`
    episodes is split into multiple same-sized-ish buckets. Returns a flat
    list of episode-list buckets, season order preserved, buckets within an
    oversized season in air order."""
    seasons = group_by_season(episodes)
    buckets: List[List[dict]] = []
    for season_num in sorted(seasons.keys()):
        eps = seasons[season_num]
        if len(eps) <= threshold:
            buckets.append(eps)
        else:
            for i in range(0, len(eps), threshold):
                buckets.append(eps[i : i + threshold])
    return buckets
