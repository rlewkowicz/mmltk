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

set(FASTLOADER_COMMON_MACHINE_FLAGS
    "-march=native -mtune=native -fno-semantic-interposition -ffunction-sections -fdata-sections")
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
    src/dataset/loader/dataset_loader.cpp
    src/dataset/loader/dataset_loader_runtime.cpp
    src/dataset/loader/dataset_loader_pipeline.cpp
    src/cpu_affinity.cpp
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
)

# ---- CLI tool ----
add_executable(fastloader_cli src/cli.cpp)
target_link_libraries(fastloader_cli PRIVATE fastloader_core fastloader_build_options)
target_include_directories(fastloader_cli PRIVATE ${CMAKE_SOURCE_DIR}/src)

option(BUILD_RFDETR_NATIVE "Build native RF-DETR validation backends" ON)
option(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER
    "Enable embedded Python parsing of upstream RF-DETR .pth checkpoints"
    ${BUILD_RFDETR_NATIVE})

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

    add_library(fastloader_rfdetr_cuda_ops STATIC
        src/rfdetr/layers/ms_deform_attn_op.cpp
        src/rfdetr/layers/ms_deform_attn_cuda_wrapper.cpp
        src/rfdetr/layers/training_mask_ops_cuda_wrapper.cpp
        src/rfdetr/cuda/ms_deform_attn_cuda.cu
        src/rfdetr/cuda/training_mask_ops_cuda.cu
        src/rfdetr/cuda/detr_ops_cuda.cu
        src/rfdetr/cuda/draw_cuda.cu
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

    add_library(fastloader_rfdetr_native STATIC
        src/rfdetr/inference/backend_onnx.cpp
        src/rfdetr/inference/backend_tensorrt.cpp
        src/rfdetr/checkpoint.cpp
        src/rfdetr/cli/cli.cpp
        src/rfdetr/detection_ops.cpp
        src/rfdetr/inference/evaluator.cpp
        src/rfdetr/layers/model.cpp
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
        PRIVATE
            ${ONNXRUNTIME_LIBRARY}
            ${TENSORRT_NVINFER_LIBRARY}
            ${TENSORRT_NVONNXPARSER_LIBRARY}
    )
    if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
        fastloader_enable_python_checkpoint_loader(fastloader_rfdetr_native PRIVATE)
    endif()
    target_compile_definitions(fastloader_rfdetr_native PUBLIC
        FASTLOADER_RFDETR_PYTHON_CHECKPOINT_LOADER=$<IF:$<BOOL:${BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER}>,1,0>
    )
    set_target_properties(fastloader_rfdetr_native PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )
    target_compile_definitions(fastloader_cli PRIVATE FASTLOADER_BUILD_RFDETR_NATIVE=1)
    target_link_libraries(fastloader_cli PRIVATE fastloader_rfdetr_native)
    set_target_properties(fastloader_cli PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )
    fastloader_enable_native_runtime_rpath(fastloader_cli)
else()
    target_compile_definitions(fastloader_cli PRIVATE FASTLOADER_BUILD_RFDETR_NATIVE=0)
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
