#!/usr/bin/env python3
"""Creates/updates genre-based movie channels from a Tunarr-connected
Jellyfin (or Plex/Emby) library — one channel per line in channel_genres.txt,
a movie can land on up to 2 channels (allow-list order breaks ties), and a
channel that grows past a movie-count or total-runtime ceiling spills into
a numbered overflow channel (<Genre>-2, <Genre>-3, ...).

See WORKFLOW.md for the full explanation. See README.md for prerequisites
(Jellyfin connected + library scanned inside Tunarr's own UI first).

Usage:
    create_movie_channels.py --library "Movies" --config channel_genres.txt [--dry-run]
"""

import argparse
import sys

import genre_matcher as gm
import tunarr_client as tc

CHANNEL_NUMBER_BASE = 200
CHANNEL_BLOCK_SIZE = 10  # 10 numbers reserved per genre, see WORKFLOW.md
OVERFLOW_MAX_MOVIES = 50
OVERFLOW_MAX_HOURS = 72  # 3 days


def fetch_movies_with_genres(library_id: str, verbose: bool = False) -> list:
    """list_library_programs() doesn't include genres (confirmed against a
    real instance) — one get_program_detail() call per movie is the price
    of getting them. Returns [{"id","title","duration_ms","genres"}, ...]."""
    programs = tc.list_library_programs(library_id)
    movies = [p for p in programs if (p.get("program") or {}).get("type") == "movie"]

    result = []
    for i, wrapped in enumerate(movies):
        program = wrapped["program"]
        detail = tc.get_program_detail(wrapped["id"])
        result.append({
            "id": wrapped["id"],
            "title": program.get("title", "?"),
            "duration_ms": wrapped.get("duration") or program.get("duration") or 0,
            "genres": tc.genre_names(detail),
        })
        if verbose and (i + 1) % 25 == 0:
            print(f"  fetched {i + 1}/{len(movies)} movie details...", file=sys.stderr)
    return result


def place_movies_on_channels(movies: list, allowlist: list) -> dict:
    """{channel_base_name: [movie, ...]} — a movie appears under every
    channel it matches (up to genre_matcher's cap), NOT deduplicated across
    channels, since a movie legitimately belongs on more than one genre
    channel."""
    placements: dict = {name: [] for name, _ in allowlist}
    for movie in movies:
        for channel in gm.genres_for_movie(movie["genres"], allowlist):
            placements[channel].append(movie)
    return placements


def split_overflow(movies: list) -> list:
    """Splits one channel's movie list into overflow-sized chunks, matching
    the original toolkit's rule: whichever ceiling (50 movies OR 72h total
    runtime) is hit first ends that chunk."""
    chunks = []
    current: list = []
    current_hours = 0.0
    for movie in movies:
        movie_hours = (movie["duration_ms"] or 0) / 1000 / 3600
        if current and (len(current) >= OVERFLOW_MAX_MOVIES or current_hours + movie_hours > OVERFLOW_MAX_HOURS):
            chunks.append(current)
            current, current_hours = [], 0.0
        current.append(movie)
        current_hours += movie_hours
    if current:
        chunks.append(current)
    return chunks


def run(library_name: str, config_path: str, transcode_config: str, source_name: str, dry_run: bool):
    allowlist = gm.load_channel_genres(config_path)
    print(f"{len(allowlist)} channels configured: {', '.join(name for name, _ in allowlist)}")

    lib = tc.find_library(source_name, library_name) if source_name else None
    if not lib:
        # Search every connected source for a library with this name.
        for src in tc.list_media_sources():
            match = next((l for l in src.get("libraries", []) if l.get("name", "").lower() == library_name.lower()), None)
            if match:
                lib = {"source_id": src["id"], **match}
                break
    if not lib:
        raise SystemExit(f"no library named '{library_name}' found on any connected source")

    print(f"Fetching movies from '{library_name}' (this fetches genre detail per movie, may take a while)...")
    movies = fetch_movies_with_genres(lib["id"], verbose=True)
    print(f"{len(movies)} movies fetched.")

    placements = place_movies_on_channels(movies, allowlist)

    transcode_config_id = None if dry_run else tc.resolve_transcode_config(transcode_config)

    for block_index, (channel_base_name, _) in enumerate(allowlist):
        movies_for_channel = placements[channel_base_name]
        if not movies_for_channel:
            print(f"  {channel_base_name}: no movies matched, skipping")
            continue

        chunks = split_overflow(movies_for_channel)
        base_number = CHANNEL_NUMBER_BASE + block_index * CHANNEL_BLOCK_SIZE

        for chunk_index, chunk in enumerate(chunks):
            name = channel_base_name if chunk_index == 0 else f"{channel_base_name}-{chunk_index + 1}"
            number = base_number + chunk_index
            print(f"  {name} (#{number}): {len(chunk)} movies")

            channel = tc.create_channel(name, number, channel_base_name, transcode_config_id, dry_run=dry_run)
            if dry_run:
                continue

            lineup = [tc.lineup_entry(m["id"], int(m["duration_ms"])) for m in chunk]
            tc.set_programming(channel["id"], lineup)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--library", required=True, help="Jellyfin/Plex/Emby library name, as shown by tunarr_status.py sources")
    parser.add_argument("--source", help="Media source name (omit to search all connected sources)")
    parser.add_argument("--config", default="channel_genres.txt", help="Path to channel_genres.txt")
    parser.add_argument("--transcode-config", default="Default", help="Tunarr transcode config name")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be created without touching Tunarr")
    args = parser.parse_args()

    run(args.library, args.config, args.transcode_config, args.source, args.dry_run)


if __name__ == "__main__":
    main()
