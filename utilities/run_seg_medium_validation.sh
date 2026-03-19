#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARGS=("$@")
FASTLOADER_RUNTIME_LIBRARY_PATH="${FASTLOADER_RUNTIME_LIBRARY_PATH:-/usr/local/cuda-13.2/lib64:/opt/pytorch-2.10.0-cu132/lib:/opt/onnxruntime/lib}"
BUILD_DIR="${FASTLOADER_BUILD_DIR:-}"

if [[ -z "${BUILD_DIR}" ]]; then
    BUILD_DIR="${REPO_ROOT}/build/release"
    for arg in "${ARGS[@]}"; do
        if [[ "${arg}" == "--profile" ]]; then
            BUILD_DIR="${REPO_ROOT}/build/dev"
            break
        fi
    done
elif [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${REPO_ROOT}/${BUILD_DIR#./}"
fi

CLI="${BUILD_DIR}/fastloader_cli"
if [[ ! -x "${CLI}" ]]; then
    echo "missing fastloader_cli at ${CLI}" >&2
    echo "build it with cmake --preset release/dev and cmake --build --preset release/dev" >&2
    exit 1
fi

export LD_LIBRARY_PATH="${FASTLOADER_RUNTIME_LIBRARY_PATH}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

exec "${CLI}" rfdetr validate \
    --compiled "${REPO_ROOT}/compiled-seg-medium-synth/val.bin" \
    --source "${REPO_ROOT}/seg-medium-synth" \
    --split val \
    --resolution 432 \
    --onnx "${REPO_ROOT}/engines/output-seg-medium/inference_model.sim.onnx" \
    --tensorrt "${REPO_ROOT}/engines/output-seg-medium/inference_model.sim.onnx" \
    --save-engine "${REPO_ROOT}/engines/output-seg-medium/inference_model.engine" \
    --report-json "${REPO_ROOT}/compiled-seg-medium-synth/seg-medium-validation-report.json" \
    "${ARGS[@]}"
