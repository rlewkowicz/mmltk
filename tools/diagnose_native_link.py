#!/usr/bin/env python3
"""Repeat one generated native link into independent diagnostic storage."""

import argparse
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", help="Existing native executable target")
    parser.add_argument("--graph", choices=("release", "analysis"), default="release", help="Cached CMake graph")
    parser.add_argument("--save-temps", action=argparse.BooleanOptionalAction, default=True, help="Retain LTO intermediates")
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
    graph = workspace / ".cache/cmake" / args.graph
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
    # Saved LTO debug copies are written beside the input archives/objects.
    # Borrow inputs through diagnostic symlinks so the cached graph stays read-only.
    if args.save_temps:
        inputs = output / "inputs"
        inputs.mkdir()
        borrowed = {}
        for index, token in enumerate(command):
            source = graph / token
            if token.startswith("-") or not source.is_file():
                continue
            if source.suffix in {".a", ".o"}:
                if source not in borrowed:
                    link = inputs / f"{len(borrowed)}-{source.name}"
                    link.symlink_to(source)
                    borrowed[source] = link
                source = borrowed[source]
            command[index] = str(source)
        command.append("-save-temps=obj")
    command.append(f"-Wl,-Map,{output / 'link.map'}")
    if linker:
        command.insert(1, f"-B{linker}/")
    command.extend(f"-Wl,--trace-symbol={symbol}" for symbol in args.trace_symbol)
    selected_linker = str(linker / "ld.mold") if linker else shutil.which("ld.mold")
    if not selected_linker:
        parser.error("the development image has no ld.mold")
    versions = {"compiler": subprocess.check_output([command[0], "--version"], text=True).splitlines()[0],
                "linker": subprocess.check_output([selected_linker, "--version"], text=True).strip()}
    directory = output if args.save_temps else graph
    (output / "command.json").write_text(json.dumps({"graph": str(graph), "directory": str(directory), "arguments": command, "versions": versions}, indent=2) + "\n")
    print(json.dumps({"owner": "native-link", "event": "toolchain", **versions}), flush=True)
    return subprocess.call(command, cwd=directory)


if __name__ == "__main__":
    sys.exit(main())
