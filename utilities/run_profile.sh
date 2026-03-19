#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_PATH="${1:-${REPO_ROOT}/profiles/$(date +%s).log}"
BUILD_DIR="${2:-${FASTLOADER_BUILD_DIR:-${REPO_ROOT}/build/dev}}"
FIXTURE_DIR="${3:-/tmp/fastloader_profile_fixture}"
FASTLOADER_RUNTIME_LIBRARY_PATH="${FASTLOADER_RUNTIME_LIBRARY_PATH:-/usr/local/cuda-13.2/lib64:/opt/pytorch-2.10.0-cu132/lib:/opt/onnxruntime/lib}"

if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${REPO_ROOT}/${BUILD_DIR#./}"
fi

NUM_IMAGES="${FASTLOADER_PROFILE_NUM_IMAGES:-500}"
WIDTH="${FASTLOADER_PROFILE_WIDTH:-384}"
HEIGHT="${FASTLOADER_PROFILE_HEIGHT:-384}"
BATCH_SIZE="${FASTLOADER_PROFILE_BATCH_SIZE:-32}"
EPOCHS="${FASTLOADER_PROFILE_EPOCHS:-3}"
REPETITIONS="${FASTLOADER_PROFILE_REPETITIONS:-10}"
WARMUP_RUNS="${FASTLOADER_PROFILE_WARMUP_RUNS:-1}"
ENABLE_RFDETR_TRAIN="${FASTLOADER_PROFILE_ENABLE_RFDETR_TRAIN:-0}"
RFDETR_TRAIN_WEIGHTS="${FASTLOADER_PROFILE_RFDETR_TRAIN_WEIGHTS:-${REPO_ROOT}/engines/output-seg-medium/rf-detr-seg-medium.pt}"
RFDETR_TRAIN_REPETITIONS="${FASTLOADER_PROFILE_RFDETR_TRAIN_REPETITIONS:-1}"
RFDETR_TRAIN_WARMUP_RUNS="${FASTLOADER_PROFILE_RFDETR_TRAIN_WARMUP_RUNS:-0}"
RFDETR_TRAIN_NUM_IMAGES="${FASTLOADER_PROFILE_RFDETR_TRAIN_NUM_IMAGES:-24}"
RFDETR_TRAIN_WIDTH="${FASTLOADER_PROFILE_RFDETR_TRAIN_WIDTH:-432}"
RFDETR_TRAIN_HEIGHT="${FASTLOADER_PROFILE_RFDETR_TRAIN_HEIGHT:-432}"
RFDETR_TRAIN_BATCH_SIZE="${FASTLOADER_PROFILE_RFDETR_TRAIN_BATCH_SIZE:-6}"
RFDETR_TRAIN_EPOCHS="${FASTLOADER_PROFILE_RFDETR_TRAIN_EPOCHS:-1}"
RFDETR_TRAIN_DEVICE_ID="${FASTLOADER_PROFILE_RFDETR_TRAIN_DEVICE_ID:-0}"
RFDETR_TRAIN_WORKERS="${FASTLOADER_PROFILE_RFDETR_TRAIN_WORKERS:-16}"
RFDETR_TRAIN_PREFETCH_FACTOR="${FASTLOADER_PROFILE_RFDETR_TRAIN_PREFETCH_FACTOR:-2}"
RFDETR_TRAIN_CPU_AFFINITY="${FASTLOADER_PROFILE_RFDETR_TRAIN_CPU_AFFINITY:-}"
ENABLE_RFDETR_VALIDATE="${FASTLOADER_PROFILE_ENABLE_RFDETR_VALIDATE:-0}"
RFDETR_VALIDATE_COMPILED="${FASTLOADER_PROFILE_RFDETR_VALIDATE_COMPILED:-${REPO_ROOT}/compiled-seg-medium-synth/val.bin}"
RFDETR_VALIDATE_CHECKPOINT="${FASTLOADER_PROFILE_RFDETR_VALIDATE_CHECKPOINT:-${REPO_ROOT}/engines/output-seg-medium/checkpoint_best_ema.pth}"
RFDETR_VALIDATE_REPETITIONS="${FASTLOADER_PROFILE_RFDETR_VALIDATE_REPETITIONS:-1}"
RFDETR_VALIDATE_WARMUP_RUNS="${FASTLOADER_PROFILE_RFDETR_VALIDATE_WARMUP_RUNS:-0}"
RFDETR_VALIDATE_LIMIT_IMAGES="${FASTLOADER_PROFILE_RFDETR_VALIDATE_LIMIT_IMAGES:-0}"
RFDETR_VALIDATE_DEVICE_ID="${FASTLOADER_PROFILE_RFDETR_VALIDATE_DEVICE_ID:-0}"
RFDETR_VALIDATE_WORKERS="${FASTLOADER_PROFILE_RFDETR_VALIDATE_WORKERS:-0}"
RFDETR_VALIDATE_BATCH_SIZE="${FASTLOADER_PROFILE_RFDETR_VALIDATE_BATCH_SIZE:-5}"
RFDETR_VALIDATE_CPU_AFFINITY="${FASTLOADER_PROFILE_RFDETR_VALIDATE_CPU_AFFINITY:-}"

mkdir -p "$(dirname "${LOG_PATH}")"
rm -f "${LOG_PATH}"
rm -rf "${FIXTURE_DIR}"

export LD_LIBRARY_PATH="${FASTLOADER_RUNTIME_LIBRARY_PATH}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export FASTLOADER_PROFILE_LOG="${LOG_PATH}"
export FASTLOADER_PROFILE_APPEND=0
export FASTLOADER_BUILD_DIR="${BUILD_DIR}"

echo "[profile] core repetitions=${REPETITIONS} warmup=${WARMUP_RUNS} train=${ENABLE_RFDETR_TRAIN} validate=${ENABLE_RFDETR_VALIDATE}"
echo "[profile] build_dir=${BUILD_DIR}"
if [[ "${ENABLE_RFDETR_TRAIN}" != "0" ]]; then
    echo "[profile] train repetitions=${RFDETR_TRAIN_REPETITIONS} warmup=${RFDETR_TRAIN_WARMUP_RUNS} images=${RFDETR_TRAIN_NUM_IMAGES}"
    echo "[profile] train workers=${RFDETR_TRAIN_WORKERS} batch_size=${RFDETR_TRAIN_BATCH_SIZE} prefetch=${RFDETR_TRAIN_PREFETCH_FACTOR} cpu_affinity=${RFDETR_TRAIN_CPU_AFFINITY:-auto}"
fi
if [[ "${ENABLE_RFDETR_VALIDATE}" != "0" ]]; then
    validate_images="${RFDETR_VALIDATE_LIMIT_IMAGES}"
    if [[ "${validate_images}" == "0" ]]; then
        validate_images="full"
    fi
    echo "[profile] validate_checkpoint repetitions=${RFDETR_VALIDATE_REPETITIONS} warmup=${RFDETR_VALIDATE_WARMUP_RUNS} limit_images=${validate_images}"
    echo "[profile] validate_checkpoint workers=${RFDETR_VALIDATE_WORKERS} batch_size=${RFDETR_VALIDATE_BATCH_SIZE} cpu_affinity=${RFDETR_VALIDATE_CPU_AFFINITY:-auto}"
fi

if [[ ! -x "${BUILD_DIR}/fastloader_profile_runner" ]]; then
    echo "fastloader_profile_runner is required for profiling" >&2
    exit 1
fi

"${BUILD_DIR}/fastloader_profile_runner" \
    --keep-artifacts \
    --test-dir "${FIXTURE_DIR}" \
    --width "${WIDTH}" \
    --height "${HEIGHT}" \
    --num-images "${NUM_IMAGES}" \
    --batch-size "${BATCH_SIZE}" \
    --num-epochs "${EPOCHS}" \
    --shuffle-prefetch "${FASTLOADER_PROFILE_SHUFFLE_PREFETCH:-3}" \
    --compile-workers "${FASTLOADER_PROFILE_COMPILE_WORKERS:--1}" \
    --repetitions "${REPETITIONS}" \
    --warmup-runs "${WARMUP_RUNS}"

if [[ "${ENABLE_RFDETR_TRAIN}" != "0" ]]; then
    export FASTLOADER_PROFILE_APPEND=1
    if [[ ! -x "${BUILD_DIR}/fastloader_rfdetr_train_profile_runner" ]]; then
        echo "fastloader_rfdetr_train_profile_runner is required for train profiling" >&2
        exit 1
    fi
    "${BUILD_DIR}/fastloader_rfdetr_train_profile_runner" \
        --test-dir "${FIXTURE_DIR}/rfdetr-train" \
        --weights-path "${RFDETR_TRAIN_WEIGHTS}" \
        --width "${RFDETR_TRAIN_WIDTH}" \
        --height "${RFDETR_TRAIN_HEIGHT}" \
        --num-images "${RFDETR_TRAIN_NUM_IMAGES}" \
        --batch-size "${RFDETR_TRAIN_BATCH_SIZE}" \
        --epochs "${RFDETR_TRAIN_EPOCHS}" \
        --device-id "${RFDETR_TRAIN_DEVICE_ID}" \
        --workers "${RFDETR_TRAIN_WORKERS}" \
        --prefetch-factor "${RFDETR_TRAIN_PREFETCH_FACTOR}" \
        --cpu-affinity "${RFDETR_TRAIN_CPU_AFFINITY}" \
        --compile-workers "${FASTLOADER_PROFILE_COMPILE_WORKERS:--1}" \
        --repetitions "${RFDETR_TRAIN_REPETITIONS}" \
        --warmup-runs "${RFDETR_TRAIN_WARMUP_RUNS}"
fi

if [[ "${ENABLE_RFDETR_VALIDATE}" != "0" ]]; then
    export FASTLOADER_PROFILE_APPEND=1
    if [[ ! -x "${BUILD_DIR}/fastloader_rfdetr_profile_runner" ]]; then
        echo "fastloader_rfdetr_profile_runner is required for validation profiling" >&2
        exit 1
    fi
    "${BUILD_DIR}/fastloader_rfdetr_profile_runner" \
        --compiled-path "${RFDETR_VALIDATE_COMPILED}" \
        --checkpoint-path "${RFDETR_VALIDATE_CHECKPOINT}" \
        --repetitions "${RFDETR_VALIDATE_REPETITIONS}" \
        --warmup-runs "${RFDETR_VALIDATE_WARMUP_RUNS}" \
        --device-id "${RFDETR_VALIDATE_DEVICE_ID}" \
        --workers "${RFDETR_VALIDATE_WORKERS}" \
        --batch-size "${RFDETR_VALIDATE_BATCH_SIZE}" \
        --cpu-affinity "${RFDETR_VALIDATE_CPU_AFFINITY}" \
        --limit-images "${RFDETR_VALIDATE_LIMIT_IMAGES}"
fi

echo "[profile] wrote ${LOG_PATH}"
