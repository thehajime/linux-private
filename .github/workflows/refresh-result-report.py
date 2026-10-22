#!/usr/bin/env python3
"""Re-render saved CI measurements without rerunning benchmarks or tests."""

import base64
import gzip
import json
import os
import re
import runpy
from pathlib import Path

work = Path(os.environ["RUNNER_TEMP"])
snapshot_path = next((work / "source-report").rglob("result-snapshot.json"))
snapshot = json.loads(snapshot_path.read_text())
original = (work / "source-live-body.md").read_text()
marker = re.search(r"<!-- uml-ci-result-snapshot:v1\s+([A-Za-z0-9+/=\s]+?)\s*-->", original)
if not marker or json.loads(gzip.decompress(base64.b64decode(marker[1]))) != snapshot:
    raise SystemExit("The issue comment and source-run snapshot do not match")
render = runpy.run_path(str(Path(__file__).with_name("result-report.py")))["render"]
report = render(snapshot, None)
heading = "### Changes from previous report"
if heading in original:
    previous_changes = original.split(heading, 1)[1].split("<!-- uml-ci-result-snapshot:v1", 1)[0].strip()
    report = report.split(heading, 1)[0] + heading + "\n" + previous_changes + "\n"
report = ("Report display refreshed from the original measurements; test and benchmark results are unchanged.\n\n" + report)
(work / "result-snapshot.json").write_text(json.dumps(snapshot, indent=2) + "\n")
(work / "comparison-report.md").write_text(report)
(work / "snapshot-marker.txt").write_text(marker[0])
(work / "nginx-matrix.json").write_text(json.dumps(snapshot.get("nginx", {})))
