#!/usr/bin/env python3
"""Build the pinned Trunk with a narrowly patched native dependency."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib


def source_copy(package, destination):
    subprocess.run(["cargo", "info", package], check=True)
    name, version = package.split("@")
    sources = list((Path(os.environ["CARGO_HOME"]) / "registry/src").glob(f"*/{name}-{version}"))
    if len(sources) != 1:
        raise RuntimeError(f"missing or ambiguous Cargo source for {package}: {sources}")
    shutil.copytree(sources[0], destination)


def main():
    trunk = Path("/tmp/mmltk-trunk")
    dependency = Path("/tmp/mmltk-libdeflate-sys")
    source_copy(f"trunk@{sys.argv[1]}", trunk)
    source_copy("libdeflate-sys@1.23.1", dependency)
    with Path("/tmp/libdeflate-gcc16.patch").open("rb") as patch:
        subprocess.run(["patch", "--batch", "--fuzz=0", "-p1"], cwd=dependency,
                       stdin=patch, check=True)
    subprocess.run(["cargo", "fetch", "--locked"], cwd=trunk, check=True)
    lock = trunk / "Cargo.lock"
    original = tomllib.loads(lock.read_text())
    with (trunk / "Cargo.toml").open("a") as manifest:
        manifest.write(f'\n[patch.crates-io]\nlibdeflate-sys = {{ path = "{dependency}" }}\n')
    subprocess.run(["cargo", "update", "--offline", "-p", "libdeflate-sys"],
                   cwd=trunk, check=True)
    expected = original
    selected = [package for package in expected["package"] if package["name"] == "libdeflate-sys"]
    if len(selected) != 1 or selected[0]["version"] != "1.23.1":
        raise RuntimeError("Trunk no longer locks the patched libdeflate-sys version")
    selected[0].pop("source")
    selected[0].pop("checksum")
    if tomllib.loads(lock.read_text()) != expected:
        raise RuntimeError("Trunk patch changed other locked dependencies")
    subprocess.run(["cargo", "install", "--path", str(trunk), "--locked"], check=True)
    shutil.rmtree(trunk)
    shutil.rmtree(dependency)


if __name__ == "__main__":
    main()
