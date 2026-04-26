#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image_name="${MMLTK_IMAGE:-mmltk}"
trace_root="${repo_root}/.mmltk-data"
trace_dir="${trace_root}/time-trace"
base_image="${MMLTK_PYTORCH_DEVEL_IMAGE:-pytorch/pytorch:2.10.0-cuda13.0-cudnn9-devel}"

log() {
    printf 'build_time_trace: %s\n' "$*" >&2
}

require_docker() {
    command -v docker >/dev/null 2>&1 || {
        log "docker is not installed"
        exit 1
    }
    docker version --format '{{.Server.Version}}' >/dev/null 2>&1 || {
        log "docker daemon is unavailable"
        exit 1
    }
}

clear_cpp_cache_mounts() {
    log "clearing cached C++ build mounts"
    docker buildx build \
        --progress=plain \
        --file - \
        "${repo_root}" <<EOF
# syntax=docker/dockerfile:1.7
FROM ${base_image}
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN --mount=type=cache,id=mmltk-release-build,target=/cache,sharing=locked \\
    find /cache -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
RUN --mount=type=cache,id=mmltk-test-build,target=/cache,sharing=locked \\
    find /cache -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
EOF
}

count_json_files() {
    local path="$1"
    if [[ ! -d "${path}" ]]; then
        printf '0'
        return 0
    fi
    find "${path}" -type f -name '*.json' | wc -l | tr -d '[:space:]'
}

print_ccache_stats() {
    local stats_file="$1"
    if [[ ! -f "${stats_file}" ]]; then
        log "ccache stats file is missing at ${stats_file}"
        return 0
    fi

    log "ccache stats"
    sed 's/^/ccache: /' "${stats_file}" >&2
}

require_docker
mkdir -p "${trace_root}"
rm -rf "${trace_dir}"

clear_cpp_cache_mounts

log "building traced runtime image '${image_name}'"
docker buildx build \
    --progress=plain \
    --build-arg MMLTK_ENABLE_TIME_TRACE=ON \
    --build-arg MMLTK_RESET_CCACHE_STATS=ON \
    --load \
    -t "${image_name}" \
    "${repo_root}"

log "exporting trace artifacts into ${trace_root}"
docker buildx build \
    --progress=plain \
    --build-arg MMLTK_ENABLE_TIME_TRACE=ON \
    --target time-trace-export \
    --output "type=local,dest=${trace_root}" \
    "${repo_root}"

release_count="$(count_json_files "${trace_dir}/release")"
test_count="$(count_json_files "${trace_dir}/tests")"
total_count="$((release_count + test_count))"
print_ccache_stats "${trace_dir}/ccache-stats.txt"

if (( total_count == 0 )); then
    log "no trace JSON files were exported"
    exit 1
fi

log "exported ${total_count} trace JSON files (${release_count} release, ${test_count} tests)"
