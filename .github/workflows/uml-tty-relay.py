#!/usr/bin/env python3
"""Bridge a host-only command FIFO to a UML serial tty, without guest polling.

A FIFO inode on hostfs has separate pipe state in the host and guest kernels.
Keep both host PTY ends open before UML starts, so its tty backend never sees
an initial hangup while the host controller is connecting.
"""

import argparse
import os
from pathlib import Path
import pty
import select
import signal
import tty


def serve(fifo: Path, endpoint: Path, log: Path) -> None:
    master, slave = pty.openpty()
    command_fd = None
    try:
        tty.setraw(slave)
        os.set_blocking(master, False)
        os.mkfifo(fifo, 0o600)
        command_fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        endpoint.write_text(os.ttyname(slave) + "\n")
        with log.open("ab", buffering=0) as output:
            while True:
                ready, _, _ = select.select([command_fd, master], [], [])
                for fd in ready:
                    data = os.read(fd, 65536)
                    if not data:
                        continue
                    if fd == command_fd:
                        pending = memoryview(data)
                        while pending:
                            _, writable, _ = select.select([], [master], [])
                            if writable:
                                pending = pending[os.write(master, pending):]
                    else:
                        output.write(data)
    finally:
        if command_fd is not None:
            os.close(command_fd)
            fifo.unlink(missing_ok=True)
        os.close(master)
        os.close(slave)
        endpoint.unlink(missing_ok=True)


def stop(_signal: int, _frame: object) -> None:
    raise SystemExit(0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fifo", type=Path, required=True)
    parser.add_argument("--endpoint", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    serve(args.fifo, args.endpoint, args.log)
