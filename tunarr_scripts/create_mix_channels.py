#!/usr/bin/env python3
"""Groups TV shows that don't clear series_classify's 19h channel-worthy
threshold into genre-based "tv-<Genre>-mix" channels — the fallback
destination create_series_channels.py backfill routes small series to
instead of giving each its own one-off channel.

Unlike movie genre channels (up to 2 genre matches per movie), a show only
ever lands on ONE mix channel (its first allow-list match) — see WORKFLOW.md.
Shows with no genre match at all (confirmed common in practice — many TV
show entries carry no genre tags even when their movies do) go to a
catch-all "tv-mix-other" channel rather than being silently dropped.

Usage:
    create_mix_channels.py --library "Series" --config channel_genres.txt [--dry-run]
"""

import argparse

import genre_matcher as gm
import series_classify as sc
import tunarr_client as tc

CHANNEL_NUMBER_BASE = 800
CATCHALL_NAME = "tv-mix-other"


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


def place_small_series(shows: dict, allowlist: list) -> dict:
    """{"tv-<Genre>-mix": [(show_id, info), ...]} — only shows below the
    channel-worthy threshold, top-1 genre match, unmatched shows collected
    under CATCHALL_NAME instead of being dropped."""
    placements: dict = {f"tv-{name}-mix": [] for name, _ in allowlist}
    placements[CATCHALL_NAME] = []

    for show_id, info in shows.items():
        hours = sc.series_total_runtime_hours(info["episodes"])
        if sc.is_channel_worthy(hours):
            continue
        matches = gm.genres_for_movie(info["genres"], allowlist, cap=1)
        channel = f"tv-{matches[0]}-mix" if matches else CATCHALL_NAME
        placements[channel].append((show_id, info))

    return {name: shows_list for name, shows_list in placements.items() if shows_list}


def run(library_name: str, config_path: str, transcode_config: str, source_name: str, dry_run: bool):
    allowlist = gm.load_channel_genres(config_path)
    lib = resolve_library(source_name, library_name)

    print(f"Fetching '{library_name}'...")
    programs = tc.list_library_programs(lib["id"])
    shows = tc.group_episodes_by_show(programs)

    placements = place_small_series(shows, allowlist)
    if not placements:
        print("No small (below-threshold) series found — nothing to do.")
        return

    transcode_config_id = None if dry_run else tc.resolve_transcode_config(transcode_config)

    for index, (channel_name, shows_list) in enumerate(sorted(placements.items())):
        episode_count = sum(len(info["episodes"]) for _, info in shows_list)
        titles = ", ".join(info["title"] for _, info in shows_list)
        print(f"  {channel_name}: {len(shows_list)} shows, {episode_count} episodes ({titles})")

        number = CHANNEL_NUMBER_BASE + index
        channel = tc.create_channel(channel_name, number, "TV Mix", transcode_config_id, dry_run=dry_run)
        if dry_run:
            continue

        lineup = [
            tc.lineup_entry(ep["id"], int(ep["duration_ms"]))
            for _, info in shows_list
            for ep in info["episodes"]
        ]
        tc.set_programming(channel["id"], lineup)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--library", required=True)
    parser.add_argument("--source", help="Media source name (omit to search all connected sources)")
    parser.add_argument("--config", default="channel_genres.txt")
    parser.add_argument("--transcode-config", default="Default")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    run(args.library, args.config, args.transcode_config, args.source, args.dry_run)


if __name__ == "__main__":
    main()
