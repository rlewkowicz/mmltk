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
Usage: tools/run_static_analysis.sh [--third_party] [--file <source>...] [--start-at <translation-unit>] [--cppcheck-only]

Runs the Docker-backed static-analysis pass for this repo:
1. Formats tracked first-party C/C++/CUDA files and, on full passes, the owned Rust frontend
2. Refreshes the cached Docker-side Ninja analysis compile database
3. Runs clang-tidy over tracked first-party C/C++/CUDA translation units
4. Reports cppcheck disabled while its parser lacks C++26 reflection support

Options:
  --third_party                 Include tracked third_party sources except Firefox
  --file <source>...            Format a space-delimited set of tracked C/C++/CUDA
                                sources or modules and analyze their translation units
  --start-at <translation-unit>  Resume analysis from the given file in
                                 compile-database order
  --cppcheck-only                Unavailable while cppcheck lacks C++26 reflection support
  --help                         Show this help

Environment:
  MMLTK_REPO_ROOT         Repo root mounted inside the container
  MMLTK_TIDY_BUILD_DIR    Build dir for the Docker-side compile database
  MMLTK_CLANG_FORMAT      clang-format binary override
  MMLTK_CLANG_TIDY        clang-tidy binary override
  MMLTK_CLANG_TIDY_CHECKS Override the clang-tidy check filter
EOF
}

repo_root="${MMLTK_REPO_ROOT:-$(pwd)}"
build_dir="${MMLTK_TIDY_BUILD_DIR:-${repo_root}/.cache/cmake/analysis}"
cppcheck_build_dir="${build_dir}/cppcheck"
compile_db="${build_dir}/compile_commands.json"
start_at=""
target_files=()
target_files_file=""
cppcheck_only=0
# Cppcheck 2.21 consumes GCC's compilation database but cannot parse GCC 16's
# C++26 reflection tokens and splices. Keep the corrected project selection in
# place so coverage can be restored by changing this gate once its parser
# supports the language used by the repository.
cppcheck_enabled=0
include_third_party=0
translation_units_file=""
cppcheck_units_file=""
cppcheck_project_file=""
cuda_removed_args_file=""
reflection_units_file=""
reflection_targets_file=""
tidy_progress_file=""
tidy_xargs_pid=""

cleanup_static_analysis_files() {
    rm -f "${translation_units_file:-}" "${cppcheck_units_file:-}" "${cppcheck_project_file:-}" \
        "${cuda_removed_args_file:-}" "${target_files_file:-}" "${reflection_units_file:-}" \
        "${reflection_targets_file:-}" "${tidy_progress_file:-}" \
        "${tidy_progress_file:-}.lock"
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
            ((${#target_files[@]} == 0)) || die "--file may only be specified once"
            shift
            [[ $# -gt 0 ]] || die "--file requires a source path"
            while (($# > 0)) && [[ "$1" != --* ]]; do
                target_files+=("$1")
                shift
            done
            ((${#target_files[@]} > 0)) || die "--file requires at least one source path"
            continue
            ;;
        --cppcheck-only)
            cppcheck_only=1
            ;;
        --third_party)
            include_third_party=1
            ;;
        *)
            die "unsupported argument: $1"
            ;;
    esac
    shift
done

((${#target_files[@]} == 0)) || [[ -z "${start_at}" ]] || die "--file and --start-at cannot be combined"
(( ! cppcheck_only || cppcheck_enabled )) ||
    die "--cppcheck-only is unavailable because cppcheck 2.21 cannot parse C++26 reflection"

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

normalized_target_files=()
declare -A seen_target_files=()
for target_file in "${target_files[@]}"; do
    target_file="$(normalize_repo_path "${target_file}")"
    case "${target_file}" in
        *.c|*.cc|*.cpp|*.cppm|*.cxx|*.h|*.hh|*.hpp|*.hxx|*.ipp|*.inl|*.inc|*.cu|*.cuh)
            ;;
        *)
            die "--file requires a C/C++/CUDA source, module, or header"
            ;;
    esac
    [[ "${target_file}" != third_party/firefox/* ]] || die "--file does not support owned Firefox sources"
    if (( ! include_third_party )) && [[ "${target_file}" == third_party/* ]]; then
        die "--file requires --third_party for third_party sources"
    fi
    git -C "${repo_root}" ls-files --error-unmatch -- "${target_file}" >/dev/null 2>&1 \
        || die "--file target '${target_file}' is not tracked"
    if [[ -z "${seen_target_files[${target_file}]+x}" ]]; then
        seen_target_files["${target_file}"]=1
        normalized_target_files+=("${target_file}")
    fi
done
target_files=("${normalized_target_files[@]}")
target_files_file="$(mktemp)"
printf '%s\n' "${target_files[@]}" > "${target_files_file}"

cuda_home="${CUDA_HOME:?container must provide the canonical CUDA root}"
cuda_gpu_arch="sm_86"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:?container must provide the canonical loader search order}"

clang_format_bin="${MMLTK_CLANG_FORMAT:-clang-format}"
command -v "${clang_format_bin}" >/dev/null 2>&1 || die "clang-format is unavailable; rebuild the analysis image"
clang_tidy_bin="${MMLTK_CLANG_TIDY:-clang-tidy}"
command -v "${clang_tidy_bin}" >/dev/null 2>&1 || die "clang-tidy is unavailable; rebuild the analysis image"

format_source_files() {
    local format_jobs
    local tracked_sources
    local source_count
    format_jobs="$(nproc)"
    tracked_sources="$(mktemp)"
    if ((${#target_files[@]} > 0)); then
        local target_file
        for target_file in "${target_files[@]}"; do
            printf '%s\0' "${target_file}" >> "${tracked_sources}"
        done
    elif (( include_third_party )); then
        git -C "${repo_root}" ls-files -z -- \
            '*.c' '*.cc' '*.cpp' '*.cppm' '*.cxx' \
            '*.h' '*.hh' '*.hpp' '*.hxx' \
            '*.ipp' '*.inl' '*.inc' \
            '*.cu' '*.cuh' \
            ':(exclude)third_party/firefox/**' \
            > "${tracked_sources}"
    else
        git -C "${repo_root}" ls-files -z -- \
            '*.c' '*.cc' '*.cpp' '*.cppm' '*.cxx' \
            '*.h' '*.hh' '*.hpp' '*.hxx' \
            '*.ipp' '*.inl' '*.inc' \
            '*.cu' '*.cuh' \
            ':(exclude)third_party/**' \
            > "${tracked_sources}"
    fi

    # git ls-files reads the index, so a file deleted in the worktree stays listed until the
    # deletion is staged. Drop those rather than spraying "No such file or directory" per worker.
    local deleted_sources
    deleted_sources="$(mktemp)"
    git -C "${repo_root}" ls-files -z --deleted > "${deleted_sources}"
    if [[ -s "${deleted_sources}" ]]; then
        local surviving_sources
        surviving_sources="$(mktemp)"
        grep -zvxF -f <(tr '\0' '\n' < "${deleted_sources}") "${tracked_sources}" > "${surviving_sources}" || true
        mv "${surviving_sources}" "${tracked_sources}"
    fi
    rm -f "${deleted_sources}"

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
if ((${#target_files[@]} == 0)); then
    command -v cargo >/dev/null 2>&1 || die "cargo is unavailable; rebuild the analysis image"
    log "running cargo fmt over the first-party Iced frontend"
    cargo fmt --manifest-path "${repo_root}/src/frontend/iced/Cargo.toml" --package mmltk-browser-app
fi

mkdir -p "${build_dir}" "${cppcheck_build_dir}"

log "refreshing cached Ninja analysis build tree at ${build_dir}"
(
    cd "${repo_root}"
    cmake --fresh --preset analysis -B "${build_dir}"
)

[[ -f "${compile_db}" ]] || die "missing compile_commands.json at ${compile_db}"
python3 "${repo_root}/tools/check_toolchain_invariants.py" \
    --repo-root "${repo_root}" --build-dir "${build_dir}"

translation_units_file="$(mktemp)"
cppcheck_units_file="$(mktemp)"
cppcheck_project_file="$(mktemp --suffix=.json)"
cuda_removed_args_file="$(mktemp)"
reflection_units_file="$(mktemp)"
reflection_targets_file="$(mktemp)"
analysis_scope="first-party"
if (( include_third_party )); then
    analysis_scope="first-party and non-Firefox third_party"
fi
log "selecting tracked ${analysis_scope} translation units from ${compile_db}"
python3 - "${compile_db}" "${repo_root}" "${translation_units_file}" "${cppcheck_units_file}" \
    "${cppcheck_project_file}" "${cuda_removed_args_file}" "${target_files_file}" "${include_third_party}" \
    "${reflection_units_file}" "${reflection_targets_file}" <<'PY'
import json
import shlex
import subprocess
import sys
from pathlib import Path

compile_db = Path(sys.argv[1]).resolve()
repo_root = Path(sys.argv[2]).resolve()
clang_units_path = Path(sys.argv[3])
cppcheck_units_path = Path(sys.argv[4])
cppcheck_project_path = Path(sys.argv[5])
cuda_removed_args_path = Path(sys.argv[6])
target_files_path = Path(sys.argv[7])
include_third_party = sys.argv[8] == "1"
reflection_units_path = Path(sys.argv[9])
reflection_targets_path = Path(sys.argv[10])
clang_exts = {".c", ".cc", ".cpp", ".cppm", ".cxx", ".cu"}
cppcheck_exts = {".c", ".cc", ".cpp", ".cppm", ".cxx"}
cppcheck_excluded_units = set()
tracked_pathspecs = [
    "*.c",
    "*.cc",
    "*.cpp",
    "*.cppm",
    "*.cxx",
    "*.cu",
    ":(exclude)third_party/firefox/**",
]
if not include_third_party:
    tracked_pathspecs.append(":(exclude)third_party/**")
tracked_files = {
    file_path.decode("utf-8")
    for file_path in subprocess.check_output(
        ["git", "-C", str(repo_root), "ls-files", "-z", "--", *tracked_pathspecs],
    ).split(b"\0")
    if file_path
}
# git ls-files reads the index; a worktree-deleted file is still listed until staged.
tracked_files -= {
    file_path.decode("utf-8")
    for file_path in subprocess.check_output(
        ["git", "-C", str(repo_root), "ls-files", "-z", "--deleted"],
    ).split(b"\0")
    if file_path
}
target_files = {
    target_file
    for target_file in target_files_path.read_text(encoding="utf-8").splitlines()
    if target_file
}
if target_files:
    tracked_files.intersection_update(target_files)


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


def command_arguments(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


nvcc_standalone_args = {
    "-forward-unknown-to-host-compiler",
    "--forward-unknown-to-host-compiler",
    "--allow-unsupported-compiler",
    "-allow-unsupported-compiler",
    "-lineinfo",
    "--Werror",
    "-Werror",
    "all-warnings",
}
nvcc_value_args = {
    "-ccbin",
    "--compiler-bindir",
    "-Xcompiler",
    "--compiler-options",
    "--generate-code",
    "-gencode",
    "-rdc",
    "--relocatable-device-code",
}
nvcc_arg_prefixes = tuple(f"{arg}=" for arg in nvcc_value_args)


def nvcc_removed_args(entry: dict) -> set[str]:
    arguments = command_arguments(entry)
    removed = set()
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        if argument in nvcc_standalone_args or argument.startswith(nvcc_arg_prefixes):
            removed.add(argument)
        elif argument in nvcc_value_args:
            removed.add(argument)
            if index + 1 < len(arguments):
                index += 1
                removed.add(arguments[index])
        index += 1
    return removed


clang_units = []
cppcheck_units = []
cppcheck_commands = []
cuda_removed_args = set()
seen_clang = set()
seen_cppcheck = set()
reflection_units = []
reflection_targets = set()
for entry in iter_compile_commands(compile_db):
    file_path = Path(entry["file"]).resolve()
    try:
        rel = file_path.relative_to(repo_root).as_posix()
    except ValueError:
        continue
    if rel not in tracked_files:
        continue
    ext = file_path.suffix.lower()
    arguments = command_arguments(entry)
    if ext in cppcheck_exts and rel not in cppcheck_excluded_units and rel not in seen_cppcheck:
        seen_cppcheck.add(rel)
        cppcheck_units.append(rel)
        cppcheck_commands.append(entry)
    if "-freflection" in arguments:
        reflection_units.append(rel)
        output = entry.get("output", "")
        if output:
            output_path = Path(output)
            if output_path.is_absolute():
                output_path = output_path.resolve().relative_to(compile_db.parent)
            reflection_targets.add(output_path.as_posix())
        continue
    if ext in clang_exts and rel not in seen_clang:
        seen_clang.add(rel)
        clang_units.append(rel)
        if ext == ".cu":
            cuda_removed_args.update(nvcc_removed_args(entry))

clang_units.sort()
cppcheck_units.sort()
clang_units_path.write_text("".join(f"{rel}\n" for rel in clang_units), encoding="utf-8")
cppcheck_units_path.write_text("".join(f"{rel}\n" for rel in cppcheck_units), encoding="utf-8")
cppcheck_project_path.write_text(json.dumps(cppcheck_commands), encoding="utf-8")
cuda_removed_args_path.write_text(
    "".join(f"{argument}\n" for argument in sorted(cuda_removed_args)),
    encoding="utf-8",
)
reflection_units_path.write_text(
    "".join(f"{rel}\n" for rel in sorted(reflection_units)), encoding="utf-8"
)
reflection_targets_path.write_text(
    "".join(f"{target}\n" for target in sorted(reflection_targets)), encoding="utf-8"
)
PY

mapfile -t translation_units < "${translation_units_file}"
mapfile -t cppcheck_units < "${cppcheck_units_file}"
mapfile -t reflection_units < "${reflection_units_file}"
mapfile -t reflection_targets < "${reflection_targets_file}"

if ((${#translation_units[@]} == 0 && ${#reflection_units[@]} == 0)); then
    die "no translation units found in ${compile_db}"
fi
cuda_unit_count="$(grep -cE '\.cu$' "${translation_units_file}" || true)"
host_unit_count=$((${#translation_units[@]} - cuda_unit_count))
log "selected ${#translation_units[@]} clang-tidy units (${host_unit_count} host, ${cuda_unit_count} CUDA) and ${#cppcheck_units[@]} cppcheck units"
reflection_failed=0
if ((${#reflection_units[@]} > 0)); then
    ((${#reflection_targets[@]} > 0)) || die "reflection translation units have no owning CMake target"
    log "validating ${#reflection_units[@]} GCC-reflection units through their exact supported compiler objects"
    if ! cmake --build "${build_dir}" --parallel "$(nproc)" --target "${reflection_targets[@]}" -- -k 0; then
        reflection_failed=1
    fi
    python3 "${repo_root}/tools/check_toolchain_invariants.py" \
        --repo-root "${repo_root}" --build-dir "${build_dir}"
fi

if [[ -n "${start_at}" ]]; then
    normalized_start="$(normalize_repo_path "${start_at}")"
    if (( ! include_third_party )) && [[ "${normalized_start}" == third_party/* ]]; then
        die "--start-at requires --third_party for third_party sources"
    fi

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
if (( ! cppcheck_only && total_units > 0 )); then
    log "running clang-tidy across ${total_units} translation units with $(nproc) workers"
    tidy_errors_log=$(mktemp)
    tidy_progress_file=$(mktemp)
    printf '0\n' > "${tidy_progress_file}"

    export build_dir clang_tidy_bin clang_tidy_checks cuda_gpu_arch cuda_home cuda_removed_args_file repo_root \
        tidy_errors_log tidy_progress_file total_units
    xargs -r -a "${translation_units_file}" -n 1 -P "$(nproc)" bash -c '
        rel_file="$1"
        abs_file="${repo_root}/${rel_file}"
        unit_checks="${clang_tidy_checks}"
        cuda_tidy_args=()
        tidy_host_args=()
        if [[ "${rel_file}" == *.cu ]]; then
            while IFS= read -r removed_arg; do
                [[ -n "${removed_arg}" ]] || continue
                cuda_tidy_args+=(--removed-arg="${removed_arg}")
            done < "${cuda_removed_args_file}"
            cuda_tidy_args+=(
                --extra-arg-before=--gcc-toolchain=/opt/gcc-16.2
                --extra-arg="--cuda-path=${cuda_home}"
                --extra-arg="--cuda-gpu-arch=${cuda_gpu_arch}"
                --extra-arg=-Wno-unknown-cuda-version
            )
        else
            # The GCC 16 aggregate hardening switch is intentionally replayed by
            # the actual compiler, but Clang 22 does not recognize it.  Keep
            # clang-tidy on the same GCC-header view by removing those GCC-only
            # arguments and spelling the accepted compile hardening pieces
            # explicitly. Analysis uses the stable Clang C++23 frontend while
            # GCC and cppcheck retain C++26.
            tidy_host_args=(
                --removed-arg=-fhardened
                --removed-arg=-Werror=hardened
                --removed-arg=-std=c++26
                --extra-arg-before=--gcc-toolchain=/opt/gcc-16.2
                --extra-arg=-std=c++23
                --extra-arg=-D_FORTIFY_SOURCE=3
                --extra-arg=-D_GLIBCXX_ASSERTIONS
                --extra-arg=-ftrivial-auto-var-init=zero
                --extra-arg=-fstack-protector-strong
                --extra-arg=-fstack-clash-protection
                --extra-arg=-fcf-protection=full
            )
        fi
        case "${rel_file}" in
            src/*)
                unit_header_filter="^${repo_root}/src/"
                ;;
            third_party/rapidgzip/src/external/isa-l/*)
                component="${rel_file#third_party/}"
                component="${component%%/*}"
                unit_header_filter="^${repo_root}/third_party/${component}/"
                unit_checks="${unit_checks},-bugprone-branch-clone,-bugprone-implicit-widening-of-multiplication-result,-bugprone-narrowing-conversions,-bugprone-suspicious-string-compare,-bugprone-switch-missing-default-case,-bugprone-too-small-loop-variable,-clang-analyzer-core.uninitialized.Assign,-clang-analyzer-deadcode.DeadStores,-performance-no-int-to-ptr"
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
        if ! "${clang_tidy_bin}" -p "${build_dir}" "${abs_file}" \
            --checks="${unit_checks}" \
            --warnings-as-errors="*" \
            --extra-arg=-Wno-unknown-warning-option \
            --extra-arg=-DMMLTK_CLANG_TIDY=1 \
            --header-filter="${unit_header_filter}" \
            "${tidy_host_args[@]}" \
            "${cuda_tidy_args[@]}" \
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
elif (( ! cppcheck_only )); then
    log "skipping clang-tidy because Clang does not implement the selected GCC reflection syntax"
fi

cppcheck_failed=0
if (( cppcheck_enabled && ${#cppcheck_units[@]} > 0)); then
    log "running cppcheck across ${#cppcheck_units[@]} translation units"
    if ! cppcheck \
        --project="${cppcheck_project_file}" \
        --cppcheck-build-dir="${cppcheck_build_dir}" \
        --suppressions-list="${repo_root}/tools/cppcheck.suppressions" \
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
elif (( ! cppcheck_enabled )); then
    log "skipping cppcheck because cppcheck 2.21 cannot parse C++26 reflection"
else
    log "skipping cppcheck because the selected target has no eligible host C/C++ translation unit"
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

if (( reflection_failed )); then
    log "--------------------------------------------------------------------------------"
    log "GCC reflection compiler validation FAILED"
fi

if ((${#tidy_failed_units[@]} > 0 || cppcheck_failed || reflection_failed)); then
    log "--------------------------------------------------------------------------------"
    die "static analysis failed"
fi

log "static analysis completed cleanly"
