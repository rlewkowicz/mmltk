#!/usr/bin/env bash
set -euo pipefail

CUDA_VERSION="${FASTLOADER_CUDA_VERSION:-13.2}"
CUDA_ROOT="${FASTLOADER_CUDA_TOOLKIT_ROOT:-/usr/local/cuda-${CUDA_VERSION}}"
CUDA_FLAVOR="${FASTLOADER_CUDA_FLAVOR:-cu132}"
PYTORCH_VERSION="${FASTLOADER_PYTORCH_VERSION:-2.10.0}"
PYTORCH_TAG="${FASTLOADER_PYTORCH_TAG:-v${PYTORCH_VERSION}}"
ONNXRUNTIME_VERSION="${FASTLOADER_ONNXRUNTIME_VERSION:-1.23.2}"
INSTALL_PREFIX="${FASTLOADER_INSTALL_PREFIX:-/opt}"
SRC_ROOT="${FASTLOADER_THIRD_PARTY_SRC_ROOT:-${INSTALL_PREFIX}/src}"
LIBTORCH_PREFIX="${FASTLOADER_TORCH_ROOT:-${INSTALL_PREFIX}/pytorch-${PYTORCH_VERSION}-${CUDA_FLAVOR}}"
PARSER_VENV="${FASTLOADER_PTH_PARSER_VENV:-${INSTALL_PREFIX}/fastloader-pth-parse}"
BUILD_VENV="${FASTLOADER_PYTORCH_BUILD_VENV:-${INSTALL_PREFIX}/fastloader-pytorch-build}"
PYTORCH_SOURCE_DIR="${FASTLOADER_PYTORCH_SOURCE_DIR:-${SRC_ROOT}/pytorch-${PYTORCH_TAG}}"
PYTORCH_BUILD_DIR="${FASTLOADER_PYTORCH_BUILD_DIR:-${SRC_ROOT}/pytorch-build-${PYTORCH_VERSION}-${CUDA_FLAVOR}}"
ONNXRUNTIME_ARCHIVE="onnxruntime-linux-x64-gpu-${ONNXRUNTIME_VERSION}.tgz"
ONNXRUNTIME_URL="https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${ONNXRUNTIME_ARCHIVE}"
ONNXRUNTIME_INSTALL_DIR="${INSTALL_PREFIX}/onnxruntime-linux-x64-gpu-${ONNXRUNTIME_VERSION}"
ONNXRUNTIME_SYMLINK="${INSTALL_PREFIX}/onnxruntime"
BUILD_JOBS="${FASTLOADER_BUILD_JOBS:-$(nproc)}"
PARSER_TORCH_INDEX_URL="${FASTLOADER_PTH_PARSER_TORCH_INDEX_URL:-https://download.pytorch.org/whl/cpu}"

if [[ -z "${BUILD_JOBS}" || "${BUILD_JOBS}" == "0" ]]; then
    BUILD_JOBS="$(nproc)"
fi

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 1
    fi
}

require_cmd curl
require_cmd git
require_cmd cmake
require_cmd python3

if [[ ! -x "${CUDA_ROOT}/bin/nvcc" ]]; then
    echo "CUDA ${CUDA_VERSION}.x toolkit not found at ${CUDA_ROOT}" >&2
    exit 1
fi

if command -v gcc-14 >/dev/null 2>&1 && command -v g++-14 >/dev/null 2>&1; then
    export CC="${CC:-$(command -v gcc-14)}"
    export CXX="${CXX:-$(command -v g++-14)}"
fi

mkdir -p "${INSTALL_PREFIX}" "${SRC_ROOT}"

install_onnxruntime() {
    if [[ -f "${ONNXRUNTIME_INSTALL_DIR}/lib/libonnxruntime.so" ]]; then
        ln -sfn "${ONNXRUNTIME_INSTALL_DIR}" "${ONNXRUNTIME_SYMLINK}"
        echo "[deps] onnxruntime already present at ${ONNXRUNTIME_INSTALL_DIR}"
        return
    fi

    local archive
    archive="$(mktemp)"

    echo "[deps] downloading ${ONNXRUNTIME_URL}"
    curl -L "${ONNXRUNTIME_URL}" -o "${archive}"

    echo "[deps] installing ${ONNXRUNTIME_INSTALL_DIR}"
    rm -rf "${ONNXRUNTIME_INSTALL_DIR}"
    tar -xzf "${archive}" -C "${INSTALL_PREFIX}"
    ln -sfn "${ONNXRUNTIME_INSTALL_DIR}" "${ONNXRUNTIME_SYMLINK}"
    rm -f "${archive}"
}

install_parser_venv() {
    if [[ ! -x "${PARSER_VENV}/bin/python" ]]; then
        echo "[deps] creating parser venv ${PARSER_VENV}"
        python3 -m venv "${PARSER_VENV}"
    fi

    echo "[deps] installing CPU-only parser packages into ${PARSER_VENV}"
    "${PARSER_VENV}/bin/pip" install --upgrade pip setuptools wheel
    "${PARSER_VENV}/bin/pip" install "numpy==1.26.4"
    "${PARSER_VENV}/bin/pip" install \
        --index-url "${PARSER_TORCH_INDEX_URL}" \
        "torch==${PYTORCH_VERSION}"
    "${PARSER_VENV}/bin/pip" install "vastai-sdk"
}

ensure_pytorch_source() {
    if [[ ! -d "${PYTORCH_SOURCE_DIR}/.git" ]]; then
        echo "[deps] cloning PyTorch ${PYTORCH_TAG} into ${PYTORCH_SOURCE_DIR}"
        git clone --branch "${PYTORCH_TAG}" --depth 1 --recurse-submodules --shallow-submodules \
            https://github.com/pytorch/pytorch.git "${PYTORCH_SOURCE_DIR}"
        return
    fi

    echo "[deps] refreshing PyTorch source at ${PYTORCH_SOURCE_DIR}"
    git -C "${PYTORCH_SOURCE_DIR}" fetch --depth 1 origin "${PYTORCH_TAG}"
    git -C "${PYTORCH_SOURCE_DIR}" checkout --force "${PYTORCH_TAG}"
    git -C "${PYTORCH_SOURCE_DIR}" submodule sync --recursive
    git -C "${PYTORCH_SOURCE_DIR}" submodule update --init --recursive --depth 1
}

install_build_venv() {
    if [[ ! -x "${BUILD_VENV}/bin/python" ]]; then
        echo "[deps] creating PyTorch build venv ${BUILD_VENV}"
        python3 -m venv "${BUILD_VENV}"
    fi

    echo "[deps] installing PyTorch build requirements into ${BUILD_VENV}"
    "${BUILD_VENV}/bin/pip" install --upgrade pip setuptools wheel
    "${BUILD_VENV}/bin/pip" install "cmake>=3.31" ninja
    "${BUILD_VENV}/bin/pip" install -r "${PYTORCH_SOURCE_DIR}/requirements.txt"
}

build_libtorch() {
    if [[ -f "${LIBTORCH_PREFIX}/share/cmake/Torch/TorchConfig.cmake" ]] && \
       [[ -f "${LIBTORCH_PREFIX}/lib/libtorch.so" ]]; then
        echo "[deps] libtorch already present at ${LIBTORCH_PREFIX}"
        return
    fi

    echo "[deps] configuring LibTorch ${PYTORCH_VERSION} for CUDA ${CUDA_VERSION}.x"
    mkdir -p "${PYTORCH_BUILD_DIR}" "${LIBTORCH_PREFIX}"

    local cmake_bin="${BUILD_VENV}/bin/cmake"
    local ninja_bin="${BUILD_VENV}/bin/ninja"
    if [[ ! -x "${cmake_bin}" || ! -x "${ninja_bin}" ]]; then
        echo "PyTorch build tools were not installed into ${BUILD_VENV}" >&2
        exit 1
    fi

    export CUDA_HOME="${CUDA_ROOT}"
    export CUDA_BIN_PATH="${CUDA_ROOT}"
    export CUDACXX="${CUDA_ROOT}/bin/nvcc"

    "${cmake_bin}" -S "${PYTORCH_SOURCE_DIR}" -B "${PYTORCH_BUILD_DIR}" -G Ninja \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_LAZY_TS_BACKEND=OFF \
        -DBUILD_PYTHON=OFF \
        -DBUILD_TEST=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_MAKE_PROGRAM="${ninja_bin}" \
        -DCMAKE_INSTALL_PREFIX="${LIBTORCH_PREFIX}" \
        -DPYTHON_EXECUTABLE="${BUILD_VENV}/bin/python" \
        -DCMAKE_PREFIX_PATH="${BUILD_VENV}" \
        -DCUDAToolkit_ROOT="${CUDA_ROOT}" \
        -DCUDA_TOOLKIT_ROOT_DIR="${CUDA_ROOT}" \
        -DCMAKE_CUDA_COMPILER="${CUDA_ROOT}/bin/nvcc" \
        -DUSE_CUDA=ON \
        -DUSE_CUDNN=ON \
        -DUSE_CUFILE=ON \
        -DUSE_CUSPARSELT=ON \
        -DUSE_DISTRIBUTED=ON \
        -DUSE_FBGEMM=OFF \
        -DUSE_FLASH_ATTENTION=ON \
        -DUSE_ITT=OFF \
        -DUSE_KINETO=OFF \
        -DUSE_MEM_EFF_ATTENTION=ON \
        -DUSE_MKLDNN=OFF \
        -DUSE_MPI=OFF \
        -DUSE_NNPACK=OFF \
        -DUSE_PYTORCH_QNNPACK=OFF \
        -DUSE_QNNPACK=OFF \
        -DUSE_SYSTEM_NCCL=ON \
        -DUSE_XNNPACK=OFF

    echo "[deps] building/installing LibTorch into ${LIBTORCH_PREFIX}"
    "${cmake_bin}" --build "${PYTORCH_BUILD_DIR}" --target install -j"${BUILD_JOBS}"
}

install_onnxruntime
install_parser_venv
ensure_pytorch_source
install_build_venv
build_libtorch

cat <<EOF
[deps] complete
[deps] cuda_root=${CUDA_ROOT}
[deps] onnxruntime=${ONNXRUNTIME_SYMLINK}
[deps] libtorch=${LIBTORCH_PREFIX}
[deps] parser_python=${PARSER_VENV}/bin/python
EOF
