#!/usr/bin/env python3
"""Runs every test_*.py in this directory and prints a combined summary —
run with `python3 tests/run_all.py` from tunarr_scripts/, or via `make test`
from the repo root. Mirrors the spirit of the C test suite in ../../tests/:
plain counters + diff-style failure output, no third-party test framework
(this toolkit's only third-party dependency is `requests`, full stop)."""

import importlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

TEST_MODULES = [
    "test_tunarr_client",
    "test_genre_matcher",
    "test_series_classify",
]


def main() -> int:
    total_checks = 0
    total_failures = 0
    for name in TEST_MODULES:
        mod = importlib.import_module(name)
        checks, failures = mod.run()
        total_checks += checks
        total_failures += failures

    print(f"\ntunarr_scripts: {total_checks} checks, {total_failures} failures")
    return 1 if total_failures else 0


if __name__ == "__main__":
    sys.exit(main())
