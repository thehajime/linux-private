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
    metrics = {}
    for path in sorted(root.glob("**/*-out.dat")):
        stem = path.stem
        rows = numbers(path)
        if not rows:
            if not stem.endswith("-getpid-out"):
                continue
        if stem.endswith("-lmbench-out"):
            target = stem.removesuffix("-lmbench-out")
            if len(rows) == 1 and len(rows[0]) == len(LM_BENCHMARKS):
                rows = [[value] for value in rows[0]]
            for index, row in enumerate(rows):
                label = LM_BENCHMARKS[index] if index < len(LM_BENCHMARKS) else f"metric-{index + 1}"
                metrics[f"lmbench/{target}/{label} (usec)"] = row[-1]
        elif stem.endswith("-getpid-out"):
            target = stem.removesuffix("-getpid-out")
            text = path.read_text(errors="replace")
            value = re.search(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?", text)
            metrics[f"getpid/{target} (nsec)"] = float(value.group()) if value else rows[-1][-1]
        elif stem.endswith("-iperf3-out"):
            target = stem.removesuffix("-iperf3-out")
            labels = ("forward", "reverse")
            if len(rows) == 1 and len(rows[0]) == len(labels):
                rows = [[value] for value in rows[0]]
            for index, row in enumerate(rows):
                label = labels[index] if index < len(labels) else f"direction-{index + 1}"
                metrics[f"iperf3/{target}/{label} (Mbps)"] = row[-1]
        elif stem.endswith("-netperf-fwd-out") or stem.endswith("-netperf-rev-out"):
            reverse = stem.endswith("-netperf-rev-out")
            suffix = "-netperf-rev-out" if reverse else "-netperf-fwd-out"
            target = stem.removesuffix(suffix)
            direction = "TCP_MAERTS" if reverse else "TCP_STREAM"
            for row in rows:
                if len(row) >= 2:
                    size = int(row[0])
                    metrics[f"netperf/{target}/{direction}/{size}B (Mbps)"] = row[1]
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
    # from a panic. Record those targets explicitly even when no JSON was made.
    for path in root.rglob("*-ltp.dat"):
        target = path.stem.removesuffix("-ltp")
        results.setdefault(target, {
            "state": "incomplete (no result JSON)",
            "url": report_urls.get(target, ""),
            "counts": {key: 0 for key in LTP_COUNTS},
            "cases": {},
        })
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
    for path in sorted((root / "kselftest").rglob("*")) if (root / "kselftest").exists() else []:
        if not path.is_file() or path.suffix not in (".tap", ".log", ".txt"):
            continue
        for line in path.read_text(errors="replace").splitlines():
            match = re.match(r"^\s*(not )?ok(?:\s+\d+)?(?:\s+-\s+(.+?))?\s*(?:#\s*(SKIP|TODO).*)?$", line)
            if not match:
                continue
            name = match.group(2) or f"{path.name}:{len(cases) + 1}"
            status = "FAIL" if match.group(1) else "PASS"
            if match.group(3) == "SKIP":
                status = "SKIP"
            cases[name] = status
    counts = {
        "passed": sum(status == "PASS" for status in cases.values()),
        "failed": sum(status == "FAIL" for status in cases.values()),
        "skipped": sum(status == "SKIP" for status in cases.values()),
    }
    return {"state": "collected" if cases else "disabled/no results", "counts": counts, "cases": cases}


def collect(root, run_url, sha, tested_ref, job_status):
    return {
        "version": 1,
        "run_url": run_url,
        "workflow_sha": sha,
        "tested_ref": tested_ref,
        "job_status": job_status,
        "benchmark": collect_benchmarks(root),
        "kunit": collect_kunit(root),
        "kselftest": collect_kselftest(root),
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
        elif title in ("KUnit", "kselftest"):
            comparable = before.get("state") == "collected" and after.get("state") == "collected"
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
        "",
        "### KUnit results",
    ]
    if snapshot["kunit"]["suites"]:
        lines.extend(["| Target / suite | Passed | Failed | Crashed | Skipped | Errors |", "|---|---:|---:|---:|---:|---:|"])
        for suite, result in sorted(snapshot["kunit"]["suites"].items()):
            counts = result["counts"]
            lines.append(f"| {suite} | {counts['passed']} | {counts['failed']} | {counts['crashed']} | {counts['skipped']} | {counts['errors']} |")
    else:
        lines.append("No KUnit JSON artifacts were collected.")
    kselftest = snapshot["kselftest"]
    if kselftest["state"] == "collected":
        counts = kselftest["counts"]
        kselftest_status = f"collected: {counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped"
    else:
        kselftest_status = kselftest["state"]
    lines.extend(["", "### kselftest results", f"Current status: {kselftest_status}.", ""])
    lines.extend(["### LTP results", "| Target | State | Passed | Failed | Broken | Skipped | Warnings |", "|---|---|---:|---:|---:|---:|---:|"])
    for target, result in sorted(snapshot["ltp"]["targets"].items()):
        counts = result["counts"]
        target_name = f"[{target}]({result['url']})" if result.get("url") else target
        lines.append(f"| {target_name} | {result['state']} | {counts['passed']} | {counts['failed']} | {counts['broken']} | {counts['skipped']} | {counts['warnings']} |")
    if not snapshot["ltp"]["targets"]:
        lines.append("| — | No LTP artifacts found | — | — | — | — | — |")
    lines.append("")
    lines.append("### Changes from previous report")
    if not previous:
        lines.extend(["This is the first report with a saved comparison snapshot. The next report will be compared with this run.", ""])
        return "\n".join(lines)
    lines.append(f"Compared with [{previous.get('workflow_sha', 'previous run')[:12]}]({previous.get('run_url', '#')}) (tested ref `{previous.get('tested_ref', 'unknown')}`). Benchmark readings can vary with runner load; changes are measurements, not pass/fail thresholds.")
    lines.append("")
    lines.append("#### Benchmarks")
    metric_diff = compare_metrics(previous.get("benchmark"), snapshot.get("benchmark"))
    if metric_diff:
        lines.extend(["| Metric | Previous | Current | Change |", "|---|---:|---:|---:|"])
        lines.extend(f"| {name} | {before} | {after} | {change} |" for name, before, after, change in metric_diff)
    else:
        lines.append("No benchmark value changes (or no numeric samples in both reports).")
    lines.append("")
    lines.extend(render_section("KUnit", previous.get("kunit", {}).get("suites"), snapshot.get("kunit", {}).get("suites"), "cases", KUNIT_COUNTS))
    lines.extend(render_section("kselftest", {"kselftest": previous.get("kselftest", {})}, {"kselftest": snapshot.get("kselftest", {})}, "cases", KSELFTEST_COUNTS))
    lines.extend(render_section("LTP", previous.get("ltp", {}).get("targets"), snapshot.get("ltp", {}).get("targets"), "cases", LTP_COUNTS))
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
