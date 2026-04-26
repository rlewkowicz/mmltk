# syntax=docker/dockerfile:1

ARG MMLTK_PYTORCH_DEVEL_IMAGE=pytorch/pytorch:2.10.0-cuda13.0-cudnn9-devel
ARG MMLTK_PYTORCH_RUNTIME_IMAGE=pytorch/pytorch:2.10.0-cuda13.0-cudnn9-runtime

FROM ${MMLTK_PYTORCH_DEVEL_IMAGE} AS toolchain

ARG DEBIAN_FRONTEND=noninteractive
ARG MMLTK_CUDA_VERSION=13.0
ARG MMLTK_CUDA_ARCHITECTURES="86;87;89;90;100;103;110;120;121"
ARG MMLTK_BUILD_TESTS=ON
ARG MMLTK_CEF_DEV_PACKAGES="libasound2-dev libatk-bridge2.0-dev libatk1.0-dev libcups2-dev libdbus-1-dev libdrm-dev libegl-dev libgbm-dev libglib2.0-dev libgtk-4-dev libnspr4-dev libnss3-dev libwayland-dev libxkbcommon-dev"
ARG MMLTK_CEF_WEBGPU_RUNTIME=auto
ARG MMLTK_CEF_ENABLE_UNSAFE_WEBGPU=OFF
ARG MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU=OFF
ARG MMLTK_CLANG_VERSION=18
ARG MMLTK_ENABLE_TIME_TRACE=OFF
ARG MMLTK_MOLD_VERSION=2.41.0
ARG MMLTK_MOLD_ARCHIVE_URL=https://github.com/rui314/mold/releases/download/v${MMLTK_MOLD_VERSION}/mold-${MMLTK_MOLD_VERSION}-x86_64-linux.tar.gz
ARG MMLTK_NODE_VERSION=22.12.0
ARG MMLTK_ONNXRUNTIME_VERSION=1.23.2
ARG MMLTK_ONNX_COMMIT=e709452ef2bbc1d113faf678c24e6d3467696e83
ARG MMLTK_TORCH_CUDA_ARCH_LIST="8.6 8.7 8.9 9.0 10.0 10.3 11.0 12.0 12.1"
ARG MMLTK_CONTAINER_WORKTREE=/src
ARG MMLTK_CCACHE_DIR=/var/cache/ccache
ARG MMLTK_RESET_CCACHE_STATS=OFF
ARG MMLTK_TIME_TRACE_EXPORT_DIR=/opt/mmltk-time-trace

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ENV CUDA_HOME=/usr/local/cuda-${MMLTK_CUDA_VERSION} \
    CUDA_BIN_PATH=/usr/local/cuda-${MMLTK_CUDA_VERSION} \
    CUDACXX=/usr/local/cuda-${MMLTK_CUDA_VERSION}/bin/nvcc \
    CC=/usr/bin/gcc-14 \
    CXX=/usr/bin/g++-14 \
    PATH=/opt/mold/bin:/opt/node/bin:${PATH} \
    TORCH_CUDA_ARCH_LIST=${MMLTK_TORCH_CUDA_ARCH_LIST} \
    CCACHE_BASEDIR=/src \
    CCACHE_DIR=${MMLTK_CCACHE_DIR} \
    CCACHE_NOHASHDIR=1

WORKDIR ${MMLTK_CONTAINER_WORKTREE}

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        tensorrt-dev \
    && rm -rf /var/lib/apt/lists/*

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
        bzip2 \
        ccache \
        clang-${MMLTK_CLANG_VERSION} \
        clang-format \
        cmake \
        clang-tidy \
        cppcheck \
        gcc-14 \
        g++-14 \
        gdb \
        libnuma-dev \
        libomp-dev \
        libopenblas-dev \
        libopenmpi-dev \
        libprotobuf-dev \
        ninja-build \
        openmpi-bin \
        pkg-config \
        protobuf-compiler \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "${MMLTK_MOLD_ARCHIVE_URL}" \
        -o /tmp/mold.tar.gz \
    && rm -rf /opt/mold \
    && mkdir -p /opt \
    && tar -xzf /tmp/mold.tar.gz -C /opt \
    && mv "/opt/mold-${MMLTK_MOLD_VERSION}-x86_64-linux" /opt/mold \
    && rm -f /tmp/mold.tar.gz \
    && /opt/mold/bin/mold --version

RUN apt-get update \
    && if [[ -n "${MMLTK_CEF_DEV_PACKAGES}" ]]; then \
        apt-get install -y --no-install-recommends ${MMLTK_CEF_DEV_PACKAGES}; \
    fi \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "https://nodejs.org/dist/v${MMLTK_NODE_VERSION}/node-v${MMLTK_NODE_VERSION}-linux-x64.tar.gz" \
        -o /tmp/node.tar.gz \
    && rm -rf /opt/node \
    && mkdir -p /opt \
    && tar -xzf /tmp/node.tar.gz -C /opt \
    && mv "/opt/node-v${MMLTK_NODE_VERSION}-linux-x64" /opt/node \
    && rm -f /tmp/node.tar.gz \
    && node --version \
    && npm --version

RUN mkdir -p ./workspace/deps/src ./workspace/deps/onnx ./workspace/build/onnx-build ./workspace/deps/onnxruntime ./workspace/build/release ./workspace/runtime-libs \
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

RUN if [[ ! -f /opt/pytorch/lib/libkineto.a ]]; then ar rcs /opt/pytorch/lib/libkineto.a; fi

FROM toolchain AS browser_runtime_distribution

COPY cmake/mmltk_cef_distribution.cmake ${MMLTK_CONTAINER_WORKTREE}/cmake/
COPY utilities/emit_mmltk_cef_distribution_env.cmake ${MMLTK_CONTAINER_WORKTREE}/utilities/
COPY .docker-cache/cef/ /tmp/mmltk-local-cef/

RUN mkdir -p /opt/mmltk-cef \
    && cmake \
        -DMMLTK_CEF_DISTRIBUTION_CACHE_DIR=/tmp/mmltk-local-cef \
        -DOUTPUT_PATH=/tmp/mmltk-cef-distribution.env \
        -P ${MMLTK_CONTAINER_WORKTREE}/utilities/emit_mmltk_cef_distribution_env.cmake \
    && source /tmp/mmltk-cef-distribution.env \
    && tmpdir="$(mktemp -d)" \
    && ( cd "${MMLTK_CEF_DISTRIBUTION_CACHE_DIR}" && sha256sum -c "$(basename "${MMLTK_CEF_DISTRIBUTION_SHA256_PATH}")" ) \
    && unpacked_root_name="$(python3 -c 'import sys, tarfile; archive = tarfile.open(sys.argv[1], "r:*"); print(next((member.name.split("/", 1)[0] for member in archive if member.name), ""), end=""); archive.close()' "${MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH}")" \
    && if [[ -z "${unpacked_root_name}" ]]; then \
        echo "Failed to detect CEF archive root for ${MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH}" >&2; \
        exit 1; \
    fi \
    && rm -rf "${MMLTK_CEF_DISTRIBUTION_ROOT}" \
    && mkdir -p "${MMLTK_CEF_DISTRIBUTION_ROOT}" \
    && tar -xf "${MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH}" -C "${tmpdir}" \
    && if [[ ! -d "${tmpdir}/${unpacked_root_name}" ]]; then \
        echo "Failed to unpack CEF bundle ${MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH}" >&2; \
        exit 1; \
    fi \
    && cp -a "${tmpdir}/${unpacked_root_name}/." "${MMLTK_CEF_DISTRIBUTION_ROOT}/" \
    && for required_path in \
        "${MMLTK_CEF_DISTRIBUTION_ROOT}/include/cef_app.h" \
        "${MMLTK_CEF_DISTRIBUTION_ROOT}/libcef_dll" \
        "${MMLTK_CEF_DISTRIBUTION_BINARY_ROOT}/libcef.so" \
        "${MMLTK_CEF_DISTRIBUTION_BINARY_ROOT}/snapshot_blob.bin" \
        "${MMLTK_CEF_DISTRIBUTION_BINARY_ROOT}/v8_context_snapshot.bin" \
        "${MMLTK_CEF_DISTRIBUTION_BINARY_ROOT}/libEGL.so" \
        "${MMLTK_CEF_DISTRIBUTION_BINARY_ROOT}/libGLESv2.so" \
        "${MMLTK_CEF_DISTRIBUTION_RESOURCES_ROOT}/icudtl.dat" \
        "${MMLTK_CEF_DISTRIBUTION_RESOURCES_ROOT}/resources.pak" \
        "${MMLTK_CEF_DISTRIBUTION_RESOURCES_ROOT}/chrome_100_percent.pak" \
        "${MMLTK_CEF_DISTRIBUTION_RESOURCES_ROOT}/chrome_200_percent.pak" \
        "${MMLTK_CEF_DISTRIBUTION_RESOURCES_ROOT}/locales"; do \
        if [[ ! -e "${required_path}" ]]; then \
            echo "Pinned CEF distribution is incomplete after unpack: ${required_path}" >&2; \
            exit 1; \
        fi; \
    done \
    && rm -rf "${tmpdir}" /tmp/mmltk-cef-distribution.env /tmp/mmltk-local-cef

FROM toolchain AS analysis

COPY --from=browser_runtime_distribution /opt/mmltk-cef /opt/mmltk-cef

FROM toolchain AS build

COPY --from=browser_runtime_distribution /opt/mmltk-cef /opt/mmltk-cef
COPY CMakeLists.txt CMakePresets.json Dockerfile mmltk ${MMLTK_CONTAINER_WORKTREE}/
COPY cmake/ ${MMLTK_CONTAINER_WORKTREE}/cmake/
COPY include/ ${MMLTK_CONTAINER_WORKTREE}/include/
COPY src/ ${MMLTK_CONTAINER_WORKTREE}/src/
COPY tests/ ${MMLTK_CONTAINER_WORKTREE}/tests/
COPY third_party/ ${MMLTK_CONTAINER_WORKTREE}/third_party/
COPY utilities/ ${MMLTK_CONTAINER_WORKTREE}/utilities/

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    --mount=type=cache,id=mmltk-ccache,target=${MMLTK_CCACHE_DIR},sharing=locked \
    expected_source="${MMLTK_CONTAINER_WORKTREE}" \
    && expected_build="${MMLTK_CONTAINER_WORKTREE}/workspace/build/release" \
    && if [[ "${MMLTK_RESET_CCACHE_STATS}" == "ON" ]]; then \
        ccache -z; \
    fi \
    && c_compiler="/usr/bin/gcc-14" \
    && cxx_compiler="/usr/bin/g++-14" \
    && cuda_host_compiler="/usr/bin/g++-14" \
    && if [[ "${MMLTK_ENABLE_TIME_TRACE}" == "ON" ]]; then \
        c_compiler="/usr/bin/clang-${MMLTK_CLANG_VERSION}"; \
        cxx_compiler="/usr/bin/clang++-${MMLTK_CLANG_VERSION}"; \
        cuda_host_compiler="/usr/bin/clang++-${MMLTK_CLANG_VERSION}"; \
    fi \
    && cache_file="${expected_build}/CMakeCache.txt" \
    && if [[ -f "${cache_file}" ]]; then \
        cached_source="$(sed -n 's#^CMAKE_HOME_DIRECTORY:INTERNAL=##p' "${cache_file}")"; \
        cached_build="$(sed -n 's#^CMAKE_CACHEFILE_DIR:INTERNAL=##p' "${cache_file}")"; \
        if [[ "${cached_source}" != "${expected_source}" || "${cached_build}" != "${expected_build}" ]]; then \
            find "${expected_build}" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +; \
        fi; \
    fi \
    && cmake -S ${MMLTK_CONTAINER_WORKTREE} -B ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release -G Ninja \
        -DCMAKE_BUILD_TYPE=Dev \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_MMLTK_BROWSER_HOST=ON \
        -DBUILD_RFDETR_NATIVE=ON \
        -DBUILD_RFDETR_PYTHON_CHECKPOINT_LOADER=ON \
        -DMMLTK_DEV_STRICT_WARNINGS=ON \
        -DMMLTK_WARNINGS_AS_ERRORS=ON \
        -DMMLTK_CEF_WEBGPU_RUNTIME=${MMLTK_CEF_WEBGPU_RUNTIME} \
        -DMMLTK_CEF_ENABLE_UNSAFE_WEBGPU=${MMLTK_CEF_ENABLE_UNSAFE_WEBGPU} \
        -DMMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU=${MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU} \
        -DMMLTK_CUDA_VERSION=${MMLTK_CUDA_VERSION} \
        -DMMLTK_CUDA_ARCHITECTURES="${MMLTK_CUDA_ARCHITECTURES}" \
        -DMMLTK_CUDA_TOOLKIT_ROOT=/usr/local/cuda-${MMLTK_CUDA_VERSION} \
        -DMMLTK_TORCH_ROOT=/opt/pytorch \
        -DMMLTK_ONNX_ROOT=${MMLTK_CONTAINER_WORKTREE}/workspace/deps/onnx \
        -DMMLTK_ONNXRUNTIME_ROOT=${MMLTK_CONTAINER_WORKTREE}/workspace/deps/onnxruntime \
        -DMMLTK_TORCH_CUDA_ARCH_LIST="${MMLTK_TORCH_CUDA_ARCH_LIST}" \
        -DMMLTK_USE_MOLD=ON \
        -DMMLTK_ENABLE_TIME_TRACE=${MMLTK_ENABLE_TIME_TRACE} \
        -DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc \
        -DPython3_EXECUTABLE=/usr/bin/python3 \
        -DCMAKE_C_COMPILER="${c_compiler}" \
        -DCMAKE_CXX_COMPILER="${cxx_compiler}" \
        -DCMAKE_CUDA_HOST_COMPILER="${cuda_host_compiler}"

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    --mount=type=cache,id=mmltk-ccache,target=${MMLTK_CCACHE_DIR},sharing=locked \
    cmake --build ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release --target \
        mmltk_core \
        mmltk_rfdetr_lsap \
        mmltk_rfdetr_cuda_ops \
        mmltk_live_capture \
        mmltk_live_runtime \
        -j"$(nproc)"

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    --mount=type=cache,id=mmltk-ccache,target=${MMLTK_CCACHE_DIR},sharing=locked \
    cmake --build ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release --target mmltk_rfdetr_native -j"$(nproc)"

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    --mount=type=cache,id=mmltk-ccache,target=${MMLTK_CCACHE_DIR},sharing=locked \
    cmake --build ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release --target \
        mmltk_browser_app \
        mmltk_cli \
        mmltk_gui \
        mmltk_rfdetr_onnx_simplify_tool \
        -j"$(nproc)"

RUN --mount=type=cache,id=mmltk-release-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/release,sharing=locked \
    --mount=type=cache,id=mmltk-ccache,target=${MMLTK_CCACHE_DIR},sharing=locked \
    cmake --install ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release --prefix /opt/mmltk \
    && mkdir -p ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/tensorrt \
    && mkdir -p ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/nccl \
    && cp -a /usr/lib/x86_64-linux-gnu/libnvinfer*.so* ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/tensorrt/ \
    && cp -a /usr/lib/x86_64-linux-gnu/libnvonnxparser*.so* ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/tensorrt/ \
    && cp -a /opt/pytorch/lib/../../nvidia/nccl/lib/libnccl*.so* ${MMLTK_CONTAINER_WORKTREE}/workspace/runtime-libs/nccl/ \
    && mkdir -p ${MMLTK_TIME_TRACE_EXPORT_DIR}/release \
    && if [[ "${MMLTK_ENABLE_TIME_TRACE}" == "ON" ]]; then \
        ${MMLTK_CONTAINER_WORKTREE}/utilities/collect_time_traces.sh \
            ${MMLTK_CONTAINER_WORKTREE}/workspace/build/release \
            ${MMLTK_TIME_TRACE_EXPORT_DIR}/release; \
    fi

FROM toolchain AS tests

COPY CMakeLists.txt CMakePresets.json Dockerfile mmltk ${MMLTK_CONTAINER_WORKTREE}/
COPY cmake/ ${MMLTK_CONTAINER_WORKTREE}/cmake/
COPY include/ ${MMLTK_CONTAINER_WORKTREE}/include/
COPY src/ ${MMLTK_CONTAINER_WORKTREE}/src/
COPY tests/ ${MMLTK_CONTAINER_WORKTREE}/tests/
COPY third_party/ ${MMLTK_CONTAINER_WORKTREE}/third_party/
COPY utilities/ ${MMLTK_CONTAINER_WORKTREE}/utilities/

RUN --mount=type=cache,id=mmltk-test-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/test-bundles,sharing=locked \
    --mount=type=cache,id=mmltk-ccache,target=${MMLTK_CCACHE_DIR},sharing=locked \
    expected_source="${MMLTK_CONTAINER_WORKTREE}" \
    && expected_build="${MMLTK_CONTAINER_WORKTREE}/workspace/build/test-bundles" \
    && c_compiler="/usr/bin/gcc-14" \
    && cxx_compiler="/usr/bin/g++-14" \
    && cuda_host_compiler="/usr/bin/g++-14" \
    && if [[ "${MMLTK_ENABLE_TIME_TRACE}" == "ON" ]]; then \
        c_compiler="/usr/bin/clang-${MMLTK_CLANG_VERSION}"; \
        cxx_compiler="/usr/bin/clang++-${MMLTK_CLANG_VERSION}"; \
        cuda_host_compiler="/usr/bin/clang++-${MMLTK_CLANG_VERSION}"; \
    fi \
    && cache_file="${expected_build}/CMakeCache.txt" \
    && if [[ -f "${cache_file}" ]]; then \
        cached_source="$(sed -n 's#^CMAKE_HOME_DIRECTORY:INTERNAL=##p' "${cache_file}")"; \
        cached_build="$(sed -n 's#^CMAKE_CACHEFILE_DIR:INTERNAL=##p' "${cache_file}")"; \
        if [[ "${cached_source}" != "${expected_source}" || "${cached_build}" != "${expected_build}" ]]; then \
            find "${expected_build}" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +; \
        fi; \
    fi \
    && cmake -S ${MMLTK_CONTAINER_WORKTREE} -B ${MMLTK_CONTAINER_WORKTREE}/workspace/build/test-bundles -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
        -DBUILD_TESTS=${MMLTK_BUILD_TESTS} \
        -DBUILD_MMLTK_BROWSER_APP=OFF \
        -DBUILD_MMLTK_BROWSER_HOST=OFF \
        -DBUILD_RFDETR_NATIVE=ON \
        -DBUILD_RFDETR_PYTHON_CHECKPOINT_LOADER=ON \
        -DMMLTK_CUDA_VERSION=${MMLTK_CUDA_VERSION} \
        -DMMLTK_CUDA_ARCHITECTURES="${MMLTK_CUDA_ARCHITECTURES}" \
        -DMMLTK_CUDA_TOOLKIT_ROOT=/usr/local/cuda-${MMLTK_CUDA_VERSION} \
        -DMMLTK_TORCH_ROOT=/opt/pytorch \
        -DMMLTK_ONNX_ROOT=${MMLTK_CONTAINER_WORKTREE}/workspace/deps/onnx \
        -DMMLTK_ONNXRUNTIME_ROOT=${MMLTK_CONTAINER_WORKTREE}/workspace/deps/onnxruntime \
        -DMMLTK_TORCH_CUDA_ARCH_LIST="${MMLTK_TORCH_CUDA_ARCH_LIST}" \
        -DMMLTK_USE_MOLD=ON \
        -DMMLTK_ENABLE_TIME_TRACE=${MMLTK_ENABLE_TIME_TRACE} \
        -DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc \
        -DPython3_EXECUTABLE=/usr/bin/python3 \
        -DCMAKE_C_COMPILER="${c_compiler}" \
        -DCMAKE_CXX_COMPILER="${cxx_compiler}" \
        -DCMAKE_CUDA_HOST_COMPILER="${cuda_host_compiler}"

RUN --mount=type=cache,id=mmltk-test-build,target=${MMLTK_CONTAINER_WORKTREE}/workspace/build/test-bundles,sharing=locked \
    --mount=type=cache,id=mmltk-ccache,target=${MMLTK_CCACHE_DIR},sharing=locked \
    mkdir -p /opt/mmltk-test-bundles/bin ${MMLTK_TIME_TRACE_EXPORT_DIR}/tests \
    && if [[ "${MMLTK_BUILD_TESTS}" == "ON" ]]; then \
        cmake --build ${MMLTK_CONTAINER_WORKTREE}/workspace/build/test-bundles --target \
            mmltk_tests_core \
            mmltk_tests_gui \
            mmltk_tests_model_rfdetr \
            mmltk_rfdetr_onnx_simplify_tool \
            -j"$(nproc)" \
        && cmake --install ${MMLTK_CONTAINER_WORKTREE}/workspace/build/test-bundles --prefix /opt/mmltk-test-install \
        && cp -a /opt/mmltk-test-install/bin/mmltk_tests_* /opt/mmltk-test-bundles/bin/ \
        && if [[ "${MMLTK_ENABLE_TIME_TRACE}" == "ON" ]]; then \
            ${MMLTK_CONTAINER_WORKTREE}/utilities/collect_time_traces.sh \
                ${MMLTK_CONTAINER_WORKTREE}/workspace/build/test-bundles \
                ${MMLTK_TIME_TRACE_EXPORT_DIR}/tests; \
        fi; \
    fi \
    && ccache -s > ${MMLTK_TIME_TRACE_EXPORT_DIR}/ccache-stats.txt

FROM build AS time-trace-build

FROM tests AS time-trace-tests

FROM scratch AS time-trace-export

ARG MMLTK_TIME_TRACE_EXPORT_DIR=/opt/mmltk-time-trace

COPY --from=time-trace-build ${MMLTK_TIME_TRACE_EXPORT_DIR}/ /time-trace/
COPY --from=time-trace-tests ${MMLTK_TIME_TRACE_EXPORT_DIR}/ /time-trace/

FROM ${MMLTK_PYTORCH_RUNTIME_IMAGE} AS runtime

ARG DEBIAN_FRONTEND=noninteractive
ARG MMLTK_CUDA_VERSION=13.0
ARG MMLTK_CONTAINER_WORKTREE=/src
ARG MMLTK_CEF_RUNTIME_PACKAGES="libgtk-3-0 libgtk-4-1"
ARG MMLTK_CEF_WEBGPU_RUNTIME=auto
ARG MMLTK_CEF_ENABLE_UNSAFE_WEBGPU=OFF
ARG MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU=OFF
ARG MMLTK_TIME_TRACE_EXPORT_DIR=/opt/mmltk-time-trace

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        fonts-liberation \
        gdb \
        libatk-bridge2.0-0t64 \
        libatk1.0-0t64 \
        libasound2t64 \
        libatspi2.0-0t64 \
        libcups2t64 \
        libdbus-1-3 \
        libdrm2 \
        libegl1 \
        libgbm1 \
        libgl1 \
        libgfortran5 \
        ${MMLTK_CEF_RUNTIME_PACKAGES} \
        libnspr4 \
        libnss3 \
        libnvidia-egl-wayland1 \
        libopenblas0-pthread \
        libomp5-18 \
        libopengl0 \
        libprotobuf32t64 \
        libu2f-udev \
        libva2 \
        libvulkan1 \
        libwayland-client0 \
        libwayland-cursor0 \
        libwayland-egl1 \
        libx11-6 \
        libx11-xcb1 \
        libxcb1 \
        libxcb-dri3-0 \
        libxcb-render0 \
        libxcb-shm0 \
        libxcomposite1 \
        libxdamage1 \
        libxfixes3 \
        libxext6 \
        libxi6 \
        libxkbcommon0 \
        libxkbcommon-x11-0 \
        libxrandr2 \
        libxrender1 \
        libxshmfence1 \
        libxss1 \
        libxtst6 \
        xdg-utils \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /usr/lib/x86_64-linux-gnu/gbm \
 && ln -sf /usr/lib64/gbm/nvidia-drm_gbm.so \
    /usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so

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
COPY --from=tests /opt/mmltk-test-bundles/bin/ /opt/mmltk/bin/
COPY --from=tests ${MMLTK_TIME_TRACE_EXPORT_DIR}/ccache-stats.txt /tmp/mmltk-ccache-stats.txt

RUN for required_path in \
        /opt/mmltk/lib/cef/Debug/libcef.so \
        /opt/mmltk/lib/cef/Debug/snapshot_blob.bin \
        /opt/mmltk/lib/cef/Debug/v8_context_snapshot.bin \
        /opt/mmltk/lib/cef/Debug/libEGL.so \
        /opt/mmltk/lib/cef/Debug/libGLESv2.so \
        /opt/mmltk/lib/cef/Resources/icudtl.dat \
        /opt/mmltk/lib/cef/Resources/resources.pak \
        /opt/mmltk/lib/cef/Resources/chrome_100_percent.pak \
        /opt/mmltk/lib/cef/Resources/chrome_200_percent.pak \
        /opt/mmltk/lib/cef/Resources/locales; do \
        if [[ ! -e "${required_path}" ]]; then \
            echo "Packaged CEF runtime is incomplete after runtime image staging: ${required_path}" >&2; \
            exit 1; \
        fi; \
    done

RUN for resource_file in \
        icudtl.dat \
        resources.pak \
        chrome_100_percent.pak \
        chrome_200_percent.pak; do \
        ln -sfn "../Resources/${resource_file}" \
            "/opt/mmltk/lib/cef/Debug/${resource_file}"; \
        ln -sfn "../lib/cef/Resources/${resource_file}" \
            "/opt/mmltk/bin/${resource_file}"; \
    done \
    && ln -sfn ../Resources/locales /opt/mmltk/lib/cef/Debug/locales \
    && ln -sfn ../lib/cef/Resources/locales /opt/mmltk/bin/locales

RUN cat /tmp/mmltk-ccache-stats.txt && rm -f /tmp/mmltk-ccache-stats.txt

RUN cat <<'EOF' >/usr/local/bin/mmltk-entrypoint
#!/usr/bin/env bash
set -euo pipefail

case "${1:-mmltk}" in
    mmltk|mmltk-browser-host)
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

ENV MMLTK_CEF_RUNTIME_ROOT=/opt/mmltk/lib/cef \
    MMLTK_CEF_WEBGPU_RUNTIME=${MMLTK_CEF_WEBGPU_RUNTIME} \
    MMLTK_CEF_ENABLE_UNSAFE_WEBGPU=${MMLTK_CEF_ENABLE_UNSAFE_WEBGPU} \
    MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU=${MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU} \
    PATH=/opt/mmltk/bin:${PATH} \
    LD_LIBRARY_PATH=/opt/mmltk/lib/cef/Debug:/opt/mmltk/lib:/opt/pytorch/lib:/opt/pytorch/lib/../../nvidia/cu13/lib:/opt/pytorch/lib/../../nvidia/cudnn/lib:/opt/pytorch/lib/../../nvidia/cusparselt/lib:/opt/pytorch/lib/../../nvidia/nccl/lib:/opt/pytorch/lib/../../nvidia/nvshmem/lib:/usr/local/cuda-${MMLTK_CUDA_VERSION}/lib64

WORKDIR /opt/mmltk
ENTRYPOINT ["mmltk-entrypoint"]
CMD ["mmltk"]
