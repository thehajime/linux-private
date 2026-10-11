#!/usr/bin/env python3
"""Render all nginx profile/worker matrix measurements in one comparison plot."""

import json
import math
import subprocess
import sys
from pathlib import Path

PROFILES = (
    ("um-mmu", "um"),
    ("um-mmu-seccomp", "um-mmu(s)"),
    ("um-nommu", "um-nommu"),
    ("um-nommu-seccomp", "um-nommu(s)"),
    ("um-nommu-sas-seccomp", "um-nommu(sas-s)"),
    ("um-nommu-sas-zpoline", "um-nommu(sas-z)"),
)
WORKERS = (1, 4)
CONNECTIONS = (4, 16, 64)
COLORS = ("#27ae60", "#145a32", "#2874a6", "#1f618d", "#d68910", "#7d3c98")


def quote(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "\\'") + "'"


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: plot-nginx-comparison.py AGGREGATE_JSON OUTPUT_DIR")
    source, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    matrix = json.loads(source.read_text()).get("matrix", [])
    values = {
        (row.get("profile"), row.get("workers"), row.get("connections")): row.get("requests_per_second")
        for row in matrix if row.get("status") == "PASS"
    }
    data = out_dir / "throughput-comparison.csv"
    with data.open("w") as stream:
        stream.write("connections," + ",".join(
            f"{profile}_{workers}" for profile, _label in PROFILES for workers in WORKERS
        ) + "\n")
        for connections in CONNECTIONS:
            row = [str(connections)]
            for profile, _label in PROFILES:
                for workers in WORKERS:
                    value = values.get((profile, workers, connections))
                    row.append(str(value) if finite(value) else "NaN")
            stream.write(",".join(row) + "\n")

    plot_items = []
    column = 2
    for profile_index, (_profile, label) in enumerate(PROFILES):
        for workers in WORKERS:
            if any(finite(values.get((PROFILES[profile_index][0], workers, connection)))
                   for connection in CONNECTIONS):
                plot_items.append(
                    f"{quote(data)} using 1:{column} with linespoints lw 2 pt {workers + 4} "
                    f"lc rgb {quote(COLORS[profile_index])} dt {1 if workers == 1 else 2} "
                    f"title {quote(f'{label} workers={workers}')}"
                )
            column += 1
    status_path = out_dir / "plot-status.txt"
    if not plot_items:
        status_path.write_text("Plot omitted: no successful nginx measurements are available.\n")
        print(status_path.read_text().strip())
        return
    script = out_dir / "throughput-comparison.gnuplot"
    script.write_text("\n".join((
        "set terminal pngcairo size 1800,1000 enhanced font 'Sans,12'",
        f"set output {quote(out_dir / 'nginx-comparison.png')}",
        "set datafile separator comma",
        "set datafile missing 'NaN'",
        "set title 'nginx throughput comparison'",
        "set xlabel 'wrk connections'",
        "set ylabel 'Requests per second'",
        "set xtics ('4' 4, '16' 16, '64' 64)",
        "set xrange [0:68]",
        "set grid ytics",
        "set yrange [0:*]",
        "set key outside right center",
        "plot \\\n  " + ", \\\n  ".join(plot_items),
        "",
    )))
    subprocess.run(["gnuplot", str(script)], check=True)
    status_path.write_text("Combined plot generated from available nginx matrix measurements.\n")
    print(f"Generated {out_dir / 'nginx-comparison.png'}")


if __name__ == "__main__":
    main()
