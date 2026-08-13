# tunarr_scripts

A self-contained "jumpstart" toolkit for generating a working [Tunarr](https://github.com/chrisbenincasa/tunarr)
channel lineup from your Jellyfin (or Plex/Emby) library — genre-based
movie channels, per-show channels, and a genre-based fallback for shows too
small to warrant their own channel.

This is a slimmed-down reference implementation, not a full automation
suite — see "Scope" below.

![Channels created by the scripts, including overflow channels](screenshots/12-channels-created.png)

See `WORKFLOW.md` for the full step-by-step walkthrough with screenshots,
from a fresh Tunarr install through a real script run.

## Install

A virtual environment is effectively required now — modern Debian/Ubuntu
Python refuses a bare `pip install` system-wide (PEP 668,
"externally-managed-environment").

**With [uv](https://github.com/astral-sh/uv)** (recommended if you have it —
faster, and `uv run` below means you don't even have to remember to
activate the venv):

```bash
cd tunarr_scripts
uv venv
source .venv/bin/activate
uv pip install -r requirements.txt
```

Confirmed real output from this exact sequence:
```
$ uv venv
Using CPython 3.14.4 interpreter at: /usr/bin/python3
Creating virtual environment at: .venv
Activate with: source .venv/bin/activate
$ source .venv/bin/activate
$ uv pip install -r requirements.txt
Resolved 5 packages in 386ms
Prepared 1 package in 49ms
Installed 5 packages in 37ms
 + certifi==2026.7.22
 + charset-normalizer==3.4.9
 + idna==3.18
 + requests==2.34.2
 + urllib3==2.7.0
```

Once installed, either run scripts normally inside the activated venv
(`python3 create_movie_channels.py ...`), or skip activation entirely with
`uv run` (it finds `.venv` automatically):
```bash
uv run create_movie_channels.py --library "Movies" --config channel_genres.txt
```

**Without uv** (stdlib only):

```bash
cd tunarr_scripts
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

Either way, the only third-party dependency is `requests` (plus its own
small transitive deps — `certifi`/`charset-normalizer`/`idna`/`urllib3` —
pulled in automatically).

## Setup

1. **Deploy Tunarr** if you haven't already — see `docker-compose.examples.yml`
   for annotated examples (Intel/AMD or NVIDIA GPU passthrough, and a
   Jellyfin-library bind mount for better streaming performance).
2. **Connect Jellyfin to Tunarr, inside Tunarr's own web UI** (Settings →
   Sources → Add, then Edit Libraries to enable/scan the libraries you
   want) — see `WORKFLOW.md`'s Prerequisites section for the full
   walkthrough. This is a one-time step and it's the only place Jellyfin
   credentials are ever entered.
3. Copy `.env_tunarr.example` to `.env_tunarr` and set `TUNARR_HOST`.
4. Copy `channel_genres.txt.example` to `channel_genres.txt` and edit it
   for your library's actual genre tags (only needed for the movie/mix
   channel scripts).

## Commands

| Command | Needs Jellyfin config? | What it does |
|---|---|---|
| `tunarr_status.py health` | No | Checks Tunarr is reachable |
| `tunarr_status.py list` | No | Lists existing channels |
| `tunarr_status.py sources` | No | Lists connected sources/libraries and scan status |
| `tunarr_status.py rescan <source> [<library>]` | No | Triggers an on-demand library rescan |
| `create_movie_channels.py --library <name>` | No¹ | Genre-based movie channels |
| `create_series_channels.py backfill --library <name>` | No¹ | Per-show channels for every channel-worthy show |
| `create_series_channels.py create --library <name> --series "<title>" --number N` | No¹ | One specific show's channel |
| `create_mix_channels.py --library <name>` | No¹ | Small-series genre mix channels |
| `dump_library.py --library <name>` | No¹ | Preview table: every title, genre(s), runtime, and where it would land — no changes made |

¹ None of these need a Jellyfin API key *from this toolkit* — see Auth
below — but they do need the library already connected and scanned
*inside Tunarr itself* first.

All of the `create_*` scripts accept `--dry-run` to preview what would be
created without touching Tunarr. `dump_library.py` is read-only by nature —
it uses the exact same placement logic the `create_*` scripts do (imported
from them directly), so it can't drift out of sync with what a real run
would do. Run it any time, before or after creating channels, as a
guide/reference for "what genre is this movie matched to" or "which channel
is this show on."

See `WORKFLOW.md` for what each script actually does and why, including the
duplicate-show-entry behavior and the Merge Versions warning — read that
before running `create_series_channels.py backfill` on a library with any
metadata quirks.

## Auth

**Tunarr needs no authentication at all** for anything in this toolkit —
every request just hits `TUNARR_HOST` with no headers (confirmed against
Tunarr's own API).

**Jellyfin needs no separate credentials from this toolkit either.** Once a
Jellyfin source is added and scanned inside Tunarr's own UI, Tunarr's API
already exposes everything these scripts need (genres, runtime, show/season
grouping) — confirmed by reading Tunarr's own Jellyfin-sync code directly,
nothing gets dropped in that sync. So there's no `JELLY_KEY` here, and no
second set of credentials to manage.

If you're wondering whether Jellyfin's Quick Connect (the scan-a-code login
flow) could substitute for an API key here: it's not applicable — Quick
Connect is an interactive approval flow for a person to approve from an
already-signed-in session, which doesn't fit a scripted tool with no
interactive step to hang an approval on. This doesn't come up anyway, since
Jellyfin auth lives entirely inside Tunarr's own UI, not in this toolkit.

## Scope

Included: genre movie channels, per-show channels, small-series mix
channels, a Tunarr-only status/rescan CLI.

Explicitly **not** included (this is a kickstart, not the full personal
toolkit it was extracted from): validation/drift reports, structured
logging integration, cron scheduling, encoding-policy scripts, `.nfo`-file
scanning, duplicate-series *cleanup* (this toolkit surfaces duplicates,
it doesn't fix your library's metadata for you).

## Tests

```bash
python3 tests/run_all.py
```

Pure logic only (genre matching, runtime-threshold classification, request
payload shapes) — no live server needed. Also wired into the main repo's
`make test`.
