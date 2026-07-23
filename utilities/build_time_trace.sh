#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cache_key=""
run_label=""
warm_rebuild=0
noop_run=0

log() {
    printf 'build_time_trace: %s\n' "$*" >&2
}

die() {
    log "$*"
    exit 1
}

usage() {
    printf '%s\n' \
        'Usage: utilities/build_time_trace.sh [--cache-key NAME] [--warm-rebuild|--no-op] [--label LABEL]' \
        '' \
        'Runs ./mmltk --build with an isolated repository-local compile cache and' \
        'staged release. Normal .cache/cmake, Cargo, ccache, Firefox, browser, and' \
        'build/release state is never cleared or overwritten.' \
        '' \
        'Options:' \
        '  --cache-key NAME  Reuse .cache/build-time-trace/NAME across runs.' \
        '                    Omit for a new cold cache.' \
        '  --warm-rebuild    Remove only isolated CMake/Firefox/staged products,' \
        '                    preserving compiler, Cargo, mozbuild, browser, and' \
        '                    BuildKit caches.' \
        '                    Requires a completed prior run with --cache-key.' \
        '  --no-op           Request an unchanged no-op measurement from a' \
        '                    completed prior run with --cache-key.' \
        '  --label LABEL     Store a human-readable label in run.json.' \
        '  --help            Show this help.'
}

while (($# > 0)); do
    case "$1" in
        --cache-key)
            shift
            (($# > 0)) || die '--cache-key requires a value'
            cache_key="$1"
            ;;
        --label)
            shift
            (($# > 0)) || die '--label requires a value'
            run_label="$1"
            ;;
        --warm-rebuild)
            warm_rebuild=1
            ;;
        --no-op)
            noop_run=1
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "unsupported argument: $1"
            ;;
    esac
    shift
done

(( warm_rebuild + noop_run <= 1 )) || die \
    '--warm-rebuild and --no-op are mutually exclusive'

if [[ -z "${cache_key}" ]]; then
    cache_key="cold-$(date -u +%Y%m%dt%H%M%S)z-$$"
fi
[[ "${cache_key}" =~ ^[a-z0-9][a-z0-9._-]*$ ]] || die \
    '--cache-key must start with a lowercase letter or digit and contain only a-z, 0-9, dot, underscore, or dash'

cache_namespace="${repo_root}/.cache/build-time-trace"
trace_namespace="${repo_root}/build/time-trace"
cache_root_expected="${cache_namespace}/${cache_key}"
trace_key_root_expected="${trace_namespace}/${cache_key}"
cache_root="$(realpath -m -- "${cache_root_expected}")"
trace_key_root="$(realpath -m -- "${trace_key_root_expected}")"
trace_workspace_expected="${trace_key_root}/workspace"
trace_workspace="$(realpath -m -- "${trace_workspace_expected}")"

require_descendant() {
    local candidate="$1"
    local parent="$2"
    local label="$3"
    case "${candidate}" in
        "${parent}/"*)
            ;;
        *)
            die "${label} must stay beneath ${parent}"
            ;;
    esac
}

require_exact_path() {
    local resolved_path="$1"
    local expected_path="$2"
    local label="$3"
    [[ "${resolved_path}" == "${expected_path}" ]] || die \
        "${label} resolves to ${resolved_path}, expected ${expected_path}"
}

require_descendant "${cache_root}" "${cache_namespace}" 'trace cache root'
require_descendant "${trace_key_root}" "${trace_namespace}" 'trace report root'
require_descendant "${trace_workspace}" "${trace_key_root}" 'trace workspace'
require_exact_path "${cache_root}" "${cache_root_expected}" 'trace cache root'
require_exact_path "${trace_key_root}" "${trace_key_root_expected}" 'trace report root'
require_exact_path "${trace_workspace}" "${trace_workspace_expected}" 'trace workspace'

release_stage_root="${trace_workspace}/release"
browser_stage_root="${trace_workspace}/browser-app"
mkdir -p "${cache_root}" "${trace_workspace}"

require_unaliased_path() {
    local expected_path="$1"
    local label="$2"
    local resolved_path
    resolved_path="$(realpath -m -- "${expected_path}")"
    require_exact_path "${resolved_path}" "${expected_path}" "${label}"
}

validate_trace_build_paths() {
    require_unaliased_path "${cache_root}/cmake" 'trace CMake root'
    require_unaliased_path "${cache_root}/cmake/release" 'trace release Ninja root'
    require_unaliased_path "${cache_root}/cargo" 'trace Cargo root'
    require_unaliased_path "${cache_root}/cargo/home" 'trace Cargo home'
    require_unaliased_path "${cache_root}/cargo/target" 'trace Cargo target root'
    require_unaliased_path "${cache_root}/cargo/target/browser-app" \
        'trace browser Cargo target'
    require_unaliased_path "${cache_root}/browser-app" 'trace browser state root'
    require_unaliased_path "${cache_root}/buildkit" 'trace BuildKit cache root'
    require_unaliased_path "${cache_root}/ccache" 'trace ccache root'
    require_unaliased_path "${cache_root}/ccache/tmp" 'trace ccache temporary root'
    require_unaliased_path "${cache_root}/container-home" 'trace container home'
    require_unaliased_path "${cache_root}/firefox" 'trace Firefox cache root'
    require_unaliased_path "${cache_root}/firefox/mozbuild" 'trace Firefox mozbuild root'
    require_unaliased_path "${cache_root}/firefox/sccache" 'trace Firefox sccache root'
    require_unaliased_path "${cache_root}/firefox/obj-minimal-opt" \
        'trace Firefox object root'
    require_unaliased_path "${cache_root}/image-fingerprints" \
        'trace image fingerprint root'
    require_unaliased_path "${cache_root}/locks" 'trace cache lock root'
    require_unaliased_path "${release_stage_root}" 'trace release stage'
    require_unaliased_path "${release_stage_root}.next" 'trace next release stage'
    require_unaliased_path "${release_stage_root}.previous" 'trace previous release stage'
    require_unaliased_path "${browser_stage_root}" 'trace browser stage'
    require_unaliased_path "${browser_stage_root}/dist" 'trace browser distribution'
    require_unaliased_path "${browser_stage_root}/dist.next" \
        'trace next browser distribution'
}

validate_trace_build_paths

command -v flock >/dev/null 2>&1 || die 'flock is required to serialize named timing traces'
trace_lock_expected="${cache_root}/.run.lock"
trace_lock_file="$(realpath -m -- "${trace_lock_expected}")"
require_descendant "${trace_lock_file}" "${cache_root}" 'trace lock file'
require_exact_path "${trace_lock_file}" "${trace_lock_expected}" 'trace lock file'
[[ ! -e "${trace_lock_file}" || -f "${trace_lock_file}" ]] || die \
    "trace lock path is not a regular file: ${trace_lock_file}"
exec {trace_lock_fd}>>"${trace_lock_file}"
flock -n "${trace_lock_fd}" || die \
    "another timing trace is already using cache key '${cache_key}' in this checkout"

run_mode=cold
completed_run_available=0
if [[ -f "${cache_root}/cmake/release/.ninja_log" \
    && -f "${cache_root}/firefox/obj-minimal-opt/dist/.mmltk-runtime-stage.sha256" \
    && -f "${release_stage_root}/.mmltk-package-fingerprint" ]]; then
    completed_run_available=1
fi
if (( warm_rebuild )); then
    (( completed_run_available )) || die \
        '--warm-rebuild requires a successful prior run using the same --cache-key'
    log 'resetting isolated build products while preserving compiler caches'
    warm_cache_cmake="$(realpath -m -- "${cache_root}/cmake")"
    warm_firefox_obj="$(realpath -m -- "${cache_root}/firefox/obj-minimal-opt")"
    warm_release_stage="$(realpath -m -- "${release_stage_root}")"
    warm_browser_stage="$(realpath -m -- "${browser_stage_root}")"
    require_exact_path "${warm_cache_cmake}" "${cache_root}/cmake" \
        'warm CMake reset target'
    require_exact_path "${warm_firefox_obj}" "${cache_root}/firefox/obj-minimal-opt" \
        'warm Firefox reset target'
    require_exact_path "${warm_release_stage}" "${release_stage_root}" \
        'warm release reset target'
    require_exact_path "${warm_browser_stage}" "${browser_stage_root}" \
        'warm browser reset target'
    rm -rf -- \
        "${warm_cache_cmake}" \
        "${warm_firefox_obj}" \
        "${warm_release_stage}" \
        "${warm_browser_stage}"
    run_mode=warm
elif (( noop_run )); then
    (( completed_run_available )) || die \
        '--no-op requires a successful prior run using the same --cache-key'
    run_mode=noop
elif (( completed_run_available )); then
    run_mode=reuse
fi

run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
report_parent_expected="${trace_key_root}/runs"
report_parent="$(realpath -m -- "${report_parent_expected}")"
require_exact_path "${report_parent}" "${report_parent_expected}" 'trace report parent'
mkdir -p "${report_parent}"
report_dir_expected="${report_parent}/${run_id}"
report_dir="$(realpath -m -- "${report_dir_expected}")"
require_descendant "${report_dir}" "${trace_key_root}" 'trace run report'
require_exact_path "${report_dir}" "${report_dir_expected}" 'trace run report'
mkdir -- "${report_dir}" || die "trace run report already exists: ${report_dir}"
phase_trace_file="${report_dir}/phases.jsonl"
firefox_stage_trace_file="${report_dir}/phases-firefox-stage.jsonl"
build_log_file="${report_dir}/build.log"
evidence_file="${report_dir}/cache-evidence.jsonl"
summary_file="${report_dir}/trace-summary.json"
runtime_layers_file="${report_dir}/runtime-layers.jsonl"
trace_identity_hash="$({ printf '%s\0' "${repo_root}"; printf '%s' "${cache_key}"; } \
    | sha256sum | cut -c1-16)"
runtime_image="${MMLTK_TRACE_IMAGE:-mmltk-trace:${trace_identity_hash}}"
trace_container_name="mmltk-trace-${trace_identity_hash}"
trace_buildx_builder="mmltk-trace-${trace_identity_hash}-buildx"
trace_build_image="mmltk-trace-build:${trace_identity_hash}"
trace_runtime_base_image="mmltk-trace-runtime-base:${trace_identity_hash}"

cleanup_trace_builder() {
    docker buildx rm "${trace_buildx_builder}" >/dev/null 2>&1 || true
}
cleanup_trace_builder
trap cleanup_trace_builder EXIT

log "cache key: ${cache_key}"
log "isolated cache: ${cache_root}"
log "report: ${report_dir}"
log "mode: ${run_mode}"
log "reuse --cache-key ${cache_key} unchanged for a no-op comparison"

set +e
MMLTK_BROWSER_STAGE_ROOT="${browser_stage_root}" \
MMLTK_BUILD_TRACE_FILE="${phase_trace_file}" \
MMLTK_BUILD_IMAGE="${trace_build_image}" \
MMLTK_BUILDX_BUILDER="${trace_buildx_builder}" \
MMLTK_CACHE_ROOT="${cache_root}" \
MMLTK_CCACHE_STATS_FILE="${report_dir}/ccache-stats.txt" \
MMLTK_CONTAINER_NAME="${trace_container_name}" \
MMLTK_FIREFOX_SCCACHE_STATS_FILE="${report_dir}/sccache-stats.txt" \
MMLTK_IMAGE="${runtime_image}" \
MMLTK_RELEASE_STAGE_ROOT="${release_stage_root}" \
MMLTK_RUNTIME_BASE_IMAGE="${trace_runtime_base_image}" \
    "${repo_root}/mmltk" --build 2>&1 | tee "${build_log_file}"
build_status="${PIPESTATUS[0]}"
set -e

path_bytes() {
    local path="$1"
    if [[ -e "${path}" ]]; then
        du -sb -- "${path}" | cut -f1
    else
        printf '0\n'
    fi
}

file_count() {
    local path="$1"
    if [[ -d "${path}" ]]; then
        find "${path}" -type f -printf '.' | wc -c | tr -d '[:space:]'
    else
        printf '0\n'
    fi
}

ninja_log_lines() {
    local path="$1"
    if [[ -d "${path}" ]]; then
        find "${path}" -type f -name .ninja_log \
            -exec awk 'FNR > 1 { count += 1 } END { print count + 0 }' {} + \
            | awk '{ total += $1 } END { print total + 0 }'
    else
        printf '0\n'
    fi
}

pch_file_count() {
    local path="$1"
    if [[ -d "${path}" ]]; then
        find "${path}" -type f \( -name '*.gch' -o -name '*.pch' \) -printf '%s\n' \
            | awk 'END { print NR + 0 }'
    else
        printf '0\n'
    fi
}

pch_total_bytes() {
    local path="$1"
    if [[ -d "${path}" ]]; then
        find "${path}" -type f \( -name '*.gch' -o -name '*.pch' \) -printf '%s\n' \
            | awk '{ total += $1 } END { print total + 0 }'
    else
        printf '0\n'
    fi
}

record_metric() {
    local name="$1"
    local value="$2"
    local unit="$3"
    printf '{"metric":"%s","value":%s,"unit":"%s"}\n' "${name}" "${value}" "${unit}" \
        >>"${evidence_file}"
}

: >"${evidence_file}"
touch "${phase_trace_file}"
touch "${firefox_stage_trace_file}"
: >"${runtime_layers_file}"
runtime_image_total=0
if docker image inspect "${runtime_image}" >/dev/null 2>&1; then
    runtime_image_total="$(docker image inspect "${runtime_image}" \
        --format '{{.Size}}')"
    docker image history --no-trunc \
        --format '{"id":{{json .ID}},"size":{{json .Size}},"created_by":{{json .CreatedBy}}}' \
        "${runtime_image}" >"${runtime_layers_file}" || true
fi

record_metric build_exit_status "${build_status}" code
record_metric cache_total "$(path_bytes "${cache_root}")" bytes
record_metric browser_state_total "$(path_bytes "${cache_root}/browser-app")" bytes
record_metric buildkit_total "$(path_bytes "${cache_root}/buildkit")" bytes
record_metric buildkit_files "$(file_count "${cache_root}/buildkit")" files
record_metric ccache_total "$(path_bytes "${cache_root}/ccache")" bytes
record_metric ccache_files "$(file_count "${cache_root}/ccache")" files
record_metric cargo_total "$(path_bytes "${cache_root}/cargo")" bytes
record_metric cargo_files "$(file_count "${cache_root}/cargo")" files
record_metric firefox_obj_total "$(path_bytes "${cache_root}/firefox/obj-minimal-opt")" bytes
record_metric firefox_mozbuild_total "$(path_bytes "${cache_root}/firefox/mozbuild")" bytes
record_metric firefox_sccache_total "$(path_bytes "${cache_root}/firefox/sccache")" bytes
record_metric firefox_sccache_files "$(file_count "${cache_root}/firefox/sccache")" files
record_metric image_fingerprint_files "$(file_count "${cache_root}/image-fingerprints")" files
record_metric cmake_total "$(path_bytes "${cache_root}/cmake")" bytes
record_metric ninja_log_lines "$(ninja_log_lines "${cache_root}/cmake")" lines
record_metric pch_files "$(pch_file_count "${cache_root}/cmake")" files
record_metric pch_total "$(pch_total_bytes "${cache_root}/cmake")" bytes
record_metric runtime_image_total "${runtime_image_total}" bytes
record_metric runtime_layer_records \
    "$(wc -l <"${runtime_layers_file}" | tr -d '[:space:]')" layers

firefox_runtime_fingerprint="${cache_root}/firefox/obj-minimal-opt/dist/.mmltk-runtime-stage.sha256"
if [[ -f "${firefox_runtime_fingerprint}" ]]; then
    cp -- "${firefox_runtime_fingerprint}" \
        "${report_dir}/firefox-runtime-stage.sha256"
else
    printf 'missing\n' >"${report_dir}/firefox-runtime-stage.sha256"
fi

find "${cache_root}/cmake" -type f \( -name '*.gch' -o -name '*.pch' \) \
    -printf '%p\t%s\n' >"${report_dir}/pch-artifacts.txt" 2>/dev/null || true
rg -n -- '(^|[[:space:]])Compiling |rustc|cargo|trunk|wasm-bindgen|wasm-opt' \
    "${build_log_file}" >"${report_dir}/cargo-rust-work.txt" 2>/dev/null || true
rg -n -- 'CACHED|cache-from|cache-to|exporting cache|importing cache|buildkit' \
    "${build_log_file}" >"${report_dir}/buildkit-evidence.txt" 2>/dev/null || true

if [[ ! -s "${report_dir}/ccache-stats.txt" ]]; then
    printf '%s\n' \
        'ccache stats unavailable: core compilation failed before capture.' \
        >"${report_dir}/ccache-stats.txt"
fi

if [[ ! -s "${report_dir}/sccache-stats.txt" ]]; then
    printf '%s\n' \
        'sccache stats unavailable: Firefox compilation was skipped or failed before capture.' \
        >"${report_dir}/sccache-stats.txt"
fi

rg -n --glob build.ninja -- '-fuse-ld=mold' "${cache_root}/cmake" \
    >"${report_dir}/mold-link-commands.txt" 2>/dev/null || true
rg -n --glob build.ninja -- 'cmake_pch|fpch-preprocess' "${cache_root}/cmake" \
    >"${report_dir}/pch-commands.txt" 2>/dev/null || true
rg -n -- 'mold|-fuse-ld' \
    "${cache_root}/firefox/obj-minimal-opt/config.status" \
    "${cache_root}/firefox/obj-minimal-opt/config/autoconf.mk" \
    >"${report_dir}/firefox-mold-config.txt" 2>/dev/null || true

TRACE_BUILD_LOG="${build_log_file}" \
TRACE_BUILD_STATUS="${build_status}" \
TRACE_CCACHE_STATS="${report_dir}/ccache-stats.txt" \
TRACE_EVIDENCE_FILE="${evidence_file}" \
TRACE_FIREFOX_STAGE_TRACE="${firefox_stage_trace_file}" \
TRACE_PHASE_TRACE="${phase_trace_file}" \
TRACE_SCCACHE_STATS="${report_dir}/sccache-stats.txt" \
TRACE_SUMMARY_FILE="${summary_file}" \
python3 - <<'PY'
import json
import os
import re
from pathlib import Path


def load_jsonl(path):
    records = []
    for line in Path(path).read_text(errors="replace").splitlines():
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return records


def event_count(records, phase, event):
    return sum(
        record.get("phase") == phase and record.get("event") == event
        for record in records
    )


def cache_status(records, phase):
    if event_count(records, phase, "cache_hit"):
        return "hit"
    if event_count(records, phase, "cache_miss"):
        return "miss"
    return "unknown"


def phase_span(records, phase):
    starts = [
        int(record["timestamp_ns"])
        for record in records
        if record.get("phase") == phase and record.get("event") == "start"
    ]
    finishes = [
        int(record["timestamp_ns"])
        for record in records
        if record.get("phase") == phase and record.get("event") == "finish"
    ]
    if not starts or not finishes:
        return None
    return min(starts), max(finishes)


def regex_count(pattern, text):
    return len(re.findall(pattern, text, flags=re.MULTILINE))


def parsed_stat(pattern, text):
    matches = re.findall(pattern, text, flags=re.MULTILINE | re.IGNORECASE)
    return int(matches[-1].replace(",", "")) if matches else 0


phase_records = load_jsonl(os.environ["TRACE_PHASE_TRACE"])
stage_records = load_jsonl(os.environ["TRACE_FIREFOX_STAGE_TRACE"])
build_status = int(os.environ["TRACE_BUILD_STATUS"])
build_log = Path(os.environ["TRACE_BUILD_LOG"]).read_text(errors="replace")
ccache_stats = Path(os.environ["TRACE_CCACHE_STATS"]).read_text(errors="replace")
sccache_stats = Path(os.environ["TRACE_SCCACHE_STATS"]).read_text(errors="replace")

configure_results = [
    record.get("result")
    for record in phase_records
    if record.get("phase") == "configure:release"
    and record.get("event") == "finish"
]
configure_state = configure_results[-1] if configure_results else "unknown"
jobs_records = [
    record
    for record in phase_records
    if record.get("phase") == "jobs" and record.get("event") == "assigned"
]
jobs = jobs_records[-1] if jobs_records else {}

ninja_span = phase_span(phase_records, "ninja_build")
firefox_span = phase_span(phase_records, "firefox_build")
overlap_ns = 0
if ninja_span and firefox_span:
    overlap_ns = max(
        0,
        min(ninja_span[1], firefox_span[1])
        - max(ninja_span[0], firefox_span[0]),
    )

metrics = {
    "configure_executed": int(configure_state == "executed"),
    "configure_skipped": int(configure_state == "skipped"),
    "ninja_firefox_overlap": int(overlap_ns > 0),
    "ninja_firefox_overlap_ns": overlap_ns,
    "ninja_jobs": int(jobs.get("ninja", 0)),
    "cargo_jobs": int(jobs.get("cargo", 0)),
    "firefox_jobs": int(jobs.get("firefox", 0)),
    "cargo_compile_units": regex_count(r"^\s*Compiling\s+", build_log),
    "cargo_finished_lines": regex_count(r"^\s*Finished\s+", build_log),
    "rustc_invocations": regex_count(r"(?:^|[/\s])rustc(?:[\s\"']|$)", build_log),
    "trunk_activity_lines": regex_count(
        r"^.*(?:trunk|wasm-bindgen|wasm-opt).*$", build_log
    ),
    "native_compile_edges": regex_count(
        r"^\[[0-9]+/[0-9]+\].*Building (?:C|CXX) object", build_log
    ),
    "cuda_compile_edges": regex_count(
        r"^\[[0-9]+/[0-9]+\].*Building CUDA object", build_log
    ),
    "image_fingerprint_cache_hits": sum(
        record.get("event") == "cache_hit"
        and (
            str(record.get("phase", "")).startswith("image:")
            or record.get("phase") == "runtime_image"
        )
        for record in phase_records
    ),
    "image_fingerprint_cache_misses": sum(
        record.get("event") == "cache_miss"
        and (
            str(record.get("phase", "")).startswith("image:")
            or record.get("phase") == "runtime_image"
        )
        for record in phase_records
    ),
    "buildkit_cached_steps": regex_count(r"^#[0-9]+ CACHED$", build_log),
    "buildkit_cache_imports": regex_count(
        r"^#[0-9]+ .*importing cache", build_log
    ),
    "buildkit_cache_exports": regex_count(
        r"^#[0-9]+ .*exporting cache", build_log
    ),
    "buildkit_step_log_lines": regex_count(r"^#[0-9]+ ", build_log),
    "cmake_regeneration_lines": regex_count(
        r"Re-running CMake|re-running CMake", build_log
    ),
    "firefox_stage_cache_hits": event_count(
        stage_records, "firefox_runtime_stage", "cache_hit"
    ),
    "firefox_stage_cache_misses": event_count(
        stage_records, "firefox_runtime_stage", "cache_miss"
    ),
    "package_cache_hits": event_count(phase_records, "package", "cache_hit"),
    "package_cache_misses": event_count(phase_records, "package", "cache_miss"),
    "ccache_hits": parsed_stat(r"^\s*Hits:\s*([0-9,]+)", ccache_stats),
    "sccache_hits": parsed_stat(r"^\s*Cache hits\s+([0-9,]+)", sccache_stats),
}

units = {
    "ninja_firefox_overlap_ns": "nanoseconds",
    "ninja_jobs": "workers",
    "cargo_jobs": "workers",
    "firefox_jobs": "workers",
}
with Path(os.environ["TRACE_EVIDENCE_FILE"]).open("a") as output:
    for name, value in metrics.items():
        output.write(
            json.dumps(
                {"metric": name, "value": value, "unit": units.get(name, "count")},
                separators=(",", ":"),
            )
            + "\n"
        )

summary = {
    "buildkit": {
        "cache_exports": metrics["buildkit_cache_exports"],
        "cache_imports": metrics["buildkit_cache_imports"],
        "cached_steps": metrics["buildkit_cached_steps"],
        "step_log_lines": metrics["buildkit_step_log_lines"],
    },
    "cargo_rust": {
        "compile_units": metrics["cargo_compile_units"],
        "rustc_invocations": metrics["rustc_invocations"],
        "trunk_activity_lines": metrics["trunk_activity_lines"],
    },
    "compiler_cache_hits": {
        "ccache": metrics["ccache_hits"],
        "sccache": metrics["sccache_hits"],
    },
    "configure": configure_state,
    "firefox_runtime_stage_cache": cache_status(
        stage_records, "firefox_runtime_stage"
    ),
    "jobs": {
        "cargo": metrics["cargo_jobs"],
        "firefox": metrics["firefox_jobs"],
        "ninja": metrics["ninja_jobs"],
    },
    "ninja_firefox_overlap_ns": overlap_ns,
    "package_cache": cache_status(phase_records, "package"),
    "runtime_image_cache": cache_status(phase_records, "runtime_image"),
    "wrapper_image_fingerprint_cache": {
        "hits": metrics["image_fingerprint_cache_hits"],
        "misses": metrics["image_fingerprint_cache_misses"],
    },
}
summary["observed_noop"] = (
    build_status == 0
    and configure_state == "skipped"
    and metrics["cmake_regeneration_lines"] == 0
    and metrics["native_compile_edges"] == 0
    and metrics["cuda_compile_edges"] == 0
    and metrics["cargo_compile_units"] == 0
    and metrics["rustc_invocations"] == 0
    and metrics["trunk_activity_lines"] == 0
    and metrics["image_fingerprint_cache_misses"] == 0
    and summary["firefox_runtime_stage_cache"] == "hit"
    and summary["package_cache"] == "hit"
    and summary["runtime_image_cache"] == "hit"
)
Path(os.environ["TRACE_SUMMARY_FILE"]).write_text(
    json.dumps(summary, indent=2, sort_keys=True) + "\n"
)
PY

TRACE_BUILD_STATUS="${build_status}" \
TRACE_BUILD_IMAGE="${trace_build_image}" \
TRACE_BUILDX_BUILDER="${trace_buildx_builder}" \
TRACE_CACHE_KEY="${cache_key}" \
TRACE_CACHE_ROOT="${cache_root}" \
TRACE_EVIDENCE_FILE="${evidence_file}" \
TRACE_FIREFOX_STAGE_TRACE="${firefox_stage_trace_file}" \
TRACE_LABEL="${run_label}" \
TRACE_MODE="${run_mode}" \
TRACE_REPORT_DIR="${report_dir}" \
TRACE_RUNTIME_LAYERS_FILE="${runtime_layers_file}" \
TRACE_RUNTIME_IMAGE="${runtime_image}" \
TRACE_RUNTIME_BASE_IMAGE="${trace_runtime_base_image}" \
TRACE_RUN_ID="${run_id}" \
TRACE_SUMMARY_FILE="${summary_file}" \
python3 - <<'PY' >"${report_dir}/run.json"
import json
import os
from pathlib import Path

summary = json.loads(Path(os.environ["TRACE_SUMMARY_FILE"]).read_text())
print(json.dumps({
    "build_exit_status": int(os.environ["TRACE_BUILD_STATUS"]),
    "build_image": os.environ["TRACE_BUILD_IMAGE"],
    "buildx_builder": os.environ["TRACE_BUILDX_BUILDER"],
    "cache_key": os.environ["TRACE_CACHE_KEY"],
    "cache_root": os.environ["TRACE_CACHE_ROOT"],
    "evidence_file": os.environ["TRACE_EVIDENCE_FILE"],
    "firefox_stage_trace_file": os.environ["TRACE_FIREFOX_STAGE_TRACE"],
    "label": os.environ["TRACE_LABEL"],
    "mode": os.environ["TRACE_MODE"],
    "observed_mode": "noop" if summary["observed_noop"] else "work",
    "report_dir": os.environ["TRACE_REPORT_DIR"],
    "runtime_layers_file": os.environ["TRACE_RUNTIME_LAYERS_FILE"],
    "runtime_image": os.environ["TRACE_RUNTIME_IMAGE"],
    "runtime_base_image": os.environ["TRACE_RUNTIME_BASE_IMAGE"],
    "run_id": os.environ["TRACE_RUN_ID"],
    "summary_file": os.environ["TRACE_SUMMARY_FILE"],
}, indent=2, sort_keys=True))
PY

log "build exit status: ${build_status}"
log "phase trace: ${phase_trace_file}"
log "Firefox stage trace: ${firefox_stage_trace_file}"
log "cache evidence: ${evidence_file}"
log "trace summary: ${summary_file}"
log "runtime layers: ${runtime_layers_file}"
exit "${build_status}"
