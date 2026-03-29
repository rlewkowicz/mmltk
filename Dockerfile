# syntax=docker/dockerfile:1.7

FROM nvidia/cuda:13.2.0-cudnn-devel-ubuntu24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG FASTLOADER_CUDA_VERSION=13.2
ARG FASTLOADER_CUDA_FLAVOR=cu132
ARG FASTLOADER_PYTORCH_VERSION=2.10.0
ARG FASTLOADER_ONNXRUNTIME_VERSION=1.23.2
ARG FASTLOADER_BUILD_PRESET=release
ARG FASTLOADER_CUDA_ARCHITECTURES=86
ARG FASTLOADER_TORCH_CUDA_ARCH_LIST="8.6
12.1
12.0
11.0
10.3	
10.0		
9.0	
8.9
8.7
8.6
"

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ENV FASTLOADER_INSTALL_PREFIX=/opt \
    FASTLOADER_THIRD_PARTY_SRC_ROOT=/opt/src \
    FASTLOADER_CUDA_VERSION=${FASTLOADER_CUDA_VERSION} \
    FASTLOADER_CUDA_TOOLKIT_ROOT=/usr/local/cuda-${FASTLOADER_CUDA_VERSION} \
    FASTLOADER_CUDA_FLAVOR=${FASTLOADER_CUDA_FLAVOR} \
    FASTLOADER_PYTORCH_VERSION=${FASTLOADER_PYTORCH_VERSION} \
    FASTLOADER_PYTORCH_TAG=v${FASTLOADER_PYTORCH_VERSION} \
    FASTLOADER_TORCH_ROOT=/opt/pytorch-${FASTLOADER_PYTORCH_VERSION}-${FASTLOADER_CUDA_FLAVOR} \
    FASTLOADER_PYTORCH_SOURCE_DIR=/opt/src/pytorch-v${FASTLOADER_PYTORCH_VERSION} \
    FASTLOADER_PYTORCH_BUILD_DIR=/opt/src/pytorch-build-${FASTLOADER_PYTORCH_VERSION}-${FASTLOADER_CUDA_FLAVOR} \
    FASTLOADER_PYTORCH_BUILD_VENV=/opt/fastloader-pytorch-build \
    FASTLOADER_ONNXRUNTIME_VERSION=${FASTLOADER_ONNXRUNTIME_VERSION} \
    FASTLOADER_CUDA_ARCHITECTURES=${FASTLOADER_CUDA_ARCHITECTURES} \
    FASTLOADER_TORCH_CUDA_ARCH_LIST=${FASTLOADER_TORCH_CUDA_ARCH_LIST} \
    CUDA_HOME=/usr/local/cuda-${FASTLOADER_CUDA_VERSION} \
    CUDA_BIN_PATH=/usr/local/cuda-${FASTLOADER_CUDA_VERSION} \
    CUDACXX=/usr/local/cuda-${FASTLOADER_CUDA_VERSION}/bin/nvcc \
    TORCH_CUDA_ARCH_LIST=${FASTLOADER_TORCH_CUDA_ARCH_LIST} \
    CC=/usr/bin/gcc-14 \
    CXX=/usr/bin/g++-14 \
    LD_LIBRARY_PATH=/usr/local/cuda-${FASTLOADER_CUDA_VERSION}/lib64:/opt/pytorch-${FASTLOADER_PYTORCH_VERSION}-${FASTLOADER_CUDA_FLAVOR}/lib:/opt/onnxruntime/lib \
    FASTLOADER_RUNTIME_LD_LIBRARY_PATH=/usr/local/cuda-${FASTLOADER_CUDA_VERSION}/lib64:/opt/pytorch-${FASTLOADER_PYTORCH_VERSION}-${FASTLOADER_CUDA_FLAVOR}/lib:/opt/onnxruntime/lib

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        git \
        python3 \
        python3-dev \
        python3-pip \
        python3-venv \
        software-properties-common \
    && add-apt-repository -y universe \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        gcc-14 \
        g++-14 \
        libnuma-dev \
        libomp-dev \
        libopenblas-dev \
        libopenmpi-dev \
        ninja-build \
        openmpi-bin \
        patchelf \
        pkg-config \
        tensorrt-dev \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    archive="onnxruntime-linux-x64-gpu-${FASTLOADER_ONNXRUNTIME_VERSION}.tgz"; \
    curl -L "https://github.com/microsoft/onnxruntime/releases/download/v${FASTLOADER_ONNXRUNTIME_VERSION}/${archive}" -o "/tmp/${archive}"; \
    tar -xzf "/tmp/${archive}" -C /opt; \
    ln -sfn "/opt/onnxruntime-linux-x64-gpu-${FASTLOADER_ONNXRUNTIME_VERSION}" /opt/onnxruntime; \
    rm -f "/tmp/${archive}"

WORKDIR /opt/src

RUN git clone \
        --branch "${FASTLOADER_PYTORCH_TAG}" \
        --depth 1 \
        --recurse-submodules \
        --shallow-submodules \
        https://github.com/pytorch/pytorch.git \
        "${FASTLOADER_PYTORCH_SOURCE_DIR}"

RUN python3 -m venv "${FASTLOADER_PYTORCH_BUILD_VENV}" \
    && "${FASTLOADER_PYTORCH_BUILD_VENV}/bin/pip" install --no-cache-dir --upgrade pip setuptools wheel \
    && "${FASTLOADER_PYTORCH_BUILD_VENV}/bin/pip" install --no-cache-dir "cmake>=3.31" ninja \
    && "${FASTLOADER_PYTORCH_BUILD_VENV}/bin/pip" install --no-cache-dir -r "${FASTLOADER_PYTORCH_SOURCE_DIR}/requirements.txt"

RUN set -eux; \
    cmake_bin="${FASTLOADER_PYTORCH_BUILD_VENV}/bin/cmake"; \
    mkdir -p "${FASTLOADER_PYTORCH_BUILD_DIR}" "${FASTLOADER_TORCH_ROOT}"; \
    "${cmake_bin}" -S "${FASTLOADER_PYTORCH_SOURCE_DIR}" -B "${FASTLOADER_PYTORCH_BUILD_DIR}" -G Ninja \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_LAZY_TS_BACKEND=OFF \
        -DBUILD_PYTHON=OFF \
        -DBUILD_TEST=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CUDA_COMPILER="${FASTLOADER_CUDA_TOOLKIT_ROOT}/bin/nvcc" \
        -DCMAKE_INSTALL_PREFIX="${FASTLOADER_TORCH_ROOT}" \
        -DCMAKE_MAKE_PROGRAM="${FASTLOADER_PYTORCH_BUILD_VENV}/bin/ninja" \
        -DCMAKE_PREFIX_PATH="${FASTLOADER_PYTORCH_BUILD_VENV}" \
        -DCUDAToolkit_ROOT="${FASTLOADER_CUDA_TOOLKIT_ROOT}" \
        -DCUDA_TOOLKIT_ROOT_DIR="${FASTLOADER_CUDA_TOOLKIT_ROOT}" \
        -DPYTHON_EXECUTABLE="${FASTLOADER_PYTORCH_BUILD_VENV}/bin/python" \
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
        -DUSE_NCCL=ON \
        -DUSE_NNPACK=OFF \
        -DUSE_PYTORCH_QNNPACK=OFF \
        -DUSE_QNNPACK=OFF \
        -DUSE_SYSTEM_NCCL=ON \
        -DUSE_XNNPACK=OFF; \
    "${cmake_bin}" --build "${FASTLOADER_PYTORCH_BUILD_DIR}" --target install -j"$(nproc)"; \
    rm -rf \
        "${FASTLOADER_PYTORCH_BUILD_DIR}" \
        "${FASTLOADER_PYTORCH_SOURCE_DIR}" \
        "${FASTLOADER_PYTORCH_BUILD_VENV}"

WORKDIR /workspace
COPY . /workspace

RUN cmake --preset "${FASTLOADER_BUILD_PRESET}" --fresh \
        -DFASTLOADER_CUDA_VERSION="${FASTLOADER_CUDA_VERSION}" \
        -DFASTLOADER_CUDA_TOOLKIT_ROOT="${FASTLOADER_CUDA_TOOLKIT_ROOT}" \
        -DFASTLOADER_CUDA_ARCHITECTURES="${FASTLOADER_CUDA_ARCHITECTURES}" \
        -DFASTLOADER_TORCH_ROOT="${FASTLOADER_TORCH_ROOT}" \
        -DFASTLOADER_TORCH_CUDA_ARCH_LIST="${FASTLOADER_TORCH_CUDA_ARCH_LIST}" \
        -DBUILD_FASTLOADER_GUI=OFF \
        -DBUILD_RFDETR_PYTHON_CHECKPOINT_LOADER=OFF \
        -DPython3_EXECUTABLE=/usr/bin/python3 \
    && cmake --build --preset "${FASTLOADER_BUILD_PRESET}" -j"$(nproc)"

CMD ["/bin/bash"]
