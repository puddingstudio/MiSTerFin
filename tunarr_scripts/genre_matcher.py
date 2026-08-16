#!/usr/bin/env python3
"""Pure logic, no network: matches a movie's Jellyfin Genres against a
priority-ordered channel_genres.txt allow-list. Used by
create_movie_channels.py and create_mix_channels.py."""

from pathlib import Path
from typing import List, Tuple


def load_channel_genres(path) -> List[Tuple[str, List[str]]]:
    """Parses channel_genres.txt into an ordered list of
    (channel_name, [genre, ...]) — file order is preserved, since it's both
    the channel-numbering block order and the tie-breaker for movies
    matching more than one channel's genres."""
    path = Path(path)
    if not path.exists():
        raise SystemExit(
            f"channel genre config not found: {path}\n"
            "Copy channel_genres.txt.example to channel_genres.txt and edit it."
        )

    result = []
    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        channel, genres_part = line.split(":", 1)
        channel = channel.strip()
        genres = [g.strip() for g in genres_part.split(",") if g.strip()]
        if channel and genres:
            result.append((channel, genres))
    return result


def genres_for_movie(item_genres: List[str], allowlist: List[Tuple[str, List[str]]], cap: int = 2) -> List[str]:
    """Which channels (by name) a movie belongs on, given its Genres array
    and the allow-list from load_channel_genres(). Preserves allow-list
    order and caps at `cap` matches — a movie tagged with 5 allow-listed
    genres only goes on the first `cap` channels in file order, not all 5."""
    item_genre_set = {g.lower() for g in item_genres}
    matches = []
    for channel, channel_genres in allowlist:
        if any(g.lower() in item_genre_set for g in channel_genres):
            matches.append(channel)
        if len(matches) >= cap:
            break
    return matches
