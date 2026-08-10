#!/usr/bin/env python3
"""Tunarr-only CLI — needs nothing but TUNARR_HOST in .env_tunarr. No
Jellyfin config, no API key, works the moment Tunarr itself is reachable.

Commands:
    tunarr_status.py health
    tunarr_status.py list
    tunarr_status.py sources
    tunarr_status.py programs <channel name>
    tunarr_status.py rescan <source name> [<library name>] [--force]
"""

import argparse
import sys

import tunarr_client as tc


def cmd_health(args):
    if tc.tunarr_alive():
        print(f"Tunarr is reachable at {tc.TUNARR_HOST}")
    else:
        raise SystemExit(f"Tunarr is NOT reachable at {tc.TUNARR_HOST}")


def cmd_list(args):
    channels = tc.list_channels()
    if not channels:
        print("No channels yet.")
        return
    for ch in sorted(channels, key=lambda c: c.get("number", 0)):
        print(
            f"  {ch.get('number', '?'):>4}  {ch.get('name', '?'):<30}"
            f"  ({ch.get('groupTitle', '')})  {ch.get('programCount', 0)} programs"
        )


def cmd_sources(args):
    sources = tc.list_media_sources()
    if not sources:
        print("No media sources connected — add one in Tunarr's web UI (Settings > Sources).")
        return
    for src in sources:
        print(f"{src.get('name')} ({src.get('type')})  {src.get('uri', '')}")
        for lib in src.get("libraries", []):
            status = "enabled" if lib.get("enabled") else "disabled"
            scanned = lib.get("lastScannedAt")
            scanned_str = f"last scanned {scanned}" if scanned else "never scanned"
            print(f"    {lib.get('name'):<20} [{status}]  id={lib.get('id')}  {scanned_str}")


def cmd_programs(args):
    channel = tc.get_channel(args.channel)
    programming = tc.get_programming(channel["id"])
    lineup = programming.get("lineup", programming) if isinstance(programming, dict) else programming
    count = len(lineup) if isinstance(lineup, list) else "?"
    print(f"{args.channel}: {count} programs")


def cmd_rescan(args):
    lib = tc.find_library(args.source, args.library) if args.library else None
    if lib:
        source_id, library_id = lib["source_id"], lib["id"]
    else:
        sources = tc.list_media_sources()
        matched = next((s for s in sources if s.get("name", "").lower() == args.source.lower()), None)
        if not matched:
            names = ", ".join(s.get("name", "?") for s in sources)
            raise SystemExit(f"no media source named '{args.source}' — available: {names}")
        source_id, library_id = matched["id"], "all"

    tc.scan_library(source_id, library_id, force=args.force)
    print(f"Rescan triggered for {args.source}"
          f"{'/' + args.library if args.library else ' (all libraries)'}"
          f"{' (forced)' if args.force else ''} — poll status with `sources`.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("health", help="Check that Tunarr is reachable").set_defaults(func=cmd_health)
    sub.add_parser("list", help="List channels").set_defaults(func=cmd_list)
    sub.add_parser("sources", help="List connected media sources and their libraries").set_defaults(func=cmd_sources)

    p = sub.add_parser("programs", help="Show program count for a channel")
    p.add_argument("channel")
    p.set_defaults(func=cmd_programs)

    p = sub.add_parser("rescan", help="Trigger an on-demand library rescan")
    p.add_argument("source", help="Media source name, e.g. as shown by `sources`")
    p.add_argument("library", nargs="?", help="Library name (omit to rescan all enabled libraries)")
    p.add_argument("--force", action="store_true", help="Rescan even if recently scanned")
    p.set_defaults(func=cmd_rescan)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
