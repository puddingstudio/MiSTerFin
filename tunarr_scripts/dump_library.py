#!/usr/bin/env python3
"""Preview/reference table: for every movie or show in a library, shows its
genre(s), runtime, and exactly where create_movie_channels.py /
create_series_channels.py / create_mix_channels.py would place it — without
creating or changing anything in Tunarr. Read-only, safe to run any time.

Doubles as an ongoing reference ("where's <show> playing?") once channels
exist, and as a duplicate-entry check before running `create_series_channels.py
backfill` for real (see README.md's Merge Versions warning).

Reuses the exact same placement logic the create_*_channels.py scripts use
(imported from them directly, not reimplemented), so this preview can't
drift out of sync with what a real run would actually do.

Usage:
    dump_library.py --library "Movies" --config channel_genres.txt
    dump_library.py --library "Series" --config channel_genres.txt
"""

import argparse

import create_mix_channels as mix
import create_movie_channels as movies
import create_series_channels as series
import genre_matcher as gm
import series_classify as sc
import tunarr_client as tc


def resolve_library(source_name: str, library_name: str) -> dict:
    if source_name:
        return tc.find_library(source_name, library_name)
    for src in tc.list_media_sources():
        match = next(
            (l for l in src.get("libraries", []) if l.get("name", "").lower() == library_name.lower()),
            None,
        )
        if match:
            return {"source_id": src["id"], **match}
    raise SystemExit(f"no library named '{library_name}' found on any connected source")


def fmt_hours(hours: float) -> str:
    return f"{hours:.1f}h"


def print_table(rows: list, headers: list) -> None:
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(str(cell)))
    line = "  ".join(h.ljust(widths[i]) for i, h in enumerate(headers))
    print(line)
    print("  ".join("-" * w for w in widths))
    for row in rows:
        print("  ".join(str(cell).ljust(widths[i]) for i, cell in enumerate(row)))


def dump_movies(lib: dict, allowlist: list) -> None:
    print(f"Fetching movie genre detail (one request per movie, may take a while)...")
    all_movies = movies.fetch_movies_with_genres(lib["id"], verbose=True)
    placements = movies.place_movies_on_channels(all_movies, allowlist)

    # movie id -> [channel names it landed on], for the per-row lookup below.
    channels_by_movie_id: dict = {}
    for channel_name, movie_list in placements.items():
        for m in movie_list:
            channels_by_movie_id.setdefault(m["id"], []).append(channel_name)

    rows = []
    for m in sorted(all_movies, key=lambda m: m["title"].lower()):
        genres_str = ", ".join(m["genres"]) or "(none)"
        dest = ", ".join(channels_by_movie_id.get(m["id"], [])) or "(no genre match)"
        rows.append((m["title"], genres_str, fmt_hours((m["duration_ms"] or 0) / 1000 / 3600), dest))

    print(f"\n{len(all_movies)} movies:\n")
    print_table(rows, ["Title", "Genres", "Runtime", "-> Channel(s)"])


def dump_series(lib: dict, allowlist: list) -> None:
    programs = tc.list_library_programs(lib["id"])
    shows = tc.group_episodes_by_show(programs)
    duplicates = series.find_duplicate_titles(shows)
    duplicate_show_ids = {sid for ids in duplicates.values() for sid in ids}

    mix_placements = mix.place_small_series(shows, allowlist)
    mix_dest_by_show_id: dict = {}
    for mix_channel, shows_list in mix_placements.items():
        for show_id, _ in shows_list:
            mix_dest_by_show_id[show_id] = mix_channel

    rows = []
    for show_id, info in sorted(shows.items(), key=lambda kv: kv[1]["title"].lower()):
        hours = sc.series_total_runtime_hours(info["episodes"])
        genres_str = ", ".join(info["genres"]) or "(none)"
        dup_flag = " [DUPLICATE TITLE]" if show_id in duplicate_show_ids else ""

        if sc.is_channel_worthy(hours):
            status = "channel-worthy"
            dest = f"{info['title']}{dup_flag}"
        else:
            status = "small -> mix"
            dest = mix_dest_by_show_id.get(show_id, "(unmatched)")

        rows.append((info["title"] + dup_flag, genres_str, fmt_hours(hours), status, dest))

    print(f"\n{len(shows)} shows ({len(duplicates)} duplicate title(s) — see WORKFLOW.md's Merge Versions warning):\n")
    print_table(rows, ["Title", "Genres", "Runtime", "Status", "-> Destination"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--library", required=True)
    parser.add_argument("--source", help="Media source name (omit to search all connected sources)")
    parser.add_argument("--config", default="channel_genres.txt")
    args = parser.parse_args()

    allowlist = gm.load_channel_genres(args.config)
    lib = resolve_library(args.source, args.library)
    media_type = lib.get("mediaType", "")

    if media_type == "shows":
        dump_series(lib, allowlist)
    elif media_type in ("movies", "other_videos"):
        dump_movies(lib, allowlist)
    else:
        raise SystemExit(
            f"library '{args.library}' has mediaType '{media_type}' — "
            "dump_library.py only knows how to preview movies/shows libraries."
        )


if __name__ == "__main__":
    main()
