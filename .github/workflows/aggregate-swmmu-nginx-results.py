#!/usr/bin/env python3
"""Collect the six SWMMU nginx benchmark legs and prepare plot data."""

import csv
import json
import math
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: aggregate-swmmu-nginx-results.py ARTIFACTS_DIR OUTPUT_DIR", file=sys.stderr)
        return 2
    artifacts = Path(sys.argv[1])
    output = Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    by_key = {}
    for path in artifacts.glob("swmmu-nginx-w*-c*/measurement.json"):
        try:
            item = json.loads(path.read_text())
            key = (int(item["workers"]), int(item["connections"]))
            by_key[key] = item
        except (OSError, ValueError, KeyError, TypeError) as exc:
            print(f"invalid measurement {path}: {exc}", file=sys.stderr)

    rows = []
    for workers in (1, 4):
        for connections in (4, 16, 64):
            item = by_key.get((workers, connections))
            if item is None:
                item = {
                    "status": "FAIL",
                    "failures": ["matrix artifact or measurement.json is missing"],
                    "workers": workers,
                    "connections": connections,
                    "requests_per_second": None,
                    "average_latency_ms": None,
                    "socket_errors": None,
                }
            rows.append(item)

    (output / "aggregate.json").write_text(
        json.dumps({"schema_version": 1, "matrix": rows}, indent=2, sort_keys=True) + "\n"
    )
    with (output / "throughput.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("connections", "workers_1", "workers_4"))
        for connections in (4, 16, 64):
            values = []
            for workers in (1, 4):
                item = by_key.get((workers, connections))
                value = None
                if item and item.get("status") == "PASS":
                    value = item.get("requests_per_second")
                values.append(value if isinstance(value, (int, float)) and math.isfinite(value) else "NaN")
            writer.writerow((connections, *values))

    print(f"Wrote {len(rows)} matrix results to {output / 'aggregate.json'}")
    for item in rows:
        print(
            f"workers={item['workers']} connections={item['connections']} "
            f"status={item['status']} requests_per_second={item.get('requests_per_second')} "
            f"failures={'; '.join(item.get('failures', []))}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
