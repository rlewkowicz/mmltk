#!/usr/bin/env python3
"""Read native symbols or resolve ELF addresses with the development image's tools."""

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
    parser.add_argument("--address", action="append", default=[], help="Resolve a hexadecimal address in one ELF artifact; repeatable")
    parser.add_argument("artifacts", nargs="+", help="Repository-relative or installed /opt artifacts (.a/.o, or ELF with --address)")
    args = parser.parse_args()
    if not 1 <= args.limit <= 10000:
        parser.error("--limit must be between 1 and 10000")
    if args.address and (len(args.artifacts) != 1 or len(args.address) > args.limit):
        parser.error("--address requires one artifact and at most --limit addresses")
    if any(not re.fullmatch(r"(?:0x)?[0-9a-fA-F]+", address) for address in args.address):
        parser.error("--address requires hexadecimal addresses")
    try:
        pattern = re.compile(args.match)
    except re.error as error:
        parser.error(str(error))
    root = Path("/workspace")
    paths = []
    for artifact in args.artifacts:
        path = (root / artifact).resolve(strict=True)
        if not (path.is_relative_to(root) or path.is_relative_to("/opt")) or not path.is_file():
            parser.error(f"expected a repository or installed /opt file: {artifact}")
        if args.address:
            with path.open("rb") as artifact_file:
                if artifact_file.read(4) != b"\x7fELF":
                    parser.error(f"expected an ELF artifact for --address: {artifact}")
        elif path.suffix not in {".a", ".o"}:
            parser.error(f"expected an .a or .o file: {artifact}")
        paths.append(str(path))
    if args.address:
        result = subprocess.run(["addr2line", "-C", "-f", "-p", "-e", paths[0], *args.address],
                                capture_output=True, text=True, timeout=110)
        for address, line in zip(args.address, result.stdout.splitlines(), strict=False):
            print(json.dumps({"owner": "native-symbols", "event": "address", "artifact": paths[0],
                              "address": address, "message": line}))
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        return result.returncode
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
