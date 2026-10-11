#!/usr/bin/env python3
"""Render UML benchmark charts from the normalized result snapshot."""

import argparse
import json
import math
import subprocess
from pathlib import Path


PROFILES = (
    ("native", "native", "#c0392b"),
    ("um-mmu", "um", "#27ae60"),
    ("um-mmu-seccomp", "um-mmu(s)", "#145a32"),
    ("um-nommu", "um-nommu", "#d68910"),
    ("um-nommu-seccomp", "um-nommu(s)", "#a04000"),
    ("um-nommu-sas-seccomp", "um-nommu(sas-s)", "#2874a6"),
    ("um-nommu-sas-zpoline", "um-nommu(sas-z)", "#1f618d"),
)
LATENCY_ROWS = (
    ("select-10", "select-10"),
    ("select-100", "select-100"),
    ("select-1000", "select-1000"),
    ("syscall", "syscall"),
    ("read", "read"),
    ("write", "write"),
    ("stat", "stat"),
    ("open-close", "open/close"),
    ("fork-exit", "fork+exit"),
    ("fork-execve", "fork+execve"),
    ("fork-shell", "fork+sh"),
    ("vfork-exit", "vfork+exit"),
    ("vfork-execve", "vfork+execve"),
    ("vfork-shell", "vfork+sh"),
)
PAYLOADS = (64, 128, 256, 512, 1024, 1500, 65507)
BANDWIDTH_ROWS = (
    ("bw_pipe_64k", "bw_pipe_64k"),
    ("bw_mem_512k_rdwr", "bw_mem_512k_rdwr"),
)


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def gnuplot_quote(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "\\'") + "'"


def write_rows(path, values):
    path.write_text("".join(f"{value if finite(value) else 'NaN'}\n" for value in values))


def has_finite_measurement(files):
    for path, _name, _color in files:
        for line in path.read_text().splitlines():
            try:
                value = float(line)
            except ValueError:
                continue
            if math.isfinite(value):
                return True
    return False


def render_plot(out_dir, filename, title, ylabel, labels, files, *, logscale=False, netperf=False):
    status_path = out_dir / f"{Path(filename).stem}-plot-status.txt"
    if not has_finite_measurement(files):
        status_path.write_text("Plot omitted: no finite measurements are available for this metric.\n")
        print(f"{filename}: omitted because all measurements are unavailable")
        return

    colors = [color for _target, _label, color in PROFILES]
    names = [label for _target, label, _color in PROFILES]
    width = 0.72 / len(PROFILES)
    offsets = [(index - (len(PROFILES) - 1) / 2) * width for index in range(len(PROFILES))]
    xtics = ", ".join(f"{gnuplot_quote(label)} {position}" for label, position in labels)
    plot_items = []
    for index, (path, _name, color) in enumerate(files):
        if netperf:
            xexpr = f"($0*1+({offsets[index]:.4f}))"
        else:
            xexpr = f"($0+({offsets[index]:.4f}))"
        plot_items.append(
            f"{gnuplot_quote(path)} using {xexpr}:1 with boxes "
            f"lc rgb {gnuplot_quote(color)} title {gnuplot_quote(names[index])}"
        )
    script = out_dir / f"{filename}.gnuplot"
    commands = [
        "set terminal pngcairo size 2000,1200 enhanced font 'Sans,18'",
        f"set output {gnuplot_quote(out_dir / filename)}",
        f"set title {gnuplot_quote(title)}",
        "set title font 'Sans,24'",
        "set xlabel font 'Sans,20'",
        f"set ylabel {gnuplot_quote(ylabel)}",
        "set ylabel font 'Sans,20'",
        "set grid ytics",
        "set style fill solid 0.75 border -1",
        f"set boxwidth {width * 0.9:.4f}",
        "set key outside top center horizontal font 'Sans,20'",
        f"set xtics ({xtics}) rotate by 35 right font 'Sans,20'",
        "set ytics font 'Sans,20'",
        f"set xrange [-0.8:{(2 * len(labels) if netperf else len(labels)) - 0.2}]",
        "set auto y",
    ]
    if logscale:
        commands.extend(["set logscale y", "set yrange [0.01:*]"])
    else:
        commands.append("unset logscale y")
    if netperf:
        commands.append('set label 1 "TCP_MAERTS bars are negative" at graph 0.01,0.98')
    commands.append("plot \\\n  " + ", \\\n  ".join(plot_items))
    script.write_text("\n".join(commands) + "\n")
    subprocess.run(["gnuplot", str(script)], check=True)
    status_path.write_text("Plot generated from available measurements.\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    snapshot = json.loads(args.snapshot.read_text())
    out_dir = args.output
    out_dir.mkdir(parents=True, exist_ok=True)

    lmbench = snapshot.get("lmbench_table", {})
    lmbench_files = []
    for target, label, _color in PROFILES:
        path = out_dir / f"lmbench-{target}.dat"
        values = [lmbench.get(target, {}).get(operation) for operation, _name in LATENCY_ROWS]
        write_rows(path, values)
        lmbench_files.append((path, label, _color))
    render_plot(
        out_dir,
        "lmbench.png",
        "lmbench latency",
        "Latency (usec)",
        [(label, index) for index, (_operation, label) in enumerate(LATENCY_ROWS)],
        lmbench_files,
        logscale=True,
    )

    bandwidth_files = []
    for target, label, color in PROFILES:
        path = out_dir / f"lmbench-bandwidth-{target}.dat"
        values = [lmbench.get(target, {}).get(operation) for operation, _name in BANDWIDTH_ROWS]
        write_rows(path, values)
        bandwidth_files.append((path, label, color))
    render_plot(
        out_dir,
        "lmbench-bandwidth.png",
        "lmbench bandwidth",
        "Bandwidth (MB/sec)",
        [(label, index) for index, (_operation, label) in enumerate(BANDWIDTH_ROWS)],
        bandwidth_files,
    )

    iperf_values = {target: {"forward": None, "reverse": None} for target, _label, _color in PROFILES}
    netperf_values = {
        target: {test: {size: None for size in PAYLOADS} for test in ("TCP_STREAM", "TCP_MAERTS")}
        for target, _label, _color in PROFILES
    }
    for metric in snapshot.get("benchmark", []):
        target = metric.get("target")
        if target not in iperf_values:
            continue
        labels = metric.get("labels", {})
        if metric.get("name") == "iperf3":
            iperf_values[target][labels.get("direction")] = metric.get("value")
        elif metric.get("name") == "netperf":
            test = labels.get("test")
            size = labels.get("payload_bytes")
            if test in netperf_values[target] and size in netperf_values[target][test]:
                netperf_values[target][test][size] = metric.get("value")

    iperf_files = []
    netperf_forward_files = []
    netperf_reverse_files = []
    for target, label, color in PROFILES:
        path = out_dir / f"iperf3-{target}.dat"
        write_rows(path, [
            iperf_values[target][key] / 1000
            if finite(iperf_values[target][key]) else None
            for key in ("forward", "reverse")
        ])
        iperf_files.append((path, label, color))
    render_plot(
        out_dir,
        "iperf3.png",
        "iperf3 throughput",
        "Goodput (Gbps)",
        [("forward", 0), ("reverse", 1)],
        iperf_files,
    )

    # Interleave forward and reverse values so each payload is shown as a
    # positive/negative pair with a single payload-size tick.
    interleaved = []
    for target, label, color in PROFILES:
        path = out_dir / f"netperf-{target}.dat"
        write_rows(path, [
            value
            for size in PAYLOADS
            for value in (
                netperf_values[target]["TCP_STREAM"][size] / 1000
                if finite(netperf_values[target]["TCP_STREAM"][size]) else None,
                -netperf_values[target]["TCP_MAERTS"][size] / 1000
                if finite(netperf_values[target]["TCP_MAERTS"][size]) else None,
            )
        ])
        interleaved.append((path, label, color))
    render_plot(
        out_dir,
        "tcp-stream.png",
        "netperf TCP throughput",
        "Goodput (Gbps; TCP_MAERTS shown negative)",
        [(str(size), 2 * index + 0.5) for index, size in enumerate(PAYLOADS)],
        interleaved,
        netperf=True,
    )


if __name__ == "__main__":
    main()
