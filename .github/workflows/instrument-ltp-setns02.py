#!/usr/bin/env python3
"""Add temporary runtime diagnostics to LTP setns02 for focused CI runs."""

from pathlib import Path


SOURCE = Path("testcases/kernel/syscalls/setns/setns02.c")
text = SOURCE.read_text()

replacements = (
    (
        '\ttst_res(TINFO, CP"hostname changed to: %s", uts.nodename);\n'
        '\tif (strcmp(uts_parent.nodename, uts.nodename) == 0) {',
        '\ttst_res(TINFO, CP"hostname changed to: %s", uts.nodename);\n'
        '\tdprintf(STDERR_FILENO,\n'
        '\t\t"SETNS02_DEBUG: before hostname compare parent=\'%s\' child=\'%s\'\\n",\n'
        '\t\tuts_parent.nodename, uts.nodename);\n'
        '\tif (strcmp(uts_parent.nodename, uts.nodename) == 0) {',
    ),
    (
        '\ttst_res(TINFO, CP"attempting to switch ns back to parent ns");\n'
        '\tif (tst_syscall(__NR_setns, ns_uts_fd, ns_flag) == -1) {\n'
        '\t\ttst_res(TFAIL|TERRNO, CP"setns");',
        '\ttst_res(TINFO, CP"attempting to switch ns back to parent ns");\n'
        '\tdprintf(STDERR_FILENO,\n'
        '\t\t"SETNS02_DEBUG: before setns fd=%d flag=0x%x\\n",\n'
        '\t\tns_uts_fd, ns_flag);\n'
        '\tint setns_ret = tst_syscall(__NR_setns, ns_uts_fd, ns_flag);\n'
        '\tint setns_errno = errno;\n'
        '\tdprintf(STDERR_FILENO,\n'
        '\t\t"SETNS02_DEBUG: setns ret=%d errno=%d (%s)\\n",\n'
        '\t\tsetns_ret, setns_errno, strerror(setns_errno));\n'
        '\tif (setns_ret == -1) {\n'
        '\t\terrno = setns_errno;\n'
        '\t\ttst_res(TFAIL|TERRNO, CP"setns");',
    ),
    (
        '\tSAFE_WAITPID(ret, &status, 0);\n'
        '\tfree(child_stack);',
        '\tSAFE_WAITPID(ret, &status, 0);\n'
        '\tdprintf(STDERR_FILENO,\n'
        '\t\t"SETNS02_DEBUG: clone child wait status=0x%x exited=%d code=%d signaled=%d signal=%d\\n",\n'
        '\t\tstatus, WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,\n'
        '\t\tWIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);\n'
        '\tfree(child_stack);',
    ),
)

if "SETNS02_DEBUG:" in text:
    raise SystemExit("setns02 already has diagnostics")

for before, after in replacements:
    count = text.count(before)
    if count != 1:
        raise SystemExit(f"expected one source match, found {count}: {before!r}")
    text = text.replace(before, after, 1)

SOURCE.write_text(text)
