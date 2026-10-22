#!/usr/bin/env python3
"""Use a monotonic clock for Kirk's local-command and suite durations.

LTP clock_settime tests intentionally move CLOCK_REALTIME. Keep calendar
timestamps elsewhere unchanged. Apply after the selected LTP Kirk patch.
"""

import ast
from pathlib import Path
import sys


def patch(root: Path) -> None:
    changes = []
    for relative in ("libkirk/channels/shell.py", "libkirk/scheduler.py"):
        path = root / relative
        source = path.read_text()
        tree = ast.parse(source, filename=str(path))
        lines = source.splitlines(keepends=True)
        calls = [
            node for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "time"
            and node.func.attr == "time"
            and not node.args and not node.keywords
        ]
        if not calls and "time.monotonic()" not in source:
            raise ValueError(f"Unrecognized Kirk duration implementation: {path}")
        for node in sorted(calls, key=lambda n: (n.lineno, n.col_offset), reverse=True):
            if node.lineno != node.end_lineno:
                raise ValueError(f"Unexpected multiline clock call: {path}:{node.lineno}")
            line = lines[node.lineno - 1].encode()
            lines[node.lineno - 1] = (
                line[:node.col_offset] + b"time.monotonic()" + line[node.end_col_offset:]
            ).decode()
        updated = "".join(lines)
        compile(updated, str(path), "exec")
        changes.append((path, updated, len(calls)))
    # Validate both modules before changing either.
    for path, updated, count in changes:
        if count:
            path.write_text(updated)
        print(f"{path}: {count} elapsed-time clock calls updated")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-kirk-monotonic.py KIRK_SOURCE_DIRECTORY")
    patch(Path(sys.argv[1]))
