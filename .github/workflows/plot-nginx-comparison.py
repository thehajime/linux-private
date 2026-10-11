#!/usr/bin/env python3
"""Render nginx throughput distributions by UML profile and worker count."""

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
WORKER_COLORS = {1: "#2874a6", 4: "#d68910"}


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
        stream.write("profile,workers,connections,status,requests_per_second,box_x\n")
        for profile_index, (profile, _label) in enumerate(PROFILES, start=1):
            for workers in WORKERS:
                x = profile_index + (-0.18 if workers == 1 else 0.18)
                for connections in CONNECTIONS:
                    value = values.get((profile, workers, connections))
                    status = "PASS" if finite(value) else next((
                        row.get("status", "MISSING") for row in matrix
                        if row.get("profile") == profile
                        and row.get("workers") == workers
                        and row.get("connections") == connections
                    ), "MISSING")
                    stream.write(
                        f"{profile},{workers},{connections},{status},"
                        f"{value if finite(value) else 'NaN'},{x:.2f}\n"
                    )

    plot_items = []
    for workers in WORKERS:
        worker_data = out_dir / f"throughput-boxplot-workers-{workers}.dat"
        with worker_data.open("w") as stream:
            for profile_index, (profile, _label) in enumerate(PROFILES, start=1):
                x = profile_index + (-0.18 if workers == 1 else 0.18)
                for connections in CONNECTIONS:
                    value = values.get((profile, workers, connections))
                    if finite(value):
                        stream.write(f"{x:.2f} {value}\n")
        if worker_data.stat().st_size:
            plot_items.append(
                f"{quote(worker_data)} using 1:2 with boxplot "
                f"fc rgb {quote(WORKER_COLORS[workers])} "
                f"title {quote(f'worker processes {workers}')}"
            )
    status_path = out_dir / "plot-status.txt"
    if not plot_items:
        status_path.write_text("Plot omitted: no successful nginx measurements are available.\n")
        print(status_path.read_text().strip())
        return
    script = out_dir / "throughput-comparison.gnuplot"
    script.write_text("\n".join((
        "set terminal pngcairo size 1800,1000 enhanced font 'Sans,12'",
        f"set output {quote(out_dir / 'nginx-comparison.png')}",
        "set title 'nginx throughput by UML profile and worker count\\nBoxes summarize successful runs across wrk connections 4, 16, 64'",
        "set xlabel 'UML profile'",
        "set ylabel 'Requests per second'",
        "set xtics ('um' 1, 'um-mmu(s)' 2, 'um-nommu' 3, 'um-nommu(s)' 4, 'um-nommu(sas-s)' 5, 'um-nommu(sas-z)' 6) rotate by -18",
        "set xrange [0.5:6.5]",
        "set grid ytics",
        "set logscale y",
        "set yrange [1:*]",
        "set style fill solid 0.65 border -1",
        "set style boxplot outliers",
        "set boxwidth 0.28 absolute",
        "set key outside top center horizontal",
        "set tics out",
        "plot \\\n  " + ", \\\n  ".join(plot_items),
        "",
    )))
    subprocess.run(["gnuplot", str(script)], check=True)
    status_path.write_text("Combined plot generated from available nginx matrix measurements.\n")
    print(f"Generated {out_dir / 'nginx-comparison.png'}")


if __name__ == "__main__":
    main()
