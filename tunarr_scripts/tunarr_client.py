#!/usr/bin/env python3
"""Tunarr REST client — the ONLY module in this toolkit that makes network
calls, and the only client this toolkit has at all.

There's no separate Jellyfin client: once a Jellyfin (or Plex/Emby) source
is added inside Tunarr's own web UI (Settings > Sources) and its libraries
are enabled/scanned, Tunarr's own API already exposes everything this
toolkit needs — genres, runtime, movie/episode type, and full show/season/
episode grouping (confirmed by reading Tunarr's own sync code: nothing is
dropped in the Jellyfin->Tunarr sync). So there's no Jellyfin API key, no
second set of credentials, and no id-translation step — just TUNARR_HOST.
Payload-building is kept separate from the functions that actually send it
(the `build_*`/`*_payload` functions below) so tests can assert the exact
request shape without a live server — see tests/test_tunarr_client.py.
"""

import requests

from env_loader import get_env, load_env_file

load_env_file()

TUNARR_HOST = (get_env("TUNARR_HOST", "http://127.0.0.1:8000") or "").rstrip("/")

DEFAULT_TIMEOUT = 30


def _url(path: str) -> str:
    return f"{TUNARR_HOST}{path}"


def tunarr_alive() -> bool:
    """True if Tunarr answers at all. Used by tunarr_status.py's `health`
    command and as a sanity check before any channel-creating command."""
    try:
        r = requests.get(_url("/api/channels"), timeout=10)
        return r.status_code == 200
    except requests.RequestException:
        return False


def list_channels() -> list:
    r = requests.get(_url("/api/channels"), timeout=DEFAULT_TIMEOUT)
    r.raise_for_status()
    return r.json()


def list_media_sources() -> list:
    """Every connected media source (Jellyfin/Plex/Emby/local) with its
    libraries — GET /api/media-sources. Each library has enabled/isLocked/
    lastScannedAt. This is how create_movie_channels.py etc. find a
    library's id without the user having to look it up in the UI."""
    r = requests.get(_url("/api/media-sources"), timeout=15)
    r.raise_for_status()
    return r.json()


def find_library(source_name: str, library_name: str) -> dict:
    """A specific library dict (has "id") by source + library display name,
    case-insensitive. Raises with the available names if not found."""
    sources = list_media_sources()
    matched_source = next(
        (s for s in sources if s.get("name", "").lower() == source_name.lower()), None
    )
    if not matched_source:
        names = ", ".join(s.get("name", "?") for s in sources)
        raise SystemExit(f"no media source named '{source_name}' — available: {names}")
    libraries = matched_source.get("libraries", [])
    matched_lib = next(
        (lib for lib in libraries if lib.get("name", "").lower() == library_name.lower()), None
    )
    if not matched_lib:
        names = ", ".join(lib.get("name", "?") for lib in libraries)
        raise SystemExit(
            f"no library named '{library_name}' on source '{source_name}' — available: {names}"
        )
    return {"source_id": matched_source["id"], **matched_lib}


def scan_library(media_source_id: str, library_id: str = "all", force: bool = False) -> None:
    """Triggers an on-demand rescan — library_id="all" scans every enabled
    library for that source. force=True bypasses the "recently scanned,
    skip it" throttle Tunarr otherwise applies. Fire-and-forget: Tunarr
    replies 202 immediately and scans in the background — see
    library_scan_status() to poll progress."""
    params = {"forceScan": "true"} if force else {}
    r = requests.post(
        _url(f"/api/media-sources/{media_source_id}/libraries/{library_id}/scan"),
        params=params,
        timeout=15,
    )
    if r.status_code not in (200, 202):
        raise SystemExit(f"scan request failed: {r.status_code} {r.text}")


def library_scan_status(media_source_id: str, library_id: str) -> dict:
    """{"state": "queued"|"in_progress"|"completed"|..., "percentComplete": N}
    — GET /api/media-sources/{id}/{libraryId}/status."""
    r = requests.get(
        _url(f"/api/media-sources/{media_source_id}/{library_id}/status"), timeout=15
    )
    r.raise_for_status()
    return r.json()


def list_library_programs(library_id: str) -> list:
    """Every program (movie/episode/etc.) Tunarr has indexed for one
    library — GET /api/media-libraries/{id}/programs. Requires that
    library's scan to have completed at least once (see scan_library/
    library_scan_status). Populated with duration and season/show grouping,
    but NOT genres — confirmed against a real instance, this "condensed"
    listing always returns genres: null regardless of source data. Use
    get_program_detail() per item for genres (see create_movie_channels.py)."""
    r = requests.get(_url(f"/api/media-libraries/{library_id}/programs"), timeout=60)
    r.raise_for_status()
    return r.json()


def get_program_detail(program_id: str) -> dict:
    """Full detail for one program, including genres (unlike the bulk list
    endpoint above) — GET /api/programs/{id}. One request per item, so
    calling this for every movie in a large library takes a while; fine for
    a typical library, a real cost worth knowing about for a very large one.
    See genre_names() to pull genre strings out of the result."""
    r = requests.get(_url(f"/api/programs/{program_id}"), timeout=15)
    r.raise_for_status()
    return r.json()


def group_episodes_by_show(programs: list) -> dict:
    """Groups the output of list_library_programs() (a flat list of episode
    wrappers, each with program.show/program.showId embedded) by show —
    {show_uuid: {"title": ..., "genres": [...], "episodes": [normalized, ...]}}.

    NOTE: GET /api/programming/shows/{id}/seasons looks like the obvious way
    to get a show's episodes, but confirmed against a real instance it only
    returns season metadata (index/title/uuid) with NO episodes embedded,
    despite what the TypeScript schema's optional `episodes` field suggests
    — so this groups from the episode-level library listing instead, which
    already carries full show/season grouping per episode."""
    shows: dict = {}
    for wrapped in programs:
        program = wrapped.get("program", wrapped)
        if program.get("type") != "episode":
            continue
        show = program.get("show") or {}
        show_id = program.get("showId") or show.get("uuid")
        if not show_id:
            continue
        entry = shows.setdefault(
            show_id,
            {"title": show.get("title", ""), "genres": genre_names(show), "episodes": []},
        )
        entry["episodes"].append(normalize_episode(wrapped))
    return shows


def genre_names(program: dict) -> list:
    """Tunarr's genre field is a list of {"name": ..., "uuid": ...} objects
    (not plain strings like Jellyfin's own API) — this pulls out just the
    names, in whatever order Tunarr returns them, for genre_matcher.py.
    Movies carry their own genres; episodes don't (in practice, confirmed
    against a real library — episodes carry no "genres" key at all), the
    show they belong to does, so pass program["show"] for episodes."""
    return [g.get("name") for g in (program.get("genres") or []) if g.get("name")]


def normalize_episode(wrapped: dict) -> dict:
    """Flattens one entry from list_library_programs()/get_show_seasons()
    (the verbose {"type","duration","id","program":{...season,show,...}}
    wrapper — confirmed against a real Tunarr instance) into the plain
    {duration_ms, season, episode, id} shape series_classify.py's pure
    functions operate on. Keeps that module decoupled from Tunarr's nested
    wire format, so its tests can use simple fixtures instead of the full
    nested shape."""
    program = wrapped.get("program", wrapped)
    return {
        "id": wrapped.get("id") or program.get("uuid"),
        "duration_ms": wrapped.get("duration") or program.get("duration") or 0,
        "season": (program.get("season") or {}).get("index", 0),
        "episode": program.get("episodeNumber", 0),
    }


def get_channel(name: str) -> dict:
    channels = list_channels()
    matched = [c for c in channels if c.get("name", "").lower() == name.lower()]
    if not matched:
        names = ", ".join(c.get("name", "?") for c in channels)
        raise SystemExit(f"no channel named '{name}' — available: {names}")
    return matched[0]


def get_programming(channel_id: str) -> dict:
    r = requests.get(_url(f"/api/channels/{channel_id}/programming"), timeout=DEFAULT_TIMEOUT)
    r.raise_for_status()
    return r.json()


def get_transcode_configs() -> list:
    r = requests.get(_url("/api/transcode_configs"), timeout=10)
    r.raise_for_status()
    return r.json()


def resolve_transcode_config(name: str) -> str:
    configs = get_transcode_configs()
    matched = [c for c in configs if c.get("name", "").lower() == name.lower()]
    if not matched:
        names = ", ".join(c.get("name", "?") for c in configs)
        raise SystemExit(f"no transcode config named '{name}' — available: {names}")
    return matched[0]["id"]


# ── payload builders (pure — no network, unit-testable) ────────────────────


def build_channel_payload(name: str, number: int, group_title: str, transcode_config_id: str) -> dict:
    """The exact 'create a new channel' request body. One place this gets
    built, so every caller (movie/series/mix channel scripts) sends an
    identical shape."""
    return {
        "type": "new",
        "channel": {
            "id": "",
            "name": name,
            "number": number,
            "groupTitle": group_title,
            "transcodeConfigId": transcode_config_id,
            "streamMode": "hls",
            "startTime": 0,
            "duration": 0,
            "disableFillerOverlay": False,
            "fillerRepeatCooldown": 30000,
            "guideFlexTitle": "",
            "guideMinimumDuration": 30000,
            "stealth": False,
            "icon": {"path": "", "width": 0, "duration": 0, "position": "bottom-right"},
            "offline": {"mode": "pic", "picture": "", "soundtrack": ""},
            "watermark": {
                "enabled": False,
                "width": 10,
                "verticalMargin": 1,
                "horizontalMargin": 1,
            },
            "onDemand": {"enabled": False},
            "subtitlesEnabled": False,
        },
    }


def lineup_entry(uuid: str, duration_ms: int) -> dict:
    return {"type": "content", "id": uuid, "duration": duration_ms}


def build_programming_payload(lineup: list, append: bool = False) -> dict:
    return {"type": "manual", "lineup": lineup, "append": append}


def build_show_slot(
    slot_id: str, show_id: str, season_number: int, weight: float = 1,
    program_count: int = 3, iteration_group: str = None,
) -> dict:
    """One [season -> schedule slot] entry for a per-show random schedule —
    confirmed against Tunarr's real slot schema (types/src/api/CommonSlots.ts
    ShowProgrammingSlotSchema + RandomSlots.ts BaseRandomSlotSchema/
    ShowProgrammingRandomSlotSchema), NOT a "programId"-per-episode shape
    (there isn't one — a "show" slot references showId + seasonFilter and
    Tunarr picks episodes from that pool itself each time it's scheduled).

    slot_id must be a fresh UUID (Tunarr's schema requires each slot to
    carry its own id, client-supplied — see uuid.uuid4() at the call site).
    order="chronological" + linkMode="continue" means episodes play in
    original air order and resume where a previous visit left off, rather
    than repeating from the start or shuffling — the "binge this show in
    order" behavior the whole per-show-channel feature is for.
    iteration_group: pass the SAME uuid across every slot belonging to one
    show (one per season) to make Tunarr treat them as one continuous
    watch-through across season boundaries rather than each season slot
    tracking its own independent resume position — confirmed a real field
    on LinkableSlot (CommonSlots.ts) for exactly this purpose."""
    slot = {
        "id": slot_id,
        "type": "show",
        "showId": show_id,
        "seasonFilter": [season_number],
        "seasonExcludeFilter": [],
        "order": "chronological",
        "direction": "asc",
        "linkMode": "continue",
        "cooldownMs": 0,
        "durationSpec": {"type": "dynamic", "programCount": program_count},
        "weight": weight,
    }
    if iteration_group:
        slot["iterationGroup"] = iteration_group
    return slot


def build_schedule_payload(slots: list) -> dict:
    """Top-level random-schedule request body — defaults match Tunarr's own
    web UI (web/src/model/SlotModels.ts's defaultRandomSlotSchedule),
    confirmed by reading it directly, rather than guessed: padStyle="slot",
    randomDistribution="uniform", flexPreference="distribute", maxDays=365,
    padMs=1, lockWeights=True."""
    return {
        "type": "random",
        "slots": slots,
        "padStyle": "slot",
        "randomDistribution": "uniform",
        "flexPreference": "distribute",
        "maxDays": 365,
        "padMs": 1,
        "lockWeights": True,
    }


# ── network calls that use the builders above ───────────────────────────────


def create_channel(name: str, number: int, group_title: str, transcode_config_id: str, dry_run: bool = False) -> dict:
    if dry_run:
        return {"id": "dry-run", "name": name, "number": number, "groupTitle": group_title}
    payload = build_channel_payload(name, number, group_title, transcode_config_id)
    r = requests.post(_url("/api/channels"), json=payload, timeout=DEFAULT_TIMEOUT)
    if r.status_code != 201:
        raise SystemExit(f"failed to create channel '{name}': {r.status_code} {r.text}")
    return r.json()


def set_programming(channel_id: str, lineup: list, append: bool = False) -> None:
    payload = build_programming_payload(lineup, append)
    r = requests.post(_url(f"/api/channels/{channel_id}/programming"), json=payload, timeout=60)
    r.raise_for_status()


def set_schedule(channel_id: str, slots: list) -> None:
    payload = build_schedule_payload(slots)
    r = requests.put(_url(f"/api/channels/{channel_id}/schedule"), json=payload, timeout=60)
    r.raise_for_status()
