#!/usr/bin/env python3
"""Render nginx throughput distributions by UML profile and worker count."""

import json
import math
import subprocess
import sys
from pathlib import Path

PROFILES = (
    ("um-mmu", "um", "#27ae60"),
    ("um-mmu-seccomp", "um-mmu(s)", "#145a32"),
    ("um-nommu", "um-nommu", "#d68910"),
    ("um-nommu-seccomp", "um-nommu(s)", "#a04000"),
    ("um-nommu-sas-seccomp", "um-nommu(sas-s)", "#2874a6"),
    ("um-nommu-sas-zpoline", "um-nommu(sas-z)", "#1f618d"),
)
WORKERS = (1, 4)
CONNECTIONS = (4, 16, 64)


def quote(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "\\'") + "'"


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def quantile(sorted_values, fraction):
    """Return a linearly interpolated quantile (the common type-7 method)."""
    position = (len(sorted_values) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return sorted_values[lower]
    weight = position - lower
    return sorted_values[lower] * (1 - weight) + sorted_values[upper] * weight


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
        for profile_index, (profile, _label, _color) in enumerate(PROFILES, start=1):
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
    for profile_index, (profile, label, color) in enumerate(PROFILES, start=1):
        for workers in WORKERS:
            x = profile_index + (-0.18 if workers == 1 else 0.18)
            worker_data = out_dir / f"throughput-boxplot-{profile}-workers-{workers}.dat"
            samples = []
            for connections in CONNECTIONS:
                value = values.get((profile, workers, connections))
                if finite(value):
                    samples.append(value)
            samples.sort()
            with worker_data.open("w") as stream:
                if len(samples) == len(CONNECTIONS):
                    summary = (
                        x,
                        samples[0],
                        quantile(samples, 0.25),
                        quantile(samples, 0.5),
                        quantile(samples, 0.75),
                        samples[-1],
                    )
                    stream.write(" ".join(f"{value:.12g}" for value in summary) + "\n")
            sample_count = len(samples)
            if sample_count >= 3:
                title = f"title {quote(label)}" if workers == 1 else "notitle"
                plot_items.append(
                    f"{quote(worker_data)} using 1:4:2:6 with yerrorbars "
                    f"lc rgb {quote(color)} notitle, \\\n  {quote(worker_data)} using 1:4:($1-0.14):($1+0.14):3:5 "
                    f"with boxxyerror fc rgb {quote(color)} "
                    f"fs {'solid 0.68' if workers == 1 else 'pattern 4'} "
                    f"border lc rgb {quote(color)} {title}, \\\n  {quote(worker_data)} using 1:4:(0.28):(0) with vectors nohead "
                    f"lw 2 lc rgb {quote(color)} notitle"
                )
            elif sample_count:
                print(
                    f"Omitting incomplete boxplot for {profile}, workers={workers}: "
                    f"only {sample_count} of 3 connection measurements passed",
                    file=sys.stderr,
                )
    status_path = out_dir / "plot-status.txt"
    if not plot_items:
        status_path.write_text(
            "Plot omitted: no profile/worker group has all 3 connection measurements.\n"
        )
        print(status_path.read_text().strip())
        return
    script = out_dir / "throughput-comparison.gnuplot"
    script.write_text("\n".join((
        "set terminal pngcairo size 2200,1300 enhanced font 'Sans,18'",
        f"set output {quote(out_dir / 'nginx-comparison.png')}",
        'set title "nginx throughput by UML profile and worker count\\nSolid: 1 worker; hatched: 4 workers; boxes summarize connections 4, 16, 64"',
        "set title font 'Sans,24'",
        "set xlabel 'UML profile' font 'Sans,20'",
        "set ylabel 'Requests per second' font 'Sans,20'",
        "set xtics ('um' 1, 'um-mmu(s)' 2, 'um-nommu' 3, 'um-nommu(s)' 4, 'um-nommu(sas-s)' 5, 'um-nommu(sas-z)' 6) rotate by -18 font 'Sans,20'",
        "set ytics font 'Sans,20'",
        "set xrange [0.5:6.5]",
        "set grid ytics",
        "set logscale y",
        "set yrange [1:*]",
        "set boxwidth 0.28 absolute",
        "set key outside top center horizontal font 'Sans,20'",
        "set style fill solid 0.68 border -1",
        "set bars 0.25",
        "set tics out",
        "plot \\\n  " + ", \\\n  ".join(plot_items),
        "",
    )))
    subprocess.run(["gnuplot", str(script)], check=True)
    status_path.write_text(
        "Combined box-and-whisker plot generated from profile/worker groups "
        "with all 3 connection measurements; boxes show Q1/median/Q3 and "
        "whiskers show min/max.\n"
    )
    print(f"Generated {out_dir / 'nginx-comparison.png'}")


if __name__ == "__main__":
    main()
