#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_PATH="${1:-$PWD/profiles/$(date +%s).log}"
RUNS="${2:-${FASTLOADER_BASELINE_RUNS:-10}}"
WARMUP_RUNS="${FASTLOADER_BASELINE_WARMUP_RUNS:-1}"
BUILD_DIR="${FASTLOADER_BUILD_DIR:-${REPO_ROOT}/build/dev}"

if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${REPO_ROOT}/${BUILD_DIR#./}"
fi

if ! [[ "${RUNS}" =~ ^[0-9]+$ ]] || (( RUNS <= 0 )); then
    echo "usage: $0 [log_path] [runs]" >&2
    exit 1
fi

if [[ ! -x "${BUILD_DIR}/fastloader_profile_runner" ]]; then
    cat >&2 <<EOF
missing dev profile runner at ${BUILD_DIR}/fastloader_profile_runner
build it with:
  cmake --preset dev --fresh
  cmake --build --preset dev -j"$(nproc)"
EOF
    exit 1
fi

export FASTLOADER_PROFILE_REPETITIONS="${FASTLOADER_PROFILE_REPETITIONS:-${RUNS}}"
export FASTLOADER_PROFILE_WARMUP_RUNS="${FASTLOADER_PROFILE_WARMUP_RUNS:-${WARMUP_RUNS}}"

if [[ -z "${FASTLOADER_PROFILE_RFDETR_VALIDATE_REPETITIONS:-}" ]]; then
    export FASTLOADER_PROFILE_RFDETR_VALIDATE_REPETITIONS="${RUNS}"
fi

if [[ -z "${FASTLOADER_PROFILE_RFDETR_TRAIN_REPETITIONS:-}" ]]; then
    export FASTLOADER_PROFILE_RFDETR_TRAIN_REPETITIONS="${RUNS}"
fi

exec "${REPO_ROOT}/utilities/run_profile.sh" "${LOG_PATH}" "${BUILD_DIR}"
