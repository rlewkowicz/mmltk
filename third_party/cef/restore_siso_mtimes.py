#!/usr/bin/env python3
"""Restore mtimes from Siso hashfs state for cache-restored CEF builds."""

from __future__ import annotations

import argparse
import hashlib
import os
import queue
import re
import stat
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path


_ENTRY_START = "entries: {"
_FIELD_RE = re.compile(r'^\s*(name|hash):\s+"(.*)"\s*$')
_INT_RE = re.compile(r"^\s*(mod_time|size_bytes):\s+([0-9]+)\s*$")
_NINJA_DEPS_HEADER = b"# ninjadeps\n"
_NINJA_DEPS_VERSION = 4
_NINJA_DEPS_DEPS_RECORD = 0x80000000
_NINJA_DEPS_SIZE_MASK = 0x7fffffff
_STOP = object()


@dataclass(frozen=True)
class SisoEntry:
  path: str
  digest: str
  size: int
  mtime_ns: int


@dataclass(frozen=True)
class RestoreEntry:
  path: str
  digest: str
  size: int
  mtime_ns: int
  source: str


class Counters:
  def __init__(self) -> None:
    self._lock = threading.Lock()
    self.scanned = 0
    self.eligible = 0
    self.ninja_records = 0
    self.ninja_matched = 0
    self.deps_records = 0
    self.deps_matched = 0
    self.already_current = 0
    self.restored = 0
    self.missing = 0
    self.changed = 0
    self.skipped = 0
    self.errors = 0

  def add(self, name: str, amount: int = 1) -> None:
    with self._lock:
      setattr(self, name, getattr(self, name) + amount)


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(
      description="Restore filesystem mtimes from Siso hashfs metadata.")
  parser.add_argument("--src-root", required=True,
                      help="Chromium source root, e.g. /work/.../src.")
  parser.add_argument("--build-dir", required=True,
                      help="Ninja/Siso output directory.")
  parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                      help="Parallel hash/utime workers. Defaults to nproc.")
  parser.add_argument("--require-state", action="store_true",
                      help="Fail if Siso state is missing or unreadable.")
  return parser.parse_args()


def decode_prototext_string(value: str) -> str:
  # Paths are emitted as protobuf text strings. This handles the common escaped
  # forms without depending on protobuf being installed in the build image.
  return bytes(value, "utf-8").decode("unicode_escape")


def iter_siso_entries(siso: Path, src_root: Path,
                      build_dir: Path) -> tuple[subprocess.Popen, object]:
  process = subprocess.Popen(
      [
          str(siso),
          "fs",
          "export",
          "-C",
          str(build_dir),
          "-format",
          "prototext",
      ],
      cwd=str(src_root),
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      text=True,
      encoding="utf-8",
      errors="replace")
  assert process.stdout is not None

  def iterator():
    in_entry = False
    depth = 0
    name = ""
    digest = ""
    size = -1
    mtime_ns = -1

    for line in process.stdout:
      stripped = line.strip()
      if not in_entry:
        if stripped == _ENTRY_START:
          in_entry = True
          depth = 1
          name = ""
          digest = ""
          size = -1
          mtime_ns = -1
        continue

      field_match = _FIELD_RE.match(line)
      if field_match:
        field, value = field_match.groups()
        if field == "name":
          name = decode_prototext_string(value)
        elif field == "hash":
          digest = value
        continue

      int_match = _INT_RE.match(line)
      if int_match:
        field, value = int_match.groups()
        if field == "mod_time":
          mtime_ns = int(value)
        elif field == "size_bytes":
          size = int(value)

      if stripped.endswith("{"):
        depth += 1
      elif stripped == "}":
        depth -= 1
        if depth == 0:
          in_entry = False
          if name and digest and size >= 0 and mtime_ns >= 0:
            yield SisoEntry(name, digest, size, mtime_ns)

  return process, iterator()


def sha256_file(path: str) -> str:
  digest = hashlib.sha256()
  with open(path, "rb", buffering=1024 * 1024) as fp:
    for chunk in iter(lambda: fp.read(1024 * 1024), b""):
      digest.update(chunk)
  return digest.hexdigest()


def iter_ninja_log_mtimes(build_dir: Path) -> object:
  ninja_log = build_dir / ".ninja_log"
  with open(ninja_log, "r", encoding="utf-8", errors="replace") as fp:
    header = fp.readline().strip()
    if header != "# ninja log v5":
      raise RuntimeError(f"unsupported ninja log header: {header}")

    for line in fp:
      fields = line.rstrip("\n").split("\t")
      if len(fields) < 5:
        continue
      output = fields[3]
      if output == "":
        continue
      try:
        mtime_ns = int(fields[2])
      except ValueError:
        continue
      yield os.path.normpath(str(build_dir / output)), mtime_ns


def read_exact(fp, size: int) -> bytes:
  data = fp.read(size)
  if len(data) != size:
    raise RuntimeError("truncated .siso_deps")
  return data


def iter_ninja_deps_mtimes(build_dir: Path) -> object:
  deps_log = build_dir / ".siso_deps"
  with open(deps_log, "rb") as fp:
    header = read_exact(fp, len(_NINJA_DEPS_HEADER))
    if header != _NINJA_DEPS_HEADER:
      raise RuntimeError("unsupported deps log header")

    version = struct.unpack("<I", read_exact(fp, 4))[0]
    if version != _NINJA_DEPS_VERSION:
      raise RuntimeError(f"unsupported deps log version: {version}")

    paths: list[str] = []
    while True:
      raw_size = fp.read(4)
      if raw_size == b"":
        break
      if len(raw_size) != 4:
        raise RuntimeError("truncated .siso_deps record size")

      record_size = struct.unpack("<I", raw_size)[0]
      is_deps_record = (record_size & _NINJA_DEPS_DEPS_RECORD) != 0
      payload_size = record_size & _NINJA_DEPS_SIZE_MASK
      payload = read_exact(fp, payload_size)

      if not is_deps_record:
        if payload_size < 4:
          continue
        path_bytes = payload[:-4].split(b"\0", 1)[0]
        paths.append(path_bytes.decode("utf-8", errors="replace"))
        continue

      if payload_size < 12:
        continue
      output_id, mtime_ns = struct.unpack_from("<iq", payload, 0)
      if output_id < 0 or output_id >= len(paths):
        continue
      output = paths[output_id]
      if output == "":
        continue
      yield os.path.normpath(str(build_dir / output)), mtime_ns


def worker(entries: "queue.Queue[RestoreEntry | object]",
           counters: Counters) -> None:
  while True:
    item = entries.get()
    try:
      if item is _STOP:
        return
      assert isinstance(item, RestoreEntry)
      try:
        st = os.stat(item.path, follow_symlinks=False)
      except FileNotFoundError:
        counters.add("missing")
        continue
      except OSError:
        counters.add("errors")
        continue

      if not stat.S_ISREG(st.st_mode):
        counters.add("skipped")
        continue
      if st.st_size != item.size:
        counters.add("changed")
        continue
      if st.st_mtime_ns == item.mtime_ns:
        counters.add("already_current")
        continue
      try:
        if sha256_file(item.path) != item.digest:
          counters.add("changed")
          continue
        os.utime(
            item.path,
            ns=(st.st_atime_ns, item.mtime_ns),
            follow_symlinks=False)
      except OSError:
        counters.add("errors")
        continue
      counters.add("restored")
    finally:
      entries.task_done()


def main() -> int:
  args = parse_args()
  jobs = max(1, args.jobs)
  src_root = Path(args.src_root).resolve()
  build_dir = Path(args.build_dir).resolve()
  siso = src_root / "third_party" / "siso" / "cipd" / "siso"
  fs_state = build_dir / ".siso_fs_state"
  ninja_log = build_dir / ".ninja_log"
  deps_log = build_dir / ".siso_deps"

  if not siso.is_file():
    print(f"restore_siso_mtimes.py: missing Siso binary: {siso}",
          file=sys.stderr)
    return 1 if args.require_state else 0
  if not fs_state.is_file():
    print(f"restore_siso_mtimes.py: missing Siso state: {fs_state}",
          file=sys.stderr)
    return 1 if args.require_state else 0
  if not ninja_log.is_file():
    print(f"restore_siso_mtimes.py: missing Ninja log: {ninja_log}",
          file=sys.stderr)
    return 1 if args.require_state else 0
  if not deps_log.is_file():
    print(f"restore_siso_mtimes.py: missing Siso deps log: {deps_log}",
          file=sys.stderr)
    return 1 if args.require_state else 0

  source_prefix = str(src_root) + os.sep
  build_prefix = str(build_dir) + os.sep
  counters = Counters()
  start = time.monotonic()
  siso_by_path: dict[str, SisoEntry] = {}

  process, siso_entries = iter_siso_entries(siso, src_root, build_dir)
  try:
    for entry in siso_entries:
      counters.add("scanned")
      path = os.path.normpath(entry.path)
      if not path.startswith(source_prefix):
        counters.add("skipped")
        continue
      counters.add("eligible")
      siso_by_path[path] = SisoEntry(path, entry.digest, entry.size,
                                     entry.mtime_ns)
  finally:
    assert process.stdout is not None
    process.stdout.close()

  stderr = ""
  if process.stderr is not None:
    stderr = process.stderr.read()
    process.stderr.close()
  returncode = process.wait()
  if returncode != 0:
    sys.stderr.write(stderr)
    return returncode

  restore_by_path = {
      path: RestoreEntry(entry.path, entry.digest, entry.size, entry.mtime_ns,
                         "siso")
      for path, entry in siso_by_path.items()
  }
  for path, mtime_ns in iter_ninja_log_mtimes(build_dir):
    counters.add("ninja_records")
    if not path.startswith(build_prefix):
      continue
    siso_entry = siso_by_path.get(path)
    if siso_entry is None:
      continue
    counters.add("ninja_matched")
    restore_by_path[path] = RestoreEntry(
        siso_entry.path,
        siso_entry.digest,
        siso_entry.size,
        mtime_ns,
        "ninja")

  for path, mtime_ns in iter_ninja_deps_mtimes(build_dir):
    counters.add("deps_records")
    if not path.startswith(build_prefix):
      continue
    siso_entry = siso_by_path.get(path)
    if siso_entry is None:
      continue
    counters.add("deps_matched")
    restore_by_path[path] = RestoreEntry(
        siso_entry.path,
        siso_entry.digest,
        siso_entry.size,
        mtime_ns,
        "deps")

  entries: "queue.Queue[RestoreEntry | object]" = queue.Queue(
      maxsize=jobs * 32)
  threads = [
      threading.Thread(target=worker, args=(entries, counters), daemon=True)
      for _ in range(jobs)
  ]
  for thread in threads:
    thread.start()
  for entry in restore_by_path.values():
    entries.put(entry)
  for _ in threads:
    entries.put(_STOP)
  entries.join()
  for thread in threads:
    thread.join()

  elapsed = time.monotonic() - start

  print(
      "restore_siso_mtimes.py: "
      f"jobs={jobs} scanned={counters.scanned} eligible={counters.eligible} "
      f"ninja_records={counters.ninja_records} "
      f"ninja_matched={counters.ninja_matched} "
      f"deps_records={counters.deps_records} "
      f"deps_matched={counters.deps_matched} "
      f"restored={counters.restored} already={counters.already_current} "
      f"changed={counters.changed} missing={counters.missing} "
      f"skipped={counters.skipped} errors={counters.errors} "
      f"elapsed={elapsed:.3f}s",
      flush=True)

  if counters.errors:
    return 1
  return 0


if __name__ == "__main__":
  sys.exit(main())
