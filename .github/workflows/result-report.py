#!/usr/bin/env python3
"""Collect and compare benchmark and test results for the UML CI report."""

import argparse
import base64
import gzip
import json
import math
import re
from pathlib import Path


LM_BENCHMARKS = [
    "select-10", "select-100", "select-1000", "syscall", "read", "write",
    "stat", "open-close", "fork-shell", "fork-execve",
]
LTP_COUNTS = ("passed", "failed", "broken", "skipped", "warnings")
KUNIT_COUNTS = ("passed", "failed", "crashed", "skipped", "errors")
KSELFTEST_COUNTS = ("passed", "failed", "skipped")
LTP_TARGETS = (
    "native", "um-mmu", "um-mmu-seccomp", "um-nommu", "um-nommu-seccomp",
    "um-nommu-sas-seccomp", "um-nommu-sas-zpoline", "riscv-nommu",
)
LTP_CRASH_SIGNATURES = (
    ("kernel panic", re.compile(r"Kernel panic(?:\s+-\s+not syncing)?", re.IGNORECASE)),
    ("SIGSEGV", re.compile(r"\bsegfault at\b|^\s*Segmentation fault\b", re.IGNORECASE | re.MULTILINE)),
    ("SIGABRT", re.compile(r"\bAborted \(core dumped\)|\bSIGABRT\b", re.IGNORECASE)),
)
LMBENCH_COLUMNS = (
    ("native", "native"),
    ("um-mmu", "um"),
    ("um-mmu-seccomp", "um-mmu(s)"),
    ("um-nommu", "um-nommu"),
    ("um-nommu-seccomp", "um-nommu(s)"),
    ("um-nommu-sas-seccomp", "um-nommu(sas-s)"),
    ("um-nommu-sas-zpoline", "um-nommu(sas-z)"),
)
LMBENCH_TABLE_ROWS = (
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
    ("bw_pipe_64k", "bw_pipe_64k (MB/sec)"),
    ("bw_mem_512k_rdwr", "bw_mem_512k_rdwr (MB/sec)"),
)


def load_json(path):
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def numbers(path):
    values = []
    for line in path.read_text(errors="replace").splitlines():
        fields = line.replace(",", " ").split()
        row = []
        for field in fields:
            try:
                value = float(field)
            except ValueError:
                continue
            if math.isfinite(value):
                row.append(value)
        if row:
            values.append(row)
    return values


def collect_benchmarks(root):
    """Normalize legacy benchmark output files into stable metric records."""
    records = []
    raw_lmbench = {}

    number = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
    simple_operations = {
        "syscall": "syscall",
        "read": "read",
        "write": "write",
        "stat": "stat",
        "open/close": "open-close",
    }
    select_re = re.compile(rf"Select on (\d+) fd's: ({number}) microseconds")
    simple_re = re.compile(rf"Simple (syscall|read|write|stat|open/close): ({number}) microseconds")
    process_re = re.compile(rf"Process fork\+([^:]+): ({number}) microseconds")
    pipe_re = re.compile(rf"Pipe bandwidth: ({number}) MB/sec")
    memory_re = re.compile(rf"^\s*0\.524288\s+({number})\s*$")
    for path in sorted(root.glob("*.dat")):
        target = path.stem
        method = None
        parsed = []
        for line in path.read_text(errors="replace").splitlines():
            method_match = re.search(r"lmbench process method: (fork|vfork)", line)
            if method_match:
                method = method_match.group(1)
                continue
            match = select_re.search(line)
            if match:
                parsed.append(("lmbench", float(match.group(2)), "usec", {"operation": f"select-{match.group(1)}"}))
                continue
            match = simple_re.search(line)
            if match:
                parsed.append(("lmbench", float(match.group(2)), "usec", {"operation": simple_operations[match.group(1)]}))
                continue
            match = process_re.search(line)
            if match:
                operation = match.group(1)
                operation = "shell" if operation == "/bin/sh -c" else operation
                process_method = method or "fork"
                parsed.append(("lmbench", float(match.group(2)), "usec", {"operation": f"{process_method}-{operation}"}))
                continue
            match = pipe_re.search(line)
            if match:
                parsed.append(("lmbench", float(match.group(1)), "MB/sec", {"operation": "bw_pipe_64k"}))
                continue
            match = memory_re.match(line)
            if match:
                parsed.append(("lmbench", float(match.group(1)), "MB/sec", {"operation": "bw_mem_512k_rdwr"}))
        if parsed:
            raw_lmbench[target] = parsed

    def add(name, target, value, unit, labels=None):
        records.append({
            "name": name,
            "target": target,
            "value": float(value),
            "unit": unit,
            "labels": labels or {},
        })

    for path in sorted(root.glob("**/*-out.dat")):
        stem = path.stem
        rows = numbers(path)
        if not rows:
            if not stem.endswith("-getpid-out"):
                continue
        if stem.endswith("-lmbench-out"):
            target = stem.removesuffix("-lmbench-out")
            if target in raw_lmbench:
                continue
            if len(rows) == 1 and len(rows[0]) == len(LM_BENCHMARKS):
                rows = [[value] for value in rows[0]]
            for index, row in enumerate(rows):
                label = LM_BENCHMARKS[index] if index < len(LM_BENCHMARKS) else f"metric-{index + 1}"
                add("lmbench", target, row[-1], "usec", {"operation": label})
        elif stem.endswith("-getpid-out"):
            target = stem.removesuffix("-getpid-out")
            text = path.read_text(errors="replace")
            value = re.search(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?", text)
            if value:
                add("getpid", target, float(value.group()), "nsec")
            elif rows:
                add("getpid", target, rows[-1][-1], "nsec")
        elif stem.endswith("-iperf3-out"):
            target = stem.removesuffix("-iperf3-out")
            labels = ("forward", "reverse")
            if len(rows) == 1 and len(rows[0]) == len(labels):
                rows = [[value] for value in rows[0]]
            for index, row in enumerate(rows):
                label = labels[index] if index < len(labels) else f"direction-{index + 1}"
                add("iperf3", target, row[-1], "Mbps", {"direction": label})
        elif stem.endswith("-netperf-fwd-out") or stem.endswith("-netperf-rev-out"):
            reverse = stem.endswith("-netperf-rev-out")
            suffix = "-netperf-rev-out" if reverse else "-netperf-fwd-out"
            target = stem.removesuffix(suffix)
            direction = "TCP_MAERTS" if reverse else "TCP_STREAM"
            for row in rows:
                if len(row) >= 2:
                    size = int(row[0])
                    add("netperf", target, row[1], "Mbps", {"test": direction, "payload_bytes": size})
    for target, metrics in raw_lmbench.items():
        for name, value, unit, labels in metrics:
            add(name, target, value, unit, labels)
    return records


def collect_lmbench_table(root, benchmark_records):
    """Build the fixed comparison table, preserving unavailable and failed states."""
    values = {}
    for item in benchmark_records:
        if item.get("name") != "lmbench":
            continue
        operation = item.get("labels", {}).get("operation")
        if operation:
            values.setdefault(item["target"], {})[operation] = item["value"]

    methods_by_target = {}
    statuses_by_target = {}
    for path in root.glob("*.dat"):
        target = path.stem
        methods = set()
        statuses = {}
        text = path.read_text(errors="replace")
        for line in text.splitlines():
            match = re.search(r"lmbench process methods: (fork|vfork)(?:\s+(fork|vfork))?", line)
            if match:
                methods.update(value for value in match.groups() if value)
            match = re.search(r"lmbench process method: (fork|vfork)", line)
            if match:
                methods.add(match.group(1))
            match = re.search(r"lmbench measurement status: ([a-z0-9_-]+) (PASS|FAIL)", line)
            if match:
                statuses[match.group(1)] = match.group(2)
        methods_by_target[target] = methods or {"fork", "vfork"}
        statuses_by_target[target] = statuses

    table = {}
    for target, _header in LMBENCH_COLUMNS:
        target_values = values.get(target, {})
        methods = methods_by_target.get(target, {"fork", "vfork"})
        statuses = statuses_by_target.get(target, {})
        cells = {}
        for operation, _label in LMBENCH_TABLE_ROWS:
            method = "fork" if operation.startswith("fork-") else "vfork" if operation.startswith("vfork-") else None
            if method and method not in methods:
                cells[operation] = "N/A"
                continue
            status_key = operation
            status = statuses.get(status_key)
            if status == "FAIL" or operation not in target_values:
                cells[operation] = "FAIL"
            else:
                cells[operation] = target_values[operation]
        table[target] = cells
    return table


def metric_index(records):
    """Build stable comparison keys from extensible benchmark metric records."""
    if isinstance(records, dict):  # Read snapshots produced by schema v1.
        return records
    metrics = {}
    for metric in records:
        labels = "/".join(f"{key}={value}" for key, value in sorted(metric.get("labels", {}).items()))
        key = "/".join(part for part in (
            metric.get("name", "metric"), metric.get("target", "unknown"), labels,
        ) if part)
        unit = metric.get("unit", "")
        metrics[f"{key} ({unit})" if unit else key] = metric["value"]
    return metrics


def flatten_kunit(node, prefix=""):
    cases = {}
    group = node.get("name", "suite")
    path = f"{prefix}/{group}" if prefix else group
    for case in node.get("test_cases", []):
        cases[f"{path}/{case.get('name', '<unnamed>')}"] = case.get("status", "UNKNOWN")
    for child in node.get("sub_groups", []):
        cases.update(flatten_kunit(child, path))
    return cases


def collect_kunit(root):
    suites = {}
    for path in sorted(root.rglob("kunit-*.json")):
        data = load_json(path)
        if not isinstance(data, dict):
            continue
        artifact_dir = next((parent for parent in path.parents if parent.name.startswith("kunit-results-")), None)
        target = artifact_dir.name.removeprefix("kunit-results-") if artifact_dir else "unknown-target"
        suite = path.stem.removeprefix("kunit-")
        misc = data.get("misc", {})
        suites[f"{target}/{suite}"] = {
            "state": "collected",
            "counts": {key: int(misc.get(key, 0)) for key in KUNIT_COUNTS},
            "cases": flatten_kunit(data),
        }
    return {"state": "collected" if suites else "not-collected", "suites": suites}


def collect_ltp(root):
    results = {}
    report_urls = {}
    for path in root.rglob("*-url.txt"):
        target = path.stem.removesuffix("-url")
        match = re.search(r"https?://\S+", path.read_text(errors="replace"))
        if match:
            report_urls[target] = match.group(0)
    for path in sorted(root.rglob("result-*.json")):
        data = load_json(path)
        if not isinstance(data, dict):
            continue
        target = path.stem.removeprefix("result-")
        cases = {}
        for item in data.get("results", []):
            name = item.get("test_fqn")
            if name:
                cases[name] = item.get("status", "unknown")
        stats = data.get("stats", {})
        results[target] = {
            "state": "complete",
            "url": report_urls.get(target, ""),
            "counts": {key: int(stats.get(key, 0)) for key in LTP_COUNTS},
            "cases": cases,
        }
    # Every LTP artifact is uploaded with if: always(), including partial logs
    # from a panic. Preserve crash signatures even if Kirk could not write JSON.
    for path in root.rglob("*-ltp.dat"):
        target = path.stem.removesuffix("-ltp")
        result = results.setdefault(target, {
            "state": "incomplete (no result JSON)",
            "url": report_urls.get(target, ""),
            "counts": {key: 0 for key in LTP_COUNTS},
            "cases": {},
        })
        log = path.read_text(errors="replace")
        result["alerts"] = [
            label for label, pattern in LTP_CRASH_SIGNATURES
            if pattern.search(log)
        ]
    for target in LTP_TARGETS:
        results.setdefault(target, {
            "state": "no result artifact",
            "url": report_urls.get(target, ""),
            "counts": {key: 0 for key in LTP_COUNTS},
            "cases": {},
        })
    return {"state": "collected" if results else "not-collected", "targets": results}


def collect_kselftest(root):
    cases = {}
    artifact_root = root / "kselftest-artifact"
    if not artifact_root.exists():
        artifact_root = root / "kselftest"
    tap_paths = sorted(artifact_root.rglob("*.tap")) if artifact_root.exists() else []
    totals = None
    selected_tap = None
    for path in tap_paths:
        text = path.read_text(errors="replace")
        summary = re.search(
            r"^# Totals: pass:(\d+) fail:(\d+).*?skip:(\d+).*?error:(\d+)",
            text,
            re.MULTILINE,
        )
        if summary:
            totals = {
                "passed": int(summary.group(1)),
                "failed": int(summary.group(2)) + int(summary.group(4)),
                "skipped": int(summary.group(3)),
            }
            selected_tap = path
            break

    if selected_tap is None and tap_paths:
        selected_tap = tap_paths[0]
    if selected_tap:
        for line in selected_tap.read_text(errors="replace").splitlines():
            match = re.match(r"^\s*(not )?ok\s+(\d+)(?:\s+(.*?))?\s*$", line)
            if not match:
                continue
            description = match.group(3) or ""
            skipped = re.search(r"#\s*SKIP(?:\s+(.*))?", description)
            name = (skipped.group(1) if skipped else description).strip()
            name = re.sub(r"^[- ]+|\s+#.*$", "", name).strip()
            status = "FAIL" if match.group(1) else "PASS"
            if skipped:
                status = "SKIP"
            cases[match.group(2) + (f" {name}" if name else "")] = status

    if totals is None:
        totals = {
            "passed": sum(status == "PASS" for status in cases.values()),
            "failed": sum(status == "FAIL" for status in cases.values()),
            "skipped": sum(status == "SKIP" for status in cases.values()),
        }

    exit_codes = []
    if artifact_root.exists():
        exit_paths = list(artifact_root.rglob("*.exit")) + list(artifact_root.rglob("uml-exit.txt"))
        for path in exit_paths:
            match = re.search(r"(?:uml_exit=)?(-?\d+)", path.read_text(errors="replace"))
            if match:
                exit_codes.append(int(match.group(1)))
    state = "not-collected"
    if selected_tap:
        if totals["failed"] or any(code != 0 for code in exit_codes):
            state = "failed"
        elif exit_codes:
            state = "passed"
        else:
            state = "incomplete (missing exit status)"
    return {"state": state, "counts": totals, "cases": cases}


def collect_nginx(root):
    path = root / "nginx" / "aggregate.json"
    data = load_json(path)
    if not isinstance(data, dict):
        return {"state": "not run/no results", "matrix": []}
    matrix = data.get("matrix")
    if not isinstance(matrix, list):
        return {"state": "invalid aggregate", "matrix": []}
    return {"state": "collected", "matrix": matrix}


def collect(root, run_url, sha, tested_ref, job_status):
    benchmarks = collect_benchmarks(root)
    return {
        "version": 2,
        "run_url": run_url,
        "workflow_sha": sha,
        "tested_ref": tested_ref,
        "job_status": job_status,
        "benchmark": benchmarks,
        "lmbench_table": collect_lmbench_table(root, benchmarks),
        "kunit": collect_kunit(root),
        "kselftest": collect_kselftest(root),
        "nginx": collect_nginx(root),
        "ltp": collect_ltp(root),
    }


def fmt_value(value):
    return f"{value:.6g}"


def pct_change(old, new):
    if old == 0:
        return "n/a" if new == 0 else "new from 0"
    return f"{((new - old) / abs(old)) * 100:+.2f}%"


def compare_metrics(old, new):
    changed = []
    old = old or {}
    new = new or {}
    for key in sorted(set(old) | set(new)):
        before = old.get(key)
        after = new.get(key)
        if before is None:
            changed.append((key, "new", fmt_value(after), ""))
        elif after is None:
            changed.append((key, fmt_value(before), "missing", ""))
        elif not math.isclose(float(before), float(after), rel_tol=1e-9, abs_tol=1e-9):
            changed.append((key, fmt_value(before), fmt_value(after), pct_change(float(before), float(after))))
    return changed


def compare_cases(old, new):
    old = old or {}
    new = new or {}
    return [(name, "added", "—", new[name]) for name in sorted(set(new) - set(old))] + \
        [(name, "removed", old[name], "—") for name in sorted(set(old) - set(new))] + \
        [(name, "changed", old[name], new[name]) for name in sorted(set(old) & set(new)) if old[name] != new[name]]


def count_changes(old, new, keys):
    old = old or {}
    new = new or {}
    return [(key, int(old.get(key, 0)), int(new.get(key, 0))) for key in keys if int(old.get(key, 0)) != int(new.get(key, 0))]


def render_section(title, old, new, cases_key, counts_keys):
    lines = [f"#### {title}"]
    old_suites = old or {}
    new_suites = new or {}
    names = sorted(set(old_suites) | set(new_suites))
    if not names:
        return lines + ["No results were collected in both reports.", ""]
    changes_found = False
    case_details_left = 80
    for name in names:
        before = old_suites.get(name, {})
        after = new_suites.get(name, {})
        if before.get("state") != after.get("state"):
            lines.append(f"- **{name} state:** {before.get('state', 'absent')} → {after.get('state', 'absent')}")
            changes_found = True
        comparable = before.get("state") == after.get("state")
        if title == "LTP":
            comparable = before.get("state") == "complete" and after.get("state") == "complete"
        elif title == "KUnit":
            comparable = before.get("state") == "collected" and after.get("state") == "collected"
        elif title == "kselftest":
            comparable = before.get("state") in ("passed", "failed") and after.get("state") in ("passed", "failed")
        count_diff = count_changes(before.get("counts"), after.get("counts"), counts_keys) if comparable else []
        if count_diff:
            rendered = ", ".join(f"{key} {was} → {now}" for key, was, now in count_diff)
            lines.append(f"- **{name} counts:** {rendered}")
            changes_found = True
        case_diff = compare_cases(before.get(cases_key, {}), after.get(cases_key, {})) if comparable else []
        if case_diff:
            lines.append(f"- **{name} cases:** {len(case_diff)} added, removed, or changed")
            visible = case_diff[:case_details_left]
            lines.extend(f"  - `{case}`: {kind} ({was} → {now})" for case, kind, was, now in visible)
            case_details_left -= len(visible)
            if len(case_diff) > len(visible):
                lines.append(f"  - … and {len(case_diff) - len(visible)} more")
            changes_found = True
    if not changes_found:
        lines.append("No test result changes.")
    lines.append("")
    return lines


def render(snapshot, previous):
    lines = [
        "### Run metadata",
        f"- Tested ref: `{snapshot['tested_ref']}`",
        f"- Workflow commit: [`{snapshot['workflow_sha'][:12]}`]({snapshot['run_url']})",
        "- Job status: " + ", ".join(f"{name} `{status}`" for name, status in sorted(snapshot["job_status"].items())),
    ]
    lines.append("")

    lines.extend([
        "### LTP results",
        "Status key: 🔴 panic/segfault/abort or failed tests; 🟠 incomplete report or broken tests; 🟢 complete with no failed or broken tests.",
        "", "| Target | State | Passed | Failed | Broken | Skipped | Warnings |",
        "|---|---|---:|---:|---:|---:|---:|",
    ])
    for target, result in sorted(snapshot["ltp"]["targets"].items()):
        counts = result["counts"]
        target_name = f"[{target}]({result['url']})" if result.get("url") else target
        alerts = result.get("alerts", [])
        if alerts:
            state = "🔴 " + ", ".join(alerts)
        elif result["state"] != "complete":
            state = f"🟠 {result['state']}"
        elif counts["failed"]:
            state = "🔴 complete with test failures"
        elif counts["broken"]:
            state = "🟠 complete with broken tests"
        else:
            state = "🟢 complete"
        lines.append(f"| {target_name} | {state} | {counts['passed']} | {counts['failed']} | {counts['broken']} | {counts['skipped']} | {counts['warnings']} |")
    if not snapshot["ltp"]["targets"]:
        lines.append("| — | No LTP artifacts found | — | — | — | — | — |")
    lines.append("")

    profiles = [target for target, _header in LMBENCH_COLUMNS]
    getpid_values = {
        item.get("target"): item.get("value")
        for item in snapshot.get("benchmark", []) if item.get("name") == "getpid"
    }
    lines.extend(["### do_getpid (nsec)", "", "| | " + " | ".join(profiles) + " |",
                  "|---|" + "---:|" * len(profiles)])
    lines.append("| getpid | " + " | ".join(
        f"{getpid_values[profile]:.2f}" if isinstance(getpid_values.get(profile), (int, float)) else "N/A"
        for profile in profiles
    ) + " |")
    lines.append("")
    lines.append("### lmbench (usec; bandwidth rows in MB/sec)")
    headers = [header for _target, header in LMBENCH_COLUMNS]
    lines.append("||" + "|".join(headers) + "|")
    lines.append("|--|" + "|".join("--" for _ in headers) + "|")
    lmbench_table = snapshot.get("lmbench_table", {})
    for operation, label in LMBENCH_TABLE_ROWS:
        cells = []
        for target, _header in LMBENCH_COLUMNS:
            value = lmbench_table.get(target, {}).get(operation, "FAIL")
            cells.append(f"{value:.6g}" if isinstance(value, (int, float)) else value)
        lines.append("|" + label + "|" + "|".join(cells) + "|")
    lines.extend(["", "<!-- report-chart:lmbench.png -->", "<!-- report-chart:lmbench-bandwidth.png -->", ""])

    iperf_values = {}
    netperf_values = {}
    for item in snapshot.get("benchmark", []):
        labels = item.get("labels", {})
        if item.get("name") == "iperf3":
            iperf_values[(item.get("target"), labels.get("direction"))] = item.get("value")
        elif item.get("name") == "netperf":
            netperf_values[(item.get("target"), labels.get("test"), labels.get("payload_bytes"))] = item.get("value")

    lines.extend(["### iperf3 (Mbps)", "", "| Direction | " + " | ".join(profiles) + " |",
                  "|---|" + "---:|" * len(profiles)])
    for direction, label in (("forward", "Forward"), ("reverse", "Reverse")):
        cells = [iperf_values.get((profile, direction), "N/A") for profile in profiles]
        lines.append("| " + label + " | " + " | ".join(
            f"{value:.2f}" if isinstance(value, (int, float)) else value for value in cells
        ) + " |")
    lines.extend(["", "<!-- report-chart:iperf3.png -->", ""])

    payloads = (64, 128, 256, 512, 1024, 1500, 65507)
    for test, title in (("TCP_STREAM", "TCP_STREAM"), ("TCP_MAERTS", "TCP_MAERTS")):
        lines.extend([f"### netperf {title} (Mbps)", "",
                      "| Payload bytes | " + " | ".join(profiles) + " |",
                      "|---:|" + "---:|" * len(profiles)])
        for payload in payloads:
            cells = [netperf_values.get((profile, test, payload), "N/A") for profile in profiles]
            lines.append(f"| {payload} | " + " | ".join(
                f"{value:.2f}" if isinstance(value, (int, float)) else value for value in cells
            ) + " |")
        lines.append("")
    lines.extend(["<!-- report-chart:tcp-stream.png -->", ""])

    nginx = snapshot.get("nginx", {"state": "not run/no results", "matrix": []})
    lines.extend(["### nginx matrix", ""])
    if nginx.get("matrix"):
        lines.extend([
            "| Profile | Workers | Connections | Status | Requests/s | Avg latency (ms) | Socket errors |",
            "|---|---:|---:|---|---:|---:|---|",
        ])
        ordered_matrix = sorted(
            nginx["matrix"],
            key=lambda item: (item.get("profile", ""), int(item.get("workers", 0)), int(item.get("connections", 0))),
        )
        for item in ordered_matrix:
            def cell(value):
                return f"{value:.3f}" if isinstance(value, (int, float)) else "N/A"

            socket_errors = item.get("socket_errors")
            lines.append(
                f"| {item.get('profile', 'unknown')} | {item.get('workers', 'N/A')} "
                f"| {item.get('connections', 'N/A')} | {item.get('status', 'UNKNOWN')} "
                f"| {cell(item.get('requests_per_second'))} | {cell(item.get('average_latency_ms'))} "
                f"| {socket_errors if socket_errors is not None else 'N/A'} |"
            )
    else:
        lines.append(f"No nginx matrix results were collected ({nginx.get('state', 'unknown')}).")
    lines.extend(["", "<!-- report-chart:nginx-workers-1.png -->",
                  "<!-- report-chart:nginx-workers-4.png -->", ""])

    kselftest = snapshot["kselftest"]
    if kselftest["state"] in ("passed", "failed"):
        counts = kselftest["counts"]
        kselftest_status = f"{kselftest['state']}: {counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped"
    else:
        kselftest_status = kselftest["state"]
    lines.extend(["### kselftest results", f"Current status: {kselftest_status}.", ""])

    lines.extend(["### KUnit results", ""])
    if snapshot["kunit"]["suites"]:
        lines.extend(["| Target / suite | Passed | Failed | Crashed | Skipped | Errors |", "|---|---:|---:|---:|---:|---:|"])
        for suite, result in sorted(snapshot["kunit"]["suites"].items()):
            counts = result["counts"]
            lines.append(f"| {suite} | {counts['passed']} | {counts['failed']} | {counts['crashed']} | {counts['skipped']} | {counts['errors']} |")
    else:
        lines.append("No KUnit JSON artifacts were collected.")
    lines.append("")

    lines.append("### Changes from previous report")
    if not previous:
        lines.extend(["This is the first report with a saved comparison snapshot. The next report will be compared with this run.", ""])
        return "\n".join(lines)
    metric_diff = compare_metrics(metric_index(previous.get("benchmark", [])),
                                  metric_index(snapshot.get("benchmark", [])))
    metric_groups = {}
    for row in metric_diff:
        category = row[0].split("/", 1)[0]
        metric_groups[category] = metric_groups.get(category, 0) + 1
    test_changes = {}
    for title, old, new, cases_key, counts_keys in (
        ("KUnit", previous.get("kunit", {}).get("suites"), snapshot.get("kunit", {}).get("suites"), "cases", KUNIT_COUNTS),
        ("kselftest", {"kselftest": previous.get("kselftest", {})}, {"kselftest": snapshot.get("kselftest", {})}, "cases", KSELFTEST_COUNTS),
        ("LTP", previous.get("ltp", {}).get("targets"), snapshot.get("ltp", {}).get("targets"), "cases", LTP_COUNTS),
    ):
        old_suites = old or {}
        new_suites = new or {}
        changed = 0
        for name in set(old_suites) | set(new_suites):
            before = old_suites.get(name, {})
            after = new_suites.get(name, {})
            if before.get("state") != after.get("state"):
                changed += 1
            comparable = before.get("state") == after.get("state")
            if title == "LTP":
                comparable = before.get("state") == "complete" and after.get("state") == "complete"
            elif title == "KUnit":
                comparable = before.get("state") == "collected" and after.get("state") == "collected"
            elif title == "kselftest":
                comparable = before.get("state") in ("passed", "failed") and after.get("state") in ("passed", "failed")
            if comparable:
                changed += bool(count_changes(before.get("counts"), after.get("counts"), counts_keys))
                changed += bool(compare_cases(before.get(cases_key, {}), after.get(cases_key, {})))
        test_changes[title] = changed
    benchmark_summary = ", ".join(f"{name} {count}" for name, count in sorted(metric_groups.items())) or "none"
    test_summary = ", ".join(f"{name} {count}" for name, count in test_changes.items())
    lines.append(f"Compared with [{previous.get('workflow_sha', 'previous run')[:12]}]({previous.get('run_url', '#')}) (tested ref `{previous.get('tested_ref', 'unknown')}`). Benchmark readings can vary with runner load; changes are measurements, not pass/fail thresholds.")
    lines.append("")
    lines.extend([
        f"<details><summary>Change details — benchmarks: {len(metric_diff)} ({benchmark_summary}); test categories: {test_summary}</summary>",
        "",
        "#### Benchmarks",
    ])
    if metric_diff:
        for category in sorted({row[0].split("/", 1)[0] for row in metric_diff}):
            lines.extend([f"##### {category}", "| Metric | Previous | Current | Change |", "|---|---:|---:|---:|"])
            lines.extend(f"| {name} | {before} | {after} | {change} |"
                         for name, before, after, change in metric_diff
                         if name.split("/", 1)[0] == category)
    else:
        lines.append("No benchmark value changes (or no numeric samples in both reports).")
    lines.append("")
    lines.extend(render_section("LTP", previous.get("ltp", {}).get("targets"), snapshot.get("ltp", {}).get("targets"), "cases", LTP_COUNTS))
    lines.extend(render_section("kselftest", {"kselftest": previous.get("kselftest", {})}, {"kselftest": snapshot.get("kselftest", {})}, "cases", KSELFTEST_COUNTS))
    lines.extend(render_section("KUnit", previous.get("kunit", {}).get("suites"), snapshot.get("kunit", {}).get("suites"), "cases", KUNIT_COUNTS))
    lines.extend(["</details>", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--tested-ref", required=True)
    parser.add_argument("--build-status", required=True)
    parser.add_argument("--benchmark-status", required=True)
    parser.add_argument("--ltp-status", required=True)
    args = parser.parse_args()

    snapshot = collect(args.data_dir, args.run_url, args.sha, args.tested_ref, {
        "UML/KUnit build": args.build_status,
        "benchmarks": args.benchmark_status,
        "LTP": args.ltp_status,
    })
    previous = load_json(args.previous) if args.previous else None
    args.snapshot.write_text(json.dumps(snapshot, indent=2, sort_keys=True) + "\n")
    report = render(snapshot, previous)
    compressed = base64.b64encode(gzip.compress(
        json.dumps(snapshot, separators=(",", ":"), sort_keys=True).encode(), compresslevel=9, mtime=0
    )).decode()
    args.report.write_text(report + "\n\n<!-- uml-ci-result-snapshot:v1\n" +
                           compressed + "\n-->\n")


if __name__ == "__main__":
    main()
