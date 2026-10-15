#!/usr/bin/env python3
"""Plot nginx throughput by UML profile, CPU count, and worker count."""

import csv
import json
import math
import subprocess
import sys
from pathlib import Path

PROFILES = (
    ("native", "native", "#c0392b"),
    ("um-mmu", "um", "#27ae60"),
    ("um-mmu-seccomp", "um-mmu(s)", "#145a32"),
    ("um-nommu", "um-nommu", "#d68910"),
    ("um-nommu-seccomp", "um-nommu(s)", "#a04000"),
    ("um-nommu-sas-seccomp", "sas-s", "#0000ff"),
    ("um-nommu-sas-zpoline", "sas-z", "#4169e1"),
)
WORKERS = (1, 4)
CONNECTIONS = (4, 16, 64)


def quote(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "\\'") + "'"


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def load_measurements(source):
    rows = json.loads(source.read_text()).get("matrix", [])
    values = {}
    statuses = {}
    host_cpu_counts = {}
    for row in rows:
        key = (row.get("profile"), int(row.get("ncpus", 1)),
               int(row.get("workers", 0)), int(row.get("connections", 0)))
        statuses[key] = row.get("status", "MISSING")
        if row.get("profile") == "native" and isinstance(row.get("host_cpu_count"), int):
            host_cpu_counts[row["profile"]] = row["host_cpu_count"]
        if row.get("status") == "PASS":
            values[key] = row.get("requests_per_second")
    return values, statuses, host_cpu_counts


def series_for(worker, statuses, host_cpu_counts):
    series = []
    for profile, label, color in PROFILES:
        smp = "SMP=y" if profile.startswith("um-mmu") else "SMP=n"
        cpu_counts = sorted({key[1] for key in statuses
                             if key[0] == profile and key[2] == worker})
        for cpus in cpu_counts:
            if profile == "native":
                host_cpus = host_cpu_counts.get(profile, "unknown")
                legend = f"native [host: {host_cpus} CPUs]"
            else:
                legend = f"{label} [{smp}, {cpus} CPU{'s' if cpus != 1 else ''}]"
            series.append((profile, cpus, legend, color))
    return series


def write_csv(out_dir, values, statuses):
    path = out_dir / "throughput-comparison.csv"
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("profile", "ncpus", "workers", "connections", "status", "requests_per_second"))
        for key in sorted(statuses):
            value = values.get(key)
            writer.writerow((*key[:1], key[1], key[2], key[3],
                             "PASS" if finite(value) else statuses[key],
                             value if finite(value) else "NaN"))
    return path


def write_worker_plot(out_dir, worker, values, statuses, host_cpu_counts, all_values):
    series = series_for(worker, statuses, host_cpu_counts)
    if not series:
        return None
    width = min(0.09, 0.72 / len(series))
    plot_items = []
    for index, (profile, cpus, label, color) in enumerate(series):
        offset = (index - (len(series) - 1) / 2) * width
        data_file = out_dir / f"throughput-{profile}-{cpus}cpu-workers-{worker}.dat"
        lines = []
        has_measurement = False
        for x_index, connections in enumerate(CONNECTIONS, start=1):
            value = values.get((profile, cpus, worker, connections))
            has_measurement |= finite(value)
            x = x_index + offset
            lines.append(f"{x:.5f} {value:.12g}\n" if finite(value) else f"{x:.5f} NaN\n")
        data_file.write_text("".join(lines))
        if not has_measurement:
            continue
        fill = f"fs solid 0.70" if cpus == 1 else "fs pattern 4"
        plot_items.append(
            f"{quote(data_file)} using 1:2 with boxes {fill} "
            f"fc rgb {quote(color)} lc rgb {quote(color)} lw 0.7 title {quote(label)}"
        )

    if not plot_items:
        return None
    maximum = max(value for value in all_values if finite(value))
    upper = max(10.0, maximum * 2.5)
    image = out_dir / f"nginx-workers-{worker}.png"
    script = out_dir / f"nginx-workers-{worker}.gnuplot"
    commands = (
        "set terminal pngcairo size 2000,1100 enhanced font 'Sans,20' linewidth 2",
        f"set output {quote(image)}",
        "set xlabel 'Client connections' font 'Sans,22'",
        "set ylabel 'Requests per second' font 'Sans,22'",
        "set xtics ('4' 1, '16' 2, '64' 3) font 'Sans,20'",
        "set ytics font 'Sans,20'",
        "set xrange [0.5:3.5]",
        f"set yrange [1:{upper:.8g}]",
        "set logscale y",
        "set grid ytics",
        f"set boxwidth {width * 0.86:.5f} absolute",
        "set lmargin 11",
        "set rmargin 2",
        "set bmargin 7",
        "set tmargin 7",
        "set key top center horizontal maxrows 3 opaque font 'Sans,15'",
        "set tics out",
        "plot \\\n  " + ", \\\n  ".join(plot_items),
        "",
    )
    script.write_text("\n".join(commands))
    subprocess.run(["gnuplot", str(script)], check=True)
    return image


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: plot-nginx-comparison.py AGGREGATE_JSON OUTPUT_DIR")
    source, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    values, statuses, host_cpu_counts = load_measurements(source)
    write_csv(out_dir, values, statuses)
    all_values = list(values.values())
    generated = []
    for worker in WORKERS:
        image = write_worker_plot(out_dir, worker, values, statuses, host_cpu_counts, all_values)
        if image:
            generated.append(image.name)
    status = out_dir / "plot-status.txt"
    if generated:
        status.write_text(
            "Separate charts compare workers=1 and workers=4. The x-axis is client "
            "connections. MMU profiles use CONFIG_SMP=y; um-mmu uses one online "
            "CPU, while um-mmu-seccomp compares one and four CPUs (solid versus "
            "patterned fill). NOMMU profiles use CONFIG_SMP=n and currently show "
            "one CPU. Add their four-CPU matrix entries when NOMMU SMP is "
            "supported. Native is measured on the runner and its host CPU count "
            "is recorded in the legend. "
            "Profile colors are consistent between both charts.\n"
        )
        for image in generated:
            print(f"Generated {out_dir / image}")
    else:
        status.write_text("Plots omitted: no finite nginx measurements are available.\n")
        print(status.read_text().strip())


if __name__ == "__main__":
    main()
