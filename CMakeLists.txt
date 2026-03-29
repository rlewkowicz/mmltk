cmake_minimum_required(VERSION 3.18)
if(POLICY CMP0138)
    cmake_policy(SET CMP0138 NEW)
endif()
project(fastloader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(FASTLOADER_CUDA_VERSION "13.2" CACHE STRING "Pinned CUDA toolkit version for fastloader builds")
set(FASTLOADER_CUDA_TOOLKIT_ROOT "/usr/local/cuda-${FASTLOADER_CUDA_VERSION}" CACHE PATH
    "Pinned CUDA toolkit root for fastloader builds")
set(FASTLOADER_CUDA_ARCHITECTURES "native" CACHE STRING
    "CUDA architectures for fastloader CUDA targets")
get_filename_component(FASTLOADER_CUDA_TOOLKIT_ROOT "${FASTLOADER_CUDA_TOOLKIT_ROOT}" REALPATH)
set(FASTLOADER_CUDA_NVCC "${FASTLOADER_CUDA_TOOLKIT_ROOT}/bin/nvcc")
if(NOT EXISTS "${FASTLOADER_CUDA_NVCC}")
    message(FATAL_ERROR
        "fastloader requires CUDA ${FASTLOADER_CUDA_VERSION}.x at ${FASTLOADER_CUDA_TOOLKIT_ROOT}. "
        "Install the toolkit there or override FASTLOADER_CUDA_TOOLKIT_ROOT.")
endif()

# Force both FindCUDAToolkit and enable_language(CUDA) to resolve the same toolkit
# even when the shell PATH still exposes an older nvcc.
set(CUDAToolkit_ROOT "${FASTLOADER_CUDA_TOOLKIT_ROOT}" CACHE PATH
    "Pinned CUDA toolkit root for fastloader builds" FORCE)
set(CMAKE_CUDA_COMPILER "${FASTLOADER_CUDA_NVCC}" CACHE FILEPATH
    "Pinned CUDA compiler for fastloader builds" FORCE)
set(CUDA_TOOLKIT_ROOT_DIR "${FASTLOADER_CUDA_TOOLKIT_ROOT}" CACHE PATH
    "Pinned legacy CUDA toolkit root for Torch/Caffe2 detection" FORCE)
set(CUDA_BIN_PATH "${FASTLOADER_CUDA_TOOLKIT_ROOT}" CACHE PATH
    "Pinned legacy CUDA bin path for Torch/Caffe2 detection" FORCE)
set(CUDA_NVCC_EXECUTABLE "${FASTLOADER_CUDA_NVCC}" CACHE FILEPATH
    "Pinned legacy CUDA compiler for Torch/Caffe2 detection" FORCE)
set(ENV{CUDA_HOME} "${FASTLOADER_CUDA_TOOLKIT_ROOT}")
set(ENV{CUDA_BIN_PATH} "${FASTLOADER_CUDA_TOOLKIT_ROOT}")
set(ENV{CUDACXX} "${FASTLOADER_CUDA_NVCC}")
set(ENV{PATH} "${FASTLOADER_CUDA_TOOLKIT_ROOT}/bin:$ENV{PATH}")

set(FASTLOADER_SUPPORTED_BUILD_TYPES Dev Release)
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${FASTLOADER_SUPPORTED_BUILD_TYPES}" CACHE STRING
        "Supported fastloader build types" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${FASTLOADER_SUPPORTED_BUILD_TYPES})
    list(FIND FASTLOADER_SUPPORTED_BUILD_TYPES "${CMAKE_BUILD_TYPE}" FASTLOADER_BUILD_TYPE_INDEX)
    if(FASTLOADER_BUILD_TYPE_INDEX EQUAL -1)
        string(JOIN ", " FASTLOADER_SUPPORTED_BUILD_TYPES_TEXT ${FASTLOADER_SUPPORTED_BUILD_TYPES})
        message(FATAL_ERROR
            "fastloader supports only these build types: ${FASTLOADER_SUPPORTED_BUILD_TYPES_TEXT}")
    endif()
endif()

option(FASTLOADER_DEV_STRICT_WARNINGS
    "Enable aggressive host-side warnings for the Dev build"
    ON)
option(FASTLOADER_DEV_ENABLE_ASAN
    "Enable AddressSanitizer for host-side Dev C++ builds"
    OFF)
option(FASTLOADER_DEV_ENABLE_UBSAN
    "Enable UndefinedBehaviorSanitizer for host-side Dev C++ builds"
    OFF)

set(FASTLOADER_X86_SIMD_FLAGS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|amd64|AMD64)$")
    set(FASTLOADER_X86_SIMD_FLAGS "-mssse3 -msse4.2 -mavx2 -mfma")
endif()
set(FASTLOADER_COMMON_MACHINE_FLAGS
    "-march=native -mtune=native ${FASTLOADER_X86_SIMD_FLAGS} -fno-semantic-interposition -ffunction-sections -fdata-sections")
set(FASTLOADER_COMMON_WARNING_FLAGS "-Wall -Wextra -Wpedantic")
set(FASTLOADER_STRICT_WARNING_FLAGS "")
if(FASTLOADER_DEV_STRICT_WARNINGS)
    set(FASTLOADER_STRICT_WARNING_FLAGS
        "-Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual -Wconversion -Wformat=2 -Wnull-dereference -Wimplicit-fallthrough")
endif()
set(FASTLOADER_DEV_SANITIZER_FLAGS "")
if(FASTLOADER_DEV_ENABLE_ASAN)
    string(APPEND FASTLOADER_DEV_SANITIZER_FLAGS " -fsanitize=address")
endif()
if(FASTLOADER_DEV_ENABLE_UBSAN)
    string(APPEND FASTLOADER_DEV_SANITIZER_FLAGS " -fsanitize=undefined")
endif()
if(NOT FASTLOADER_DEV_SANITIZER_FLAGS STREQUAL "")
    string(APPEND FASTLOADER_DEV_SANITIZER_FLAGS " -fno-sanitize-recover=all")
endif()
set(FASTLOADER_RELEASE_FLAGS
    "-O3 -DNDEBUG -fomit-frame-pointer -fno-plt ${FASTLOADER_COMMON_MACHINE_FLAGS}")
set(FASTLOADER_DEV_FLAGS
    "-O2 -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls -D_GLIBCXX_ASSERTIONS -D_FORTIFY_SOURCE=3 ${FASTLOADER_COMMON_MACHINE_FLAGS} ${FASTLOADER_COMMON_WARNING_FLAGS} ${FASTLOADER_STRICT_WARNING_FLAGS} ${FASTLOADER_DEV_SANITIZER_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "${FASTLOADER_RELEASE_FLAGS}")
set(CMAKE_CXX_FLAGS_DEV "${FASTLOADER_DEV_FLAGS}")

set(CMAKE_CUDA_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CUDA_FLAGS_DEV "-O2 -g -lineinfo -Xcompiler=-fno-omit-frame-pointer")

set(FASTLOADER_COMMON_LINK_FLAGS "-Wl,-O2 -Wl,--as-needed")
set(FASTLOADER_RELEASE_LINK_FLAGS "${FASTLOADER_COMMON_LINK_FLAGS} -Wl,--gc-sections")
set(FASTLOADER_DEV_LINK_FLAGS
    "${FASTLOADER_COMMON_LINK_FLAGS} -Wl,--gc-sections ${FASTLOADER_DEV_SANITIZER_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${FASTLOADER_RELEASE_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${FASTLOADER_RELEASE_LINK_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_DEV "${FASTLOADER_DEV_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_DEV "${FASTLOADER_DEV_LINK_FLAGS}")

include(CheckIPOSupported)
check_ipo_supported(RESULT FASTLOADER_IPO_SUPPORTED OUTPUT FASTLOADER_IPO_OUTPUT LANGUAGES CXX)
if(FASTLOADER_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(STATUS "IPO disabled: ${FASTLOADER_IPO_OUTPUT}")
endif()

find_package(CUDAToolkit REQUIRED)
if(NOT "${CUDAToolkit_VERSION}" MATCHES "^${FASTLOADER_CUDA_VERSION}(\\.|$)")
    message(FATAL_ERROR
        "Expected CUDA ${FASTLOADER_CUDA_VERSION}.x from ${FASTLOADER_CUDA_TOOLKIT_ROOT}, "
        "but CMake resolved CUDA ${CUDAToolkit_VERSION} from ${CUDAToolkit_BIN_DIR}.")
endif()
message(STATUS "Using CUDA toolkit ${CUDAToolkit_VERSION} from ${CUDAToolkit_ROOT}")
find_package(Threads REQUIRED)

# ---- Third party (vendored headers) ----
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party/stb)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party/json/include)

add_library(cli11 INTERFACE)
target_include_directories(cli11 SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

set(FASTLOADER_AVIR_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/avir")
if(NOT EXISTS "${FASTLOADER_AVIR_SOURCE_DIR}/avir.h")
    message(FATAL_ERROR
        "Missing vendored AVIR checkout at ${FASTLOADER_AVIR_SOURCE_DIR}. "
        "CPU image resize is implemented via AVIR only; there is no fallback.")
endif()

add_library(avir INTERFACE)
target_include_directories(avir SYSTEM INTERFACE ${FASTLOADER_AVIR_SOURCE_DIR})

add_library(fastloader_build_options INTERFACE)
target_compile_definitions(fastloader_build_options INTERFACE
    FASTLOADER_ENABLE_PROFILING=$<IF:$<CONFIG:Dev>,1,0>
    FASTLOADER_BUILD_CONFIG=\"$<CONFIG>\"
)

# ---- Core library ----
add_library(fastloader_core SHARED
    src/dataset/compile/dataset_compiler.cpp
    src/dataset/compile/dataset_compiler_scan_labels.cpp
    src/dataset/compile/dataset_compiler_pixels.cpp
    src/dataset/compile/dataset_compiler_layout.cpp
    src/image_resize.cpp
    src/dataset/loader/dataset_loader.cpp
    src/dataset/loader/dataset_loader_runtime.cpp
    src/dataset/loader/dataset_loader_pipeline.cpp
    src/cpu_affinity.cpp
    src/execution_policy.cpp
    src/cuda_stream_manager.cpp
    src/profile_utils.cpp
    src/worker_pool.cpp
    src/stb_image_write.cpp
)
target_include_directories(fastloader_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fastloader_core PUBLIC
    CUDA::cudart
    Threads::Threads
    stb
    nlohmann_json
    fastloader_build_options
PRIVATE
    avir
)

add_library(fastloader_runtime STATIC
    src/runtime/async_runtime.cpp
    src/runtime/model_registry.cpp
)
target_include_directories(fastloader_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fastloader_runtime PUBLIC
    fastloader_core
    fastloader_build_options
)

# ---- CLI tool ----
add_executable(fastloader_cli src/cli.cpp)
target_link_libraries(fastloader_cli PRIVATE fastloader_core fastloader_build_options cli11)
target_include_directories(fastloader_cli PRIVATE ${CMAKE_SOURCE_DIR}/src)

option(BUILD_RFDETR_NATIVE "Build native RF-DETR validation backends" ON)
option(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER
    "Enable embedded Python parsing of upstream RF-DETR .pth checkpoints"
    ${BUILD_RFDETR_NATIVE})
option(BUILD_FASTLOADER_GUI "Build the standalone ImGui RF-DETR desktop GUI" OFF)
set(FASTLOADER_IMGUI_SOURCE_DIR "" CACHE PATH
    "Optional local Dear ImGui checkout for BUILD_FASTLOADER_GUI")
target_compile_definitions(fastloader_core PRIVATE
    FASTLOADER_DATASET_COMPILER_CUDA=$<IF:$<BOOL:${BUILD_RFDETR_NATIVE}>,1,0>
)

add_library(fastloader_rfdetr_lsap STATIC
    src/rfdetr/utils/scipy_rectangular_lsap.cpp
)
target_include_directories(fastloader_rfdetr_lsap
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fastloader_rfdetr_lsap
    PUBLIC
        fastloader_build_options
)

set(FASTLOADER_NEEDS_TORCH OFF)
if(BUILD_RFDETR_NATIVE)
    set(FASTLOADER_NEEDS_TORCH ON)
endif()

set(FASTLOADER_NEEDS_PYTHON OFF)
if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
    if(NOT BUILD_RFDETR_NATIVE)
        message(FATAL_ERROR
            "BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER requires BUILD_RFDETR_NATIVE=ON.")
    endif()
    set(FASTLOADER_NEEDS_PYTHON ON)
endif()

set(FASTLOADER_TORCH_ROOT "" CACHE PATH
    "Optional LibTorch installation prefix containing share/cmake/Torch")
set(FASTLOADER_TORCH_SOURCE_ROOT "/opt/src/pytorch-v2.10.0" CACHE PATH
    "PyTorch source tree matching the LibTorch build, used for vendored ONNX headers")
set(FASTLOADER_TORCH_BUILD_ROOT "/opt/src/pytorch-build-2.10.0-cu132" CACHE PATH
    "PyTorch build tree matching the LibTorch build, used for generated vendored ONNX protobuf headers")
set(FASTLOADER_TORCH_CUDA_ARCH_LIST "" CACHE STRING
    "Optional TORCH_CUDA_ARCH_LIST override for LibTorch/Caffe2 consumers")

if(FASTLOADER_NEEDS_TORCH)
    if(FASTLOADER_TORCH_CUDA_ARCH_LIST)
        set(TORCH_CUDA_ARCH_LIST "${FASTLOADER_TORCH_CUDA_ARCH_LIST}" CACHE STRING
            "Pinned CUDA architectures for LibTorch/Caffe2 consumers" FORCE)
    endif()
    set(_fastloader_nvrtc_library "${FASTLOADER_CUDA_TOOLKIT_ROOT}/lib64/libnvrtc.so")
    if(EXISTS "${_fastloader_nvrtc_library}")
        file(SHA256 "${_fastloader_nvrtc_library}" _fastloader_nvrtc_hash)
        string(SUBSTRING "${_fastloader_nvrtc_hash}" 0 8 CUDA_NVRTC_SHORTHASH)
        set(CUDA_NVRTC_SHORTHASH "${CUDA_NVRTC_SHORTHASH}" CACHE STRING
            "Pinned nvrtc shorthash for LibTorch/Caffe2 detection" FORCE)
    endif()
    if(FASTLOADER_TORCH_ROOT)
        list(PREPEND CMAKE_PREFIX_PATH "${FASTLOADER_TORCH_ROOT}")
    endif()
    set(_fastloader_saved_cuda_architectures "")
    if(DEFINED CMAKE_CUDA_ARCHITECTURES)
        if(NOT CMAKE_CUDA_ARCHITECTURES STREQUAL "")
            set(_fastloader_saved_cuda_architectures "${CMAKE_CUDA_ARCHITECTURES}")
        endif()
        unset(CMAKE_CUDA_ARCHITECTURES CACHE)
        unset(CMAKE_CUDA_ARCHITECTURES)
    endif()
    find_package(Torch REQUIRED)
    if(NOT _fastloader_saved_cuda_architectures STREQUAL "")
        set(CMAKE_CUDA_ARCHITECTURES "${_fastloader_saved_cuda_architectures}" CACHE STRING
            "CUDA architectures" FORCE)
    endif()

    if(DEFINED TORCH_INSTALL_PREFIX AND NOT TORCH_INSTALL_PREFIX STREQUAL "")
        get_filename_component(TORCH_LIBRARY_DIR "${TORCH_INSTALL_PREFIX}/lib" ABSOLUTE)
    elseif(FASTLOADER_TORCH_ROOT)
        get_filename_component(TORCH_LIBRARY_DIR "${FASTLOADER_TORCH_ROOT}/lib" ABSOLUTE)
    elseif(DEFINED Torch_DIR AND NOT Torch_DIR STREQUAL "")
        get_filename_component(TORCH_LIBRARY_DIR "${Torch_DIR}/../../../lib" ABSOLUTE)
    else()
        foreach(_torch_library IN LISTS TORCH_LIBRARIES)
            if(IS_ABSOLUTE "${_torch_library}")
                get_filename_component(TORCH_LIBRARY_DIR "${_torch_library}" DIRECTORY)
                break()
            endif()
        endforeach()
    endif()
    if(NOT DEFINED TORCH_LIBRARY_DIR OR TORCH_LIBRARY_DIR STREQUAL "" OR NOT EXISTS "${TORCH_LIBRARY_DIR}")
        message(FATAL_ERROR
            "Failed to resolve the LibTorch library directory. "
            "Set FASTLOADER_TORCH_ROOT or Torch_DIR to your CUDA 13.2 LibTorch install.")
    endif()
    separate_arguments(TORCH_CXX_FLAGS_LIST NATIVE_COMMAND "${TORCH_CXX_FLAGS}")
    target_compile_options(fastloader_core PUBLIC ${TORCH_CXX_FLAGS_LIST})
    target_compile_options(fastloader_cli PRIVATE ${TORCH_CXX_FLAGS_LIST})
endif()

function(fastloader_enable_torch target visibility)
    if(NOT FASTLOADER_NEEDS_TORCH)
        message(FATAL_ERROR "Torch support requested for ${target} but Torch is not enabled")
    endif()
    target_include_directories(${target} SYSTEM ${visibility} ${TORCH_INCLUDE_DIRS})
    target_compile_options(${target} ${visibility} ${TORCH_CXX_FLAGS_LIST})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} ${visibility}
            $<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:CXX>>:-Wno-null-dereference>)
    endif()
    target_link_libraries(${target} ${visibility} ${TORCH_LIBRARIES})
endfunction()

function(fastloader_enable_python_checkpoint_loader target visibility)
    if(NOT FASTLOADER_NEEDS_PYTHON)
        message(FATAL_ERROR "Python checkpoint loader requested for ${target} but it is not enabled")
    endif()
    target_compile_definitions(${target} ${visibility}
        FASTLOADER_RFDETR_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
        FASTLOADER_RFDETR_PYTHON_CHECKPOINT_BRIDGE="${FASTLOADER_RFDETR_PYTHON_CHECKPOINT_BRIDGE}"
    )
endfunction()

function(fastloader_enable_native_runtime_rpath target)
    set(_fastloader_build_rpath "")
    foreach(_fastloader_system_libdir IN ITEMS
            /lib64
            /usr/lib64
            /lib/x86_64-linux-gnu
            /usr/lib/x86_64-linux-gnu)
        if(EXISTS "${_fastloader_system_libdir}")
            list(APPEND _fastloader_build_rpath "${_fastloader_system_libdir}")
        endif()
    endforeach()
    if(DEFINED TORCH_LIBRARY_DIR AND NOT TORCH_LIBRARY_DIR STREQUAL "")
        list(APPEND _fastloader_build_rpath "${TORCH_LIBRARY_DIR}")
    endif()
    if(DEFINED ONNXRUNTIME_LIBRARY_DIR AND NOT ONNXRUNTIME_LIBRARY_DIR STREQUAL "")
        list(APPEND _fastloader_build_rpath "${ONNXRUNTIME_LIBRARY_DIR}")
    endif()
    if(_fastloader_build_rpath)
        list(JOIN _fastloader_build_rpath ";" _fastloader_build_rpath_value)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "${_fastloader_build_rpath_value}")
    endif()
endfunction()

if(FASTLOADER_NEEDS_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(FASTLOADER_RFDETR_PYTHON_CHECKPOINT_BRIDGE
        "${CMAKE_SOURCE_DIR}/utilities/rfdetr_checkpoint_bridge.py")
    if(NOT EXISTS "${FASTLOADER_RFDETR_PYTHON_CHECKPOINT_BRIDGE}")
        message(FATAL_ERROR
            "Missing RF-DETR Python checkpoint bridge: ${FASTLOADER_RFDETR_PYTHON_CHECKPOINT_BRIDGE}")
    endif()
endif()

if(BUILD_RFDETR_NATIVE)
    if(NOT EXISTS "${FASTLOADER_TORCH_SOURCE_ROOT}/third_party/onnx/onnx/checker.h"
       OR NOT EXISTS "${FASTLOADER_TORCH_SOURCE_ROOT}/third_party/onnx/onnx/shape_inference/implementation.h")
        message(FATAL_ERROR
            "Missing PyTorch-matching vendored ONNX headers under ${FASTLOADER_TORCH_SOURCE_ROOT}/third_party/onnx. "
            "Set FASTLOADER_TORCH_SOURCE_ROOT to the matching PyTorch source tree before building.")
    endif()
    if(NOT EXISTS "${FASTLOADER_TORCH_BUILD_ROOT}/third_party/onnx/onnx/onnx-ml.pb.h"
       OR NOT EXISTS "${FASTLOADER_TORCH_BUILD_ROOT}/third_party/onnx/onnx/onnx-data.pb.h")
        message(FATAL_ERROR
            "Missing PyTorch-matching generated ONNX protobuf headers under ${FASTLOADER_TORCH_BUILD_ROOT}/third_party/onnx. "
            "Set FASTLOADER_TORCH_BUILD_ROOT to the matching PyTorch build tree before building.")
    endif()
    set(FASTLOADER_PYTORCH_ONNX_LIBRARY "${FASTLOADER_TORCH_BUILD_ROOT}/lib/libonnx.a")
    set(FASTLOADER_PYTORCH_ONNX_PROTO_LIBRARY "${FASTLOADER_TORCH_BUILD_ROOT}/lib/libonnx_proto.a")
    set(FASTLOADER_PYTORCH_PROTOBUF_LIBRARY "${FASTLOADER_TORCH_BUILD_ROOT}/lib/libprotobuf.a")
    if(NOT EXISTS "${FASTLOADER_PYTORCH_ONNX_LIBRARY}"
       OR NOT EXISTS "${FASTLOADER_PYTORCH_ONNX_PROTO_LIBRARY}"
       OR NOT EXISTS "${FASTLOADER_PYTORCH_PROTOBUF_LIBRARY}")
        message(FATAL_ERROR
            "Missing PyTorch-matching ONNX/protobuf static libraries under ${FASTLOADER_TORCH_BUILD_ROOT}/lib. "
            "Set FASTLOADER_TORCH_BUILD_ROOT to the matching PyTorch build tree before building.")
    endif()
    set(FASTLOADER_PYTORCH_ONNX_INCLUDE_DIR "${FASTLOADER_TORCH_SOURCE_ROOT}/third_party/onnx")
    set(FASTLOADER_PYTORCH_ONNX_BUILD_INCLUDE_DIR "${FASTLOADER_TORCH_BUILD_ROOT}/third_party/onnx")

    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        set(_fastloader_cuda_architectures "${FASTLOADER_CUDA_ARCHITECTURES}")
        if(_fastloader_cuda_architectures STREQUAL "" AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
            set(_fastloader_cuda_architectures native)
        elseif(_fastloader_cuda_architectures STREQUAL "")
            set(_fastloader_cuda_architectures "75;80;86;87;88;89;90")
        endif()
        set(CMAKE_CUDA_ARCHITECTURES "${_fastloader_cuda_architectures}" CACHE STRING
            "CUDA architectures" FORCE)
    endif()
    enable_language(CUDA)
    target_sources(fastloader_core PRIVATE
        src/dataset/compile/dataset_compiler_cuda.cu
    )
    set_target_properties(fastloader_core PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        HINTS
            /opt/onnxruntime/include
            /usr/local/include
            /usr/include)
    find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        HINTS
            /opt/onnxruntime/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu)
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR
            "ONNX Runtime C++ GPU libraries were not found. Install the official onnxruntime GPU package "
            "or run utilities/install_local_rfdetr_deps.sh before building.")
    endif()
    get_filename_component(ONNXRUNTIME_LIBRARY_DIR "${ONNXRUNTIME_LIBRARY}" DIRECTORY)

    find_path(TENSORRT_INCLUDE_DIR
        NAMES NvInfer.h
        HINTS
            /usr/include
            /usr/include/x86_64-linux-gnu
            /usr/local/include
            /opt/TensorRT/include
            /opt/tensorrt/include)
    find_library(TENSORRT_NVINFER_LIBRARY
        NAMES nvinfer
        HINTS
            /usr/lib/x86_64-linux-gnu
            /usr/local/lib
            /usr/local/lib64
            /opt/TensorRT/lib
            /opt/tensorrt/lib)
    find_library(TENSORRT_NVONNXPARSER_LIBRARY
        NAMES nvonnxparser
        HINTS
            /usr/lib/x86_64-linux-gnu
            /usr/local/lib
            /usr/local/lib64
            /opt/TensorRT/lib
            /opt/tensorrt/lib)
    if(NOT TENSORRT_INCLUDE_DIR OR NOT TENSORRT_NVINFER_LIBRARY OR NOT TENSORRT_NVONNXPARSER_LIBRARY)
        message(FATAL_ERROR "TensorRT shared libraries and headers were not found. Install libnvinfer10, libnvinfer-headers-dev, and libnvonnxparsers-dev before building.")
    endif()

    set(FASTLOADER_RFDETR_LIVE_CAPTURE_ENABLED ON)

    add_library(fastloader_rfdetr_cuda_ops STATIC
        src/rfdetr/layers/ms_deform_attn_op.cpp
        src/rfdetr/layers/ms_deform_attn_cuda_wrapper.cpp
        src/rfdetr/layers/training_mask_ops_cuda_wrapper.cpp
        src/rfdetr/cuda/ms_deform_attn_cuda.cu
        src/rfdetr/cuda/training_mask_ops_cuda.cu
        src/rfdetr/cuda/detr_ops_cuda.cu
        src/rfdetr/cuda/draw_cuda.cu
        src/rfdetr/cuda/live_preprocess.cu
    )
    file(MAKE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/fastloader_rfdetr_cuda_ops.dir/src/rfdetr"
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/fastloader_rfdetr_cuda_ops.dir/src/rfdetr/cuda"
    )
    target_include_directories(fastloader_rfdetr_cuda_ops
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_include_directories(fastloader_rfdetr_cuda_ops
        SYSTEM PRIVATE
            ${TORCH_INCLUDE_DIRS}
    )
    target_link_libraries(fastloader_rfdetr_cuda_ops
        PUBLIC
            fastloader_build_options
            CUDA::cudart
        PRIVATE
            ${TORCH_LIBRARIES}
    )
    target_compile_options(fastloader_rfdetr_cuda_ops PRIVATE ${TORCH_CXX_FLAGS_LIST})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(fastloader_rfdetr_cuda_ops PRIVATE
            $<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:CXX>>:-Wno-null-dereference>)
    endif()
    set_target_properties(fastloader_rfdetr_cuda_ops PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
    )

    add_library(fastloader_live_capture STATIC
        src/frameshow/capture_session_core.cpp
        src/frameshow/capture_session_device.cpp
        src/frameshow/capture_session_internal.cpp
        src/frameshow/capture_session_preview.cpp
    )
    target_include_directories(fastloader_live_capture
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(fastloader_live_capture
        PUBLIC
            fastloader_build_options
            fastloader_rfdetr_cuda_ops
            CUDA::cudart
    )
    target_compile_options(fastloader_live_capture
        PRIVATE
            -Wall -Wextra -Wpedantic
    )
    set_target_properties(fastloader_live_capture PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_library(fastloader_rfdetr_native STATIC
        src/rfdetr/inference/backend_onnx.cpp
        src/rfdetr/inference/backend_tensorrt.cpp
        src/rfdetr/checkpoint.cpp
        src/rfdetr/cli/cli.cpp
        src/rfdetr/detection_ops.cpp
        src/rfdetr/inference/evaluator.cpp
        src/rfdetr/inference/live_predict.cpp
        src/rfdetr/onnx_lowering.cpp
        src/rfdetr/layers/model.cpp
        src/rfdetr/onnx_simplify.cpp
        src/rfdetr/layers/model_components.cpp
        src/rfdetr/layers/model_config.cpp
        src/rfdetr/layers/modules.cpp
        src/rfdetr/draw.cpp
        src/rfdetr/training/native_optimizer.cpp
        src/rfdetr/inference/postprocess.cpp
        src/rfdetr/inference/predict.cpp
        src/rfdetr/checkpoint/python_checkpoint_bridge.cpp
        src/rfdetr/core/runtime.cpp
        src/rfdetr/target_builder.cpp
        src/rfdetr/training/train.cpp
        src/rfdetr/inference/validate.cpp
        src/rfdetr/inference/validate_workers.cpp
        src/rfdetr/weight_catalog.cpp
    )
    target_include_directories(fastloader_rfdetr_native
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_include_directories(fastloader_rfdetr_native
        SYSTEM PRIVATE
            ${FASTLOADER_PYTORCH_ONNX_INCLUDE_DIR}
            ${FASTLOADER_PYTORCH_ONNX_BUILD_INCLUDE_DIR}
            ${ONNXRUNTIME_INCLUDE_DIR}
            ${TENSORRT_INCLUDE_DIR}
    )
    fastloader_enable_torch(fastloader_rfdetr_native PRIVATE)
    target_link_libraries(fastloader_rfdetr_native
        PUBLIC
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_lsap
            fastloader_rfdetr_cuda_ops
            cli11
        PRIVATE
            ${FASTLOADER_PYTORCH_ONNX_LIBRARY}
            ${FASTLOADER_PYTORCH_ONNX_PROTO_LIBRARY}
            ${FASTLOADER_PYTORCH_PROTOBUF_LIBRARY}
            ${ONNXRUNTIME_LIBRARY}
            ${TENSORRT_NVINFER_LIBRARY}
            ${TENSORRT_NVONNXPARSER_LIBRARY}
    )
    if(FASTLOADER_RFDETR_LIVE_CAPTURE_ENABLED)
        target_link_libraries(fastloader_rfdetr_native PRIVATE fastloader_live_capture)
    endif()
    if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
        fastloader_enable_python_checkpoint_loader(fastloader_rfdetr_native PRIVATE)
    endif()
    target_compile_definitions(fastloader_rfdetr_native PUBLIC
        FASTLOADER_RFDETR_PYTHON_CHECKPOINT_LOADER=$<IF:$<BOOL:${BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER}>,1,0>
        FASTLOADER_RFDETR_LIVE_CAPTURE=$<IF:$<BOOL:${FASTLOADER_RFDETR_LIVE_CAPTURE_ENABLED}>,1,0>
    )
    target_compile_definitions(fastloader_rfdetr_native PRIVATE ONNX_ML=1)
    set_target_properties(fastloader_rfdetr_native PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )
    add_library(fastloader_rfdetr ALIAS fastloader_rfdetr_native)
    target_compile_definitions(fastloader_cli PRIVATE FASTLOADER_BUILD_RFDETR_NATIVE=1)
    target_link_libraries(fastloader_cli PRIVATE fastloader_rfdetr_native)
    set_target_properties(fastloader_cli PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )
    fastloader_enable_native_runtime_rpath(fastloader_cli)
else()
    target_compile_definitions(fastloader_cli PRIVATE FASTLOADER_BUILD_RFDETR_NATIVE=0)
endif()

if(BUILD_FASTLOADER_GUI)
    if(NOT BUILD_RFDETR_NATIVE)
        message(FATAL_ERROR "BUILD_FASTLOADER_GUI requires BUILD_RFDETR_NATIVE=ON.")
    endif()
    if(NOT FASTLOADER_NEEDS_PYTHON)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()
    set(FASTLOADER_GUI_VAST_BRIDGE "${CMAKE_SOURCE_DIR}/utilities/vast_bridge.py")
    if(NOT EXISTS "${FASTLOADER_GUI_VAST_BRIDGE}")
        message(FATAL_ERROR
            "Missing Vast GUI bridge script: ${FASTLOADER_GUI_VAST_BRIDGE}")
    endif()

    include(FetchContent)
    find_package(OpenGL REQUIRED)
    find_package(glfw3 QUIET)

    if(NOT TARGET glfw)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FASTLOADER_GUI_WAYLAND QUIET
            wayland-client
            wayland-cursor
            wayland-egl
            xkbcommon
        )
        pkg_check_modules(FASTLOADER_GUI_X11 QUIET
            x11
            xrandr
            xinerama
            xcursor
            xi
        )
        if(NOT FASTLOADER_GUI_WAYLAND_FOUND AND NOT FASTLOADER_GUI_X11_FOUND)
            message(FATAL_ERROR
                "BUILD_FASTLOADER_GUI could not find either a complete Wayland or X11 GLFW dependency set. "
                "Install Fedora's glfw-devel package or the matching Wayland/X11 development packages.")
        endif()
        if(FASTLOADER_GUI_X11_FOUND)
            set(_fastloader_gui_glfw_x11 ON)
        else()
            set(_fastloader_gui_glfw_x11 OFF)
        endif()
        if(FASTLOADER_GUI_WAYLAND_FOUND)
            set(_fastloader_gui_glfw_wayland ON)
        else()
            set(_fastloader_gui_glfw_wayland OFF)
        endif()

        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_X11 ${FASTLOADER_GUI_X11_FOUND} CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND ${FASTLOADER_GUI_WAYLAND_FOUND} CACHE BOOL "" FORCE)
        message(STATUS
            "fastloader GUI GLFW fallback backends: "
            "X11=${_fastloader_gui_glfw_x11} Wayland=${_fastloader_gui_glfw_wayland}")

        FetchContent_Declare(
            glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
        )
        FetchContent_MakeAvailable(glfw)
    endif()

    if(FASTLOADER_IMGUI_SOURCE_DIR)
        get_filename_component(_fastloader_imgui_source_dir "${FASTLOADER_IMGUI_SOURCE_DIR}" REALPATH)
        if(NOT EXISTS "${_fastloader_imgui_source_dir}/imgui.cpp")
            message(FATAL_ERROR
                "FASTLOADER_IMGUI_SOURCE_DIR must point at a Dear ImGui checkout containing imgui.cpp.")
        endif()
    else()
        FetchContent_Declare(
            imgui_src
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.91.8
        )
        FetchContent_MakeAvailable(imgui_src)
        set(_fastloader_imgui_source_dir "${imgui_src_SOURCE_DIR}")
    endif()

    add_library(fastloader_imgui STATIC
        ${_fastloader_imgui_source_dir}/imgui.cpp
        ${_fastloader_imgui_source_dir}/imgui_draw.cpp
        ${_fastloader_imgui_source_dir}/imgui_tables.cpp
        ${_fastloader_imgui_source_dir}/imgui_widgets.cpp
        ${_fastloader_imgui_source_dir}/misc/cpp/imgui_stdlib.cpp
        ${_fastloader_imgui_source_dir}/backends/imgui_impl_glfw.cpp
        ${_fastloader_imgui_source_dir}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(fastloader_imgui
        PUBLIC
            ${_fastloader_imgui_source_dir}
            ${_fastloader_imgui_source_dir}/backends
    )
    target_link_libraries(fastloader_imgui
        PUBLIC
            fastloader_build_options
            glfw
            OpenGL::GL
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(fastloader_imgui PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-conversion -Wno-null-dereference>
        )
    endif()

    add_library(fastloader_gui_module STATIC
        src/gui/annotation_core.cpp
        src/gui/annotation_geometry.cpp
        src/gui/annotation_live_capture.cpp
        src/gui/app.cpp
        src/gui/app_options.cpp
        src/gui/canvas_layers.cpp
        src/gui/file_picker.cpp
        src/gui/gui_settings.cpp
        src/gui/live_preview_texture.cpp
        src/gui/rfdetr_module.cpp
        src/gui/rfdetr_workflows.cpp
        src/gui/source_runtime.cpp
        src/gui/source_selection.cpp
        src/gui/still_image_preview.cpp
        src/gui/train_command.cpp
        src/gui/train_process_runtime.cpp
        src/gui/ui_style.cpp
        src/gui/vast_runtime.cpp
    )
    target_include_directories(fastloader_gui_module PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(fastloader_gui_module
        PUBLIC
            fastloader_build_options
            fastloader_imgui
            fastloader_core
            fastloader_runtime
            fastloader_rfdetr_native
        PRIVATE
            cli11
            nlohmann_json
    )
    target_compile_definitions(fastloader_gui_module PRIVATE
        FASTLOADER_GUI_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
        FASTLOADER_GUI_VAST_BRIDGE_SCRIPT="${FASTLOADER_GUI_VAST_BRIDGE}"
        FASTLOADER_GUI_ASSET_ROOT="${CMAKE_SOURCE_DIR}/src/gui/res"
    )
    fastloader_enable_torch(fastloader_gui_module PRIVATE)
    if(FASTLOADER_RFDETR_LIVE_CAPTURE_ENABLED)
        target_link_libraries(fastloader_gui_module PRIVATE fastloader_live_capture)
    endif()
    set_target_properties(fastloader_gui_module PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )

    add_executable(fastloader_gui
        src/gui/main.cpp
    )
    target_link_libraries(fastloader_gui PRIVATE fastloader_gui_module)
    fastloader_enable_torch(fastloader_gui PRIVATE)
    set_target_properties(fastloader_gui PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )
    fastloader_enable_native_runtime_rpath(fastloader_gui)

endif()

# ---- Tests ----
option(BUILD_TESTS "Build tests and profile runners" OFF)
if(BUILD_TESTS)
    enable_testing()

    add_executable(test_roundtrip
        tests/test_roundtrip.cpp
        tests/test_fixture.cpp
    )
    target_link_libraries(test_roundtrip PRIVATE fastloader_core fastloader_build_options)
    add_test(NAME roundtrip COMMAND test_roundtrip)

    add_executable(test_worker_pool
        tests/test_worker_pool.cpp
    )
    target_link_libraries(test_worker_pool PRIVATE fastloader_core fastloader_build_options)
    add_test(NAME worker_pool COMMAND test_worker_pool)

    add_executable(test_image_resize
        tests/test_image_resize.cpp
    )
    target_link_libraries(test_image_resize PRIVATE fastloader_core fastloader_build_options)
    target_include_directories(test_image_resize PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME image_resize COMMAND test_image_resize)

    add_executable(test_compile_progress
        tests/test_compile_progress.cpp
        tests/test_fixture.cpp
    )
    target_link_libraries(test_compile_progress PRIVATE fastloader_core fastloader_build_options)
    target_include_directories(test_compile_progress PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME compile_progress COMMAND test_compile_progress)

    add_executable(test_gui_train_command
        tests/test_gui_train_command.cpp
        src/gui/train_command.cpp
    )
    target_include_directories(test_gui_train_command PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(test_gui_train_command PRIVATE
        fastloader_build_options
        CUDA::cudart
    )
    fastloader_enable_torch(test_gui_train_command PRIVATE)
    add_test(NAME gui_train_command COMMAND test_gui_train_command)

    add_executable(test_gui_annotation_core
        tests/test_gui_annotation_core.cpp
        src/gui/annotation_core.cpp
    )
    target_include_directories(test_gui_annotation_core PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_gui_annotation_core PRIVATE
        fastloader_build_options
        fastloader_core
        nlohmann_json
    )
    fastloader_enable_torch(test_gui_annotation_core PRIVATE)
    add_test(NAME gui_annotation_core COMMAND test_gui_annotation_core)

    add_executable(test_gui_annotation_geometry
        tests/test_gui_annotation_geometry.cpp
        src/gui/annotation_core.cpp
        src/gui/annotation_geometry.cpp
    )
    target_include_directories(test_gui_annotation_geometry PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_gui_annotation_geometry PRIVATE
        fastloader_build_options
        fastloader_core
        nlohmann_json
    )
    fastloader_enable_torch(test_gui_annotation_geometry PRIVATE)
    add_test(NAME gui_annotation_geometry COMMAND test_gui_annotation_geometry)

    add_executable(test_gui_vast_runtime
        tests/test_gui_vast_runtime.cpp
        src/gui/vast_runtime.cpp
    )
    target_include_directories(test_gui_vast_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_gui_vast_runtime PRIVATE
        fastloader_build_options
        nlohmann_json
    )
    add_test(NAME gui_vast_runtime COMMAND test_gui_vast_runtime)

    add_executable(test_gui_app_options
        tests/test_gui_app_options.cpp
        src/gui/app_options.cpp
    )
    target_include_directories(test_gui_app_options PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_gui_app_options PRIVATE
        fastloader_build_options
        cli11
    )
    add_test(NAME gui_app_options COMMAND test_gui_app_options)

    add_executable(test_gui_settings
        tests/test_gui_settings.cpp
        src/gui/gui_settings.cpp
    )
    target_include_directories(test_gui_settings PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_gui_settings PRIVATE
        fastloader_build_options
        nlohmann_json
    )
    add_test(NAME gui_settings COMMAND test_gui_settings)

    add_executable(test_gui_canvas_layers
        tests/test_gui_canvas_layers.cpp
        src/gui/canvas_layers.cpp
    )
    target_include_directories(test_gui_canvas_layers PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(test_gui_canvas_layers PRIVATE
        fastloader_build_options
    )
    fastloader_enable_torch(test_gui_canvas_layers PRIVATE)
    add_test(NAME gui_canvas_layers COMMAND test_gui_canvas_layers)

    add_executable(test_async_runtime
        tests/test_async_runtime.cpp
    )
    target_link_libraries(test_async_runtime PRIVATE
        fastloader_build_options
        fastloader_runtime
    )
    add_test(NAME async_runtime COMMAND test_async_runtime)

    add_executable(test_model_registry
        tests/test_model_registry.cpp
    )
    target_link_libraries(test_model_registry PRIVATE
        fastloader_build_options
        fastloader_runtime
    )
    add_test(NAME model_registry COMMAND test_model_registry)

    add_executable(fastloader_profile_runner
        tests/profile_runner.cpp
        tests/test_fixture.cpp
    )
    target_link_libraries(fastloader_profile_runner PRIVATE fastloader_core fastloader_build_options)

    if(BUILD_RFDETR_NATIVE)
        add_executable(fastloader_rfdetr_profile_runner
            tests/rfdetr_profile_runner.cpp
        )
        target_link_libraries(fastloader_rfdetr_profile_runner PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        target_include_directories(fastloader_rfdetr_profile_runner PRIVATE ${CMAKE_SOURCE_DIR}/src)
        fastloader_enable_torch(fastloader_rfdetr_profile_runner PRIVATE)
        set_target_properties(fastloader_rfdetr_profile_runner PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        fastloader_enable_native_runtime_rpath(fastloader_rfdetr_profile_runner)

        add_executable(fastloader_rfdetr_train_profile_runner
            tests/rfdetr_train_profile_runner.cpp
            tests/test_fixture.cpp
        )
        target_link_libraries(fastloader_rfdetr_train_profile_runner PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        target_include_directories(fastloader_rfdetr_train_profile_runner PRIVATE ${CMAKE_SOURCE_DIR}/src)
        fastloader_enable_torch(fastloader_rfdetr_train_profile_runner PRIVATE)
        set_target_properties(fastloader_rfdetr_train_profile_runner PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        fastloader_enable_native_runtime_rpath(fastloader_rfdetr_train_profile_runner)

        add_executable(test_rfdetr_weight_catalog
            tests/test_rfdetr_weight_catalog.cpp
        )
        target_link_libraries(test_rfdetr_weight_catalog PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        target_include_directories(test_rfdetr_weight_catalog PRIVATE ${CMAKE_SOURCE_DIR}/src)
        fastloader_enable_torch(test_rfdetr_weight_catalog PRIVATE)
        set_target_properties(test_rfdetr_weight_catalog PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
        fastloader_enable_native_runtime_rpath(test_rfdetr_weight_catalog)
        add_test(NAME rfdetr_weight_catalog COMMAND test_rfdetr_weight_catalog)

        if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
            add_executable(test_rfdetr_checkpoint
                tests/test_rfdetr_checkpoint.cpp
            )
            target_link_libraries(test_rfdetr_checkpoint PRIVATE
                fastloader_core
                fastloader_build_options
                fastloader_rfdetr_native
            )
            target_include_directories(test_rfdetr_checkpoint PRIVATE ${CMAKE_SOURCE_DIR}/src)
            fastloader_enable_torch(test_rfdetr_checkpoint PRIVATE)
            set_target_properties(test_rfdetr_checkpoint PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
            fastloader_enable_native_runtime_rpath(test_rfdetr_checkpoint)
            add_test(NAME rfdetr_checkpoint COMMAND test_rfdetr_checkpoint)
        endif()

        add_executable(test_rfdetr_modules
            tests/test_rfdetr_modules.cpp
        )
        target_link_libraries(test_rfdetr_modules PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        target_include_directories(test_rfdetr_modules PRIVATE ${CMAKE_SOURCE_DIR}/src)
        fastloader_enable_torch(test_rfdetr_modules PRIVATE)
        set_target_properties(test_rfdetr_modules PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
        fastloader_enable_native_runtime_rpath(test_rfdetr_modules)
        add_test(NAME rfdetr_modules COMMAND test_rfdetr_modules)

        add_executable(test_rfdetr_native_ops
            tests/test_rfdetr_native_ops.cpp
        )
        target_link_libraries(test_rfdetr_native_ops PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        fastloader_enable_torch(test_rfdetr_native_ops PRIVATE)
        set_target_properties(test_rfdetr_native_ops PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
        fastloader_enable_native_runtime_rpath(test_rfdetr_native_ops)
        add_test(NAME rfdetr_native_ops COMMAND test_rfdetr_native_ops)

        add_executable(test_rfdetr_onnx_lowering
            tests/test_rfdetr_onnx_lowering.cpp
        )
        target_link_libraries(test_rfdetr_onnx_lowering PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        target_include_directories(test_rfdetr_onnx_lowering PRIVATE ${CMAKE_SOURCE_DIR}/src)
        fastloader_enable_torch(test_rfdetr_onnx_lowering PRIVATE)
        set_target_properties(test_rfdetr_onnx_lowering PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
        fastloader_enable_native_runtime_rpath(test_rfdetr_onnx_lowering)
        add_test(NAME rfdetr_onnx_lowering COMMAND test_rfdetr_onnx_lowering)

        add_executable(test_rfdetr_target_builder
            tests/test_rfdetr_target_builder.cpp
            tests/test_fixture.cpp
        )
        target_link_libraries(test_rfdetr_target_builder PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        fastloader_enable_torch(test_rfdetr_target_builder PRIVATE)
        set_target_properties(test_rfdetr_target_builder PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
        fastloader_enable_native_runtime_rpath(test_rfdetr_target_builder)
        add_test(NAME rfdetr_target_builder COMMAND test_rfdetr_target_builder)

        add_executable(test_rfdetr_model_trainability
            tests/test_rfdetr_model_trainability.cpp
        )
        target_link_libraries(test_rfdetr_model_trainability PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        target_include_directories(test_rfdetr_model_trainability PRIVATE ${CMAKE_SOURCE_DIR}/src)
        fastloader_enable_torch(test_rfdetr_model_trainability PRIVATE)
        set_target_properties(test_rfdetr_model_trainability PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
        fastloader_enable_native_runtime_rpath(test_rfdetr_model_trainability)
        add_test(NAME rfdetr_model_trainability COMMAND test_rfdetr_model_trainability)

        add_executable(test_rfdetr_native_optimizer
            tests/test_rfdetr_native_optimizer.cpp
        )
        target_link_libraries(test_rfdetr_native_optimizer PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        target_include_directories(test_rfdetr_native_optimizer PRIVATE ${CMAKE_SOURCE_DIR}/src)
        fastloader_enable_torch(test_rfdetr_native_optimizer PRIVATE)
        set_target_properties(test_rfdetr_native_optimizer PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
        fastloader_enable_native_runtime_rpath(test_rfdetr_native_optimizer)
        add_test(NAME rfdetr_native_optimizer COMMAND test_rfdetr_native_optimizer)

        add_executable(test_rfdetr_cli_aliases
            tests/test_rfdetr_cli_aliases.cpp
        )
        target_link_libraries(test_rfdetr_cli_aliases PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        target_include_directories(test_rfdetr_cli_aliases PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_compile_definitions(test_rfdetr_cli_aliases PRIVATE
            FASTLOADER_TEST_FASTLOADER_CLI_PATH="$<TARGET_FILE:fastloader_cli>"
        )
        add_dependencies(test_rfdetr_cli_aliases fastloader_cli)
        fastloader_enable_torch(test_rfdetr_cli_aliases PRIVATE)
        set_target_properties(test_rfdetr_cli_aliases PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        fastloader_enable_native_runtime_rpath(test_rfdetr_cli_aliases)
        add_test(NAME rfdetr_cli_aliases COMMAND test_rfdetr_cli_aliases)

        add_executable(test_rfdetr_eval_sample_writer
            tests/test_rfdetr_eval_sample_writer.cpp
        )
        target_link_libraries(test_rfdetr_eval_sample_writer PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        target_include_directories(test_rfdetr_eval_sample_writer PRIVATE ${CMAKE_SOURCE_DIR}/src)
        fastloader_enable_torch(test_rfdetr_eval_sample_writer PRIVATE)
        set_target_properties(test_rfdetr_eval_sample_writer PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        fastloader_enable_native_runtime_rpath(test_rfdetr_eval_sample_writer)
        add_test(NAME rfdetr_eval_sample_writer COMMAND test_rfdetr_eval_sample_writer)

        add_executable(test_rfdetr_public_headers
            tests/test_rfdetr_public_headers.cpp
        )
        target_link_libraries(test_rfdetr_public_headers PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        fastloader_enable_torch(test_rfdetr_public_headers PRIVATE)
        set_target_properties(test_rfdetr_public_headers PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        fastloader_enable_native_runtime_rpath(test_rfdetr_public_headers)
        add_test(NAME rfdetr_public_headers COMMAND test_rfdetr_public_headers)

        add_executable(test_rfdetr_live_preprocess
            tests/test_rfdetr_live_preprocess.cpp
            src/rfdetr/cuda/live_preprocess.cu
        )
        target_include_directories(test_rfdetr_live_preprocess PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/include
        )
        target_link_libraries(test_rfdetr_live_preprocess PRIVATE
            fastloader_build_options
            CUDA::cudart
        )
        set_target_properties(test_rfdetr_live_preprocess PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        add_test(NAME rfdetr_live_preprocess COMMAND test_rfdetr_live_preprocess)

        add_executable(test_rfdetr_prediction_json
            tests/test_rfdetr_prediction_json.cpp
        )
        target_link_libraries(test_rfdetr_prediction_json PRIVATE
            fastloader_core
            fastloader_build_options
            fastloader_rfdetr_native
        )
        fastloader_enable_torch(test_rfdetr_prediction_json PRIVATE)
        set_target_properties(test_rfdetr_prediction_json PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        fastloader_enable_native_runtime_rpath(test_rfdetr_prediction_json)
        add_test(NAME rfdetr_prediction_json COMMAND test_rfdetr_prediction_json)

        if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
            add_executable(test_rfdetr_native_integration
                tests/test_rfdetr_native_integration.cpp
                tests/test_fixture.cpp
            )
            target_link_libraries(test_rfdetr_native_integration PRIVATE
                fastloader_core
                fastloader_build_options
                fastloader_rfdetr_native
            )
            target_include_directories(test_rfdetr_native_integration PRIVATE ${CMAKE_SOURCE_DIR}/src)
            target_compile_definitions(test_rfdetr_native_integration PRIVATE
                FASTLOADER_TEST_FASTLOADER_CLI_PATH="$<TARGET_FILE:fastloader_cli>"
            )
            add_dependencies(test_rfdetr_native_integration fastloader_cli)
            fastloader_enable_torch(test_rfdetr_native_integration PRIVATE)
            set_target_properties(test_rfdetr_native_integration PROPERTIES
                CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
            )
            fastloader_enable_native_runtime_rpath(test_rfdetr_native_integration)
            add_test(NAME rfdetr_native_integration COMMAND test_rfdetr_native_integration)
        endif()

        if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
            add_executable(test_rfdetr_checkpoint_parity
                tests/test_rfdetr_checkpoint_parity.cpp
            )
            target_link_libraries(test_rfdetr_checkpoint_parity PRIVATE
                fastloader_core
                fastloader_build_options
                fastloader_rfdetr_native
            )
            target_include_directories(test_rfdetr_checkpoint_parity PRIVATE ${CMAKE_SOURCE_DIR}/src)
            fastloader_enable_torch(test_rfdetr_checkpoint_parity PRIVATE)
            set_target_properties(test_rfdetr_checkpoint_parity PROPERTIES
                CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
            )
            fastloader_enable_native_runtime_rpath(test_rfdetr_checkpoint_parity)
            add_test(NAME rfdetr_checkpoint_parity_nano COMMAND test_rfdetr_checkpoint_parity rf-detr-nano)
            add_test(NAME rfdetr_checkpoint_parity_seg_nano COMMAND test_rfdetr_checkpoint_parity rf-detr-seg-nano)
        endif()
    endif()
endif()
