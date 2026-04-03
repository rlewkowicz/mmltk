# syntax=docker/dockerfile:1.7

ARG MMLTK_PYTORCH_DEVEL_IMAGE=pytorch/pytorch:2.10.0-cuda13.0-cudnn9-devel
ARG MMLTK_PYTORCH_RUNTIME_IMAGE=pytorch/pytorch:2.10.0-cuda13.0-cudnn9-runtime

FROM ${MMLTK_PYTORCH_DEVEL_IMAGE} AS toolchain

ARG DEBIAN_FRONTEND=noninteractive
ARG MMLTK_CUDA_VERSION=13.0
ARG MMLTK_CUDA_ARCHITECTURES="86;87;89;90;100;103;110;120;121"
ARG MMLTK_BUILD_TESTS=ON
ARG MMLTK_ONNXRUNTIME_VERSION=1.23.2
ARG MMLTK_ONNX_COMMIT=e709452ef2bbc1d113faf678c24e6d3467696e83
ARG MMLTK_TORCH_CUDA_ARCH_LIST="8.6 8.7 8.9 9.0 10.0 10.3 11.0 12.0 12.1"
ARG MMLTK_CONTAINER_WORKTREE=/src

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ENV CUDA_HOME=/usr/local/cuda-${MMLTK_CUDA_VERSION} \
    CUDA_BIN_PATH=/usr/local/cuda-${MMLTK_CUDA_VERSION} \
    CUDACXX=/usr/local/cuda-${MMLTK_CUDA_VERSION}/bin/nvcc \
    CC=/usr/bin/gcc-14 \
    CXX=/usr/bin/g++-14 \
    TORCH_CUDA_ARCH_LIST=${MMLTK_TORCH_CUDA_ARCH_LIST}

WORKDIR ${MMLTK_CONTAINER_WORKTREE}

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        git \
        software-properties-common \
    && add-apt-repository -y universe \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        clang-tidy \
        cppcheck \
        gcc-14 \
        g++-14 \
        gdb \
        libegl1-mesa-dev \
        libgl1-mesa-dev \
        libglfw3-dev \
        libnuma-dev \
        libomp-dev \
        libopenblas-dev \
        libopenmpi-dev \
        libprotobuf-dev \
        libwayland-dev \
        libx11-dev \
        libxcursor-dev \
        libxi-dev \
        libxinerama-dev \
        libxkbcommon-dev \
        libxrandr-dev \
        ninja-build \
        openmpi-bin \
        pkg-config \
        protobuf-compiler \
        tensorrt-dev \
        unzip \
        wayland-protocols \
        zip \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p ./workspace/deps/src ./workspace/deps/onnx ./workspace/build/onnx-build ./workspace/deps/onnxruntime ./workspace/deps/imgui ./workspace/build/release ./workspace/runtime-libs \
    && python3 - <<'PY'
from pathlib import Path
import torch

torch_root = Path(torch.utils.cmake_prefix_path).resolve().parents[1]
link_path = Path("/opt/pytorch")
if link_path.exists() or link_path.is_symlink():
    link_path.unlink()
link_path.symlink_to(torch_root)
print(f"linked /opt/pytorch -> {torch_root}")
PY

RUN curl -L "https://github.com/microsoft/onnxruntime/releases/download/v${MMLTK_ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-gpu-${MMLTK_ONNXRUNTIME_VERSION}.tgz" \
        -o /tmp/onnxruntime.tgz \
    && tar -xzf /tmp/onnxruntime.tgz -C ./workspace/deps \
    && rm -rf ./workspace/deps/onnxruntime \
    && mv "./workspace/deps/onnxruntime-linux-x64-gpu-${MMLTK_ONNXRUNTIME_VERSION}" ./workspace/deps/onnxruntime \
    && rm -f /tmp/onnxruntime.tgz

RUN git clone --depth 1 --branch v1.91.8 https://github.com/ocornut/imgui.git ./workspace/deps/imgui

RUN git clone https://github.com/onnx/onnx.git ./workspace/deps/src/onnx \
    && git -C ./workspace/deps/src/onnx fetch --depth 1 origin "${MMLTK_ONNX_COMMIT}" \
    && git -C ./workspace/deps/src/onnx checkout --force FETCH_HEAD

RUN --mount=type=cache,id=mmltk-onnx-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/onnx-build,sharing=locked \
    expected_source="${MMLTK_CONTAINER_WORKTREE}/workspace/deps/src/onnx" \
    && expected_build="${MMLTK_CONTAINER_WORKTREE}/workspace/build/onnx-build" \
    && cache_file="${expected_build}/CMakeCache.txt" \
    && if [[ -f "${cache_file}" ]]; then \
        cached_source="$(sed -n 's#^CMAKE_HOME_DIRECTORY:INTERNAL=##p' "${cache_file}")"; \
        cached_build="$(sed -n 's#^CMAKE_CACHEFILE_DIR:INTERNAL=##p' "${cache_file}")"; \
        if [[ "${cached_source}" != "${expected_source}" || "${cached_build}" != "${expected_build}" ]]; then \
            find "${expected_build}" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +; \
        fi; \
    fi \
    && cmake -S ./workspace/deps/src/onnx -B ./workspace/build/onnx-build -G Ninja \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${MMLTK_CONTAINER_WORKTREE}/workspace/deps/onnx \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DONNX_BUILD_CUSTOM_PROTOBUF=OFF \
        -DONNX_BUILD_PYTHON=OFF \
        -DONNX_BUILD_TESTS=OFF \
        -DONNX_CUSTOM_PROTOC_EXECUTABLE=/usr/bin/protoc \
        -DONNX_ML=ON \
        -DONNX_NAMESPACE=onnx_torch \
        -DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc \
        -DONNX_USE_LITE_PROTO=OFF \
        -DONNX_USE_PROTOBUF_SHARED_LIBS=ON \
        -DONNX_WERROR=OFF

RUN --mount=type=cache,id=mmltk-onnx-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/onnx-build,sharing=locked \
    cmake --build ./workspace/build/onnx-build -j"$(nproc)" \
    && cmake --install ./workspace/build/onnx-build

FROM toolchain AS analysis

# Static analysis binds the host repo into /host/... and configures a fresh
# compile database at runtime, so it only needs the compiler toolchain and
# vendored dependency tree from the toolchain stage.

FROM toolchain AS build

COPY . ${MMLTK_CONTAINER_WORKTREE}

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    expected_source="${MMLTK_CONTAINER_WORKTREE}" \
    && expected_build="${MMLTK_CONTAINER_WORKTREE}/workspace/build/release" \
    && cache_file="${expected_build}/CMakeCache.txt" \
    && if [[ -f "${cache_file}" ]]; then \
        cached_source="$(sed -n 's#^CMAKE_HOME_DIRECTORY:INTERNAL=##p' "${cache_file}")"; \
        cached_build="$(sed -n 's#^CMAKE_CACHEFILE_DIR:INTERNAL=##p' "${cache_file}")"; \
        if [[ "${cached_source}" != "${expected_source}" || "${cached_build}" != "${expected_build}" ]]; then \
            find "${expected_build}" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +; \
        fi; \
    fi \
    && cmake -S ${MMLTK_CONTAINER_WORKTREE} -B ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DBUILD_TESTS=${MMLTK_BUILD_TESTS} \
        -DBUILD_RFDETR_NATIVE=ON \
        -DBUILD_RFDETR_PYTHON_CHECKPOINT_LOADER=ON \
        -DBUILD_MMLTK_GUI=ON \
        -DMMLTK_CUDA_VERSION=${MMLTK_CUDA_VERSION} \
        -DMMLTK_CUDA_ARCHITECTURES="${MMLTK_CUDA_ARCHITECTURES}" \
        -DMMLTK_CUDA_TOOLKIT_ROOT=/usr/local/cuda-${MMLTK_CUDA_VERSION} \
        -DMMLTK_TORCH_ROOT=/opt/pytorch \
        -DMMLTK_ONNX_ROOT=${MMLTK_CONTAINER_WORKTREE}/workspace/deps/onnx \
        -DMMLTK_ONNXRUNTIME_ROOT=${MMLTK_CONTAINER_WORKTREE}/workspace/deps/onnxruntime \
        -DMMLTK_IMGUI_SOURCE_DIR=${MMLTK_CONTAINER_WORKTREE}/workspace/deps/imgui \
        -DMMLTK_TORCH_CUDA_ARCH_LIST="${MMLTK_TORCH_CUDA_ARCH_LIST}" \
        -DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc \
        -DPython3_EXECUTABLE=/usr/bin/python3 \
        -DCMAKE_CXX_COMPILER=/usr/bin/g++-14

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    cmake --build ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release --target \
        mmltk_core \
        mmltk_rfdetr_lsap \
        mmltk_rfdetr_cuda_ops \
        mmltk_live_capture \
        mmltk_live_runtime \
        -j"$(nproc)"

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    cmake --build ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release --target mmltk_rfdetr_native -j"$(nproc)"

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    if [[ "${MMLTK_BUILD_TESTS}" == "ON" ]]; then \
        cmake --build ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release --target mmltk_tests_core mmltk_tests_gui mmltk_tests_model_rfdetr -j"$(nproc)"; \
    fi

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    cmake --build ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release --target mmltk_cli mmltk_gui mmltk_rfdetr_onnx_simplify_tool -j"$(nproc)"

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    cmake --install ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release --prefix /opt/mmltk \
    && mkdir -p ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/tensorrt \
    && mkdir -p ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/nccl \
    && cp -a /usr/lib/x86_64-linux-gnu/libnvinfer*.so* ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/tensorrt/ \
    && cp -a /usr/lib/x86_64-linux-gnu/libnvonnxparser*.so* ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/tensorrt/ \
    && cp -a /opt/pytorch/lib/../../nvidia/nccl/lib/libnccl*.so* ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/nccl/ \
    && { strip --strip-unneeded /opt/mmltk/bin/mmltk /opt/mmltk/bin/mmltk-gui /opt/mmltk/bin/mmltk-rfdetr-onnx-simplify /opt/mmltk/lib/libmmltk_core.so /opt/mmltk/lib/libmmltk_rfdetr_native.so || true; }

FROM ${MMLTK_PYTORCH_RUNTIME_IMAGE} AS runtime

ARG DEBIAN_FRONTEND=noninteractive
ARG MMLTK_CUDA_VERSION=13.0
ARG MMLTK_CONTAINER_WORKTREE=/src

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gdb \
        libegl1 \
        libgl1 \
        libglfw3 \
        libgfortran5 \
        libopenblas0-pthread \
        libomp5-18 \
        libopengl0 \
        libprotobuf32t64 \
        libwayland-client0 \
        libwayland-cursor0 \
        libwayland-egl1 \
        libx11-6 \
        libxcursor1 \
        libxi6 \
        libxinerama1 \
        libxkbcommon0 \
        libxrandr2 \
    && rm -rf /var/lib/apt/lists/*

RUN python3 - <<'PY'
from pathlib import Path
import torch

torch_root = Path(torch.utils.cmake_prefix_path).resolve().parents[1]
link_path = Path("/opt/pytorch")
if link_path.exists() or link_path.is_symlink():
    link_path.unlink()
link_path.symlink_to(torch_root)
print(f"linked /opt/pytorch -> {torch_root}")
PY

COPY --from=build /opt/mmltk /opt/mmltk
COPY --from=build ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/tensorrt/ /opt/mmltk/lib/
COPY --from=build ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/nccl/ /opt/mmltk/lib/

RUN cat <<'EOF' >/usr/local/bin/mmltk-entrypoint
#!/usr/bin/env bash
set -euo pipefail

case "${1:-mmltk}" in
    mmltk|mmltk-gui)
        exec "$@"
        ;;
    -*)
        exec mmltk "$@"
        ;;
    *)
        exec mmltk "$@"
        ;;
esac
EOF
RUN chmod 0755 /usr/local/bin/mmltk-entrypoint

ENV PATH=/opt/mmltk/bin:${PATH} \
    LD_LIBRARY_PATH=/opt/mmltk/lib:/opt/pytorch/lib:/opt/pytorch/lib/../../nvidia/cu13/lib:/opt/pytorch/lib/../../nvidia/cudnn/lib:/opt/pytorch/lib/../../nvidia/cusparselt/lib:/opt/pytorch/lib/../../nvidia/nccl/lib:/opt/pytorch/lib/../../nvidia/nvshmem/lib:/usr/local/cuda-${MMLTK_CUDA_VERSION}/lib64

WORKDIR /opt/mmltk
ENTRYPOINT ["mmltk-entrypoint"]
CMD ["mmltk"]
