#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root_config="${repo_root}/.uncommentrc.toml"
firefox_root="${repo_root}/third_party/firefox"
firefox_vendor_root="${firefox_root}/third_party/rust"
uncomment_bin="${UNCOMMENT_BIN:-uncomment}"

command -v "${uncomment_bin}" >/dev/null 2>&1 || {
    printf 'run_uncomment: uncomment is required\n' >&2
    exit 1
}
command -v python3 >/dev/null 2>&1 || {
    printf 'run_uncomment: python3 is required\n' >&2
    exit 1
}
[[ -f "${root_config}" ]] || {
    printf 'run_uncomment: missing %s\n' "${root_config}" >&2
    exit 1
}
[[ -d "${firefox_root}" ]] || {
    printf 'run_uncomment: missing %s\n' "${firefox_root}" >&2
    exit 1
}

install -d -m 0755 "${repo_root}/.cache"
work_root="$(mktemp -d "${repo_root}/.cache/uncomment.XXXXXX")"
cleanup() {
    case "${work_root}" in
        "${repo_root}/.cache/uncomment."*)
            rm -rf -- "${work_root}"
            ;;
    esac
}
trap cleanup EXIT

firefox_config="${work_root}/firefox.toml"
cp -- "${root_config}" "${firefox_config}"
printf '%s\n' \
    '' \
    '[languages.rust]' \
    'name = "rust"' \
    'extensions = [".rs"]' \
    'comment_nodes = ["line_comment", "block_comment"]' \
    'doc_comment_nodes = ["doc_comment"]' \
    'remove_docs = false' \
    >>"${firefox_config}"

printf 'run_uncomment: processing Firefox\n'
"${uncomment_bin}" \
    --config "${firefox_config}" \
    --no-default-ignores \
    --threads 0 \
    "${firefox_root}"

python3 - "${firefox_vendor_root}" <<'PY'
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

root = Path(sys.argv[1])
changed_manifests = 0
changed_entries = 0

for manifest in sorted(root.glob("*/.cargo-checksum.json")):
    original = manifest.read_bytes()
    data = json.loads(original)
    files = {}

    for path in sorted(manifest.parent.rglob("*")):
        if not path.is_file() or path.name == ".cargo-checksum.json":
            continue
        if path.name.startswith(".cargo-checksum.json."):
            continue
        relative = path.relative_to(manifest.parent).as_posix()
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        files[relative] = digest
        if data.get("files", {}).get(relative) != digest:
            changed_entries += 1

    data["files"] = files
    rendered = json.dumps(
        data, ensure_ascii=False, separators=(",", ":")
    ).encode() + b"\n"
    if rendered == original:
        continue

    metadata = manifest.stat()
    with tempfile.NamedTemporaryFile(
        mode="wb",
        dir=manifest.parent,
        prefix=".cargo-checksum.json.",
        delete=False,
    ) as handle:
        handle.write(rendered)
        temporary = Path(handle.name)

    os.chmod(temporary, metadata.st_mode)
    try:
        os.chown(temporary, metadata.st_uid, metadata.st_gid)
    except PermissionError:
        pass
    os.replace(temporary, manifest)
    changed_manifests += 1

manifest_count = 0
entry_count = 0
errors = []

for manifest in sorted(root.glob("*/.cargo-checksum.json")):
    manifest_count += 1
    data = json.loads(manifest.read_bytes())
    listed = set(data.get("files", {}))
    present = {
        path.relative_to(manifest.parent).as_posix()
        for path in manifest.parent.rglob("*")
        if path.is_file()
        and path.name != ".cargo-checksum.json"
        and not path.name.startswith(".cargo-checksum.json.")
    }
    if listed != present:
        errors.append(f"{manifest}: file set differs")
    for relative, expected in data.get("files", {}).items():
        entry_count += 1
        path = manifest.parent / relative
        if not path.is_file():
            errors.append(f"{manifest}: missing {relative}")
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            errors.append(f"{manifest}: hash mismatch for {relative}")

if errors:
    for error in errors[:20]:
        print(f"run_uncomment: {error}", file=sys.stderr)
    raise SystemExit(1)

print(
    "run_uncomment: Firefox crate hashes "
    f"updated_manifests={changed_manifests} "
    f"updated_entries={changed_entries} "
    f"verified_manifests={manifest_count} "
    f"verified_entries={entry_count}"
)
PY

rest_paths=()
while IFS= read -r -d '' path; do
    case "$(basename "${path}")" in
        .git|.cache|build|third_party)
            ;;
        *)
            rest_paths+=("${path}")
            ;;
    esac
done < <(find "${repo_root}" -mindepth 1 -maxdepth 1 -print0)

while IFS= read -r -d '' path; do
    [[ "$(basename "${path}")" == "firefox" ]] || rest_paths+=("${path}")
done < <(find "${repo_root}/third_party" -mindepth 1 -maxdepth 1 -print0)

printf 'run_uncomment: processing the rest of the repository\n'
"${uncomment_bin}" \
    --config "${root_config}" \
    --no-default-ignores \
    --threads 0 \
    "${rest_paths[@]}"

printf 'run_uncomment: complete\n'
