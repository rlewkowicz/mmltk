#!/usr/bin/env python3
"""Validate the effective container compilation policy, including cached graphs."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tomllib


# The recorder is installed beside this checker in images and lives in docker/
# when the current checker is invoked from the bind-mounted repository.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "docker"))
from record_compiler_evidence import (EVIDENCE_ROOT, absolute, cmake_cache,
                                      compiler_command, executable, expand_arguments,
                                      require)

GCC_ROOT = Path("/opt/gcc-16.2")
_manifest_path = Path(__file__).resolve().parent.parent / "docker/nvidia-payload.json"
if _manifest_path.is_file():
    _manifest = json.loads(_manifest_path.read_text())
else:
    _manifest = json.loads(Path("/usr/share/mmltk/nvidia-payload.json").read_text())["manifest"]
CUDA_ROOT = Path(_manifest["cuda_root"])
CUDA_VERSION = _manifest["versions"]["cuda"].rsplit(".", 1)[0]
CXX_SUFFIXES = {".cc", ".cpp", ".cxx", ".c++", ".C", ".cppm", ".ixx"}
REFLECTION_OPTIONS = {"-freflection", "-fno-reflection"}


def option_values(arguments, names):
    values = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument in names:
            index += 1
            require(index < len(arguments), f"missing value for {argument}")
            values.append(arguments[index])
        else:
            for name in names:
                if argument.startswith(name + "="):
                    values.append(argument[len(name) + 1:])
                    break
        index += 1
    return values


class CompilerPolicy:
    def __init__(self):
        self.versions = {}
        self.nasm_compiler = None

    def version(self, compiler, cuda=False):
        key = (compiler, cuda)
        if key not in self.versions:
            arguments = ["--version"] if cuda else ["-dumpfullversion", "-dumpversion"]
            self.versions[key] = subprocess.check_output(
                [str(compiler), *arguments], text=True, stderr=subprocess.STDOUT).strip()
        return self.versions[key]

    def gcc(self, compiler, language):
        expected = GCC_ROOT / "bin" / ("gcc" if language == "C" else "g++")
        require(compiler == expected.resolve(),
                f"{language} requires {expected}, found {compiler}")
        require(self.version(compiler) == "16.2.0",
                f"{compiler} is not GCC 16.2.0")

    def nvcc(self, compiler):
        require(compiler == (CUDA_ROOT / "bin/nvcc").resolve(),
                f"CUDA requires {CUDA_ROOT}/bin/nvcc, found {compiler}")
        require(re.search(r"\brelease " + re.escape(CUDA_VERSION) + ",", self.version(compiler, cuda=True)),
                f"{compiler} is not NVCC {CUDA_VERSION}")

    def cache(self, build_dir):
        values = cmake_cache(build_dir)
        expected = {
            "CMAKE_CUDA_STANDARD": "23",
            "CMAKE_CUDA_STANDARD_REQUIRED": "ON",
            "CMAKE_CUDA_EXTENSIONS": "OFF",
            "MMLTK_CUDA_VERSION": CUDA_VERSION,
        }
        for name, value in expected.items():
            require(values.get(name) == value,
                    f"configured {name} must be {value}, found {values.get(name)}")
        for name in ("CUDAToolkit_ROOT", "MMLTK_CUDA_TOOLKIT_ROOT"):
            require(absolute(values.get(name, ""), build_dir) == CUDA_ROOT.resolve(),
                    f"configured {name} must resolve to {CUDA_ROOT}")
        for name, language in (("CMAKE_C_COMPILER", "C"),
                               ("CMAKE_CXX_COMPILER", "CXX"),
                               ("CMAKE_CUDA_HOST_COMPILER", "CXX")):
            self.gcc(executable(values.get(name, ""), build_dir, os.environ), language)
        self.nvcc(executable(values.get("CMAKE_CUDA_COMPILER", ""), build_dir, os.environ))
        if "CMAKE_ASM_NASM_COMPILER" in values:
            self.nasm_compiler = executable(
                values["CMAKE_ASM_NASM_COMPILER"], build_dir, os.environ)
            require(self.nasm_compiler == Path("/usr/bin/nasm").resolve(),
                    f"ASM_NASM requires /usr/bin/nasm, found {self.nasm_compiler}")
        require("--allow-unsupported-compiler" in shlex.split(values.get("CMAKE_CUDA_FLAGS", "")),
                "configured CUDA flags lack --allow-unsupported-compiler")

    def command(self, entry, repo_root, first_party_targets=(), dependency=None):
        compiler, arguments, directory, environment = compiler_command(entry)
        source = absolute(entry["file"], directory)
        standards = option_values(arguments, {"-std", "--std"})
        is_cuda = source.suffix == ".cu" or compiler.name == "nvcc"
        if is_cuda:
            require(source.suffix in CXX_SUFFIXES or source.suffix in {".c", ".cu"},
                    f"unsupported CUDA compilation category: {source}")
            self.nvcc(compiler)
            for name in ("NVCC_PREPEND_FLAGS", "NVCC_APPEND_FLAGS", "NVCC_CCBIN"):
                require(not environment.get(name), f"ambient {name} obscures configured CUDA policy")
            require(standards and all(value == "c++23" for value in standards),
                    f"CUDA requires only -std=c++23, found {standards}")
            require("--allow-unsupported-compiler" in arguments or
                    "-allow-unsupported-compiler" in arguments,
                    "CUDA command lacks unsupported-host override")
            hosts = option_values(arguments, {"-ccbin", "--compiler-bindir"})
            require(hosts, "CUDA command lacks explicit host compiler")
            for host in hosts:
                host_path = absolute(host, directory)
                if host_path.is_dir():
                    host_path /= "g++"
                    host = str(host_path)
                self.gcc(executable(host, directory, environment), "CXX")
            forwarded = []
            for value in option_values(arguments, {"-Xcompiler", "--compiler-options"}):
                for part in shlex.split(value):
                    forwarded.extend(part.split(","))
            forwarded = expand_arguments(forwarded, directory)
            require(not any(flag.split("=", 1)[0] in REFLECTION_OPTIONS
                            for flag in [*arguments, *forwarded]),
                    "CUDA must not receive reflection flags")
            require("-ansi" not in arguments and "-ansi" not in forwarded,
                    "CUDA must not override C++23 with -ansi")
            host_standards = option_values(forwarded, {"-std", "--std"})
            require(all(value == "c++23" for value in host_standards),
                    f"CUDA forwards a conflicting host dialect: {host_standards}")
            return "CUDA"
        if source.suffix == ".asm":
            require(source.is_relative_to(
                repo_root / "third_party/rapidgzip/src/external/isa-l"),
                f"unsupported assembly source: {source}")
            require(self.nasm_compiler is not None and compiler == self.nasm_compiler,
                    f"ISA-L assembly requires configured NASM, found {compiler}")
            return "ASM_NASM"
        # Foreign sources may retain their own dialect; they share the exact host
        # compiler. GCC 14 is permitted only in the separate GCC bootstrap image.
        require(source.suffix in CXX_SUFFIXES or source.suffix == ".c",
                f"unsupported compilation category: {source}")
        language = "CXX" if source.suffix in CXX_SUFFIXES else "C"
        self.gcc(compiler, language)
        first_party = source.is_relative_to(repo_root) and not source.is_relative_to(repo_root / "third_party")
        outputs = option_values(arguments, {"-o", "--output-file"})
        if entry.get("output"):
            outputs.append(entry["output"])
        for output in outputs:
            parts = Path(output).parts
            first_party |= any(
                parts[index] == "CMakeFiles" and parts[index + 1] in first_party_targets
                for index in range(len(parts) - 1))
        if language == "CXX" and (first_party or dependency in {"onnx", "simdjson"}):
            owner = "first-party C++" if first_party else f"{dependency} C++"
            require(standards and standards[-1] in {"c++26", "c++2c"},
                    f"{owner} requires C++26 without extensions, found {standards}")
            require("-ansi" not in arguments, f"{owner} must not override C++26 with -ansi")
        if language == "CXX" and first_party:
            reflection = [flag for flag in arguments if flag in REFLECTION_OPTIONS]
            require(reflection and reflection[-1] == "-freflection",
                    "first-party C++ requires effective -freflection")
        return language


def check_dependency_evidence(policy):
    for name in ("cargo-tools", "simdjson", "onnx", "cppcheck"):
        path = EVIDENCE_ROOT / f"{name}.json"
        evidence = json.loads(path.read_text())
        require(evidence["gcc_version"] == "16.2.0", f"wrong compiler evidence: {path}")
        for language in ("C", "CXX"):
            policy.gcc(Path(evidence["configured_compilers"][language]), language)
        if name != "cargo-tools":
            require(evidence["compile_commands"], f"empty dependency evidence: {path}")
            for entry in evidence["compile_commands"]:
                policy.command(entry, Path("/workspace"), dependency=name)


def check_dependency_declarations(policy, repo_root):
    """Keep GCC 14 selection confined to the independently built GCC bootstrap."""
    for path in (repo_root / "docker").glob("Dockerfile*"):
        if path.name == "Dockerfile.gcc":
            continue
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            require(not re.search(r"\b(?:gcc|g\+\+|cc|c\+\+)-14\b", line),
                    f"GCC 14 outside bootstrap: {path}:{number}")
    cargo_config = repo_root / "src/frontend/iced/.cargo/config.toml"
    with cargo_config.open("rb") as stream:
        cargo = tomllib.load(stream)
    linker = cargo["target"]["x86_64-unknown-linux-gnu"]["linker"]
    try:
        policy.gcc(executable(linker, cargo_config.parent, os.environ), "C")
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise ValueError(f"{cargo_config}: Linux linker={linker!r}: {error}") from error


def pch_environment(arguments, source, directory):
    """Retain every option except the mechanics of creating/using the artifact."""
    result = []
    index = 0
    paired = {"-o", "--output-file", "-MF", "-MT", "-MQ", "-x", "-include"}
    mechanics = {"-c", "-MD", "-MMD", "-MP", "-fpch-preprocess",
                 "-fpch-instantiate-templates", "-Winvalid-pch"}
    while index < len(arguments):
        argument = arguments[index]
        if argument in paired:
            index += 2
            continue
        is_source = not argument.startswith("-") and absolute(argument, directory) == source
        if argument not in mechanics and not is_source:
            result.append(argument)
        index += 1
    return result


def check_precompiled_headers(entries, first_party_targets):
    """Compare generated creation/use against the owning CMake registrations."""
    owners = {}
    for entry in entries:
        compiler, arguments, directory, environment = compiler_command(entry)
        source = absolute(entry["file"], directory)
        outputs = option_values(arguments, {"-o", "--output-file"})
        require(entry.get("output") or outputs, f"missing compile output: {source}")
        output = absolute(entry.get("output") or outputs[-1], directory)
        owner = next((parent for parent in output.parents
                      if parent.name.endswith(".dir") and
                      parent.parent.name == "CMakeFiles"), None)
        if owner is None or owner.name not in first_party_targets:
            continue
        includes = option_values(arguments, {"-include", "-include-pch", "-imacros"})
        pch_includes = [absolute(value, directory) for value in includes
                        if Path(value).name.startswith("cmake_pch.")]
        creation = output.suffix == ".gch"
        policy_path = owner / "mmltk-pch-policy.txt"
        if "header-isolation" in source.parts:
            require(not includes and not creation,
                    f"header isolation must remain unforced and PCH-free: {source}")
        excluded = (source.suffix in {".c", ".cu", ".cppm", ".ixx"} or
                    any(arg.startswith(("-fmodules", "-fmodule-mapper"))
                        for arg in arguments))
        require(not excluded or (not creation and not pch_includes),
                f"excluded compilation received a PCH: {source}")
        if not policy_path.is_file():
            require(not creation and not pch_includes,
                    f"PCH without an owning registration: {source}")
            continue
        state = owners.setdefault(owner, {"creations": [], "uses": {}})
        if not creation and not pch_includes:
            continue
        require(len(pch_includes) == 1,
                f"expected one target-local PCH include for {source}: {pch_includes}")
        header = pch_includes[0]
        require(header.parent == owner and header.name == "cmake_pch.hxx",
                f"cross-target or unexpected PCH artifact: {header}")
        require("-Werror=invalid-pch" in arguments and "-Winvalid-pch" in arguments
                and "-Wno-invalid-pch" not in arguments
                and "-Wno-error=invalid-pch" not in arguments,
                f"invalid-PCH diagnostics must be fatal: {source}")
        signature = (compiler, pch_environment(arguments, source, directory),
                     environment)
        if creation:
            require(source.name == "cmake_pch.hxx.cxx" and
                    output == Path(str(header) + ".gch") and
                    option_values(arguments, {"-x"}) == ["c++-header"],
                    f"unexpected PCH creation command: {source}")
            state["creations"].append((header, signature))
        else:
            require(source not in state["uses"], f"duplicate PCH use: {source}")
            state["uses"][source] = signature
    for owner, state in owners.items():
        rows = [line.split("\t", 1) for line in
                (owner / "mmltk-pch-policy.txt").read_text().splitlines()]
        expected = {Path(value) for kind, value in rows if kind == "use"}
        excluded = {Path(value) for kind, value in rows if kind == "skip"}
        headers = [Path(value) for kind, value in rows if kind == "header"]
        require(len(state["creations"]) == 1,
                f"expected one PCH creation for {owner}")
        header, signature = state["creations"][0]
        require(set(state["uses"]) == expected and len(expected) >= 2,
                f"PCH consumption differs from registration for {owner}: "
                f"missing={expected - state['uses'].keys()}, "
                f"extra={state['uses'].keys() - expected}")
        require(not (excluded & state["uses"].keys()),
                f"excluded source consumes a PCH for {owner}")
        require(all(value == signature for value in state["uses"].values()),
                f"PCH compiler, options, definitions or environment differ for {owner}")
        actual_headers = [Path(value) for value in re.findall(
            r'^\s*#include "([^"]+)"', header.read_text(), flags=re.MULTILINE)]
        require(actual_headers == headers,
                f"generated PCH groups differ from registration for {owner}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--repo-root", type=Path)
    parser.add_argument("--dependencies-only", action="store_true")
    options = parser.parse_args()
    policy = CompilerPolicy()
    if options.dependencies_only:
        check_dependency_evidence(policy)
        return
    require(options.build_dir, "--build-dir is required")
    build_dir = options.build_dir.resolve()
    require(options.repo_root, "--repo-root is required")
    repo_root = options.repo_root.resolve()
    policy.cache(build_dir)
    check_dependency_declarations(policy, repo_root)
    check_dependency_evidence(policy)
    entries = json.loads((build_dir / "compile_commands.json").read_text())
    require(entries, "empty compile database")
    first_party_targets = {
        f"{name}.dir" for name in
        (build_dir / "mmltk-first-party-targets.txt").read_text().splitlines() if name
    }
    require(first_party_targets, "missing registered first-party targets")
    counts = {"C": 0, "CXX": 0, "CUDA": 0, "ASM_NASM": 0}
    for entry in entries:
        try:
            counts[policy.command(entry, repo_root, first_party_targets)] += 1
        except (ValueError, OSError, subprocess.CalledProcessError) as error:
            raise ValueError(f"{entry.get('file', '<missing source>')}: {error}") from error
    check_precompiled_headers(entries, first_party_targets)
    print(f"mmltk: compiler invariants verified: {json.dumps(counts, sort_keys=True)}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        sys.exit(f"mmltk: compiler invariant failure: {error}")
