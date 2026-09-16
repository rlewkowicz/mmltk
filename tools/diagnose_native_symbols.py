#!/usr/bin/env python3
"""Read native archive/object symbols with the development image's GCC tools."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--match", default=".", help="Regular expression over demangled symbol records")
    parser.add_argument("--mangled", action="store_true", help="Keep mangled names for linker tracing")
    parser.add_argument("--limit", type=int, default=200, help="Maximum emitted symbol records")
    parser.add_argument("artifacts", nargs="+", help="Repository-relative .a or .o paths")
    args = parser.parse_args()
    if not 1 <= args.limit <= 10000:
        parser.error("--limit must be between 1 and 10000")
    try:
        pattern = re.compile(args.match)
    except re.error as error:
        parser.error(str(error))
    root = Path("/workspace")
    paths = []
    for artifact in args.artifacts:
        path = (root / artifact).resolve(strict=True)
        if not path.is_relative_to(root) or path.suffix not in {".a", ".o"} or not path.is_file():
            parser.error(f"expected a repository .a or .o file: {artifact}")
        paths.append(str(path))
    command = ["/opt/gcc-16.2/bin/gcc-nm", "-A"]
    if not args.mangled:
        command.append("-C")
    command.extend(paths)
    matched = 0
    emitted = 0
    with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as process:
        for line in process.stdout:
            if pattern.search(line):
                matched += 1
                if emitted < args.limit:
                    print(json.dumps({"owner": "native-symbols", "event": "symbol", "message": line.rstrip()}))
                    emitted += 1
        status = process.wait()
    print(json.dumps({"owner": "native-symbols", "event": "summary", "matched": matched,
                      "emitted": emitted, "exit_code": status}))
    return status


if __name__ == "__main__":
    sys.exit(main())
