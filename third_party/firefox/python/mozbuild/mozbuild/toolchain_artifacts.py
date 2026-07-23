# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Small, owned Taskcluster toolchain installer used by configure bootstrap."""

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

from mozbuild.util import TASKCLUSTER_ROOT_URL


_BUFFER_SIZE = 4 * 1024 * 1024
_HASH_PREFERENCE = ("sha512", "sha256")


def _artifact_url(root_url, task_id, artifact_name):
    quoted_task = urllib.parse.quote(task_id, safe="")
    quoted_artifact = urllib.parse.quote(artifact_name, safe="/")
    return (
        f"{root_url}/api/queue/v1/task/{quoted_task}/artifacts/{quoted_artifact}"
    )


def _download_json(url):
    with urllib.request.urlopen(url, timeout=120) as response:
        return json.load(response)


def _expected_digest(chain_of_trust, artifact_name):
    digests = chain_of_trust.get("artifacts", {}).get(artifact_name, {})
    for algorithm in _HASH_PREFERENCE:
        digest = digests.get(algorithm)
        if digest:
            return algorithm, digest
    raise RuntimeError(
        f"Taskcluster chain of trust has no supported digest for {artifact_name}"
    )


def _file_digest(path, algorithm):
    digest = hashlib.new(algorithm)
    with open(path, "rb", buffering=0) as stream:
        while chunk := stream.read(_BUFFER_SIZE):
            digest.update(chunk)
    return digest.hexdigest()


def _download_verified(url, destination, algorithm, expected_digest):
    destination = Path(destination)
    if destination.is_file() and _file_digest(destination, algorithm) == expected_digest:
        return

    partial = destination.with_name(f".{destination.name}.partial")
    partial.unlink(missing_ok=True)
    last_error = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=300) as response, open(
                partial, "wb", buffering=0
            ) as output:
                shutil.copyfileobj(response, output, length=_BUFFER_SIZE)
            actual_digest = _file_digest(partial, algorithm)
            if actual_digest != expected_digest:
                raise RuntimeError(
                    f"Taskcluster artifact digest mismatch: expected {expected_digest}, "
                    f"got {actual_digest}"
                )
            os.replace(partial, destination)
            return
        except Exception as error:
            last_error = error
            partial.unlink(missing_ok=True)
            if attempt != 3:
                time.sleep(2**attempt)
    raise RuntimeError(f"Failed to download {url}: {last_error}") from last_error


def _extract_archive(archive, destination):
    name = archive.name
    if name.endswith(".tar.zst"):
        command = ["tar", "--extract", "--zstd", "--file", archive]
    elif name.endswith(".tar.xz"):
        command = ["tar", "--extract", "--xz", "--file", archive]
    elif name.endswith((".tar.bz2", ".tbz2")):
        command = ["tar", "--extract", "--bzip2", "--file", archive]
    elif name.endswith((".tar.gz", ".tgz")):
        command = ["tar", "--extract", "--gzip", "--file", archive]
    elif name.endswith(".tar"):
        command = ["tar", "--extract", "--file", archive]
    elif name.endswith(".zip"):
        command = ["unzip", "-q", archive]
    else:
        raise RuntimeError(f"Unsupported Taskcluster toolchain archive: {name}")
    subprocess.run(command, cwd=destination, check=True)


def _replace_path(source, destination):
    previous = destination.with_name(f".{destination.name}.previous")
    if previous.is_dir() and not previous.is_symlink():
        shutil.rmtree(previous)
    else:
        previous.unlink(missing_ok=True)

    had_destination = destination.exists() or destination.is_symlink()
    if had_destination:
        os.replace(destination, previous)
    try:
        os.replace(source, destination)
    except Exception:
        if had_destination and not destination.exists():
            os.replace(previous, destination)
        raise
    if previous.is_dir() and not previous.is_symlink():
        shutil.rmtree(previous)
    else:
        previous.unlink(missing_ok=True)


def install_task_artifact(
    *, task_id, artifact_name, install_root, expected_root, unpack=True
):
    """Download, verify, and atomically publish one public toolchain artifact."""

    root_url = os.environ.get("TASKCLUSTER_ROOT_URL", TASKCLUSTER_ROOT_URL).rstrip("/")
    cot_url = _artifact_url(root_url, task_id, "public/chain-of-trust.json")
    artifact_url = _artifact_url(root_url, task_id, artifact_name)
    chain_of_trust = _download_json(cot_url)
    algorithm, digest = _expected_digest(chain_of_trust, artifact_name)

    install_root = Path(install_root)
    cache_root = install_root / "toolchains"
    cache_root.mkdir(parents=True, exist_ok=True)
    archive_name = Path(artifact_name).name
    archive = cache_root / f"{task_id}-{archive_name}"
    _download_verified(artifact_url, archive, algorithm, digest)

    destination = install_root / expected_root
    if not unpack:
        temporary = install_root / f".{expected_root}.next"
        temporary.unlink(missing_ok=True)
        os.link(archive, temporary)
        _replace_path(temporary, destination)
        return

    extract_root = Path(tempfile.mkdtemp(prefix=".mmltk-toolchain-", dir=install_root))
    try:
        _extract_archive(archive, extract_root)
        extracted = extract_root / expected_root
        if not extracted.exists() and not extracted.is_symlink():
            raise RuntimeError(
                f"Taskcluster artifact {artifact_name} did not contain {expected_root}"
            )
        _replace_path(extracted, destination)
    finally:
        shutil.rmtree(extract_root, ignore_errors=True)
