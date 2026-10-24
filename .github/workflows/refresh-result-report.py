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
source_snapshot = snapshot.copy()
original = (work / "source-live-body.md").read_text()
marker = re.search(r"<!-- uml-ci-result-snapshot:v1\s+([A-Za-z0-9+/=\s]+?)\s*-->", original)
module = runpy.run_path(str(Path(__file__).with_name("result-report.py")))
render = module["render"]
recollected = os.environ.get("RECOLLECT_KSELFTEST", "false").lower() == "true"
nginx_matrix = os.environ.get("NGINX_MATRIX_PATH", "")
if recollected:
    result = module["collect_kselftest"](work / "source-kselftest")
    if result["state"] not in ("passed", "failed") or not result["suites"]:
        raise SystemExit("Original TAP artifacts are missing or incomplete")
    snapshot["kselftest"] = result
if nginx_matrix:
    snapshot["nginx"] = json.loads(Path(nginx_matrix).read_text())
if not marker or json.loads(gzip.decompress(base64.b64decode(marker[1]))) not in (source_snapshot, snapshot):
    raise SystemExit("The issue comment and source-run snapshot do not match")
report = render(snapshot, None)
heading = "### Changes from previous report"
if heading in original:
    previous_changes = original.split(heading, 1)[1].split("<!-- uml-ci-result-snapshot:v1", 1)[0].strip()
    report = report.split(heading, 1)[0] + heading + "\n" + previous_changes + "\n"
if nginx_matrix:
    notice = "Report regenerated with validated expanded nginx replay measurements; other saved results are unchanged."
elif recollected:
    notice = "Report regenerated from original measurements and per-mode TAP artifacts; no tests were rerun."
else:
    notice = "Report display refreshed from the original measurements; test and benchmark results are unchanged."
report = notice + "\n\n" + report
(work / "result-snapshot.json").write_text(json.dumps(snapshot, indent=2) + "\n")
(work / "comparison-report.md").write_text(report)
if recollected or nginx_matrix:
    payload = base64.b64encode(gzip.compress(
        json.dumps(snapshot, separators=(",", ":")).encode())).decode()
    snapshot_marker = "<!-- uml-ci-result-snapshot:v1\n" + payload + "\n-->"
else:
    snapshot_marker = marker[0]
(work / "snapshot-marker.txt").write_text(snapshot_marker)
(work / "nginx-matrix.json").write_text(json.dumps(snapshot.get("nginx", {})))
