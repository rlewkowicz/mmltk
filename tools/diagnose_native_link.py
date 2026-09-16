#!/usr/bin/env python3
"""Repeat one generated native link into independent diagnostic storage."""

import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", help="Existing Release executable target")
    parser.add_argument("--linker-directory", help="Repository-relative directory containing a candidate ld.mold")
    parser.add_argument("--trace-symbol", action="append", default=[], help="Trace one mangled symbol (repeatable)")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_]+", args.target):
        parser.error("expected an ordinary native target name")
    workspace = Path("/workspace")
    linker = None
    if args.linker_directory:
        relative = Path(args.linker_directory)
        linker = (workspace / relative).resolve()
        if relative.is_absolute() or not linker.is_relative_to(workspace) or not (linker / "ld.mold").is_file():
            parser.error("linker directory must remain within the repository and contain ld.mold")
        if not (linker / "ld.mold").resolve().is_relative_to(workspace):
            parser.error("candidate linker must remain within the repository")
    if any(not re.fullmatch(r"[A-Za-z0-9_.$@]+", symbol) for symbol in args.trace_symbol):
        parser.error("expected a mangled symbol without linker option separators")
    graph = Path("/workspace/.cache/cmake/release")
    rule = None
    for line in (graph / "build.ninja").read_text().splitlines():
        if line.startswith(f"build {args.target}: "):
            rule = line.split(": ", 1)[1].split(" ", 1)[0]
            break
    if not rule or not rule.startswith("CXX_EXECUTABLE_LINKER__"):
        parser.error("target has no generated C++ executable link")
    entries = json.loads(subprocess.check_output(["ninja", "-t", "compdb", rule], cwd=graph))
    entry = next(item for item in entries if item["output"] == args.target)
    command = shlex.split(entry["command"])
    if command[:2] != [":", "&&"] or command[-2:] != ["&&", ":"]:
        parser.error("unsupported generated link command shape")
    command = command[2:-2]
    if command[0] != "/opt/gcc-16.2/bin/g++" or any(token in {"&&", ";", "|"} for token in command):
        parser.error("expected one direct GCC link command")
    output = Path("/diagnostic/output")
    command[command.index("-o") + 1] = str(output / args.target)
    command = [f"-Wl,--dependency-file={output / 'link.d'}" if token.startswith("-Wl,--dependency-file=") else token for token in command]
    command += ["-save-temps=obj", f"-Wl,-Map,{output / 'link.map'}"]
    if linker:
        command.insert(1, f"-B{linker}/")
    command.extend(f"-Wl,--trace-symbol={symbol}" for symbol in args.trace_symbol)
    (output / "command.json").write_text(json.dumps({"directory": str(graph), "arguments": command}, indent=2) + "\n")
    return subprocess.call(command, cwd=graph)


if __name__ == "__main__":
    sys.exit(main())
