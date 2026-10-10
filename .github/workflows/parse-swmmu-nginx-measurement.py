#!/usr/bin/env python3
"""Turn one raw wrk run and its smoke checks into a strict JSON result."""

import argparse
import json
import re
from pathlib import Path


def latency_ms(value: str) -> float:
    match = re.fullmatch(r"([0-9.]+)(ns|us|µs|ms|s)", value.strip())
    if not match:
        raise ValueError(f"unrecognized latency value: {value!r}")
    amount = float(match.group(1))
    scale = {"ns": 1e-6, "us": 1e-3, "µs": 1e-3, "ms": 1.0, "s": 1000.0}
    return amount * scale[match.group(2)]


def read_text(path: Path) -> str:
    try:
        return path.read_text(errors="replace").strip()
    except OSError:
        return ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--workers", required=True, type=int)
    parser.add_argument("--threads", required=True, type=int)
    parser.add_argument("--connections", required=True, type=int)
    parser.add_argument("--duration", required=True, type=int)
    parser.add_argument("--kernel-sha", required=True)
    parser.add_argument("--aports-sha", required=True)
    parser.add_argument("--gcc-version", required=True)
    args = parser.parse_args()

    raw = read_text(args.output / "wrk.log")
    failures = []
    harness_status = read_text(args.output / "harness.status")
    if harness_status != "PASS":
        reason = read_text(args.output / "harness.error") or "benchmark harness did not complete"
        failures.append(reason)

    for label in ("http", "tls"):
        if read_text(args.output / f"smoke-{label}.status") != "PASS":
            failures.append(f"{label.upper()} smoke test failed")

    try:
        wrk_exit = int(read_text(args.output / "wrk.exit-code"))
    except ValueError:
        wrk_exit = None
    if wrk_exit != 0:
        failures.append(f"wrk exit status is {wrk_exit!r}")

    requests = re.search(r"Requests/sec:\s*([0-9.]+)", raw)
    latency = re.search(r"^\s*Latency\s+([0-9.]+(?:ns|us|µs|ms|s))", raw, re.MULTILINE)
    socket_errors = re.search(
        r"Socket errors:\s*connect\s+(\d+),\s*read\s+(\d+),\s*write\s+(\d+),\s*timeout\s+(\d+)",
        raw,
        re.IGNORECASE,
    )
    if not requests:
        failures.append("wrk Requests/sec measurement is missing")
    elif float(requests.group(1)) <= 0:
        failures.append("wrk Requests/sec measurement is not positive")
    if not latency:
        failures.append("wrk latency measurement is missing")
    if not socket_errors:
        # wrk only prints this line when it saw at least one socket error.
        # A successful run with a positive request count and no line therefore
        # means every socket error counter is zero.
        if wrk_exit == 0 and requests and float(requests.group(1)) > 0:
            socket_counts = [0, 0, 0, 0]
        else:
            failures.append("wrk socket error summary is missing")
            socket_counts = None
    else:
        socket_counts = [int(value) for value in socket_errors.groups()]
        if any(socket_counts):
            failures.append(f"wrk reported socket errors {socket_counts}")

    measured_latency = None
    if latency:
        try:
            measured_latency = latency_ms(latency.group(1))
        except ValueError as exc:
            failures.append(str(exc))

    result = {
        "schema_version": 1,
        "status": "FAIL" if failures else "PASS",
        "failures": failures,
        "workers": args.workers,
        "wrk_threads": args.threads,
        "connections": args.connections,
        "duration_seconds": args.duration,
        "wrk_timeout_seconds": 30,
        "kernel_sha": args.kernel_sha,
        "aports_sha": args.aports_sha,
        "gcc_version": args.gcc_version,
        "requests_per_second": float(requests.group(1)) if requests else None,
        "average_latency_ms": measured_latency,
        "socket_errors": socket_counts,
        "smoke_http": read_text(args.output / "smoke-http.status") == "PASS",
        "smoke_tls": read_text(args.output / "smoke-tls.status") == "PASS",
        "raw_logs": [
            "capability.log",
            "host-sysctl.log",
            "tap.log",
            "kernel.log",
            "nginx.log",
            "client-setup.log",
            "client-container.log",
            "smoke-http.log",
            "smoke-tls.log",
            "wrk-warmup.log",
            "wrk.log",
            "nginx-swmmu.conf",
        ],
    }
    (args.output / "measurement.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps(result, sort_keys=True))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
