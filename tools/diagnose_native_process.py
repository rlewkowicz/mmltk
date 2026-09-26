#!/usr/bin/env python3
"""Inspect or terminate one exact native process inside its wrapper container."""

import argparse
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--backtrace", action="store_true")
    actions.add_argument("--terminate", action="store_true")
    actions.add_argument("--read-file", action="store_true")
    parser.add_argument("executable")
    parser.add_argument("--pid", type=int, help="select a container PID when an executable has several workers")
    parser.add_argument("--file", type=Path, help="absolute container path of a retained diagnostic artifact")
    args = parser.parse_args()
    if args.read_file != (args.file is not None):
        parser.error("--read-file requires --file; other actions do not accept --file")
    if args.file is not None and not args.file.is_absolute():
        parser.error("--file must be an absolute container path")
    matches = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            executable = (entry / "exe").resolve(strict=True)
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if executable.name == args.executable and (args.pid is None or int(entry.name) == args.pid):
            matches.append(int(entry.name))
    if len(matches) != 1:
        parser.error(f"expected exactly one {args.executable} process; matching container PIDs: {matches}; use --pid to select one")
    pid = matches[0]
    descriptor = os.pidfd_open(pid)
    try:
        print(f"native process: {args.executable}, container pid={pid}", flush=True)
        if args.read_file:
            path = Path("/proc") / str(pid) / "root" / args.file.relative_to("/")
            with os.fdopen(os.open(path, os.O_RDONLY | os.O_NONBLOCK), "rb") as artifact:
                info = os.fstat(artifact.fileno())
                if not stat.S_ISREG(info.st_mode) or info.st_size > 4 * 1024 * 1024:
                    parser.error("diagnostic artifact must be a regular file no larger than 4 MiB")
                data = artifact.read(4 * 1024 * 1024 + 1)
                if len(data) > 4 * 1024 * 1024:
                    parser.error("diagnostic artifact grew beyond 4 MiB")
                print(f"artifact: {args.file}, bytes={len(data)}")
                print(repr(data))
            return 0
        if args.terminate:
            signal.pidfd_send_signal(descriptor, signal.SIGTERM)
            return 0
        return subprocess.run(
            [
                "gdb", "--batch", "--nx", "--quiet", "-p", str(pid),
                "-ex", "set pagination off", "-ex", "set print thread-events off",
                "-ex", "thread apply all bt", "-ex", "detach",
            ],
            check=False,
            timeout=35,
        ).returncode
    finally:
        os.close(descriptor)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"native process diagnostic failed: {error}", file=sys.stderr)
        raise SystemExit(1)
