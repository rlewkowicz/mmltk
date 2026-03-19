#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${REPO_ROOT}"

LOG_ROOT="${FASTLOADER_BUILD_LOG_ROOT:-${REPO_ROOT}/build/logs}"
BUILD_KIND="${1:-all}"
JOBS="${FASTLOADER_BUILD_JOBS:-$(nproc)}"
FRESH="${FASTLOADER_BUILD_FRESH:-0}"
RUN_TESTS="${FASTLOADER_BUILD_RUN_TESTS:-0}"

mkdir -p "${LOG_ROOT}" "${REPO_ROOT}/build"

usage() {
    cat <<'EOF' >&2
usage: ./build.sh [release|dev|all]

Environment:
  FASTLOADER_BUILD_JOBS       Parallel build jobs (default: nproc)
  FASTLOADER_BUILD_FRESH=1    Reconfigure with --fresh before building
  FASTLOADER_BUILD_RUN_TESTS=1
                              Run ctest --preset dev after building dev
EOF
    exit 1
}

case "${BUILD_KIND}" in
    release)
        PRESETS=(release)
        ;;
    dev)
        PRESETS=(dev)
        ;;
    all)
        PRESETS=(release dev)
        ;;
    *)
        usage
        ;;
esac

build_preset() {
    local preset="$1"
    local log_path="${LOG_ROOT}/${preset}.log"
    local -a configure_cmd=(cmake --preset "${preset}")

    if [[ "${FRESH}" != "0" ]]; then
        configure_cmd+=(--fresh)
    fi

    printf '[build:%s] configure\n' "${preset}" | tee "${log_path}"
    "${configure_cmd[@]}" >>"${log_path}" 2>&1

    printf '[build:%s] build jobs=%s\n' "${preset}" "${JOBS}" | tee -a "${log_path}"
    cmake --build --preset "${preset}" -j"${JOBS}" >>"${log_path}" 2>&1

    if [[ "${RUN_TESTS}" != "0" && "${preset}" == "dev" ]]; then
        printf '[build:%s] test\n' "${preset}" | tee -a "${log_path}"
        ctest --preset dev >>"${log_path}" 2>&1
    fi

    printf '[build:%s] complete\n' "${preset}" | tee -a "${log_path}"
}

for preset in "${PRESETS[@]}"; do
    build_preset "${preset}"
done

printf 'build.sh complete: %s\n' "${PRESETS[*]}"
printf 'logs: %s\n' "${LOG_ROOT}"
