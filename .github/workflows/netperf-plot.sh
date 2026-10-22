#!/bin/bash

OUTPUT="$1"
python3 - "$OUTPUT" <<'PY'
import csv
import glob
import os
import sys

root = sys.argv[1]
profiles = (
    "native", "um-mmu", "um-mmu-seccomp", "um-nommu", "um-nommu-seccomp",
    "um-nommu-plain-sas", "um-nommu-sas-seccomp", "um-nommu-sas-zpoline",
)
payloads = (64, 128, 256, 512, 1024, 1500, 65507)
results = {}
out_dir = os.path.join(root, "out")
os.makedirs(out_dir, exist_ok=True)

source_paths = glob.glob(os.path.join(root, "*-iperf3.dat"))
source_paths += glob.glob(os.path.join(root, "*-netperf.dat"))
for path in source_paths:
    basename = os.path.basename(path)
    target = basename.removesuffix("-iperf3.dat").removesuffix("-netperf.dat")
    if target not in profiles:
        continue
    with open(path, encoding="utf-8", errors="replace", newline="") as stream:
        for row in csv.reader(stream):
            if len(row) < 4 or not row[1].endswith("bits/s"):
                continue
            command = row[3]
            test = next((name for name in ("TCP_STREAM", "TCP_MAERTS") if f"-t {name}" in command), None)
            if not test:
                continue
            try:
                size = int(row[2])
                throughput = float(row[0])
            except ValueError:
                continue
            results[(target, test, size)] = throughput

for target in profiles:
    for test, suffix in (("TCP_STREAM", "fwd"), ("TCP_MAERTS", "rev")):
        path = os.path.join(out_dir, f"{target}-netperf-{suffix}-out.dat")
        with open(path, "w", encoding="utf-8") as stream:
            for size in payloads:
                value = results.get((target, test, size))
                if value is not None:
                    stream.write(f"{size} {value}\n")

for test, heading in (("TCP_STREAM", "TCP_STREAM"), ("TCP_MAERTS", "TCP_MAERTS")):
    print(f"\n### netperf bench ({heading}) (Mbps)\n")
    print("| Payload bytes | " + " | ".join(profiles) + " |")
    print("|---:|" + "---:|" * len(profiles))
    for size in payloads:
        cells = []
        for profile in profiles:
            value = results.get((profile, test, size))
            cells.append(f"{value:.2f}" if value is not None else "N/A")
        print(f"| {size} | " + " | ".join(cells) + " |")
PY
