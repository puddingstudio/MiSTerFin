#!/usr/bin/env python3
"""Creates a dedicated channel for one TV show, or backfills channels for
every show in a library that clears the 19h total-runtime threshold (see
series_classify.py). One schedule slot per season, chronological order,
linked across seasons so playback resumes rather than repeating.

Below-threshold shows are skipped here — see create_mix_channels.py for
where they go instead. See WORKFLOW.md for the full explanation, and
README.md for the Merge Versions warning before running this against a
library with duplicate/merged series entries.

Usage:
    create_series_channels.py backfill --library "Series" --number-start 600 [--dry-run]
    create_series_channels.py create --library "Series" --series "The Simpsons" --number 601 [--dry-run]
"""

import argparse
import sys
import uuid

import series_classify as sc
import tunarr_client as tc

TRANSCODE_CONFIG_DEFAULT = "Default"


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


def build_show_schedule(show_id: str, episodes: list) -> dict:
    """One slot per season, all sharing an iterationGroup so the show plays
    through season-to-season rather than each season resuming independently.
    Not using series_classify.bucket_seasons() here — Tunarr's seasonFilter
    operates on whole season numbers, there's no API-level way to split one
    season's episodes across two slots (confirmed against the real schema),
    so bucket_seasons' >20-episode splitting doesn't apply to slot-building;
    it remains available for reporting/stats."""
    seasons = sc.group_by_season(episodes)
    group_id = str(uuid.uuid4())
    slots = [
        tc.build_show_slot(str(uuid.uuid4()), show_id, season_num, iteration_group=group_id)
        for season_num in sorted(seasons.keys())
    ]
    return tc.build_schedule_payload(slots)


def create_one(show_id: str, title: str, episodes: list, number: int, transcode_config_id: str, dry_run: bool):
    hours = sc.series_total_runtime_hours(episodes)
    if not sc.is_channel_worthy(hours):
        print(
            f"  WARNING: '{title}' is only {hours:.1f}h total runtime, below the "
            f"{sc.SMALL_SERIES_RUNTIME_THRESHOLD_HOURS}h threshold — creating anyway "
            "since it was explicitly requested (backfill would have routed this to "
            "a mix channel instead, see create_mix_channels.py)."
        )
    print(f"  {title} (#{number}): {hours:.1f}h across {len(sc.group_by_season(episodes))} seasons")

    channel = tc.create_channel(title, number, "TV Shows", transcode_config_id, dry_run=dry_run)
    if dry_run:
        return
    schedule = build_show_schedule(show_id, episodes)
    tc.set_schedule(channel["id"], schedule)


def cmd_create(args):
    lib = resolve_library(args.source, args.library)
    programs = tc.list_library_programs(lib["id"])
    shows = tc.group_episodes_by_show(programs)

    matches = [(sid, info) for sid, info in shows.items() if info["title"].lower() == args.series.lower()]
    if not matches:
        raise SystemExit(f"no series found matching '{args.series}' in library '{args.library}'")
    if len(matches) > 1:
        raise SystemExit(
            f"'{args.series}' matched {len(matches)} distinct shows in Tunarr's index — "
            "this almost always means Jellyfin has duplicate/merged entries for this "
            "title. See README.md's Merge Versions warning before proceeding; this "
            "tool does not guess which entry you meant."
        )
    show_id, info = matches[0]
    transcode_config_id = None if args.dry_run else tc.resolve_transcode_config(args.transcode_config)
    create_one(show_id, info["title"], info["episodes"], args.number, transcode_config_id, args.dry_run)


def find_duplicate_titles(shows: dict) -> dict:
    """title (lowercase) -> [show_id, ...] for any title Tunarr's index has
    more than one distinct show for. This is a REAL, confirmed-live problem
    (validated against an actual library: "Avatar: The Last Airbender" and
    "Futurama" both appeared as two separate shows here) — usually stale/
    duplicate Jellyfin entries, sometimes a genuinely distinct second dub or
    cut. This is a kickstart tool meant to get a WHOLE library onto channels
    in one pass, so backfill doesn't skip these or ask the user to sort
    Jellyfin's data out first — see cmd_backfill's -2/-3 suffixing. If you
    know two entries really are duplicates you want merged into one channel,
    resolve that in Jellyfin and rerun; `create --series` remains the
    explicit, one-at-a-time tool for picking a specific entry deliberately."""
    by_title: dict = {}
    for show_id, info in shows.items():
        by_title.setdefault(info["title"].lower(), []).append(show_id)
    return {title: ids for title, ids in by_title.items() if len(ids) > 1}


def cmd_backfill(args):
    lib = resolve_library(args.source, args.library)
    programs = tc.list_library_programs(lib["id"])
    shows = tc.group_episodes_by_show(programs)

    duplicates = find_duplicate_titles(shows)
    if duplicates:
        print(f"{len(duplicates)} title(s) matched more than one distinct show in Tunarr's "
              "index (usually duplicate/stale Jellyfin entries, occasionally a real second "
              "dub/cut — see README.md's Merge Versions warning) — creating a numbered "
              "channel for each rather than dropping any of them:")
        for title, ids in duplicates.items():
            print(f"    {shows[ids[0]]['title']}  ({len(ids)} entries -> suffixed -2, -3, ...)")
        print()

    existing_names = {c.get("name", "").lower() for c in tc.list_channels()}
    transcode_config_id = None if args.dry_run else tc.resolve_transcode_config(args.transcode_config)

    # Stable per-title ordering (title, then show_id) so a repeat run
    # assigns the same -2/-3 suffix to the same entry rather than reshuffling.
    seen_title_count: dict = {}
    number = args.number_start
    skipped = []
    for show_id, info in sorted(shows.items(), key=lambda kv: (kv[1]["title"].lower(), kv[0])):
        hours = sc.series_total_runtime_hours(info["episodes"])
        if not sc.is_channel_worthy(hours):
            skipped.append((info["title"], hours))
            continue

        title_key = info["title"].lower()
        seen_title_count[title_key] = seen_title_count.get(title_key, 0) + 1
        suffix_n = seen_title_count[title_key]
        name = info["title"] if suffix_n == 1 else f"{info['title']}-{suffix_n}"

        if name.lower() in existing_names:
            print(f"  {name}: channel already exists, skipping")
            continue
        create_one(show_id, name, info["episodes"], number, transcode_config_id, args.dry_run)
        number += 1

    if skipped:
        print(f"\n{len(skipped)} shows below {sc.SMALL_SERIES_RUNTIME_THRESHOLD_HOURS}h, "
              "routed to mix channels instead (see create_mix_channels.py):")
        for title, hours in skipped:
            print(f"    {title} ({hours:.1f}h)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--library", required=True)
    common.add_argument("--source", help="Media source name (omit to search all connected sources)")
    common.add_argument("--transcode-config", default=TRANSCODE_CONFIG_DEFAULT)
    common.add_argument("--dry-run", action="store_true")

    p = sub.add_parser("create", parents=[common], help="Create one named series' channel")
    p.add_argument("--series", required=True)
    p.add_argument("--number", type=int, required=True)
    p.set_defaults(func=cmd_create)

    p = sub.add_parser("backfill", parents=[common], help="Create channels for every channel-worthy series")
    p.add_argument("--number-start", type=int, default=600)
    p.set_defaults(func=cmd_backfill)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
