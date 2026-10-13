#!/usr/bin/env python3
"""Create the grouped nginx throughput plot from the matrix aggregate."""

import json
import math
import subprocess
import sys
from pathlib import Path

PROFILES = (
    ("um-mmu", "um [SMP=y]", "#27ae60"),
    ("um-mmu-seccomp", "um-mmu(s) [SMP=y]", "#145a32"),
    ("um-nommu", "um-nommu [SMP=n]", "#d68910"),
    ("um-nommu-seccomp", "um-nommu(s) [SMP=n]", "#a04000"),
    ("um-nommu-sas-seccomp", "um-nommu(sas-s) [SMP=n]", "#0000ff"),
    ("um-nommu-sas-zpoline", "um-nommu(sas-z) [SMP=n]", "#4169e1"),
)
WORKERS = (1, 4)
CONNECTIONS = (4, 16, 64)


def gnuplot_quote(value):
    """Quote a value for a single-quoted gnuplot string."""
    escaped = str(value).replace("\\", "\\\\").replace("'", "\\'")
    return f"'{escaped}'"


def is_finite_number(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def load_measurements(path):
    matrix = json.loads(path.read_text()).get("matrix", [])
    values = {
        (row.get("profile"), row.get("workers"), row.get("connections")): row.get(
            "requests_per_second"
        )
        for row in matrix
        if row.get("status") == "PASS"
    }
    statuses = {
        (row.get("profile"), row.get("workers"), row.get("connections")): row.get(
            "status", "MISSING"
        )
        for row in matrix
    }
    return values, statuses


def write_measurement_files(output_dir, values, statuses):
    """Write the complete CSV and one gnuplot data file per profile/worker."""
    group_count = len(PROFILES) * len(WORKERS)
    box_width = 0.84 / group_count
    series = {}

    with (output_dir / "throughput-comparison.csv").open("w") as csv_file:
        csv_file.write("profile,workers,connections,status,requests_per_second,plot_x\n")
        for profile_index, (profile, _label, _color) in enumerate(PROFILES):
            for worker_index, workers in enumerate(WORKERS):
                group_offset = (
                    profile_index * len(WORKERS)
                    + worker_index
                    - (group_count - 1) / 2
                ) * box_width
                points = []

                for connection_index, connections in enumerate(CONNECTIONS, start=1):
                    x_value = connection_index + group_offset
                    key = (profile, workers, connections)
                    measurement = values.get(key)
                    status = "PASS" if is_finite_number(measurement) else statuses.get(
                        key, "MISSING"
                    )
                    display_value = measurement if is_finite_number(measurement) else "NaN"
                    csv_file.write(
                        f"{profile},{workers},{connections},{status},"
                        f"{display_value},{x_value:.4f}\n"
                    )
                    if is_finite_number(measurement):
                        points.append((x_value, measurement))

                series[(profile, workers)] = points

    plot_items = []
    for profile, label, color in PROFILES:
        for workers in WORKERS:
            points = series[(profile, workers)]
            if not points:
                continue

            data_file = output_dir / f"throughput-{profile}-workers-{workers}.dat"
            data_file.write_text(
                "".join(f"{x_value:.4f} {value:.12g}\n" for x_value, value in points)
            )
            fill = "fs solid 0.75" if workers == 1 else "fs pattern 2"
            legend = f"title {gnuplot_quote(label)}" if workers == 1 else "notitle"
            plot_items.append(
                f"{gnuplot_quote(data_file)} using 1:2 with boxes {fill} "
                f"lc rgb {gnuplot_quote(color)} lt 1 lw 0.4 {legend}"
            )

    return box_width, plot_items


def write_gnuplot_script(output_dir, box_width, plot_items, values):
    """Write a line-oriented gnuplot program so chart settings are easy to edit."""
    finite_values = [value for value in values.values() if is_finite_number(value)]
    maximum = max(finite_values)
    y_upper = max(100.0, maximum * 100.0)
    plot_clause = ", \\\n  ".join(plot_items)

    script = f"""\
set terminal pngcairo size 1200,720 enhanced font 'Helvetica,14' linewidth 3
set output {gnuplot_quote(output_dir / 'nginx-comparison.png')}
set xlabel 'Client connections'
set ylabel 'Requests per second'
set xtics ('4' 1, '16' 2, '64' 3)
set xrange [0.45:3.55]
set grid ytics
set logscale y
set yrange [1:{y_upper:.8g}]
set boxwidth {box_width * 0.9:.5f} absolute
set size 1.0,1.0
set key top left Left reverse font ',12'
set style fill pattern 2
set tics out
plot \\
  {plot_clause}
"""
    script_path = output_dir / "throughput-comparison.gnuplot"
    script_path.write_text(script)
    return script_path


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: plot-nginx-comparison.py AGGREGATE_JSON OUTPUT_DIR")

    aggregate_path = Path(sys.argv[1])
    output_dir = Path(sys.argv[2])
    output_dir.mkdir(parents=True, exist_ok=True)
    values, statuses = load_measurements(aggregate_path)

    box_width, plot_items = write_measurement_files(output_dir, values, statuses)
    status_path = output_dir / "plot-status.txt"
    if not plot_items:
        status_path.write_text("Plot omitted: no finite nginx measurements are available.\n")
        return

    script_path = write_gnuplot_script(output_dir, box_width, plot_items, values)
    subprocess.run(["gnuplot", str(script_path)], check=True)
    status_path.write_text(
        "Grouped throughput bars: client connections on x-axis; color is UML profile, "
        "solid fill is one worker and pattern fill is four workers. SMP=y means "
        "CONFIG_SMP=y; SMP=n means CONFIG_SMP=n. Missing measurements are omitted. "
        "The y-axis has additional headroom so the in-plot legend does not cover bars.\n"
    )
    print(f"Generated {output_dir / 'nginx-comparison.png'}")


if __name__ == "__main__":
    main()
