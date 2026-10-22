#!/usr/bin/env python3
"""Render UML benchmark charts from the normalized result snapshot."""

import argparse
import json
import math
import subprocess
from pathlib import Path


PROFILES = (
    ("native", "(native)", "#c0392b"),
    ("um-mmu", "um-mmu(p)", "#27ae60"),
    ("um-mmu-seccomp", "um-mmu(s)", "#145a32"),
    ("um-mmu-tt-seccomp", "um-mmu-tt(s)", "#196f3d"),
    ("um-nommu", "um-nommu(p)", "#d68910"),
    ("um-nommu-seccomp", "um-nommu(s)", "#a04000"),
    ("um-nommu-tt-seccomp", "um-nommu-tt(s)", "#873600"),
    ("um-nommu-plain-sas", "um-nommu-sas(s)", "#566573"),
    ("um-nommu-plain-sas-zpoline", "um-nommu-sas(z)", "#85929e"),
    ("um-nommu-sas-seccomp", "um-nommu-sas-swmmu(s)", "#0000ff"),
    ("um-nommu-sas-zpoline", "um-nommu-sas-swmmu(z)", "#4169e1"),
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
    ("bw_pipe_64k", "bw pipe (64 KiB)"),
    ("bw_mem_512k_rdwr", "bw mem (512 KiB)"),
)


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def gnuplot_quote(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "\\'") + "'"


def write_rows(path, values):
    path.write_text("".join(f"{value if finite(value) else 'NaN'}\n" for value in values))


def has_finite_measurement(files, *, paired=False):
    for path, _name, _color in files:
        for line in path.read_text().splitlines():
            fields = line.split()
            if paired:
                fields = fields[1:]
            for field in fields:
                try:
                    value = float(field)
                except ValueError:
                    continue
                if math.isfinite(value):
                    return True
    return False


def symmetric_plot_limit(files, *, paired=False, minimum=0.0):
    """Return a symmetric rounded range, honoring an optional lower bound."""
    peak = 0.0
    for path, _name, _color in files:
        for line in path.read_text().splitlines():
            fields = line.split()
            if paired:
                fields = fields[1:]
            for field in fields:
                try:
                    value = abs(float(field))
                except ValueError:
                    continue
                if math.isfinite(value):
                    peak = max(peak, value)
    if peak == 0:
        return minimum if minimum else 1.0

    magnitude = 10 ** math.floor(math.log10(peak))
    for factor in (1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        limit = factor * magnitude
        if peak <= limit:
            return max(minimum, limit)
    return max(minimum, 10 * magnitude)


def render_plot(out_dir, filename, ylabel, labels, files, *, logscale=False, paired=False):
    status_path = out_dir / f"{Path(filename).stem}-plot-status.txt"
    if not has_finite_measurement(files, paired=paired):
        status_path.write_text("Plot omitted: no finite measurements are available for this metric.\n")
        print(f"{filename}: omitted because all measurements are unavailable")
        return

    colors = [color for _target, _label, color in PROFILES]
    names = [label for _target, label, _color in PROFILES]
    group_width = 0.72 if paired else 0.9
    step = group_width / len(files)
    width = step * 0.62 if paired else step
    offsets = [(index - (len(files) - 1) / 2) * step for index in range(len(files))]
    xtics = ", ".join(f"{gnuplot_quote(label)} {position}" for label, position in labels)
    plot_items = []
    for index, (path, _name, color) in enumerate(files):
        xexpr = f"($1+({offsets[index]:.4f}))" if paired else f"($0+({offsets[index]:.4f}))"
        style = (
            f"with boxes fc rgb {gnuplot_quote(color)} fs pattern 2 "
            f"lt 1 lc rgb {gnuplot_quote(color)} lw 0.4"
        )
        if paired:
            # Direction pairs use separate columns at the same x coordinate;
            # their positive and negative bars share one profile legend.
            plot_items.append(
                f"{gnuplot_quote(path)} using {xexpr}:2 {style} "
                f"title {gnuplot_quote(names[index])}"
            )
            plot_items.append(
                f"{gnuplot_quote(path)} using {xexpr}:3 {style} notitle"
            )
        else:
            plot_items.append(
                f"{gnuplot_quote(path)} using {xexpr}:1 {style} "
                f"title {gnuplot_quote(names[index])}"
            )
    script = out_dir / f"{filename}.gnuplot"
    commands = [
        "set terminal pngcairo size 1200,720 enhanced font 'Helvetica,14' linewidth 3",
        f"set output {gnuplot_quote(out_dir / filename)}",
        "set pointsize 2",
        *([] if logscale else ["set xzeroaxis"]),
        (
            "set xlabel 'Payload size (bytes)'" if filename == "tcp-stream.png"
            else "set xlabel 'Benchmark'"
        ),
        f"set ylabel {gnuplot_quote(ylabel)}",
        "set grid ytics",
        "set style fill pattern 2",
        f"set boxwidth {width:.4f} absolute" if paired else f"set boxwidth {width:.4f}",
        "set size 1.0,1.0",
        'set key font ",12"',
        "set key top left Left reverse",
        f"set xtics ({xtics})" + (" rotate by 45 right" if filename in ("lmbench.png", "lmbench-bandwidth.png") else ""),
        f"set xrange [{-group_width / 2 - width / 2 - 0.1:.3f}:{len(labels) - 1 + group_width / 2 + width / 2 + 0.1:.3f}]",
    ]
    range_note = ""
    if logscale:
        commands.extend(["set logscale y", "set yrange [0.01:*]"])
    else:
        commands.append("unset logscale y")
        if paired:
            minimum = 50.0 if filename == "tcp-stream.png" else 0.0
            limit = symmetric_plot_limit(files, paired=True, minimum=minimum)
            commands.append(f"set yrange [-{limit:g}:{limit:g}]")
            if filename == "tcp-stream.png":
                native_available = has_finite_measurement(files[:1], paired=True)
                basis = "native plus other profiles" if native_available else "available non-native profiles (native was unavailable)"
                range_note = f" Symmetric y-range: +/- {limit:g} Gbps (50 Gbps minimum; expanded to contain the largest finite value in {basis})."
    commands.append("plot \\\n  " + ", \\\n  ".join(plot_items))
    script.write_text("\n".join(commands) + "\n")
    subprocess.run(["gnuplot", str(script)], check=True)
    status_path.write_text("Plot generated from available measurements." + range_note + "\n")


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
        forward = iperf_values[target]["forward"]
        reverse = iperf_values[target]["reverse"]
        path.write_text(
            f"0 {forward / 1000 if finite(forward) else 'NaN'} "
            f"{-reverse / 1000 if finite(reverse) else 'NaN'}\n"
        )
        iperf_files.append((path, label, color))
    render_plot(
        out_dir,
        "iperf3.png",
        "Goodput (Gbps)",
        [("iperf3", 0)],
        iperf_files,
        paired=True,
    )

    # Keep STREAM and MAERTS in separate columns on each payload row, so the
    # positive and negative bars for one profile share the same x coordinate.
    interleaved = []
    for target, label, color in PROFILES:
        path = out_dir / f"netperf-{target}.dat"
        path.write_text("".join(
            f"{index} "
            f"{netperf_values[target]['TCP_STREAM'][size] / 1000 if finite(netperf_values[target]['TCP_STREAM'][size]) else 'NaN'} "
            f"{-netperf_values[target]['TCP_MAERTS'][size] / 1000 if finite(netperf_values[target]['TCP_MAERTS'][size]) else 'NaN'}\n"
            for index, size in enumerate(PAYLOADS)
        ))
        interleaved.append((path, label, color))
    render_plot(
        out_dir,
        "tcp-stream.png",
        "Goodput (Gbps)",
        [(str(size), index) for index, size in enumerate(PAYLOADS)],
        interleaved,
        paired=True,
    )


if __name__ == "__main__":
    main()
