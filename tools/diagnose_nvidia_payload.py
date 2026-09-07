#!/usr/bin/env python3
"""Report public NVIDIA header locations without changing the inspected image."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, "/diagnostic_metadata")
from nvidia_payload import load_manifest, payload_file


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("headers", nargs="*")
    parser.add_argument("--header-list")
    args = parser.parse_args()
    if args.header_list:
        args.headers.extend(args.header_list.split(","))
    if not args.headers:
        parser.error("at least one public header filename is required")
    if any(re.fullmatch(r"[A-Za-z0-9_.-]+", name) is None for name in args.headers):
        parser.error("headers must be public filenames, without directory paths")
    wanted = set(args.headers)
    manifest = load_manifest("/diagnostic_metadata/nvidia-payload.json")
    memory = {}
    memory_limit = Path("/sys/fs/cgroup/memory.max")
    for line in Path("/proc/meminfo").read_text().splitlines():
        key, value = line.split(":", 1)
        if key in ("MemTotal", "MemAvailable"):
            memory[key] = int(value.split()[0]) * 1024
    records = {name: [] for name in sorted(wanted)}
    for root in ("/usr", "/opt"):
        for directory, _, files in os.walk(root):
            for name in wanted.intersection(files):
                path = Path(directory) / name
                resolved = path.resolve(strict=True)
                package = subprocess.run(
                    ["dpkg-query", "-S", str(resolved)], capture_output=True, text=True, check=False)
                records[name].append({
                    "path": str(path),
                    "resolved": str(resolved),
                    "development_selected": payload_file(manifest, resolved, "development"),
                    "package": package.stdout.strip(),
                    "includes": re.findall(r"^\s*#\s*include\s*[<\"]([^>\"]+)", path.read_text(), re.MULTILINE),
                })
    print(json.dumps({
        "image": os.environ["MMLTK_DIAGNOSTIC_IMAGE"],
        "image_id": os.environ.get("MMLTK_DIAGNOSTIC_IMAGE_ID"),
        "available_cpus": len(os.sched_getaffinity(0)),
        "host_memory_bytes": memory,
        "container_memory_limit": memory_limit.read_text().strip() if memory_limit.is_file() else None,
        "headers": records,
    }, sort_keys=True))


if __name__ == "__main__":
    main()
