#!/usr/bin/env python3
"""Require nonempty, internally consistent, zero-error Kirk LTP results."""
import json
import argparse
from collections import Counter
from pathlib import Path
import re
import sys


def check(path, runtest=None, skip_file=None, pattern_file=None):
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
    invalid = [row["test_fqn"] for row in results if row["status"] not in ("pass", "conf", "skip")]
    if invalid:
        raise ValueError(f"non-passing test statuses: {invalid}")
    if runtest is not None:
        # Match Kirk's regex-search filtering. CI does not use max_runtime
        # metadata filtering or suite iteration; snapshots belong to this run.
        skip = "|".join(line.rstrip() for line in Path(skip_file).read_text().splitlines()
                        if line.strip() and not re.search(r"^\s*#", line))
        pattern = Path(pattern_file).read_text().strip()
        names = [line.split()[0] for line in Path(runtest).read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        expected = Counter(name for name in names
                           if (not skip or not re.search(skip, name))
                           and (not pattern or re.search(pattern, name)))
        actual = Counter(row["test_fqn"] for row in results)
        if not expected:
            raise ValueError("empty expected test selection")
        if actual != expected:
            raise ValueError(f"incomplete/mismatched suite: missing={dict(expected - actual)}, "
                             f"unexpected={dict(actual - expected)}")
        print(f"Suite coverage accepted: {sum(expected.values())} test records")
    print(f"LTP assertions accepted: {stats['passed']} pass, {stats['skipped']} skip")
    if runtest is None:
        print("Suite coverage was not checked: no run selection supplied.")
    print("Normal termination must also be checked by the runner.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result")
    parser.add_argument("--runtest")
    parser.add_argument("--skip-file")
    parser.add_argument("--pattern-file")
    args = parser.parse_args()
    selection = (args.runtest, args.skip_file, args.pattern_file)
    if any(selection) and not all(selection):
        parser.error("--runtest, --skip-file and --pattern-file must be supplied together")
    try:
        check(args.result, *selection)
    except (OSError, ValueError, KeyError, TypeError, re.error) as exc:
        print(f"LTP acceptance failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
