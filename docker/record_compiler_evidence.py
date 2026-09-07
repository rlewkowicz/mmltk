#!/usr/bin/env python3
"""Capture effective compiler evidence before temporary build trees disappear.

Capture is independent of application validation policy; consumers validate the
retained commands against the current policy after expensive builds complete.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

EVIDENCE_ROOT = Path("/usr/local/share/mmltk/compiler-evidence")
LAUNCHERS = {"ccache", "sccache", "module_safe_compiler_launcher.sh"}

def require(condition, message):
    if not condition:
        raise ValueError(message)


def absolute(path, directory):
    candidate = Path(path)
    return (candidate if candidate.is_absolute() else directory / candidate).resolve()


def executable(name, directory, environment):
    if "/" in name:
        result = absolute(name, directory)
    else:
        found = shutil.which(name, path=environment.get("PATH", os.defpath))
        require(found, f"cannot resolve compiler/launcher {name}")
        result = Path(found).resolve()
    require(result.is_file() and os.access(result, os.X_OK),
            f"compiler/launcher is not executable: {result}")
    return result


def expand_arguments(arguments, directory, active=()):
    """NVCC option files and GNU response files both affect effective policy."""
    expanded = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        files = None
        if argument.startswith("@"):
            files = [argument[1:]]
        elif argument in {"--options-file", "-optf"}:
            index += 1
            require(index < len(arguments), f"missing value for {argument}")
            files = arguments[index].split(",")
        elif argument.startswith(("--options-file=", "-optf=")):
            files = argument.split("=", 1)[1].split(",")
        if files is None:
            expanded.append(argument)
        else:
            for name in files:
                path = absolute(name, directory)
                require(path not in active and len(active) < 32,
                        f"recursive response file: {path}")
                require(path.is_file(), f"missing response file: {path}")
                expanded.extend(expand_arguments(
                    shlex.split(path.read_text()), directory, (*active, path)))
        index += 1
    return expanded


def compiler_command(entry):
    directory = Path(entry["directory"]).resolve()
    arguments = expand_arguments(
        entry.get("arguments") or shlex.split(entry["command"]), directory)
    environment = dict(os.environ)
    index = 0
    if len(arguments) >= 3 and Path(arguments[0]).name == "cmake":
        require(arguments[1:3] == ["-E", "env"], "unsupported CMake compiler launcher")
        index = 3
    elif arguments and Path(arguments[0]).name == "env":
        index = 1
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--":
            index += 1
            break
        if argument in {"-u", "--unset"}:
            require(index + 1 < len(arguments), "missing env unset name")
            environment.pop(arguments[index + 1], None)
            index += 2
        elif argument.startswith("--unset="):
            environment.pop(argument.split("=", 1)[1], None)
            index += 1
        elif re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*=.*", argument, re.DOTALL):
            name, value = argument.split("=", 1)
            environment[name] = value
            index += 1
        else:
            break
    while index < len(arguments) and Path(arguments[index]).name in LAUNCHERS:
        executable(arguments[index], directory, environment)
        index += 1
    require(index < len(arguments), "missing compiler after launcher")
    compiler = executable(arguments[index], directory, environment)
    return compiler, arguments[index + 1:], directory, environment


def cmake_cache(build_dir):
    values = {}
    for line in (build_dir / "CMakeCache.txt").read_text().splitlines():
        match = re.match(r"([^#/:][^:]*):[^=]+=(.*)", line)
        if match:
            values[match[1]] = match[2]
    return values


def compiler_version(configured):
    return subprocess.check_output(
        [configured["CXX"], "-dumpfullversion", "-dumpversion"], text=True).strip()


def record_dependency(build_dir, name):
    """Retain expanded commands before dependency build directories are removed."""
    require(name in {"simdjson", "onnx", "cppcheck"}, f"unknown dependency {name}")
    cache = cmake_cache(build_dir)
    configured = {}
    for language in ("C", "CXX"):
        compiler = executable(cache[f"CMAKE_{language}_COMPILER"], build_dir, os.environ)
        configured[language] = str(compiler)
    commands = json.loads((build_dir / "compile_commands.json").read_text())
    require(commands, f"{name} has no compilation evidence")
    expanded = []
    for entry in commands:
        compiler, arguments, directory, _ = compiler_command(entry)
        expanded.append({
            "directory": str(directory),
            "file": str(absolute(entry["file"], directory)),
            "arguments": [str(compiler), *arguments],
        })
    EVIDENCE_ROOT.mkdir(parents=True, exist_ok=True)
    (EVIDENCE_ROOT / f"{name}.json").write_text(json.dumps({
        "gcc_version": compiler_version(configured),
        "configured_compilers": configured,
        "compile_commands": expanded,
    }, indent=2) + "\n")


def host_environment():
    configured = {}
    for variable, language in (("CC", "C"), ("CXX", "CXX")):
        require(os.environ.get(variable), f"missing dependency environment {variable}")
        compiler = executable(os.environ[variable], Path.cwd(), os.environ)
        configured[language] = str(compiler)
    EVIDENCE_ROOT.mkdir(parents=True, exist_ok=True)
    (EVIDENCE_ROOT / "cargo-tools.json").write_text(json.dumps({
        "gcc_version": compiler_version(configured),
        "configured_compilers": configured,
        "tools": ["trunk", "wasm-bindgen-cli", "cargo-dupes"],
    }, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--record-dependency", choices=("simdjson", "onnx", "cppcheck"))
    parser.add_argument("--record-host-environment", action="store_true")
    options = parser.parse_args()
    if options.record_host_environment:
        host_environment()
    else:
        require(options.build_dir and options.record_dependency, "dependency and build directory required")
        record_dependency(options.build_dir.resolve(), options.record_dependency)


if __name__ == "__main__":
    main()
