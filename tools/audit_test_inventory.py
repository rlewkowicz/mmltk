#!/usr/bin/env python3

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


TEST_SUFFIXES = (".test.c", ".test.cc", ".test.cpp", ".test.cxx", ".test.cu")
TARGET_PATTERN = re.compile(r"(?:^|/)CMakeFiles/([^/]+)\.dir/")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Audit tracked native test sources, configured CMake targets, and the mmltk all-suite inventory."
    )
    parser.add_argument("--compile-database", required=True, type=Path)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--expected-targets", required=True)
    return parser.parse_args()


def tracked_test_sources(repo_root: Path) -> set[str]:
    tracked = subprocess.check_output(
        ["git", "-C", str(repo_root), "ls-files", "-z", "--", "src"]
    )
    return {
        path
        for raw_path in tracked.split(b"\0")
        if raw_path
        for path in [raw_path.decode("utf-8")]
        if path.endswith(TEST_SUFFIXES)
    }


def repo_relative(path: str, repo_root: Path) -> str | None:
    try:
        return Path(path).resolve().relative_to(repo_root).as_posix()
    except ValueError:
        return None


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    expected_targets = set(args.expected_targets.split())
    test_sources = tracked_test_sources(repo_root)
    source_targets: dict[str, set[str]] = defaultdict(set)
    graph_targets: set[str] = set()

    with args.compile_database.open(encoding="utf-8") as handle:
        compile_commands = json.load(handle)

    for command in compile_commands:
        output = command.get("output", "")
        target_match = TARGET_PATTERN.search(output)
        if target_match is None:
            continue
        target = target_match.group(1)
        graph_targets.add(target)
        source = repo_relative(command["file"], repo_root)
        if source in test_sources:
            source_targets[source].add(target)

    missing_sources = sorted(test_sources - source_targets.keys())
    multiply_owned_sources = sorted(
        (source, sorted(targets)) for source, targets in source_targets.items() if len(targets) != 1
    )
    discovered_test_targets = {next(iter(targets)) for targets in source_targets.values() if len(targets) == 1}
    missing_all_targets = sorted(discovered_test_targets - expected_targets)
    stale_all_targets = sorted(expected_targets - graph_targets)
    all_targets_without_test_sources = sorted(expected_targets - discovered_test_targets)

    findings: list[str] = []
    if missing_sources:
        findings.append(
            "tracked test sources missing from the configured compile graph:\n  " + "\n  ".join(missing_sources)
        )
    if multiply_owned_sources:
        findings.append(
            "test sources without exactly one owning CMake target:\n  "
            + "\n  ".join(f"{source}: {', '.join(targets)}" for source, targets in multiply_owned_sources)
        )
    if missing_all_targets:
        findings.append(
            "configured test targets missing from the mmltk all-suite inventory:\n  "
            + "\n  ".join(missing_all_targets)
        )
    if stale_all_targets:
        findings.append(
            "mmltk all-suite targets missing from the configured CMake graph:\n  " + "\n  ".join(stale_all_targets)
        )
    if all_targets_without_test_sources:
        findings.append(
            "mmltk all-suite targets without a tracked *.test source:\n  "
            + "\n  ".join(all_targets_without_test_sources)
        )

    if findings:
        print("mmltk-test-inventory: FAILED", file=sys.stderr)
        for finding in findings:
            print(f"mmltk-test-inventory: {finding}", file=sys.stderr)
        return 1

    print(
        "mmltk-test-inventory: "
        f"{len(test_sources)} tracked test sources map one-to-one into "
        f"{len(discovered_test_targets)} configured all-suite executables"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
