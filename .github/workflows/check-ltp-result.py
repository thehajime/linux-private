#!/usr/bin/env python3
"""Require nonempty, internally consistent, zero-error Kirk LTP results."""
import json
from pathlib import Path
import sys


def check(path):
    data = json.loads(Path(path).read_text())
    results = data["results"]
    stats = data["stats"]
    if not isinstance(results, list) or not results:
        raise ValueError("missing or empty test records")
    for key in ("passed", "failed", "broken", "skipped", "warnings"):
        values = [stats[key]] + [row["test"][key] for row in results]
        if any(type(value) is not int or value < 0 for value in values):
            raise ValueError(f"invalid {key} counter")
        if values[0] != sum(values[1:]):
            raise ValueError(f"{key} summary does not match test records")
    if stats["passed"] == 0:
        raise ValueError("no passing assertions: not an acceptance run")
    errors = {key: stats[key] for key in ("failed", "broken", "warnings") if stats[key]}
    if errors:
        raise ValueError(f"LTP errors: {errors}")
    print(f"LTP assertions accepted: {stats['passed']} pass, {stats['skipped']} skip")
    print("Suite coverage and normal termination must also be checked by the runner.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: check-ltp-result.py RESULT.json")
    try:
        check(sys.argv[1])
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"LTP acceptance failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
