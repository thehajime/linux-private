#!/usr/bin/env python3
"""Assemble a small static GitHub Pages report from CI outputs."""

import argparse
import html
import json
import shutil
from pathlib import Path


CHARTS = {
    "lmbench.png": ("lmbench latency", "Latency measurements across the UML profiles."),
    "lmbench-bandwidth.png": ("lmbench bandwidth", "Pipe and memory bandwidth measurements across the UML profiles."),
    "iperf3.png": ("iperf3 throughput", "Forward and reverse iperf3 goodput by UML profile, in Gbit/s."),
    "tcp-stream.png": ("netperf TCP throughput", "TCP_STREAM and TCP_MAERTS goodput by payload size and UML profile; reverse-direction bars extend below zero."),
}
NGINX_CHARTS = (
    (
        "nginx-workers-1.png",
        "Nginx requests per second with one worker, grouped by client connections (4, 16, 64). Each group compares UML profiles; color identifies the profile, and labels identify whether CONFIG_SMP is enabled.",
    ),
    (
        "nginx-workers-4.png",
        "Nginx requests per second with four workers, grouped by client connections (4, 16, 64). Each group compares UML profiles; color identifies the profile, and labels identify whether CONFIG_SMP is enabled.",
    ),
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--charts", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    snapshot = json.loads(args.snapshot.read_text())
    args.output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.snapshot, args.output / "result-snapshot.json")
    report_copy = args.output / "comparison-report.md"
    report = args.report.read_text()
    chart_descriptions = {
        filename: description for filename, (_title, description) in CHARTS.items()
    }
    chart_descriptions.update(NGINX_CHARTS)
    for filename, description in chart_descriptions.items():
        marker = f"<!-- report-chart:{filename} -->"
        if marker not in report:
            continue
        if (args.charts / filename).is_file():
            replacement = f"{description}\n\n![{filename.removesuffix('.png')}]({filename})"
        else:
            replacement = ""
        report = report.replace(marker, replacement)
    report_copy.write_text(report)

    run_url = html.escape(snapshot.get("run_url", ""), quote=True)
    tested_ref = html.escape(snapshot.get("tested_ref", "unknown"))
    sha = html.escape(snapshot.get("workflow_sha", "unknown"))
    charts = []
    chart_files = list(CHARTS.items())
    chart_files.extend(
        (filename, (f"nginx throughput: {filename.removesuffix('.png')}", description))
        for filename, description in NGINX_CHARTS
    )
    for filename, (title, description) in chart_files:
        source = args.charts / filename
        if not source.is_file():
            continue
        shutil.copy2(source, args.output / filename)
        heading = f"<h2>{html.escape(title)}</h2>" if title else ""
        caption = f"<figcaption>{html.escape(description)}</figcaption>" if description else ""
        charts.append(
            f'<section>{heading}<figure><a href="{filename}">'
            f'<img src="{filename}" alt="{html.escape(title or description)}"></a>{caption}'
            f'</figure></section>'
        )
    chart_html = "\n".join(charts) or "<p>No benchmark charts were generated for this run.</p>"
    content = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>UML CI results</title>
  <style>
    body {{ font: 16px/1.5 system-ui, sans-serif; max-width: 1100px; margin: 2rem auto; padding: 0 1rem; color: #222; }}
    img {{ max-width: 100%; height: auto; }}
    section {{ margin: 2rem 0; }}
    .meta {{ color: #555; }}
  </style>
</head>
<body>
  <h1>UML CI results</h1>
  <p class="meta">Tested ref: <code>{tested_ref}</code> · workflow commit: <code>{sha}</code></p>
  <p><a href="{run_url}">Open GitHub Actions run</a> ·
     <a href="result-snapshot.json">Download structured JSON results</a> ·
     <a href="comparison-report.md">Read comparison report</a></p>
  {chart_html}
</body>
</html>
"""
    (args.output / "index.html").write_text(content)


if __name__ == "__main__":
    main()
