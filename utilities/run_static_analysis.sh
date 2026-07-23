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
Usage: utilities/run_static_analysis.sh [--file <translation-unit>] [--start-at <translation-unit>] [--cppcheck-only]

Runs the Docker-backed static-analysis pass for this repo:
1. Runs clang-format -i over every tracked C/C++/CUDA source file except Firefox
2. Refreshes the cached Docker-side Ninja analysis compile database
3. Runs clang-tidy over every tracked host translation unit except Firefox
4. Runs cppcheck over the same project file set

Options:
  --file <translation-unit>     Run clang-format, clang-tidy, and cppcheck only
                                for one tracked C/C++ translation unit
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
build_dir="${MMLTK_TIDY_BUILD_DIR:-${repo_root}/.cache/cmake/analysis}"
cppcheck_build_dir="${build_dir}/cppcheck"
compile_db="${build_dir}/compile_commands.json"
start_at=""
target_file=""
cppcheck_only=0
translation_units_file=""
cppcheck_units_file=""
cppcheck_project_file=""
tidy_progress_file=""
tidy_xargs_pid=""

cleanup_static_analysis_files() {
    rm -f "${translation_units_file:-}" "${cppcheck_units_file:-}" "${cppcheck_project_file:-}" \
        "${tidy_progress_file:-}" "${tidy_progress_file:-}.lock"
}

terminate_static_analysis() {
    trap - INT TERM EXIT
    log "received termination; stopping static-analysis workers"
    if [[ -n "${tidy_xargs_pid}" ]]; then
        pkill -TERM -P "${tidy_xargs_pid}" 2>/dev/null || true
        kill -TERM "${tidy_xargs_pid}" 2>/dev/null || true
    fi
    cleanup_static_analysis_files
    exit 130
}

trap cleanup_static_analysis_files EXIT
trap terminate_static_analysis INT TERM

setup_tidy_logging() {
    local tidy_log_file="${MMLTK_TIDY_LOG_FILE:-}"
    if [[ -z "${tidy_log_file}" ]]; then
        tidy_log_file="${repo_root}/build/logs/mmltk-tidy-$(date +%Y%m%d-%H%M%S).log"
    elif [[ "${tidy_log_file}" != /* ]]; then
        tidy_log_file="${repo_root}/${tidy_log_file}"
    fi

    export MMLTK_TIDY_LOG_FILE="${tidy_log_file}"
    if [[ "${MMLTK_TIDY_TEE_STARTED:-0}" != "1" ]]; then
        mkdir -p "$(dirname "${tidy_log_file}")"
        touch "${tidy_log_file}"
        export MMLTK_TIDY_TEE_STARTED=1
        exec > >(tee -a "${tidy_log_file}") 2>&1
    fi
    log "writing tidy log to ${tidy_log_file}"
    log "tail with: tail -f ${tidy_log_file}"
}

setup_tidy_logging

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
        --file)
            shift
            [[ $# -gt 0 ]] || die "--file requires a translation-unit path"
            [[ -z "${target_file}" ]] || die "--file may only be specified once"
            target_file="$1"
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

[[ -z "${target_file}" || -z "${start_at}" ]] || die "--file and --start-at cannot be combined"

repo_root_no_host="${repo_root#/host}"
normalize_repo_path() {
    local candidate="$1"
    candidate="${candidate#/host}"
    if [[ "${candidate}" == "${repo_root_no_host}/"* ]]; then
        printf '%s\n' "${candidate#${repo_root_no_host}/}"
        return 0
    fi
    if [[ "${candidate}" == /* ]]; then
        die "path '${candidate}' is outside ${repo_root}"
    fi
    printf '%s\n' "${candidate#./}"
}

if [[ -n "${target_file}" ]]; then
    target_file="$(normalize_repo_path "${target_file}")"
    case "${target_file}" in
        *.c|*.cc|*.cpp|*.cxx)
            ;;
        *)
            die "--file requires a C/C++ translation unit (.c, .cc, .cpp, or .cxx)"
            ;;
    esac
    [[ "${target_file}" != third_party/firefox/* ]] || die "--file does not support owned Firefox sources"
    git -C "${repo_root}" ls-files --error-unmatch -- "${target_file}" >/dev/null 2>&1 \
        || die "--file target '${target_file}' is not tracked"
fi

export LD_LIBRARY_PATH="/usr/local/cuda-13.0/lib64:/opt/pytorch/lib:/opt/onnx/lib:/opt/onnxruntime/lib:${LD_LIBRARY_PATH:-}"

clang_format_bin="${MMLTK_CLANG_FORMAT:-clang-format}"
command -v "${clang_format_bin}" >/dev/null 2>&1 || die "clang-format is unavailable; rebuild the analysis image"

format_source_files() {
    local format_jobs
    local tracked_sources
    local source_count
    format_jobs="$(nproc)"
    tracked_sources="$(mktemp)"
    if [[ -n "${target_file}" ]]; then
        printf '%s\0' "${target_file}" > "${tracked_sources}"
    else
        git -C "${repo_root}" ls-files -z -- \
            '*.c' '*.cc' '*.cpp' '*.cxx' \
            '*.h' '*.hh' '*.hpp' '*.hxx' \
            '*.ipp' '*.inl' '*.inc' \
            '*.cu' '*.cuh' \
            ':(exclude)third_party/firefox/**' \
            > "${tracked_sources}"
    fi

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

log "refreshing cached Ninja analysis build tree at ${build_dir}"
(
    cd "${repo_root}"
    cmake --preset analysis -B "${build_dir}"
)

[[ -f "${compile_db}" ]] || die "missing compile_commands.json at ${compile_db}"

translation_units_file="$(mktemp)"
cppcheck_units_file="$(mktemp)"
cppcheck_project_file="$(mktemp --suffix=.json)"
log "selecting tracked non-Firefox translation units from ${compile_db}"
python3 - "${compile_db}" "${repo_root}" "${translation_units_file}" "${cppcheck_units_file}" \
    "${cppcheck_project_file}" "${target_file}" <<'PY'
import json
import subprocess
import sys
from pathlib import Path

compile_db = Path(sys.argv[1]).resolve()
repo_root = Path(sys.argv[2]).resolve()
clang_units_path = Path(sys.argv[3])
cppcheck_units_path = Path(sys.argv[4])
cppcheck_project_path = Path(sys.argv[5])
target_file = sys.argv[6]
# clang-tidy in this toolchain cannot reliably replay the NVCC command lines in
# compile_commands.json for CUDA 13 builds, so keep the pass on host C/C++ TUs.
clang_exts = {".c", ".cc", ".cpp", ".cxx"}
cppcheck_exts = {".c", ".cc", ".cpp", ".cxx"}
tracked_files = {
    file_path.decode("utf-8")
    for file_path in subprocess.check_output(
        [
            "git",
            "-C",
            str(repo_root),
            "ls-files",
            "-z",
            "--",
            "*.c",
            "*.cc",
            "*.cpp",
            "*.cxx",
            ":(exclude)third_party/firefox/**",
        ],
    ).split(b"\0")
    if file_path
}
if target_file:
    tracked_files.intersection_update({target_file})


def iter_compile_commands(path: Path):
    decoder = json.JSONDecoder()
    buffer = ""
    started = False
    with path.open("r", encoding="utf-8") as handle:
        while True:
            if not buffer:
                chunk = handle.read(1 << 20)
                if not chunk:
                    return
                buffer += chunk
            if not started:
                stripped = buffer.lstrip()
                if not stripped:
                    buffer = ""
                    continue
                if stripped[0] != "[":
                    raise ValueError(f"{path} is not a JSON array")
                buffer = stripped[1:]
                started = True
            buffer = buffer.lstrip()
            if not buffer:
                continue
            if buffer[0] == "]":
                return
            if buffer[0] == ",":
                buffer = buffer[1:]
                continue
            while True:
                try:
                    entry, offset = decoder.raw_decode(buffer)
                    break
                except json.JSONDecodeError:
                    chunk = handle.read(1 << 20)
                    if not chunk:
                        raise
                    buffer += chunk
            yield entry
            buffer = buffer[offset:]


clang_units = []
cppcheck_units = []
cppcheck_commands = []
seen_clang = set()
seen_cppcheck = set()
for entry in iter_compile_commands(compile_db):
    file_path = Path(entry["file"]).resolve()
    try:
        rel = file_path.relative_to(repo_root).as_posix()
    except ValueError:
        continue
    if rel not in tracked_files:
        continue
    ext = file_path.suffix.lower()
    if ext in clang_exts and rel not in seen_clang:
        seen_clang.add(rel)
        clang_units.append(rel)
    if ext in cppcheck_exts and rel not in seen_cppcheck:
        seen_cppcheck.add(rel)
        cppcheck_units.append(rel)
        cppcheck_commands.append(entry)

clang_units.sort()
cppcheck_units.sort()
clang_units_path.write_text("".join(f"{rel}\n" for rel in clang_units), encoding="utf-8")
cppcheck_units_path.write_text("".join(f"{rel}\n" for rel in cppcheck_units), encoding="utf-8")
cppcheck_project_path.write_text(json.dumps(cppcheck_commands), encoding="utf-8")
PY

mapfile -t translation_units < "${translation_units_file}"
mapfile -t cppcheck_units < "${cppcheck_units_file}"

((${#translation_units[@]} > 0)) || die "no translation units found in ${compile_db}"
((${#cppcheck_units[@]} > 0)) || die "no cppcheck inputs found in ${compile_db}"
log "selected ${#translation_units[@]} clang-tidy units and ${#cppcheck_units[@]} cppcheck units"

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
    printf '%s\n' "${translation_units[@]}" > "${translation_units_file}"
fi

clang_tidy_checks="${MMLTK_CLANG_TIDY_CHECKS:--*,clang-analyzer-*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-macro-parentheses,performance-*,portability-*,-portability-avoid-pragma-once,-portability-simd-intrinsics}"
total_units="${#translation_units[@]}"

tidy_failed_units=()
if (( ! cppcheck_only )); then
    log "running clang-tidy across ${total_units} translation units with $(nproc) workers"
    tidy_errors_log=$(mktemp)
    tidy_progress_file=$(mktemp)
    printf '0\n' > "${tidy_progress_file}"

    export build_dir clang_tidy_checks repo_root tidy_errors_log tidy_progress_file total_units
    xargs -r -a "${translation_units_file}" -n 1 -P "$(nproc)" bash -c '
        rel_file="$1"
        abs_file="${repo_root}/${rel_file}"
        case "${rel_file}" in
            src/*)
                unit_header_filter="^${repo_root}/(include|src)/"
                ;;
            tests/*)
                unit_header_filter="^${repo_root}/(include|src|tests)/"
                ;;
            third_party/*)
                component="${rel_file#third_party/}"
                component="${component%%/*}"
                unit_header_filter="^${repo_root}/third_party/${component}/"
                ;;
            *)
                unit_header_filter="^${repo_root}/"
                ;;
        esac
        output_log="$(mktemp)"
        printf "mmltk-tidy: clang-tidy start %s\n" "${rel_file}" >&2
        status="ok"
        if ! clang-tidy -p "${build_dir}" "${abs_file}" \
            --checks="${clang_tidy_checks}" \
            --warnings-as-errors="*" \
            --extra-arg=-Wno-unknown-warning-option \
            --extra-arg=-DMMLTK_CLANG_TIDY=1 \
            --header-filter="${unit_header_filter}" \
            --quiet >"${output_log}" 2>&1; then
            status="FAILED"
            sed -E "/^[0-9]+ warnings?( and [0-9]+ errors?)? generated\.$/d" "${output_log}" >&2
            printf "%s\n" "${rel_file}" >> "${tidy_errors_log}"
        fi
        rm -f "${output_log}"
        {
            flock 9
            completed="$(cat "${tidy_progress_file}")"
            completed="$((completed + 1))"
            printf "%s\n" "${completed}" > "${tidy_progress_file}"
            printf "mmltk-tidy: clang-tidy [%s/%s] %s %s\n" \
                "${completed}" "${total_units}" "${status}" "${rel_file}" >&2
        } 9>"${tidy_progress_file}.lock"
    ' -- &
    tidy_xargs_pid="$!"
    wait "${tidy_xargs_pid}"
    tidy_xargs_pid=""

    if [[ -f "${tidy_errors_log}" ]]; then
        mapfile -t tidy_failed_units < "${tidy_errors_log}"
        rm "${tidy_errors_log}"
    fi
    rm -f "${tidy_progress_file}" "${tidy_progress_file}.lock"
    tidy_progress_file=""
fi

log "running cppcheck across ${#cppcheck_units[@]} translation units"

cppcheck_failed=0
if ! cppcheck \
    --project="${cppcheck_project_file}" \
    --cppcheck-build-dir="${cppcheck_build_dir}" \
    --suppressions-list="${repo_root}/utilities/cppcheck.suppressions" \
    --suppress='*:/src/workspace/deps/*' \
    --suppress='*:*/workspace/deps/*' \
    --inline-suppr \
    --quiet \
    --enable=warning,performance,portability \
    --inconclusive \
    --check-level=exhaustive \
    --error-exitcode=1 \
    --platform=unix64 \
    --relative-paths="${repo_root}" \
    -j "$(nproc)"; then
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
