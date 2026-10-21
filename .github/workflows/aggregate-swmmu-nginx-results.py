#!/usr/bin/env python3
"""Collect all UML nginx matrix legs into machine-readable tables."""

import csv
import json
import math
import sys
from pathlib import Path

PROFILES = (
    "native",
    "um-mmu",
    "um-mmu-seccomp",
    "um-nommu",
    "um-nommu-seccomp",
    "um-nommu-sas-seccomp",
    "um-nommu-sas-zpoline",
)
WORKERS = (1, 4)
CONNECTIONS = (4, 16, 64)
CPU_COUNTS = {
    "native": (1,),
    "um-mmu": (1,),
    "um-mmu-seccomp": (1, 4),
    "um-nommu": (1,),
    "um-nommu-seccomp": (1, 4),
    "um-nommu-sas-seccomp": (1,),
    "um-nommu-sas-zpoline": (1,),
}


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: aggregate-swmmu-nginx-results.py ARTIFACTS_DIR OUTPUT_DIR", file=sys.stderr)
        return 2
    artifacts = Path(sys.argv[1])
    output = Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    by_key = {}
    for path in artifacts.glob("nginx-*-w*-n*-c*/measurement.json"):
        try:
            item = json.loads(path.read_text())
            key = (item["profile"], int(item.get("ncpus", 1)),
                   int(item["workers"]), int(item["connections"]))
            if key[0] in PROFILES:
                by_key[key] = item
        except (OSError, ValueError, KeyError, TypeError) as exc:
            print(f"invalid measurement {path}: {exc}", file=sys.stderr)

    rows = []
    for profile in PROFILES:
        profile_rows = []
        for ncpus in CPU_COUNTS[profile]:
            for workers in WORKERS:
                for connections in CONNECTIONS:
                    item = by_key.get((profile, ncpus, workers, connections))
                    if item is None:
                        item = {
                            "profile": profile,
                            "ncpus": ncpus,
                            "status": "FAIL",
                            "failures": ["matrix artifact or measurement.json is missing"],
                            "workers": workers,
                            "connections": connections,
                            "requests_per_second": None,
                            "average_latency_ms": None,
                            "socket_errors": None,
                        }
                    profile_rows.append(item)
                    rows.append(item)
        (output / f"{profile}.json").write_text(
            json.dumps({"schema_version": 2, "profile": profile, "matrix": profile_rows}, indent=2, sort_keys=True) + "\n"
        )
        with (output / f"throughput-{profile}.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("ncpus", "connections", "workers_1", "workers_4"))
            for connections in CONNECTIONS:
                for ncpus in CPU_COUNTS[profile]:
                    values = []
                    for workers in WORKERS:
                        item = by_key.get((profile, ncpus, workers, connections))
                        value = item.get("requests_per_second") if item and item.get("status") == "PASS" else None
                        values.append(value if finite(value) else "NaN")
                    writer.writerow((ncpus, connections, *values))

    (output / "aggregate.json").write_text(
        json.dumps({"schema_version": 2, "profiles": PROFILES, "cpu_counts": CPU_COUNTS, "matrix": rows}, indent=2, sort_keys=True) + "\n"
    )
    with (output / "throughput.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("profile", "ncpus", "workers", "connections", "status", "requests_per_second", "average_latency_ms", "socket_errors", "failures"))
        for item in rows:
            writer.writerow((
                item["profile"], item["ncpus"], item["workers"], item["connections"], item["status"],
                item.get("requests_per_second"), item.get("average_latency_ms"),
                json.dumps(item.get("socket_errors")), "; ".join(item.get("failures", [])),
            ))
    print(f"Wrote {len(rows)} matrix results to {output / 'aggregate.json'}")
    for item in rows:
        print(
            f"profile={item['profile']} ncpus={item['ncpus']} "
            f"workers={item['workers']} connections={item['connections']} "
            f"status={item['status']} requests_per_second={item.get('requests_per_second')} "
            f"failures={'; '.join(item.get('failures', []))}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
