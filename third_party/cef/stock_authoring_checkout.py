#!/usr/bin/env python3
"""Prepare the cached CEF/Chromium checkout for patch authoring.

This tool intentionally resets only source worktrees and patch-created target
paths. It leaves build outputs, siso/ninja state, depot_tools downloads,
artifacts, and dependency caches in place.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from typing import Iterable


SAFE_DIR_SUFFIXES = (
    "code/depot_tools",
    "code/chromium_git/cef",
    "code/chromium_git/chromium/src",
    "code/chromium_git/chromium/src/cef",
    "code/chromium_git/chromium/src/third_party/angle",
    "code/chromium_git/chromium/src/third_party/depot_tools",
    "code/chromium_git/chromium/src/v8",
)

BUILD_STATE_SCHEMA = 2
BUILD_STATE_PATH = ".mmltk-cef-build-state.json"
BUILD_ENV_KEYS = (
    "CEF_INSTALL_SYSROOT",
    "GN_ARGUMENTS",
    "GN_DEFINES",
    "MMLTK_CEF_PATCH_ROOT",
    "MMLTK_CEF_BUILD_TARGET",
    "MMLTK_CEF_BUILD_ARCH",
)
GN_INPUT_SUFFIXES = (".gn", ".gni", ".mojom")
GN_INPUT_NAMES = {
    "BUILD.gn",
    "DEPS",
    "args.gn",
    "gn_args.py",
    "gclient_hook.py",
    "cef_create_projects.sh",
    "libcef.lst",
}
BUILD_GRAPH_FILES = (
    "out/Debug_GN_x64/args.gn",
    "out/Debug_GN_x64/build.ninja",
)


class CommandError(RuntimeError):
  pass


def run(args: list[str],
        cwd: Path | None = None,
        *,
        check: bool = True,
        capture: bool = False,
        input_bytes: bytes | None = None) -> subprocess.CompletedProcess:
  cwd_text = str(cwd) if cwd is not None else os.getcwd()
  print(f"+ ({cwd_text}) {' '.join(args)}", flush=True)
  stdout = subprocess.PIPE if capture else None
  stderr = subprocess.PIPE if capture else None
  result = subprocess.run(
      args,
      cwd=cwd,
      input=input_bytes,
      stdout=stdout,
      stderr=stderr,
      check=False)
  if check and result.returncode != 0:
    message = f"command failed with exit code {result.returncode}: {' '.join(args)}"
    if capture:
      out = result.stdout.decode("utf-8", errors="replace")
      err = result.stderr.decode("utf-8", errors="replace")
      message = f"{message}\nstdout:\n{out}\nstderr:\n{err}"
    raise CommandError(message)
  return result


def git_capture(cwd: Path, *args: str) -> str:
  result = run(["git", *args], cwd, capture=True)
  return result.stdout.decode("utf-8", errors="replace").strip()


def git_ok(cwd: Path, *args: str) -> bool:
  return run(["git", *args], cwd, check=False, capture=True).returncode == 0


def sha256_file(path: Path) -> str:
  digest = hashlib.sha256()
  with path.open("rb") as input_file:
    for block in iter(lambda: input_file.read(1024 * 1024), b""):
      digest.update(block)
  return digest.hexdigest()


def checkout_paths(build_root: Path) -> tuple[Path, Path, Path]:
  chromium_src = build_root / "code/chromium_git/chromium/src"
  cef_checkout = build_root / "code/chromium_git/cef"
  cef_src = chromium_src / "cef"
  return chromium_src, cef_checkout, cef_src


def parse_buildcef_vars(repo_root: Path) -> tuple[str, str]:
  buildcef = repo_root / "buildcef.sh"
  text = buildcef.read_text(encoding="utf-8")

  def parse_var(name: str) -> str:
    match = re.search(rf"^\s*{re.escape(name)}=\"([^\"]+)\"", text,
                      re.MULTILINE)
    if not match:
      raise CommandError(f"could not parse {name} from {buildcef}")
    return match.group(1)

  return parse_var("cef_branch"), parse_var("chromium_tag")


def configure_safe_directories(build_root: Path) -> None:
  for suffix in SAFE_DIR_SUFFIXES:
    safe_dir = build_root / suffix
    if safe_dir.exists():
      run(["git", "config", "--global", "--add", "safe.directory",
           str(safe_dir)])


def require_checkout(path: Path, label: str) -> None:
  if not (path / ".git").exists():
    raise CommandError(f"{label} is not a git checkout: {path}")


def resolve_ref(checkout: Path, ref: str, label: str) -> str:
  try:
    return git_capture(checkout, "rev-parse", "--verify", f"{ref}^{{commit}}")
  except CommandError as exc:
    raise CommandError(f"{label} ref is not available in {checkout}: {ref}") from exc


def reset_git_checkout(checkout: Path, ref: str, label: str) -> str:
  desired = resolve_ref(checkout, ref, label)
  print(f"Resetting {label} to {ref} ({desired})", flush=True)
  run(["git", "checkout", "--detach", "--force", desired], checkout)
  run(["git", "reset", "--hard", desired], checkout)
  actual = git_capture(checkout, "rev-parse", "HEAD")
  if actual != desired:
    raise CommandError(f"{label} reset mismatch: expected {desired}, got {actual}")
  return desired


def load_patch_config(repo_root: Path) -> list[dict[str, str]]:
  patch_cfg = repo_root / "third_party/cef/patch/patch.cfg"
  scope: dict[str, object] = {}
  code = compile(patch_cfg.read_bytes(), str(patch_cfg), "exec")
  exec(code, {"__builtins__": {}}, scope)
  patches = scope.get("patches")
  if not isinstance(patches, list):
    raise CommandError(f"{patch_cfg} did not define a patches list")
  return patches


def patch_file_for(repo_root: Path, cef_src: Path,
                   patch_name: str) -> tuple[Path, bool]:
  local_path = repo_root / "third_party/cef/patch/patches" / f"{patch_name}.patch"
  if local_path.is_file():
    return local_path, True

  upstream_path = cef_src / "patch/patches" / f"{patch_name}.patch"
  if upstream_path.is_file():
    return upstream_path, False

  raise CommandError(f"patch file not found for {patch_name}")


def normalize_patch_path(path: str) -> str:
  path = path.split("\t", 1)[0].strip()
  if path.startswith("a/") or path.startswith("b/"):
    path = path[2:]
  return path


def patch_targets(patch_path: Path) -> list[str]:
  targets: list[str] = []
  for line in patch_path.read_text(encoding="utf-8").splitlines():
    if not (line.startswith("+++ ") or line.startswith("--- ")):
      continue
    target = normalize_patch_path(line[4:])
    if target == "/dev/null" or target in targets:
      continue
    targets.append(target)
  return targets


def patch_has_gn_inputs(patch_path: Path) -> bool:
  for target in patch_targets(patch_path):
    path = Path(target)
    if path.name in GN_INPUT_NAMES or path.suffix in GN_INPUT_SUFFIXES:
      return True
  return False


def mapped_patch_root(chromium_src: Path, cef_src: Path, patch_root: Path,
                      target: str) -> tuple[Path, str]:
  if patch_root == chromium_src and target.startswith("cef/") and (
      cef_src / ".git").exists():
    return cef_src, target[len("cef/"):]
  return patch_root, target


def chunks(values: list[str], size: int = 128) -> Iterable[list[str]]:
  for offset in range(0, len(values), size):
    yield values[offset:offset + size]


def tracked_patch_targets(root: Path, targets: list[str]) -> set[str]:
  tracked: set[str] = set()
  for target_chunk in chunks(targets):
    result = run(["git", "ls-files", "-z", "--", *target_chunk],
                 root,
                 capture=True)
    output = result.stdout.decode("utf-8", errors="replace")
    tracked.update(path for path in output.split("\0") if path)
  return tracked


def cleanup_patch_targets_for_root(root: Path, targets: list[str]) -> None:
  if not root.exists() or not targets:
    return
  if not (root / ".git").exists():
    raise CommandError(f"patch target root is not a git checkout: {root}")

  unique_targets = sorted(set(targets))
  tracked = tracked_patch_targets(root, unique_targets)
  if tracked:
    tracked_list = sorted(tracked)
    print(f"Restoring {len(tracked_list)} tracked patch target(s) in {root}",
          flush=True)
    for target_chunk in chunks(tracked_list):
      run(["git", "checkout", "--", *target_chunk], root)

  for target in unique_targets:
    if target in tracked:
      continue
    target_path = root / target
    if target_path.is_dir():
      print(f"Removing patch-created directory {target_path}", flush=True)
      shutil.rmtree(target_path)
    elif target_path.exists() or target_path.is_symlink():
      print(f"Removing patch-created file {target_path}", flush=True)
      target_path.unlink()


def patch_root_for(chromium_src: Path, patch: dict[str, str]) -> Path:
  patch_path = patch.get("path")
  if patch_path:
    return chromium_src / patch_path
  return chromium_src


def cleanup_patch_created_targets(repo_root: Path, chromium_src: Path,
                                  cef_src: Path,
                                  patches: Iterable[dict[str, str]]) -> None:
  targets_by_root: dict[Path, list[str]] = defaultdict(list)
  seen: set[tuple[Path, str]] = set()
  for patch in patches:
    patch_name = patch["name"]
    patch_path, _ = patch_file_for(repo_root, cef_src, patch_name)
    base_root = patch_root_for(chromium_src, patch)
    for target in patch_targets(patch_path):
      root, mapped_target = mapped_patch_root(chromium_src, cef_src, base_root,
                                              target)
      key = (root, mapped_target)
      if key in seen:
        continue
      seen.add(key)
      targets_by_root[root].append(mapped_target)
  for root, targets in sorted(targets_by_root.items(), key=lambda item: str(item[0])):
    cleanup_patch_targets_for_root(root, targets)


def patch_target_keys(repo_root: Path, chromium_src: Path, cef_src: Path,
                      patches: Iterable[dict[str, str]]) -> set[tuple[str, str]]:
  keys: set[tuple[str, str]] = set()
  for patch in patches:
    patch_path, _ = patch_file_for(repo_root, cef_src, patch["name"])
    base_root = patch_root_for(chromium_src, patch)
    for target in patch_targets(patch_path):
      root, mapped_target = mapped_patch_root(chromium_src, cef_src, base_root,
                                              target)
      keys.add((str(root), mapped_target))
  return keys


def patch_target_key_set(repo_root: Path, chromium_src: Path, cef_src: Path,
                         patch: dict[str, str]) -> set[tuple[Path, str]]:
  patch_path, _ = patch_file_for(repo_root, cef_src, patch["name"])
  base_root = patch_root_for(chromium_src, patch)
  keys: set[tuple[Path, str]] = set()
  for target in patch_targets(patch_path):
    root, mapped_target = mapped_patch_root(chromium_src, cef_src, base_root,
                                            target)
    keys.add((root, mapped_target))
  return keys


def patch_target_key_sets(repo_root: Path, chromium_src: Path, cef_src: Path,
                          patches: list[dict[str, str]]
                         ) -> list[set[tuple[Path, str]]]:
  return [
      patch_target_key_set(repo_root, chromium_src, cef_src, patch)
      for patch in patches
  ]


def cleanup_patch_target_keys(targets: set[tuple[Path, str]]) -> None:
  targets_by_root: dict[Path, list[str]] = defaultdict(list)
  for root, target in targets:
    targets_by_root[root].append(target)
  for root, root_targets in sorted(targets_by_root.items(),
                                   key=lambda item: str(item[0])):
    cleanup_patch_targets_for_root(root, root_targets)


def patch_target_closure(
    targets_by_patch: list[set[tuple[Path, str]]],
    changed_indices: list[int]) -> tuple[set[tuple[Path, str]], list[int]]:
  impacted_targets: set[tuple[Path, str]] = set()
  for index in changed_indices:
    impacted_targets.update(targets_by_patch[index])

  changed = True
  while changed:
    changed = False
    for patch_targets_for_index in targets_by_patch:
      if impacted_targets.isdisjoint(patch_targets_for_index):
        continue
      previous_count = len(impacted_targets)
      impacted_targets.update(patch_targets_for_index)
      if len(impacted_targets) != previous_count:
        changed = True

  patch_indices = [
      index for index, patch_targets_for_index in enumerate(targets_by_patch)
      if not impacted_targets.isdisjoint(patch_targets_for_index)
  ]
  return impacted_targets, patch_indices


def target_requires_gn(target: str) -> bool:
  path = Path(target)
  return path.name in GN_INPUT_NAMES or path.suffix in GN_INPUT_SUFFIXES


def target_keys_require_gn(targets: set[tuple[Path, str]]) -> bool:
  return any(target_requires_gn(target) for _root, target in targets)


def fetch_checkout_refs(checkout: Path, label: str, *, include_tags: bool) -> None:
  print(f"Fetching {label} refs in {checkout}", flush=True)
  run(["git", "fetch"], checkout)
  if include_tags:
    run(["git", "fetch", "--tags"], checkout)


def prepare_checkout_paths(repo_root: Path, build_root: Path,
                           *, fetch: bool) -> tuple[str, str, Path, Path, Path]:
  cef_branch, chromium_tag = parse_buildcef_vars(repo_root)
  chromium_src, cef_checkout, cef_src = checkout_paths(build_root)

  configure_safe_directories(build_root)
  require_checkout(chromium_src, "Chromium source")
  require_checkout(cef_checkout, "CEF source")
  require_checkout(cef_src, "embedded CEF source")

  if fetch:
    fetch_checkout_refs(chromium_src, "Chromium source", include_tags=True)
    fetch_checkout_refs(cef_checkout, "CEF source", include_tags=False)
    fetch_checkout_refs(cef_src, "embedded CEF source", include_tags=False)

  return cef_branch, chromium_tag, chromium_src, cef_checkout, cef_src


def reset_to_stock(repo_root: Path,
                   build_root: Path,
                   *,
                   fetch: bool) -> tuple[Path, Path, list[dict[str, str]]]:
  cef_branch, chromium_tag, chromium_src, cef_checkout, cef_src = prepare_checkout_paths(
      repo_root, build_root, fetch=fetch)

  chromium_hash = reset_git_checkout(chromium_src, chromium_tag, "Chromium")
  cef_ref = f"origin/{cef_branch}"
  cef_hash = reset_git_checkout(cef_checkout, cef_ref, "CEF source")
  if git_ok(cef_src, "rev-parse", "--verify", f"{cef_ref}^{{commit}}"):
    reset_git_checkout(cef_src, cef_ref, "embedded CEF source")
  else:
    reset_git_checkout(cef_src, cef_hash, "embedded CEF source")

  patches = load_patch_config(repo_root)
  cleanup_patch_created_targets(repo_root, chromium_src, cef_src, patches)

  print(f"Verified Chromium stock checkout: {chromium_tag} ({chromium_hash})",
        flush=True)
  print(f"Verified CEF stock checkout: {cef_ref} ({cef_hash})", flush=True)
  return chromium_src, cef_src, patches


def prepare_existing_checkouts(repo_root: Path, build_root: Path,
                               *,
                               fetch: bool) -> tuple[Path, Path, Path, list[dict[str, str]],
                                                      str, str]:
  cef_branch, chromium_tag, chromium_src, cef_checkout, cef_src = prepare_checkout_paths(
      repo_root, build_root, fetch=fetch)

  patches = load_patch_config(repo_root)
  chromium_hash = resolve_ref(chromium_src, chromium_tag, "Chromium")
  cef_hash = resolve_ref(cef_checkout, f"origin/{cef_branch}", "CEF source")
  return chromium_src, cef_checkout, cef_src, patches, chromium_hash, cef_hash


def helper_fingerprint(repo_root: Path) -> dict[str, str]:
  files = (
      "third_party/cef/patcher.py",
      "third_party/cef/git_util.py",
      "third_party/cef/stock_authoring_checkout.py",
  )
  return {file_path: sha256_file(repo_root / file_path) for file_path in files}


def patch_fingerprint(repo_root: Path, cef_src: Path,
                      patches: list[dict[str, str]]) -> list[dict[str, object]]:
  fingerprint: list[dict[str, object]] = []
  for patch in patches:
    patch_path, is_local = patch_file_for(repo_root, cef_src, patch["name"])
    fingerprint.append({
        "name": patch["name"],
        "path": patch.get("path", ""),
        "condition": patch.get("condition", ""),
        "source": "local" if is_local else "upstream",
        "sha256": sha256_file(patch_path),
        "gn_input": patch_has_gn_inputs(patch_path),
        "targets": patch_targets(patch_path),
    })
  return fingerprint


def build_fingerprint(repo_root: Path, build_root: Path, cef_src: Path,
                      patches: list[dict[str, str]], chromium_hash: str,
                      cef_hash: str) -> dict[str, object]:
  cef_branch, chromium_tag = parse_buildcef_vars(repo_root)
  return {
      "schema": BUILD_STATE_SCHEMA,
      "cef_branch": cef_branch,
      "chromium_tag": chromium_tag,
      "chromium_hash": chromium_hash,
      "cef_hash": cef_hash,
      "patch_cfg_sha256": sha256_file(repo_root / "third_party/cef/patch/patch.cfg"),
      "patches": patch_fingerprint(repo_root, cef_src, patches),
      "helpers": helper_fingerprint(repo_root),
      "env": {key: os.environ.get(key, "") for key in BUILD_ENV_KEYS},
  }


def state_path(build_root: Path) -> Path:
  return build_root / BUILD_STATE_PATH


def load_build_state(build_root: Path) -> dict[str, object] | None:
  path = state_path(build_root)
  if not path.is_file():
    return None
  try:
    return json.loads(path.read_text(encoding="utf-8"))
  except json.JSONDecodeError:
    return None


def repo_side_patch_file_for(repo_root: Path, build_root: Path,
                             patch_name: str) -> Path:
  local_path = repo_root / "third_party/cef/patch/patches" / f"{patch_name}.patch"
  if local_path.is_file():
    return local_path

  cached_upstream = (build_root /
                     "code/chromium_git/cef/patch/patches" /
                     f"{patch_name}.patch")
  if cached_upstream.is_file():
    return cached_upstream

  raise CommandError(f"patch file not found for {patch_name}")


def repo_side_patch_fingerprint(
    repo_root: Path, build_root: Path,
    patches: list[dict[str, str]]) -> list[dict[str, object]]:
  local_patches_dir = (repo_root / "third_party/cef/patch/patches").resolve()
  fingerprint: list[dict[str, object]] = []
  for patch in patches:
    patch_path = repo_side_patch_file_for(repo_root, build_root, patch["name"])
    is_local = patch_path.resolve().parent == local_patches_dir
    fingerprint.append({
        "name": patch["name"],
        "path": patch.get("path", ""),
        "condition": patch.get("condition", ""),
        "source": "local" if is_local else "upstream",
        "sha256": sha256_file(patch_path),
        "gn_input": patch_has_gn_inputs(patch_path),
        "targets": patch_targets(patch_path),
    })
  return fingerprint


CACHE_HIT_HELPER_FILES = (
    "third_party/cef/patcher.py",
    "third_party/cef/git_util.py",
)


def cache_hit_helpers(helpers: dict[str, object]) -> dict[str, object]:
  return {
      key: helpers.get(key)
      for key in CACHE_HIT_HELPER_FILES
      if isinstance(helpers, dict)
  }


def repo_side_fingerprint(repo_root: Path,
                          build_root: Path) -> dict[str, object]:
  cef_branch, chromium_tag = parse_buildcef_vars(repo_root)
  patches = load_patch_config(repo_root)
  return {
      "schema": BUILD_STATE_SCHEMA,
      "cef_branch": cef_branch,
      "chromium_tag": chromium_tag,
      "patch_cfg_sha256": sha256_file(repo_root /
                                      "third_party/cef/patch/patch.cfg"),
      "patches": repo_side_patch_fingerprint(repo_root, build_root, patches),
      "helpers": helper_fingerprint(repo_root),
      "env": {key: os.environ.get(key, "") for key in BUILD_ENV_KEYS},
  }


def verify_archive_sha256(archive_path: Path) -> str | None:
  sha_path = archive_path.with_name(archive_path.name + ".sha256")
  if not archive_path.is_file():
    return f"archive missing: {archive_path}"
  if not sha_path.is_file():
    return f"archive sha256 sidecar missing: {sha_path}"

  try:
    sidecar = sha_path.read_text(encoding="utf-8").strip().split()
  except OSError as exc:
    return f"archive sha256 sidecar unreadable: {sha_path} ({exc})"
  if not sidecar:
    return f"archive sha256 sidecar empty: {sha_path}"
  expected = sidecar[0]
  actual = sha256_file(archive_path)
  if actual != expected:
    return (f"archive sha256 mismatch: expected {expected}, got {actual} "
            f"({archive_path})")
  return None


def first_fingerprint_mismatch(previous: dict[str, object],
                               current: dict[str, object]) -> str | None:
  for key in ("schema", "cef_branch", "chromium_tag", "patch_cfg_sha256",
              "env"):
    if previous.get(key) != current.get(key):
      return key
  previous_helpers = cache_hit_helpers(previous.get("helpers", {}))
  current_helpers = cache_hit_helpers(current.get("helpers", {}))
  if previous_helpers != current_helpers:
    for helper in CACHE_HIT_HELPER_FILES:
      if previous_helpers.get(helper) != current_helpers.get(helper):
        return f"helpers[{helper}]"
    return "helpers"
  previous_patches = previous.get("patches")
  current_patches = current.get("patches")
  if previous_patches != current_patches:
    if (isinstance(previous_patches, list) and
        isinstance(current_patches, list)):
      max_len = max(len(previous_patches), len(current_patches))
      for index in range(max_len):
        if (index >= len(previous_patches) or
            index >= len(current_patches) or
            previous_patches[index] != current_patches[index]):
          return f"patches[{index}]"
    return "patches"
  return None


def check_cache_hit(repo_root: Path, build_root: Path,
                    archive_path: Path) -> int:
  previous = load_build_state(build_root)
  if previous is None:
    print(f"cache miss: state file missing or unreadable: "
          f"{state_path(build_root)}")
    return 1

  current = repo_side_fingerprint(repo_root, build_root)
  mismatch = first_fingerprint_mismatch(previous, current)
  if mismatch is not None:
    print(f"cache miss: fingerprint mismatch on {mismatch}")
    return 1

  archive_error = verify_archive_sha256(archive_path)
  if archive_error is not None:
    print(f"cache miss: {archive_error}")
    return 1

  print(f"cache hit: {archive_path}")
  return 0


def write_build_state(build_root: Path, fingerprint: dict[str, object]) -> None:
  path = state_path(build_root)
  path.write_text(
      json.dumps(fingerprint, indent=2, sort_keys=True) + "\n",
      encoding="utf-8")


def write_build_env(path: Path | None, *, no_update: bool,
                    skip_gclient_hook: bool, mode: str) -> None:
  if path is None:
    return
  path.parent.mkdir(parents=True, exist_ok=True)
  path.write_text(
      "\n".join((
          f"MMLTK_CEF_AUTOMATE_NO_UPDATE={1 if no_update else 0}",
          f"MMLTK_CEF_SKIP_GCLIENT_HOOK={1 if skip_gclient_hook else 0}",
          f"MMLTK_CEF_BUILD_CHECKOUT_MODE={mode}",
          "",
      )),
      encoding="utf-8")


def build_graph_ready(chromium_src: Path) -> bool:
  return all((chromium_src / graph_file).is_file()
             for graph_file in BUILD_GRAPH_FILES)


def current_heads_match(chromium_src: Path, cef_src: Path, chromium_hash: str,
                        cef_hash: str) -> bool:
  chromium_head = git_capture(chromium_src, "rev-parse", "HEAD")
  cef_head = git_capture(cef_src, "rev-parse", "HEAD")
  return chromium_head == chromium_hash and cef_head == cef_hash


def first_changed_patch_index(previous: dict[str, object] | None,
                              current: dict[str, object]) -> int | None:
  if previous is None:
    return None
  previous_patches = previous.get("patches")
  current_patches = current.get("patches")
  if not isinstance(previous_patches, list) or not isinstance(current_patches, list):
    return None
  max_len = max(len(previous_patches), len(current_patches))
  for index in range(max_len):
    if index >= len(previous_patches) or index >= len(current_patches):
      return index
    if previous_patches[index] != current_patches[index]:
      return index
  return None


PATCH_STABLE_KEYS = ("name", "path", "condition", "source", "gn_input",
                     "targets")


def changed_patch_indices_with_stable_targets(
    previous: dict[str, object] | None,
    current: dict[str, object]) -> list[int] | None:
  if previous is None:
    return None
  previous_patches = previous.get("patches")
  current_patches = current.get("patches")
  if not isinstance(previous_patches, list) or not isinstance(current_patches, list):
    return None
  if len(previous_patches) != len(current_patches):
    return None

  changed_indices: list[int] = []
  for index, (previous_patch, current_patch) in enumerate(
      zip(previous_patches, current_patches)):
    if not isinstance(previous_patch, dict) or not isinstance(current_patch, dict):
      return None
    for key in PATCH_STABLE_KEYS:
      if previous_patch.get(key) != current_patch.get(key):
        return None
    if previous_patch.get("sha256") != current_patch.get("sha256"):
      changed_indices.append(index)
  return changed_indices


def refresh_fingerprint_view(fingerprint: dict[str, object]) -> dict[str, object]:
  view = dict(fingerprint)
  view["helpers"] = cache_hit_helpers(view.get("helpers", {}))
  return view


def source_fingerprint_matches(previous: dict[str, object] | None,
                               current: dict[str, object]) -> bool:
  if previous is None:
    return False
  return refresh_fingerprint_view(previous) == refresh_fingerprint_view(current)


def only_patch_fingerprint_changed(previous: dict[str, object] | None,
                                   current: dict[str, object]) -> bool:
  if previous is None:
    return False
  previous_without_patches = refresh_fingerprint_view(previous)
  current_without_patches = refresh_fingerprint_view(current)
  previous_without_patches.pop("patches", None)
  current_without_patches.pop("patches", None)
  return previous_without_patches == current_without_patches


def suffix_refresh_is_safe(repo_root: Path, chromium_src: Path, cef_src: Path,
                           patches: list[dict[str, str]], start_index: int) -> bool:
  prior_targets = patch_target_keys(repo_root, chromium_src, cef_src,
                                    patches[:start_index])
  suffix_targets = patch_target_keys(repo_root, chromium_src, cef_src,
                                     patches[start_index:])
  return prior_targets.isdisjoint(suffix_targets)


def suffix_targets_unchanged(previous: dict[str, object] | None,
                             current: dict[str, object],
                             start_index: int) -> bool:
  if previous is None:
    return False
  previous_patches = previous.get("patches")
  current_patches = current.get("patches")
  if not isinstance(previous_patches, list) or not isinstance(current_patches, list):
    return False
  for index in range(start_index, max(len(previous_patches), len(current_patches))):
    if index >= len(previous_patches) or index >= len(current_patches):
      return False
    previous_patch = previous_patches[index]
    current_patch = current_patches[index]
    if not isinstance(previous_patch, dict) or not isinstance(current_patch, dict):
      return False
    if previous_patch.get("targets") != current_patch.get("targets"):
      return False
  return True


def suffix_requires_gn(repo_root: Path, cef_src: Path,
                       patches: list[dict[str, str]], start_index: int) -> bool:
  for patch in patches[start_index:]:
    patch_path, _ = patch_file_for(repo_root, cef_src, patch["name"])
    if patch_has_gn_inputs(patch_path):
      return True
  return False


def git_apply_check(root: Path, patch_path: Path, *, reverse: bool) -> bool:
  args = ["git", "apply", "-p0", "--ignore-whitespace", "--check"]
  if reverse:
    args.insert(2, "--reverse")
  return run(args, root, input_bytes=patch_path.read_bytes(), check=False,
             capture=True).returncode == 0


def git_apply(root: Path, patch_path: Path) -> None:
  run(["git", "apply", "-p0", "--ignore-whitespace"], root,
      input_bytes=patch_path.read_bytes())


def apply_patch_entry(repo_root: Path, chromium_src: Path, cef_src: Path,
                      patch: dict[str, str],
                      *,
                      require_fresh_local: bool) -> None:
  patch_name = patch["name"]
  patch_path, is_local = patch_file_for(repo_root, cef_src, patch_name)
  root = patch_root_for(chromium_src, patch)
  if not root.is_dir():
    print(f"Skipping {patch_name}: target directory does not exist: {root}",
          flush=True)
    return
  if not (root / ".git").exists():
    raise CommandError(f"patch target is not a git checkout: {root}")

  print(f"Checking {patch_name} in {root}", flush=True)
  reverse_ok = git_apply_check(root, patch_path, reverse=True)
  if is_local and require_fresh_local and reverse_ok:
    raise CommandError(
        f"repo-local patch {patch_name} was already present before application")

  forward_ok = git_apply_check(root, patch_path, reverse=False)
  if not forward_ok:
    if reverse_ok:
      print(f"Skipping already-applied upstream patch {patch_name}", flush=True)
      return
    raise CommandError(f"patch does not apply cleanly: {patch_name}")

  git_apply(root, patch_path)

  if is_local and not git_apply_check(root, patch_path, reverse=True):
    raise CommandError(
        f"repo-local patch {patch_name} did not pass reverse check after apply")


def apply_configured_prefix(repo_root: Path, chromium_src: Path, cef_src: Path,
                            patches: list[dict[str, str]],
                            *,
                            stop_before: str | None,
                            apply_through: str | None,
                            validate_local: bool) -> None:
  found_marker = stop_before is None and apply_through is None

  for patch in patches:
    patch_name = patch["name"]
    if stop_before is not None and patch_name == stop_before:
      found_marker = True
      print(f"Stopped before {stop_before}", flush=True)
      return

    apply_patch_entry(repo_root, chromium_src, cef_src, patch,
                      require_fresh_local=validate_local)

    if apply_through is not None and patch_name == apply_through:
      found_marker = True
      print(f"Applied through {apply_through}", flush=True)
      return

  if not found_marker:
    marker = stop_before if stop_before is not None else apply_through
    raise CommandError(f"patch name not found in patch.cfg: {marker}")


def refresh_patch_suffix(repo_root: Path, chromium_src: Path, cef_src: Path,
                         patches: list[dict[str, str]], start_index: int) -> None:
  suffix = patches[start_index:]
  cleanup_patch_created_targets(repo_root, chromium_src, cef_src, suffix)
  apply_configured_prefix(
      repo_root,
      chromium_src,
      cef_src,
      suffix,
      stop_before=None,
      apply_through=None,
      validate_local=False)


def refresh_patch_targets(repo_root: Path, chromium_src: Path, cef_src: Path,
                          patches: list[dict[str, str]],
                          changed_indices: list[int]
                         ) -> tuple[set[tuple[Path, str]],
                                    set[tuple[Path, str]]]:
  targets_by_patch = patch_target_key_sets(repo_root, chromium_src, cef_src,
                                           patches)
  changed_targets: set[tuple[Path, str]] = set()
  for index in changed_indices:
    changed_targets.update(targets_by_patch[index])
  impacted_targets, patch_indices = patch_target_closure(targets_by_patch,
                                                         changed_indices)
  if not impacted_targets:
    raise CommandError("changed patches did not identify any target files")

  print(
      f"Refreshing {len(impacted_targets)} CEF patch target(s) across "
      f"{len(patch_indices)} ordered patch(es); changed patch indices: "
      f"{', '.join(str(index) for index in changed_indices)}.",
      flush=True)
  cleanup_patch_target_keys(impacted_targets)
  for index in patch_indices:
    apply_patch_entry(repo_root, chromium_src, cef_src, patches[index],
                      require_fresh_local=False)
  return impacted_targets, changed_targets


def apply_full_patch_stack(repo_root: Path, chromium_src: Path, cef_src: Path,
                           patches: list[dict[str, str]]) -> None:
  apply_configured_prefix(
      repo_root,
      chromium_src,
      cef_src,
      patches,
      stop_before=None,
      apply_through=None,
      validate_local=True)


def reset_and_apply_full_stack(repo_root: Path, build_root: Path,
                               *,
                               fetch: bool,
                               fingerprint: dict[str, object] | None = None
                              ) -> tuple[Path, Path, list[dict[str, str]],
                                         dict[str, object]]:
  chromium_src, cef_src, patches = reset_to_stock(
      repo_root, build_root, fetch=fetch)
  if fingerprint is None:
    chromium_hash = git_capture(chromium_src, "rev-parse", "HEAD")
    cef_hash = git_capture(cef_src, "rev-parse", "HEAD")
    fingerprint = build_fingerprint(repo_root, build_root, cef_src, patches,
                                    chromium_hash, cef_hash)
  apply_full_patch_stack(repo_root, chromium_src, cef_src, patches)
  return chromium_src, cef_src, patches, fingerprint


def ensure_patched_build_checkout(repo_root: Path,
                                  build_root: Path,
                                  *,
                                  fetch: bool,
                                  state_output: Path | None) -> None:
  (chromium_src, _cef_checkout, cef_src, patches, chromium_hash,
   cef_hash) = prepare_existing_checkouts(repo_root, build_root, fetch=fetch)
  current = build_fingerprint(repo_root, build_root, cef_src, patches,
                              chromium_hash, cef_hash)
  previous = load_build_state(build_root)

  source_matches = source_fingerprint_matches(previous, current)
  heads_match = current_heads_match(chromium_src, cef_src, chromium_hash,
                                    cef_hash)
  graph_ready = build_graph_ready(chromium_src)

  if source_matches and heads_match and graph_ready:
    print("CEF build checkout is already patched for the current fingerprint.",
          flush=True)
    write_build_env(
        state_output, no_update=True, skip_gclient_hook=True, mode="hot")
    return

  if source_matches and heads_match:
    print("CEF build checkout source is current, but the output graph is "
          "incomplete; preserving source and running the CEF hook.",
          flush=True)
    write_build_env(
        state_output,
        no_update=True,
        skip_gclient_hook=False,
        mode="graph-refresh")
    return

  changed_indices = changed_patch_indices_with_stable_targets(previous, current)
  if (changed_indices and only_patch_fingerprint_changed(previous, current) and
      heads_match and graph_ready):
    _impacted_targets, changed_targets = refresh_patch_targets(
        repo_root, chromium_src, cef_src, patches, changed_indices)
    write_build_env(
        state_output,
        no_update=True,
        skip_gclient_hook=not target_keys_require_gn(changed_targets),
        mode="patch-targets")
    return

  print("Resetting CEF source checkout before applying patch stack.", flush=True)
  reset_and_apply_full_stack(
      repo_root, build_root, fetch=False, fingerprint=current)
  write_build_env(
      state_output, no_update=True, skip_gclient_hook=False, mode="reset")


def record_patched_build_checkout(repo_root: Path,
                                  build_root: Path,
                                  *,
                                  fetch: bool,
                                  state_output: Path | None) -> None:
  (chromium_src, _cef_checkout, cef_src, patches, chromium_hash,
   cef_hash) = prepare_existing_checkouts(repo_root, build_root, fetch=fetch)
  if not current_heads_match(chromium_src, cef_src, chromium_hash, cef_hash):
    raise CommandError("cannot record CEF build state: checkout heads differ "
                       "from the configured Chromium/CEF refs")
  if not build_graph_ready(chromium_src):
    raise CommandError("cannot record CEF build state: output graph files are "
                       "missing")
  current = build_fingerprint(repo_root, build_root, cef_src, patches,
                              chromium_hash, cef_hash)
  write_build_state(build_root, current)
  write_build_env(
      state_output, no_update=True, skip_gclient_hook=True, mode="record")


def parse_args(argv: list[str]) -> argparse.Namespace:
  parser = argparse.ArgumentParser(
      description="Prepare the cached CEF/Chromium checkout for patch authoring.")
  parser.add_argument(
      "--repo-root",
      type=Path,
      default=Path(__file__).resolve().parents[2],
      help="repository root; defaults to this tool's repo")
  parser.add_argument(
      "--build-root",
      type=Path,
      default=None,
      help=("CEF build root containing code/chromium_git; defaults to "
            "<repo-root>/.docker-cache/cef-build"))
  parser.add_argument(
      "--fetch",
      action="store_true",
      help="fetch refs and tags before resolving the fixed CEF/Chromium refs")
  parser.add_argument(
      "--state-output",
      type=Path,
      default=None,
      help="write buildcef shell state for cache-aware build invocation")
  parser.add_argument(
      "--archive-path",
      type=Path,
      default=None,
      help=("CEF archive to verify in --check-cache-hit mode; defaults to "
            "<repo-root>/.docker-cache/cef/mmltk_cef_linux64_minimal_debug.tar.bz2"))

  mode = parser.add_mutually_exclusive_group(required=True)
  mode.add_argument(
      "--check-cache-hit",
      action="store_true",
      help=("compare repo-side fingerprint against the recorded build state "
            "and verify the archive sha256; exits 0 on cache hit, non-zero "
            "otherwise. Does not require a git checkout under build-root."))
  mode.add_argument(
      "--ensure-patched-build-checkout",
      action="store_true",
      help=("leave a matching patched checkout untouched; otherwise refresh "
            "only the needed source files while preserving build outputs"))
  mode.add_argument(
      "--record-patched-build-checkout",
      action="store_true",
      help=("record the current patched build checkout fingerprint without "
            "resetting source or touching build outputs"))
  mode.add_argument(
      "--reset-stock",
      action="store_true",
      help="reset Chromium and CEF source checkouts to stock only")
  mode.add_argument(
      "--prepare-patch",
      metavar="PATCH",
      help="reset stock and apply patch.cfg entries before PATCH")
  mode.add_argument(
      "--apply-through",
      metavar="PATCH",
      help="reset stock and apply patch.cfg entries through PATCH")
  mode.add_argument(
      "--dry-run-stack",
      action="store_true",
      help=("reset stock, check every repo-local patch before applying it, "
            "apply the full configured stack, and reverse-check local patches"))
  return parser.parse_args(argv)


def main(argv: list[str]) -> int:
  args = parse_args(argv)
  repo_root = args.repo_root.resolve()
  build_root = (args.build_root.resolve() if args.build_root else
                repo_root / ".docker-cache/cef-build")

  if args.check_cache_hit:
    archive_path = (args.archive_path.resolve() if args.archive_path else
                    repo_root /
                    ".docker-cache/cef/mmltk_cef_linux64_minimal_debug.tar.bz2")
    return check_cache_hit(repo_root, build_root, archive_path)

  if args.ensure_patched_build_checkout:
    state_output = args.state_output.resolve() if args.state_output else None
    ensure_patched_build_checkout(
        repo_root, build_root, fetch=args.fetch, state_output=state_output)
    return 0

  if args.record_patched_build_checkout:
    state_output = args.state_output.resolve() if args.state_output else None
    record_patched_build_checkout(
        repo_root, build_root, fetch=args.fetch, state_output=state_output)
    return 0

  chromium_src, cef_src, patches = reset_to_stock(
      repo_root, build_root, fetch=args.fetch)

  if args.reset_stock:
    return 0

  if args.prepare_patch:
    apply_configured_prefix(
        repo_root,
        chromium_src,
        cef_src,
        patches,
        stop_before=args.prepare_patch,
        apply_through=None,
        validate_local=False)
    return 0

  if args.apply_through:
    apply_configured_prefix(
        repo_root,
        chromium_src,
        cef_src,
        patches,
        stop_before=None,
        apply_through=args.apply_through,
        validate_local=False)
    return 0

  apply_full_patch_stack(repo_root, chromium_src, cef_src, patches)
  return 0


if __name__ == "__main__":
  try:
    raise SystemExit(main(sys.argv[1:]))
  except CommandError as exc:
    print(f"stock_authoring_checkout.py: {exc}", file=sys.stderr)
    raise SystemExit(1)
