#!/usr/bin/env python3
"""Minimal .env file loader — vendored/trimmed from a larger internal helper.

Keeps only the two behaviors that matter for this toolkit: search the
current directory and its parents for the file (so scripts work no matter
which directory they're run from), and never override a variable that's
already set in the real environment (so `FOO=bar python3 script.py` can
override the .env file for testing without editing it). No ~-expansion —
none of this toolkit's config values (TUNARR_HOST, JELLYFIN_HOST, JELLY_KEY,
JF_SERVER_ID) are ever paths.
"""

import os
from pathlib import Path
from typing import Optional, Union


def load_env_file(env_filename: Optional[Union[str, Path]] = None) -> dict:
    """Loads KEY=VALUE lines from env_filename into os.environ (only for
    keys not already set) and returns what it loaded.

    env_filename: a bare filename (e.g. ".env_tunarr") searches the current
    directory and its parents; a path containing "/" or a Path object is
    used exactly as given. Defaults to ".env_tunarr".
    """
    if env_filename is None:
        env_filename = ".env_tunarr"

    if isinstance(env_filename, str) and "/" not in env_filename:
        env_file = None
        current = Path.cwd()
        for parent in [current] + list(current.parents):
            candidate = parent / env_filename
            if candidate.exists():
                env_file = candidate
                break
    else:
        env_file = Path(env_filename)

    if env_file is None or not env_file.exists():
        return {}

    loaded = {}
    with open(env_file, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip()
            if (value.startswith('"') and value.endswith('"')) or (
                value.startswith("'") and value.endswith("'")
            ):
                value = value[1:-1]
            if key not in os.environ:
                os.environ[key] = value
                loaded[key] = value
    return loaded


def get_env(key: str, default: Optional[str] = None) -> Optional[str]:
    return os.environ.get(key, default)
