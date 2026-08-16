#!/usr/bin/env python3
"""Payload-shape tests for tunarr_client.py's build_* functions — no live
HTTP, matching ../../tests/test_tunarr.c's explicit non-coverage of its own
network layer (see that file's header comment)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from testkit import Checker
import tunarr_client as tc


def run():
    c = Checker()

    p = tc.build_channel_payload("Action", 200, "Action", "cfg-id-1")
    c.eq(p["type"], "new", "channel payload type")
    c.eq(p["channel"]["name"], "Action", "channel payload name")
    c.eq(p["channel"]["number"], 200, "channel payload number")
    c.eq(p["channel"]["groupTitle"], "Action", "channel payload groupTitle")
    c.eq(p["channel"]["transcodeConfigId"], "cfg-id-1", "channel payload transcodeConfigId")
    c.eq(p["channel"]["streamMode"], "hls", "channel payload streamMode")
    c.eq(p["channel"]["id"], "", "channel payload id is empty (server assigns)")

    entry = tc.lineup_entry("abc-123", 5400000)
    c.eq(entry, {"type": "content", "id": "abc-123", "duration": 5400000}, "lineup_entry shape")

    prog = tc.build_programming_payload([entry], append=True)
    c.eq(prog["type"], "manual", "programming payload type")
    c.eq(prog["append"], True, "programming payload append")
    c.eq(prog["lineup"], [entry], "programming payload lineup")

    c.eq(
        tc.genre_names({"genres": [{"name": "Action", "uuid": "x"}, {"name": "Comedy"}, {}]}),
        ["Action", "Comedy"],
        "genre_names extracts names, skips entries without one",
    )
    c.eq(tc.genre_names({}), [], "genre_names handles missing genres key")

    # Real shape captured from a live Tunarr instance's
    # GET /api/media-libraries/{id}/programs — an anime episode, condensed
    # wrapper around the nested program/season/show objects.
    real_episode_wrapper = {
        "type": "content",
        "duration": 723301.587,
        "id": "f8be9e72-a61e-48ce-a6e8-ab16a2de721e",
        "program": {
            "uuid": "f8be9e72-a61e-48ce-a6e8-ab16a2de721e",
            "type": "episode",
            "episodeNumber": 3,
            "seasonId": "36c248e7-bc40-4607-9fae-b3d3a27972a4",
            "showId": "97b213be-56fc-4c7c-8add-63ef0d5775bf",
            "season": {"type": "season", "index": 1},
            "show": {"type": "show", "genres": []},
        },
    }
    norm = tc.normalize_episode(real_episode_wrapper)
    c.eq(norm["id"], "f8be9e72-a61e-48ce-a6e8-ab16a2de721e", "normalize_episode id")
    c.eq(norm["duration_ms"], 723301.587, "normalize_episode duration_ms from top-level duration")
    c.eq(norm["season"], 1, "normalize_episode season from program.season.index")
    c.eq(norm["episode"], 3, "normalize_episode episode from program.episodeNumber")

    # Falls back to program.uuid/program.duration if the wrapper's own
    # top-level id/duration are absent (defensive — shouldn't normally
    # happen against list_library_programs, but keeps normalize_episode
    # robust to minor shape variance).
    fallback = tc.normalize_episode({"program": {"uuid": "u1", "duration": 5000, "episodeNumber": 2}})
    c.eq(fallback["id"], "u1", "normalize_episode falls back to program.uuid")
    c.eq(fallback["duration_ms"], 5000, "normalize_episode falls back to program.duration")
    c.eq(fallback["season"], 0, "normalize_episode defaults season to 0 when absent")

    c.eq(
        tc.group_episodes_by_show([
            {"id": "e1", "duration": 1000, "program": {"type": "episode", "showId": "s1",
             "episodeNumber": 1, "season": {"index": 1},
             "show": {"title": "Show A", "genres": [{"name": "Comedy"}]}}},
            {"id": "e2", "duration": 2000, "program": {"type": "episode", "showId": "s1",
             "episodeNumber": 2, "season": {"index": 1}, "show": {"title": "Show A", "genres": []}}},
            {"id": "m1", "duration": 3000, "program": {"type": "movie", "showId": None}},
        ]),
        {"s1": {"title": "Show A", "genres": ["Comedy"], "episodes": [
            {"id": "e1", "duration_ms": 1000, "season": 1, "episode": 1},
            {"id": "e2", "duration_ms": 2000, "season": 1, "episode": 2},
        ]}},
        "group_episodes_by_show groups episodes, skips movies, keeps show title/genres",
    )

    slot = tc.build_show_slot("slot-uuid-1", "show-uuid-1", 2, weight=1.5, program_count=5)
    c.eq(slot["id"], "slot-uuid-1", "show slot id")
    c.eq(slot["type"], "show", "show slot type")
    c.eq(slot["showId"], "show-uuid-1", "show slot showId")
    c.eq(slot["seasonFilter"], [2], "show slot seasonFilter targets one season")
    c.eq(slot["order"], "chronological", "show slot order")
    c.eq(slot["linkMode"], "continue", "show slot linkMode")
    c.eq(slot["weight"], 1.5, "show slot weight")
    c.eq(slot["durationSpec"], {"type": "dynamic", "programCount": 5}, "show slot durationSpec")
    c.check("iterationGroup" not in slot, "show slot omits iterationGroup when not given")

    slot_grouped = tc.build_show_slot("slot-2", "show-uuid-1", 3, iteration_group="grp-1")
    c.eq(slot_grouped["iterationGroup"], "grp-1", "show slot includes iterationGroup when given")

    sched = tc.build_schedule_payload([slot])
    c.eq(sched["type"], "random", "schedule payload type")
    c.eq(sched["slots"], [slot], "schedule payload slots")
    c.eq(sched["maxDays"], 365, "schedule payload maxDays matches Tunarr's own UI default")
    c.eq(sched["padStyle"], "slot", "schedule payload padStyle matches Tunarr's own UI default")

    print(f"test_tunarr_client: {c.checks} checks, {c.failures} failures")
    return c.checks, c.failures


if __name__ == "__main__":
    _, failures = run()
    sys.exit(1 if failures else 0)
