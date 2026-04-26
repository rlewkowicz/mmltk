#!/usr/bin/env bash
set -euo pipefail

log() {
    printf 'mmltk-tidy: %s\n' "$*" >&2
}

die() {
    log "$*"
    exit 1
}

usage() {
    cat <<'EOF'
Usage: utilities/run_static_analysis.sh [--start-at <translation-unit>] [--cppcheck-only]

Runs the Docker-backed static-analysis pass for this repo:
1. Runs clang-format in-place over tracked C/C++/CUDA source files
2. Regenerates a clean Docker-side Dev compile database
3. Runs clang-tidy over repo translation units in deterministic order
4. Runs cppcheck over the same project file set

Options:
  --start-at <translation-unit>  Resume analysis from the given file in
                                 compile-database order
  --cppcheck-only                Skip clang-tidy and run only cppcheck
  --help                         Show this help

Environment:
  MMLTK_REPO_ROOT         Repo root mounted inside the container
  MMLTK_TIDY_BUILD_DIR    Build dir for the Docker-side compile database
  MMLTK_CLANG_FORMAT      clang-format binary override
  MMLTK_CLANG_TIDY_CHECKS Override the clang-tidy check filter
EOF
}

repo_root="${MMLTK_REPO_ROOT:-$(pwd)}"
build_dir="${MMLTK_TIDY_BUILD_DIR:-${repo_root}/build/docker-dev-tidy}"
cppcheck_build_dir="${build_dir}/cppcheck"
compile_db="${build_dir}/compile_commands.json"
start_at=""
cppcheck_only=0

while (($# > 0)); do
    case "$1" in
        --help|-h)
            usage
            exit 0
            ;;
        --start-at)
            shift
            [[ $# -gt 0 ]] || die "--start-at requires a file path"
            start_at="$1"
            ;;
        --cppcheck-only)
            cppcheck_only=1
            ;;
        *)
            die "unsupported argument: $1"
            ;;
    esac
    shift
done

export LD_LIBRARY_PATH="/usr/local/cuda-13.0/lib64:/opt/pytorch/lib:/src/workspace/deps/onnx/lib:/src/workspace/deps/onnxruntime/lib:${LD_LIBRARY_PATH:-}"

clang_format_bin="${MMLTK_CLANG_FORMAT:-clang-format}"
command -v "${clang_format_bin}" >/dev/null 2>&1 || die "clang-format is unavailable; rebuild the analysis image"

format_source_files() {
    local format_jobs
    local tracked_sources
    local source_count
    format_jobs="$(nproc)"
    tracked_sources="$(mktemp)"
    git -C "${repo_root}" ls-files -z -- \
        '*.c' '*.cc' '*.cpp' '*.cxx' \
        '*.h' '*.hh' '*.hpp' '*.hxx' \
        '*.ipp' '*.inl' '*.inc' \
        '*.cu' '*.cuh' \
        > "${tracked_sources}"

    if [[ ! -s "${tracked_sources}" ]]; then
        rm -f "${tracked_sources}"
        die "no tracked C/C++/CUDA source files found"
    fi

    source_count="$(tr -cd '\0' < "${tracked_sources}" | wc -c)"
    log "running clang-format (${clang_format_bin}) over ${source_count} tracked C/C++/CUDA files with ${format_jobs} workers"
    (
        cd "${repo_root}"
        xargs -0 -a "${tracked_sources}" -r -n 1 -P "${format_jobs}" "${clang_format_bin}" -i --style=file
    )
    rm -f "${tracked_sources}"
}

format_source_files

mkdir -p "${build_dir}" "${cppcheck_build_dir}"

cache_file="${build_dir}/CMakeCache.txt"
if [[ -f "${cache_file}" ]]; then
    cached_source="$(sed -n 's#^CMAKE_HOME_DIRECTORY:INTERNAL=##p' "${cache_file}")"
    cached_build="$(sed -n 's#^CMAKE_CACHEFILE_DIR:INTERNAL=##p' "${cache_file}")"
    if [[ "${cached_source}" != "${repo_root}" || "${cached_build}" != "${build_dir}" ]]; then
        find "${build_dir}" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
    fi
fi

log "configuring Docker-side Dev build tree at ${build_dir}"
cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Dev \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_TESTS=ON \
    -DBUILD_MMLTK_BROWSER_APP=ON \
    -DBUILD_MMLTK_BROWSER_HOST=ON \
    -DBUILD_RFDETR_NATIVE=ON \
    -DBUILD_RFDETR_PYTHON_CHECKPOINT_LOADER=ON \
    -DMMLTK_CUDA_VERSION=13.0 \
    -DMMLTK_CUDA_ARCHITECTURES="86;87;89;90;100;103;110;120;121" \
    -DMMLTK_CUDA_TOOLKIT_ROOT=/usr/local/cuda-13.0 \
    -DMMLTK_TORCH_ROOT=/opt/pytorch \
    -DMMLTK_ONNX_ROOT=/src/workspace/deps/onnx \
    -DMMLTK_ONNXRUNTIME_ROOT=/src/workspace/deps/onnxruntime \
    -DMMLTK_TORCH_CUDA_ARCH_LIST="8.6 8.7 8.9 9.0 10.0 10.3 11.0 12.0 12.1" \
    -DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc \
    -DPython3_EXECUTABLE=/usr/bin/python3 \
    -DCMAKE_CXX_COMPILER=/usr/bin/g++-14

[[ -f "${compile_db}" ]] || die "missing compile_commands.json at ${compile_db}"

translation_units_output="$(
python3 - "${compile_db}" "${repo_root}" <<'PY'
import json
import subprocess
import sys
from pathlib import Path

compile_db = Path(sys.argv[1]).resolve()
repo_root = Path(sys.argv[2]).resolve()
allowed_roots = (repo_root / "src", repo_root / "tests")
diagnostic_roots = (repo_root / "include", repo_root / "src", repo_root / "tests")
# `third_party/spdmon` is a repo-owned snapshot, but static analysis treats it as
# frozen vendor code for now. Keep the exclusion explicit so it does not silently
# ride along with the generic third_party filtering below.
frozen_vendor_roots = (repo_root / "third_party" / "spdmon",)
# clang-tidy in this toolchain cannot reliably replay the NVCC command lines in
# compile_commands.json for CUDA 13 builds, so keep the pass on host C/C++ TUs.
clang_exts = {".c", ".cc", ".cpp", ".cxx"}
cppcheck_exts = {".c", ".cc", ".cpp", ".cxx"}
tracked_files = {
    file_path.decode("utf-8")
    for file_path in subprocess.check_output(
        ["git", "-C", str(repo_root), "ls-files", "-z"],
    ).split(b"\0")
    if file_path
}
tracked_diagnostic_files = []
for rel in sorted(tracked_files):
    file_path = repo_root / rel
    if not any(str(file_path).startswith(str(root) + "/") for root in diagnostic_roots):
        continue
    if any(str(file_path).startswith(str(root) + "/") for root in frozen_vendor_roots):
        continue
    if any(part in {"third_party", "_deps", "build"} for part in file_path.parts):
        continue
    tracked_diagnostic_files.append({"name": file_path.as_posix()})

entries = json.loads(compile_db.read_text())
clang_units = []
cppcheck_units = []
seen_clang = set()
seen_cppcheck = set()
for entry in entries:
    file_path = Path(entry["file"]).resolve()
    if not any(str(file_path).startswith(str(root) + "/") for root in allowed_roots):
        continue
    if any(str(file_path).startswith(str(root) + "/") for root in frozen_vendor_roots):
        continue
    if any(part in {"third_party", "_deps", "build"} for part in file_path.parts):
        continue
    rel = file_path.relative_to(repo_root).as_posix()
    if rel not in tracked_files:
        continue
    ext = file_path.suffix.lower()
    if ext in clang_exts and rel not in seen_clang:
        seen_clang.add(rel)
        clang_units.append(rel)
    if ext in cppcheck_exts and rel not in seen_cppcheck:
        seen_cppcheck.add(rel)
        cppcheck_units.append(rel)

clang_units.sort()
cppcheck_units.sort()
print("CLANG")
for rel in clang_units:
    print(rel)
print("CPPCHECK")
for rel in cppcheck_units:
    print(rel)
print("LINE_FILTER")
print(json.dumps(tracked_diagnostic_files, separators=(",", ":")))
PY
)"

mapfile -t translation_units < <(printf '%s\n' "${translation_units_output}" | awk '
    /^CLANG$/ { section="clang"; next }
    /^CPPCHECK$/ { section="cppcheck"; next }
    section == "clang" && NF { print }
')
mapfile -t cppcheck_units < <(printf '%s\n' "${translation_units_output}" | awk '
    /^CLANG$/ { section="clang"; next }
    /^CPPCHECK$/ { section="cppcheck"; next }
    /^LINE_FILTER$/ { section="line_filter"; next }
    section == "cppcheck" && NF { print }
')
clang_line_filter="$(printf '%s\n' "${translation_units_output}" | awk '
    /^LINE_FILTER$/ { getline; print; exit }
')"

((${#translation_units[@]} > 0)) || die "no translation units found in ${compile_db}"
((${#cppcheck_units[@]} > 0)) || die "no cppcheck inputs found in ${compile_db}"
[[ -n "${clang_line_filter}" ]] || die "no clang-tidy line filter found"

repo_root_no_host="${repo_root#/host}"
normalize_repo_path() {
    local candidate="$1"
    candidate="${candidate#/host}"
    if [[ "${candidate}" == "${repo_root_no_host}/"* ]]; then
        printf '%s\n' "${candidate#${repo_root_no_host}/}"
        return 0
    fi
    if [[ "${candidate}" == /* ]]; then
        die "start path '${candidate}' is outside ${repo_root}"
    fi
    printf '%s\n' "${candidate#./}"
}

if [[ -n "${start_at}" ]]; then
    normalized_start="$(normalize_repo_path "${start_at}")"

    filtered_units=()
    found_clang=0
    for file in "${translation_units[@]}"; do
        if [[ "${file}" == "${normalized_start}" ]]; then
            found_clang=1
        fi
        if (( found_clang )); then
            filtered_units+=("${file}")
        fi
    done
    (( found_clang )) || die "start file '${normalized_start}' is not a clang-tidy translation unit"
    translation_units=("${filtered_units[@]}")
fi

clang_tidy_checks="${MMLTK_CLANG_TIDY_CHECKS:--*,clang-analyzer-*,bugprone-*,-bugprone-easily-swappable-parameters,performance-*,portability-*,-portability-avoid-pragma-once,-portability-simd-intrinsics,modernize-*,-modernize-use-trailing-return-type}"
header_filter="^${repo_root}/(include|src|tests)(/|$)"
total_units="${#translation_units[@]}"

tidy_failed_units=()
if (( ! cppcheck_only )); then
    log "running clang-tidy across ${total_units} translation units with $(nproc) workers"
    tidy_errors_log=$(mktemp)

    export build_dir clang_tidy_checks header_filter clang_line_filter repo_root tidy_errors_log
    printf '%s\n' "${translation_units[@]}" | xargs -I {} -P "$(nproc)" bash -c '
        rel_file="$1"
        abs_file="${repo_root}/${rel_file}"
        output_log="$(mktemp)"
        if ! clang-tidy -p "${build_dir}" "${abs_file}" \
            --checks="${clang_tidy_checks}" \
            --warnings-as-errors="*" \
            --extra-arg=-Wno-unknown-warning-option \
            --header-filter="${header_filter}" \
            --line-filter="${clang_line_filter}" \
            --quiet >"${output_log}" 2>&1; then
            cat "${output_log}" >&2
            printf "%s\n" "${rel_file}" >> "${tidy_errors_log}"
        fi
        rm -f "${output_log}"
    ' -- {}

    if [[ -f "${tidy_errors_log}" ]]; then
        mapfile -t tidy_failed_units < "${tidy_errors_log}"
        rm "${tidy_errors_log}"
    fi
fi

log "running cppcheck across ${#cppcheck_units[@]} translation units"
cppcheck_filters=()
for rel_file in "${cppcheck_units[@]}"; do
    cppcheck_filters+=("--file-filter=*/${rel_file}")
done

cppcheck_failed=0
if ! cppcheck \
    --project="${compile_db}" \
    --cppcheck-build-dir="${cppcheck_build_dir}" \
    --suppressions-list="${repo_root}/utilities/cppcheck.suppressions" \
    --suppress='*:third_party/*' \
    --suppress='*:*/third_party/*' \
    --suppress='*:/src/workspace/deps/*' \
    --suppress='*:*/workspace/deps/*' \
    --inline-suppr \
    --enable=warning,performance,portability \
    --inconclusive \
    --check-level=exhaustive \
    --error-exitcode=1 \
    --platform=unix64 \
    --quiet \
    --relative-paths="${repo_root}" \
    -j "$(nproc)" \
    "${cppcheck_filters[@]}"; then
    cppcheck_failed=1
fi

if ((${#tidy_failed_units[@]} > 0)); then
    log "--------------------------------------------------------------------------------"
    log "clang-tidy FAILED for the following ${#tidy_failed_units[@]} units:"
    for rel_file in "${tidy_failed_units[@]}"; do
        log "  ${rel_file}"
    done
fi

if (( cppcheck_failed )); then
    log "--------------------------------------------------------------------------------"
    log "cppcheck FAILED"
fi

if ((${#tidy_failed_units[@]} > 0 || cppcheck_failed)); then
    log "--------------------------------------------------------------------------------"
    die "static analysis failed"
fi

log "static analysis completed cleanly"
