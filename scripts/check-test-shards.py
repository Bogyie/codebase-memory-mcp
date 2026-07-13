#!/usr/bin/env python3
"""Fail when the CI shard manifest misses or duplicates a C test suite."""

import json
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = ROOT / "tests" / "test_main.c"
MANIFEST = ROOT / ".github" / "test-shards.json"


def main() -> int:
    selected = re.findall(r"RUN_SELECTED_SUITE\(([^)]+)\);", MAIN.read_text(encoding="utf-8"))
    shards = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assigned = [suite for suites in shards.values() for suite in suites]

    expected = set(selected)
    actual = set(assigned)
    duplicates = sorted({suite for suite in assigned if assigned.count(suite) > 1})
    missing = sorted(expected - actual)
    unknown = sorted(actual - expected)

    if duplicates or missing or unknown or len(selected) != len(expected):
        if duplicates:
            print(f"duplicate shard suites: {', '.join(duplicates)}", file=sys.stderr)
        if missing:
            print(f"missing shard suites: {', '.join(missing)}", file=sys.stderr)
        if unknown:
            print(f"unknown shard suites: {', '.join(unknown)}", file=sys.stderr)
        if len(selected) != len(expected):
            print("test_main.c contains duplicate suite registrations", file=sys.stderr)
        return 1

    print(f"test shard coverage OK: {len(expected)} suites across {len(shards)} shards")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
