#!/usr/bin/env python3
"""Render separate nginx profile comparisons for each worker count."""

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
BAR_WIDTH = 0.105


def quote(value):
    """Quote a string for a gnuplot single-quoted literal."""
    return "'" + str(value).replace("\\", "\\\\").replace("'", "\\'") + "'"


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def load_measurements(source):
    rows = json.loads(source.read_text()).get("matrix", [])
    values = {}
    statuses = {}
    for row in rows:
        key = (row.get("profile"), row.get("workers"), row.get("connections"))
        statuses[key] = row.get("status", "MISSING")
        if row.get("status") == "PASS":
            values[key] = row.get("requests_per_second")
    return values, statuses


def write_csv(out_dir, values, statuses):
    path = out_dir / "throughput-comparison.csv"
    with path.open("w") as stream:
        stream.write(
            "profile,workers,connections,status,requests_per_second,plot_x\n"
        )
        for worker in WORKERS:
            for index, connection in enumerate(CONNECTIONS, start=1):
                for profile_index, (profile, _label, _color) in enumerate(PROFILES):
                    offset = (
                        profile_index - (len(PROFILES) - 1) / 2
                    ) * BAR_WIDTH
                    key = (profile, worker, connection)
                    value = values.get(key)
                    status = (
                        "PASS" if finite(value) else statuses.get(key, "MISSING")
                    )
                    display = value if finite(value) else "NaN"
                    stream.write(
                        f"{profile},{worker},{connection},{status},"
                        f"{display},{index + offset:.4f}\n"
                    )
    return path


def write_worker_plot(out_dir, worker, values, all_values):
    plot_items = []
    worker_values = [
        value
        for (profile, item_worker, _connection), value in values.items()
        if item_worker == worker and finite(value)
    ]
    if not worker_values:
        return None

    for profile_index, (profile, label, color) in enumerate(PROFILES):
        data_file = out_dir / f"throughput-{profile}-workers-{worker}.dat"
        offset = (profile_index - (len(PROFILES) - 1) / 2) * BAR_WIDTH
        lines = []
        has_measurement = False
        for index, connection in enumerate(CONNECTIONS, start=1):
            value = values.get((profile, worker, connection))
            has_measurement |= finite(value)
            lines.append(
                f"{index + offset:.4f} "
                f"{value:.12g}\n" if finite(value) else f"{index + offset:.4f} NaN\n"
            )
        data_file.write_text("".join(lines))
        if not has_measurement:
            continue
        plot_items.append(
            f"{quote(data_file)} using 1:2 with boxes "
            f"fs solid 0.72 fc rgb {quote(color)} "
            f"lc rgb {quote(color)} lw 0.8 "
            f"title {quote(label)}"
        )

    maximum = max(value for value in all_values if finite(value))
    upper = max(10.0, maximum * 2.5)
    image = out_dir / f"nginx-workers-{worker}.png"
    script = out_dir / f"nginx-workers-{worker}.gnuplot"
    commands = (
        "set terminal pngcairo size 1800,1000 enhanced font 'Sans,20' linewidth 2",
        f"set output {quote(image)}",
        "set xlabel 'Client connections' font 'Sans,22'",
        "set ylabel 'Requests per second' font 'Sans,22'",
        "set xtics ('4' 1, '16' 2, '64' 3) font 'Sans,20'",
        "set ytics font 'Sans,20'",
        "set xrange [0.5:3.5]",
        f"set yrange [1:{upper:.8g}]",
        "set logscale y",
        "set grid ytics",
        f"set boxwidth {BAR_WIDTH * 0.86:.5f} absolute",
        "set lmargin 11",
        "set rmargin 2",
        "set bmargin 7",
        "set tmargin 5",
        "set key top center horizontal maxrows 2 opaque font 'Sans,16'",
        "set style fill solid 0.72 border",
        "set tics out",
        "plot \\\n  " + ", \\\n  ".join(plot_items),
        "",
    )
    script.write_text("\n".join(commands))
    subprocess.run(["gnuplot", str(script)], check=True)
    return image


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: plot-nginx-comparison.py AGGREGATE_JSON OUTPUT_DIR"
        )

    source, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    values, statuses = load_measurements(source)
    write_csv(out_dir, values, statuses)

    all_values = list(values.values())
    generated = []
    for worker in WORKERS:
        image = write_worker_plot(out_dir, worker, values, all_values)
        if image:
            generated.append(image.name)

    status = out_dir / "plot-status.txt"
    if generated:
        status.write_text(
            "Nginx profile comparisons were generated separately for "
            "workers=1 and workers=4. Each plot groups UML profiles within "
            "client connection counts 4, 16, and 64. Both plots use the same "
            "logarithmic y-axis range; missing measurements are omitted. "
            "Profile names include whether CONFIG_SMP is enabled.\n"
        )
        for image in generated:
            print(f"Generated {out_dir / image}")
    else:
        status.write_text(
            "Plots omitted: no finite nginx measurements are available.\n"
        )
        print(status.read_text().strip())


if __name__ == "__main__":
    main()
