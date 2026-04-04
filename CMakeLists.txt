cmake_minimum_required(VERSION 3.18)
if(POLICY CMP0138)
    cmake_policy(SET CMP0138 NEW)
endif()
project(mmltk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
set(CMAKE_INSTALL_BINDIR "bin" CACHE STRING "Install subdirectory for executables" FORCE)
set(CMAKE_INSTALL_LIBDIR "lib" CACHE STRING "Install subdirectory for libraries" FORCE)
set(CMAKE_INSTALL_DATADIR "share" CACHE STRING "Install subdirectory for shared data" FORCE)

set(MMLTK_CUDA_VERSION "13.0" CACHE STRING "Pinned CUDA toolkit version for mmltk builds")
set(MMLTK_CUDA_TOOLKIT_ROOT "/usr/local/cuda-${MMLTK_CUDA_VERSION}" CACHE PATH
    "Pinned CUDA toolkit root for mmltk builds")
set(MMLTK_DEFAULT_CUDA_ARCHITECTURES "86;87;89;90;100;103;110;120;121")
set(MMLTK_CUDA_ARCHITECTURES "${MMLTK_DEFAULT_CUDA_ARCHITECTURES}" CACHE STRING
    "CUDA architectures for mmltk CUDA targets")
set(MMLTK_INSTALL_TORCH_ROOT "/opt/pytorch" CACHE PATH
    "Runtime Torch root used for installed mmltk RPATHs")
set(MMLTK_ONNXRUNTIME_ROOT "/opt/onnxruntime" CACHE PATH
    "ONNX Runtime installation prefix containing include/ and lib/")
set(MMLTK_INSTALL_PYTHONDIR "${CMAKE_INSTALL_DATADIR}/mmltk/python" CACHE STRING
    "Install-relative directory for Python bridge scripts")
set(MMLTK_INSTALL_FONTDIR "${CMAKE_INSTALL_DATADIR}/mmltk/fonts" CACHE STRING
    "Install-relative directory for GUI font assets")
get_filename_component(MMLTK_CUDA_TOOLKIT_ROOT "${MMLTK_CUDA_TOOLKIT_ROOT}" REALPATH)
set(MMLTK_CUDA_NVCC "${MMLTK_CUDA_TOOLKIT_ROOT}/bin/nvcc")
if(NOT EXISTS "${MMLTK_CUDA_NVCC}")
    message(FATAL_ERROR
        "mmltk requires CUDA ${MMLTK_CUDA_VERSION}.x at ${MMLTK_CUDA_TOOLKIT_ROOT}. "
        "Install the toolkit there or override MMLTK_CUDA_TOOLKIT_ROOT.")
endif()

# Force both FindCUDAToolkit and enable_language(CUDA) to resolve the same toolkit
# even when the shell PATH still exposes an older nvcc.
set(CUDAToolkit_ROOT "${MMLTK_CUDA_TOOLKIT_ROOT}" CACHE PATH
    "Pinned CUDA toolkit root for mmltk builds" FORCE)
set(CMAKE_CUDA_COMPILER "${MMLTK_CUDA_NVCC}" CACHE FILEPATH
    "Pinned CUDA compiler for mmltk builds" FORCE)
set(CUDA_TOOLKIT_ROOT_DIR "${MMLTK_CUDA_TOOLKIT_ROOT}" CACHE PATH
    "Pinned legacy CUDA toolkit root for Torch/Caffe2 detection" FORCE)
set(CUDA_BIN_PATH "${MMLTK_CUDA_TOOLKIT_ROOT}" CACHE PATH
    "Pinned legacy CUDA bin path for Torch/Caffe2 detection" FORCE)
set(CUDA_NVCC_EXECUTABLE "${MMLTK_CUDA_NVCC}" CACHE FILEPATH
    "Pinned legacy CUDA compiler for Torch/Caffe2 detection" FORCE)
set(ENV{CUDA_HOME} "${MMLTK_CUDA_TOOLKIT_ROOT}")
set(ENV{CUDA_BIN_PATH} "${MMLTK_CUDA_TOOLKIT_ROOT}")
set(ENV{CUDACXX} "${MMLTK_CUDA_NVCC}")
set(ENV{PATH} "${MMLTK_CUDA_TOOLKIT_ROOT}/bin:$ENV{PATH}")

set(MMLTK_SUPPORTED_BUILD_TYPES Dev Release)
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${MMLTK_SUPPORTED_BUILD_TYPES}" CACHE STRING
        "Supported mmltk build types" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${MMLTK_SUPPORTED_BUILD_TYPES})
    list(FIND MMLTK_SUPPORTED_BUILD_TYPES "${CMAKE_BUILD_TYPE}" MMLTK_BUILD_TYPE_INDEX)
    if(MMLTK_BUILD_TYPE_INDEX EQUAL -1)
        string(JOIN ", " MMLTK_SUPPORTED_BUILD_TYPES_TEXT ${MMLTK_SUPPORTED_BUILD_TYPES})
        message(FATAL_ERROR
            "mmltk supports only these build types: ${MMLTK_SUPPORTED_BUILD_TYPES_TEXT}")
    endif()
endif()

option(MMLTK_DEV_STRICT_WARNINGS
    "Enable aggressive host-side warnings for the Dev build"
    ON)
option(MMLTK_DEV_ENABLE_ASAN
    "Enable AddressSanitizer for host-side Dev C++ builds"
    OFF)
option(MMLTK_DEV_ENABLE_UBSAN
    "Enable UndefinedBehaviorSanitizer for host-side Dev C++ builds"
    OFF)

set(MMLTK_X86_SIMD_FLAGS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|amd64|AMD64)$")
    set(MMLTK_X86_SIMD_FLAGS "-mssse3 -msse4.2 -mavx2 -mfma")
endif()
set(MMLTK_COMMON_MACHINE_FLAGS
    "-march=native -mtune=native ${MMLTK_X86_SIMD_FLAGS} -fno-semantic-interposition -ffunction-sections -fdata-sections")
set(MMLTK_COMMON_WARNING_FLAGS "-Wall -Wextra -Wpedantic")
set(MMLTK_STRICT_WARNING_FLAGS "")
if(MMLTK_DEV_STRICT_WARNINGS)
    set(MMLTK_STRICT_WARNING_FLAGS
        "-Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual -Wconversion -Wformat=2 -Wnull-dereference -Wimplicit-fallthrough")
endif()
set(MMLTK_DEV_SANITIZER_FLAGS "")
if(MMLTK_DEV_ENABLE_ASAN)
    string(APPEND MMLTK_DEV_SANITIZER_FLAGS " -fsanitize=address")
endif()
if(MMLTK_DEV_ENABLE_UBSAN)
    string(APPEND MMLTK_DEV_SANITIZER_FLAGS " -fsanitize=undefined")
endif()
if(NOT MMLTK_DEV_SANITIZER_FLAGS STREQUAL "")
    string(APPEND MMLTK_DEV_SANITIZER_FLAGS " -fno-sanitize-recover=all")
endif()
set(MMLTK_RELEASE_FLAGS
    "-O3 -DNDEBUG -fomit-frame-pointer -fno-plt ${MMLTK_COMMON_MACHINE_FLAGS}")
set(MMLTK_DEV_FLAGS
    "-O2 -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls -D_GLIBCXX_ASSERTIONS -D_FORTIFY_SOURCE=3 ${MMLTK_COMMON_MACHINE_FLAGS} ${MMLTK_COMMON_WARNING_FLAGS} ${MMLTK_STRICT_WARNING_FLAGS} ${MMLTK_DEV_SANITIZER_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "${MMLTK_RELEASE_FLAGS}")
set(CMAKE_CXX_FLAGS_DEV "${MMLTK_DEV_FLAGS}")

set(CMAKE_CUDA_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CUDA_FLAGS_DEV "-O2 -g -lineinfo -Xcompiler=-fno-omit-frame-pointer")

set(MMLTK_COMMON_LINK_FLAGS "-Wl,-O2 -Wl,--as-needed")
set(MMLTK_RELEASE_LINK_FLAGS "${MMLTK_COMMON_LINK_FLAGS} -Wl,--gc-sections")
set(MMLTK_DEV_LINK_FLAGS
    "${MMLTK_COMMON_LINK_FLAGS} -Wl,--gc-sections ${MMLTK_DEV_SANITIZER_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${MMLTK_RELEASE_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${MMLTK_RELEASE_LINK_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_DEV "${MMLTK_DEV_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_DEV "${MMLTK_DEV_LINK_FLAGS}")

include(CheckIPOSupported)
check_ipo_supported(RESULT MMLTK_IPO_SUPPORTED OUTPUT MMLTK_IPO_OUTPUT LANGUAGES CXX)
if(MMLTK_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(STATUS "IPO disabled: ${MMLTK_IPO_OUTPUT}")
endif()

find_package(CUDAToolkit REQUIRED)
if(NOT "${CUDAToolkit_VERSION}" MATCHES "^${MMLTK_CUDA_VERSION}(\\.|$)")
    message(FATAL_ERROR
        "Expected CUDA ${MMLTK_CUDA_VERSION}.x from ${MMLTK_CUDA_TOOLKIT_ROOT}, "
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

set(MMLTK_AVIR_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/avir")
if(NOT EXISTS "${MMLTK_AVIR_SOURCE_DIR}/avir.h")
    message(FATAL_ERROR
        "Missing vendored AVIR checkout at ${MMLTK_AVIR_SOURCE_DIR}. "
        "CPU image resize is implemented via AVIR only; there is no fallback.")
endif()

add_library(avir INTERFACE)
target_include_directories(avir SYSTEM INTERFACE ${MMLTK_AVIR_SOURCE_DIR})

add_library(mmltk_build_options INTERFACE)
target_compile_definitions(mmltk_build_options INTERFACE
    MMLTK_ENABLE_PROFILING=$<IF:$<CONFIG:Dev>,1,0>
    MMLTK_BUILD_CONFIG=\"$<CONFIG>\"
)

add_library(spdlog STATIC
    third_party/spdlog/src/async.cpp
    third_party/spdlog/src/cfg.cpp
    third_party/spdlog/src/color_sinks.cpp
    third_party/spdlog/src/file_sinks.cpp
    third_party/spdlog/src/spdlog.cpp
    third_party/spdlog/src/stdout_sinks.cpp
)
target_include_directories(spdlog PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_link_libraries(spdlog PUBLIC Threads::Threads)
target_compile_definitions(spdlog PUBLIC
    SPDLOG_COMPILED_LIB
    SPDLOG_USE_STD_FORMAT
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)
add_library(spdlog::spdlog ALIAS spdlog)

add_library(mmltk_logging SHARED
    src/mmltk_logging.cpp
)
target_include_directories(mmltk_logging PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_logging PUBLIC
    mmltk_build_options
    spdlog
)

# ---- Core library ----
add_library(mmltk_core SHARED
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
target_include_directories(mmltk_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_core PUBLIC
    CUDA::cudart
    Threads::Threads
    stb
    nlohmann_json
    mmltk_build_options
    mmltk_logging
PRIVATE
    avir
)

add_library(mmltk_model_api STATIC
    src/model/module.cpp
)
target_include_directories(mmltk_model_api PUBLIC
    ${CMAKE_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_model_api PUBLIC
    mmltk_core
    mmltk_build_options
)

add_library(mmltk_runtime STATIC
    src/runtime/async_runtime.cpp
)
target_include_directories(mmltk_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_runtime PUBLIC
    mmltk_core
    mmltk_build_options
    mmltk_model_api
)

# ---- CLI tool ----
add_executable(mmltk_cli
    src/cli.cpp
    src/cli_bundled_tests.cpp
)
target_link_libraries(mmltk_cli PRIVATE mmltk_core mmltk_build_options cli11 mmltk_logging)
target_include_directories(mmltk_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
)

option(BUILD_RFDETR_NATIVE "Build native RF-DETR validation backends" ON)
option(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER
    "Enable embedded Python parsing of upstream RF-DETR .pth checkpoints"
    ${BUILD_RFDETR_NATIVE})
option(BUILD_MMLTK_GUI "Build the standalone ImGui RF-DETR desktop GUI" OFF)
set(MMLTK_IMGUI_SOURCE_DIR "" CACHE PATH
    "Optional local Dear ImGui checkout for BUILD_MMLTK_GUI")
target_compile_definitions(mmltk_core PRIVATE
    MMLTK_DATASET_COMPILER_CUDA=$<IF:$<BOOL:${BUILD_RFDETR_NATIVE}>,1,0>
)

add_library(mmltk_rfdetr_lsap STATIC
    src/rfdetr/utils/scipy_rectangular_lsap.cpp
)
target_include_directories(mmltk_rfdetr_lsap
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_rfdetr_lsap
    PUBLIC
        mmltk_build_options
)

add_library(mmltk_rfdetr_core STATIC
    src/rfdetr/core/module.cpp
    src/rfdetr/layers/model_config.cpp
    src/rfdetr/weight_catalog.cpp
)
target_include_directories(mmltk_rfdetr_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_rfdetr_core
    PUBLIC
        mmltk_build_options
        mmltk_model_api
)
set_target_properties(mmltk_rfdetr_core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

set(MMLTK_NEEDS_TORCH OFF)
if(BUILD_RFDETR_NATIVE)
    set(MMLTK_NEEDS_TORCH ON)
endif()

set(MMLTK_NEEDS_PYTHON OFF)
if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
    if(NOT BUILD_RFDETR_NATIVE)
        message(FATAL_ERROR
            "BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER requires BUILD_RFDETR_NATIVE=ON.")
    endif()
    set(MMLTK_NEEDS_PYTHON ON)
endif()

set(MMLTK_TORCH_ROOT "" CACHE PATH
    "Torch installation prefix containing share/cmake/Torch")
set(MMLTK_ONNX_ROOT "" CACHE PATH
    "ONNX installation prefix containing lib/cmake/ONNX")
set(MMLTK_DEFAULT_TORCH_CUDA_ARCH_LIST
    "8.6 8.7 8.9 9.0 10.0 10.3 11.0 12.0 12.1")
set(MMLTK_TORCH_CUDA_ARCH_LIST "${MMLTK_DEFAULT_TORCH_CUDA_ARCH_LIST}" CACHE STRING
    "Default TORCH_CUDA_ARCH_LIST for LibTorch/Caffe2 consumers")

if(MMLTK_NEEDS_TORCH)
    if(MMLTK_TORCH_CUDA_ARCH_LIST)
        set(TORCH_CUDA_ARCH_LIST "${MMLTK_TORCH_CUDA_ARCH_LIST}" CACHE STRING
            "Pinned CUDA architectures for LibTorch/Caffe2 consumers" FORCE)
    endif()
    set(_mmltk_nvrtc_library "${MMLTK_CUDA_TOOLKIT_ROOT}/lib64/libnvrtc.so")
    if(EXISTS "${_mmltk_nvrtc_library}")
        file(SHA256 "${_mmltk_nvrtc_library}" _mmltk_nvrtc_hash)
        string(SUBSTRING "${_mmltk_nvrtc_hash}" 0 8 CUDA_NVRTC_SHORTHASH)
        set(CUDA_NVRTC_SHORTHASH "${CUDA_NVRTC_SHORTHASH}" CACHE STRING
            "Pinned nvrtc shorthash for LibTorch/Caffe2 detection" FORCE)
    endif()
    if(MMLTK_TORCH_ROOT)
        list(PREPEND CMAKE_PREFIX_PATH "${MMLTK_TORCH_ROOT}")
    endif()
    set(_mmltk_saved_cuda_architectures "")
    if(DEFINED CMAKE_CUDA_ARCHITECTURES)
        if(NOT CMAKE_CUDA_ARCHITECTURES STREQUAL "")
            set(_mmltk_saved_cuda_architectures "${CMAKE_CUDA_ARCHITECTURES}")
        endif()
        unset(CMAKE_CUDA_ARCHITECTURES CACHE)
        unset(CMAKE_CUDA_ARCHITECTURES)
    endif()
    find_package(Torch REQUIRED)
    if(NOT _mmltk_saved_cuda_architectures STREQUAL "")
        set(CMAKE_CUDA_ARCHITECTURES "${_mmltk_saved_cuda_architectures}" CACHE STRING
            "CUDA architectures" FORCE)
    endif()

    if(DEFINED TORCH_INSTALL_PREFIX AND NOT TORCH_INSTALL_PREFIX STREQUAL "")
        get_filename_component(TORCH_LIBRARY_DIR "${TORCH_INSTALL_PREFIX}/lib" ABSOLUTE)
    elseif(MMLTK_TORCH_ROOT)
        get_filename_component(TORCH_LIBRARY_DIR "${MMLTK_TORCH_ROOT}/lib" ABSOLUTE)
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
            "Set MMLTK_TORCH_ROOT or Torch_DIR to your LibTorch install.")
    endif()
    get_filename_component(MMLTK_TORCH_LIBRARY_DIR_REAL "${TORCH_LIBRARY_DIR}" REALPATH)
    if(MMLTK_TORCH_ROOT)
        get_filename_component(MMLTK_TORCH_ROOT_REAL "${MMLTK_TORCH_ROOT}" REALPATH)
    else()
        set(MMLTK_TORCH_ROOT_REAL "")
    endif()
    separate_arguments(TORCH_CXX_FLAGS_LIST NATIVE_COMMAND "${TORCH_CXX_FLAGS}")
    set(MMLTK_TORCH_EXTRA_LIBRARIES "")
    unset(MMLTK_NCCL_LIBRARY CACHE)
    unset(MMLTK_NCCL_LIBRARY)
    find_file(MMLTK_NCCL_LIBRARY
        NAMES libnccl.so.2
        HINTS
            "${MMLTK_TORCH_LIBRARY_DIR_REAL}/../../nvidia/nccl/lib"
            "${MMLTK_TORCH_LIBRARY_DIR_REAL}/../nvidia/nccl/lib"
            "${MMLTK_TORCH_ROOT_REAL}/../../nvidia/nccl/lib"
            "${MMLTK_TORCH_ROOT_REAL}/../nvidia/nccl/lib"
        NO_DEFAULT_PATH)
    if(NOT MMLTK_NCCL_LIBRARY)
        find_library(MMLTK_NCCL_LIBRARY
            NAMES nccl libnccl.so.2
            HINTS
                /usr/lib/x86_64-linux-gnu
                /usr/local/lib
                /usr/local/lib64
                "${MMLTK_CUDA_TOOLKIT_ROOT}/lib64"
                "${MMLTK_CUDA_TOOLKIT_ROOT}/targets/x86_64-linux/lib")
    endif()
    if(MMLTK_NCCL_LIBRARY)
        list(APPEND MMLTK_TORCH_EXTRA_LIBRARIES "${MMLTK_NCCL_LIBRARY}")
    elseif(EXISTS "${TORCH_LIBRARY_DIR}/libtorch_cuda.so")
        message(FATAL_ERROR
            "Torch resolved libtorch_cuda.so from ${TORCH_LIBRARY_DIR}, but libnccl.so was not found. "
            "Install NCCL runtime libraries or expose them on the library search path before configuring.")
    endif()
    target_compile_options(mmltk_core PUBLIC ${TORCH_CXX_FLAGS_LIST})
    target_compile_options(mmltk_cli PRIVATE ${TORCH_CXX_FLAGS_LIST})
endif()

function(mmltk_enable_torch target visibility)
    if(NOT MMLTK_NEEDS_TORCH)
        message(FATAL_ERROR "Torch support requested for ${target} but Torch is not enabled")
    endif()
    target_include_directories(${target} SYSTEM ${visibility} ${TORCH_INCLUDE_DIRS})
    target_compile_options(${target} ${visibility} ${TORCH_CXX_FLAGS_LIST})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} ${visibility}
            $<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:CXX>>:-Wno-null-dereference>)
    endif()
    target_link_libraries(${target} ${visibility} ${TORCH_LIBRARIES})
    if(MMLTK_TORCH_EXTRA_LIBRARIES)
        target_link_libraries(${target} ${visibility} ${MMLTK_TORCH_EXTRA_LIBRARIES})
    endif()
endfunction()

function(mmltk_prefer_system_protobuf_headers target visibility)
    # ONNX-generated protobuf sources must see the system protobuf headers
    # before LibTorch's bundled protobuf copy.
    if(DEFINED MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR AND
       NOT MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR STREQUAL "")
        target_compile_options(${target} BEFORE ${visibility}
            "-I${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}")
    elseif(Protobuf_INCLUDE_DIRS)
        foreach(_mmltk_protobuf_include_dir IN LISTS Protobuf_INCLUDE_DIRS)
            target_compile_options(${target} BEFORE ${visibility} "-I${_mmltk_protobuf_include_dir}")
        endforeach()
    endif()
endfunction()

function(mmltk_enable_python_checkpoint_loader target visibility)
    if(NOT MMLTK_NEEDS_PYTHON)
        message(FATAL_ERROR "Python checkpoint loader requested for ${target} but it is not enabled")
    endif()
    target_compile_definitions(${target} ${visibility}
        MMLTK_RFDETR_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
        MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE="${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}"
    )
endfunction()

function(mmltk_enable_native_runtime_rpath target)
    set(_mmltk_build_rpath "")
    foreach(_mmltk_system_libdir IN ITEMS
            /lib64
            /usr/lib64
            /lib/x86_64-linux-gnu
            /usr/lib/x86_64-linux-gnu)
        if(EXISTS "${_mmltk_system_libdir}")
            list(APPEND _mmltk_build_rpath "${_mmltk_system_libdir}")
        endif()
    endforeach()
    if(DEFINED TORCH_LIBRARY_DIR AND NOT TORCH_LIBRARY_DIR STREQUAL "")
        list(APPEND _mmltk_build_rpath "${TORCH_LIBRARY_DIR}")
    endif()
    if(DEFINED ONNXRUNTIME_LIBRARY_DIR AND NOT ONNXRUNTIME_LIBRARY_DIR STREQUAL "")
        list(APPEND _mmltk_build_rpath "${ONNXRUNTIME_LIBRARY_DIR}")
    endif()
    if(DEFINED MMLTK_ONNX_LIBRARY_DIR AND NOT MMLTK_ONNX_LIBRARY_DIR STREQUAL "")
        list(APPEND _mmltk_build_rpath "${MMLTK_ONNX_LIBRARY_DIR}")
    endif()
    if(_mmltk_build_rpath)
        list(JOIN _mmltk_build_rpath ";" _mmltk_build_rpath_value)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "${_mmltk_build_rpath_value}")
    endif()
    set(_mmltk_install_rpath
        "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
        "${MMLTK_INSTALL_TORCH_ROOT}/lib"
        "${MMLTK_CUDA_TOOLKIT_ROOT}/lib64"
    )
    list(JOIN _mmltk_install_rpath ";" _mmltk_install_rpath_value)
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "${_mmltk_install_rpath_value}")
endfunction()

mmltk_enable_native_runtime_rpath(mmltk_core)
mmltk_enable_native_runtime_rpath(mmltk_logging)

if(MMLTK_NEEDS_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE
        "${CMAKE_SOURCE_DIR}/utilities/rfdetr_checkpoint_bridge.py")
    if(NOT EXISTS "${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}")
        message(FATAL_ERROR
            "Missing RF-DETR Python checkpoint bridge: ${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}")
    endif()
endif()

if(BUILD_RFDETR_NATIVE)
    find_path(MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR
        NAMES google/protobuf/message.h
        HINTS
            /usr/include
            /usr/local/include
        NO_DEFAULT_PATH)
    find_library(MMLTK_SYSTEM_PROTOBUF_LIBRARY
        NAMES protobuf libprotobuf
        HINTS
            /usr/lib/x86_64-linux-gnu
            /usr/local/lib
            /usr/lib64
            /usr/lib
        NO_DEFAULT_PATH)
    if(MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR)
        set(MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR
            "${CMAKE_BINARY_DIR}/mmltk_system_protobuf")
        file(MAKE_DIRECTORY "${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}")
        file(REMOVE_RECURSE "${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}/google")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "${MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR}/google"
                "${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}/google"
            RESULT_VARIABLE MMLTK_SYSTEM_PROTOBUF_SYMLINK_RESULT)
        if(NOT MMLTK_SYSTEM_PROTOBUF_SYMLINK_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Failed to create system protobuf include redirect under "
                "${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}")
        endif()
        set(Protobuf_INCLUDE_DIR "${MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR}" CACHE PATH
            "Pinned system protobuf headers for ONNX/native builds" FORCE)
    endif()
    if(MMLTK_SYSTEM_PROTOBUF_LIBRARY)
        set(Protobuf_LIBRARY "${MMLTK_SYSTEM_PROTOBUF_LIBRARY}" CACHE FILEPATH
            "Pinned system protobuf library for ONNX/native builds" FORCE)
    endif()
    set(Protobuf_PROTOC_EXECUTABLE "/usr/bin/protoc" CACHE FILEPATH
        "Pinned protoc executable for ONNX/native builds" FORCE)
    find_package(Protobuf REQUIRED)
    if(MMLTK_SYSTEM_PROTOBUF_LIBRARY)
        set(MMLTK_PROTOBUF_LIBRARY_TARGET "${MMLTK_SYSTEM_PROTOBUF_LIBRARY}")
    elseif(Protobuf_LIBRARY)
        set(MMLTK_PROTOBUF_LIBRARY_TARGET "${Protobuf_LIBRARY}")
    elseif(Protobuf_LIBRARIES)
        set(MMLTK_PROTOBUF_LIBRARY_TARGET ${Protobuf_LIBRARIES})
    elseif(TARGET protobuf::libprotobuf)
        set(MMLTK_PROTOBUF_LIBRARY_TARGET protobuf::libprotobuf)
    else()
        message(FATAL_ERROR "Failed to resolve a protobuf library for ONNX/native builds.")
    endif()
    if(NOT MMLTK_ONNX_ROOT)
        message(FATAL_ERROR
            "BUILD_RFDETR_NATIVE requires MMLTK_ONNX_ROOT to point at a standalone ONNX install prefix.")
    endif()
    if(NOT EXISTS "${MMLTK_ONNX_ROOT}/lib/cmake/ONNX/ONNXConfig.cmake")
        message(FATAL_ERROR
            "Missing ONNX package config under ${MMLTK_ONNX_ROOT}/lib/cmake/ONNX. "
            "Set MMLTK_ONNX_ROOT to a valid ONNX install prefix before building.")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${MMLTK_ONNX_ROOT}")
    find_package(ONNX CONFIG REQUIRED)
    set(MMLTK_ONNX_LIBRARY_DIR "")
    foreach(_mmltk_onnx_location_property IN ITEMS
            IMPORTED_LOCATION_RELEASE
            IMPORTED_LOCATION_RELWITHDEBINFO
            IMPORTED_LOCATION_DEBUG
            IMPORTED_LOCATION)
        get_target_property(_mmltk_onnx_imported_location ONNX::onnx ${_mmltk_onnx_location_property})
        if(_mmltk_onnx_imported_location AND EXISTS "${_mmltk_onnx_imported_location}")
            get_filename_component(MMLTK_ONNX_LIBRARY_DIR "${_mmltk_onnx_imported_location}" DIRECTORY)
            break()
        endif()
    endforeach()

    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        set(_mmltk_cuda_architectures "${MMLTK_CUDA_ARCHITECTURES}")
        if(_mmltk_cuda_architectures STREQUAL "")
            set(_mmltk_cuda_architectures "${MMLTK_DEFAULT_CUDA_ARCHITECTURES}")
        endif()
        set(CMAKE_CUDA_ARCHITECTURES "${_mmltk_cuda_architectures}" CACHE STRING
            "CUDA architectures" FORCE)
    endif()
    enable_language(CUDA)
    target_sources(mmltk_core PRIVATE
        src/dataset/compile/dataset_compiler_cuda.cu
    )
    set_target_properties(mmltk_core PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        HINTS
            "${MMLTK_ONNXRUNTIME_ROOT}/include"
            /opt/onnxruntime/include
            /usr/local/include
            /usr/include)
    find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        HINTS
            "${MMLTK_ONNXRUNTIME_ROOT}/lib"
            /opt/onnxruntime/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu)
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR
            "ONNX Runtime C++ GPU libraries were not found. Install the official onnxruntime GPU package "
            "and set MMLTK_ONNXRUNTIME_ROOT before building.")
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
        message(FATAL_ERROR
            "TensorRT shared libraries and headers were not found. "
            "Install the TensorRT developer packages or expose the include/lib roots before building.")
    endif()

    set(MMLTK_RFDETR_LIVE_CAPTURE_ENABLED ON)

    add_library(mmltk_rfdetr_cuda_ops STATIC
        src/rfdetr/layers/ms_deform_attn_op.cpp
        src/rfdetr/layers/ms_deform_attn_cuda_wrapper.cpp
        src/rfdetr/layers/training_mask_ops_cuda_wrapper.cpp
        src/rfdetr/cuda/ms_deform_attn_cuda.cu
        src/rfdetr/cuda/training_mask_ops_cuda.cu
        src/rfdetr/cuda/detr_ops_cuda.cu
        src/rfdetr/cuda/draw_cuda.cu
        src/rfdetr/cuda/cuda_utils.cu
    )
    file(MAKE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/mmltk_rfdetr_cuda_ops.dir/src/rfdetr"
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/mmltk_rfdetr_cuda_ops.dir/src/rfdetr/cuda"
    )
    target_include_directories(mmltk_rfdetr_cuda_ops
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_include_directories(mmltk_rfdetr_cuda_ops
        SYSTEM PRIVATE
            ${TORCH_INCLUDE_DIRS}
    )
    target_link_libraries(mmltk_rfdetr_cuda_ops
        PUBLIC
            mmltk_build_options
            CUDA::cudart
        PRIVATE
            ${TORCH_LIBRARIES}
    )
    target_compile_options(mmltk_rfdetr_cuda_ops PRIVATE ${TORCH_CXX_FLAGS_LIST})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(mmltk_rfdetr_cuda_ops PRIVATE
            $<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:CXX>>:-Wno-null-dereference>)
    endif()
    set_target_properties(mmltk_rfdetr_cuda_ops PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
    )

    add_library(mmltk_live_capture STATIC
        src/frameshow/capture_session_core.cpp
        src/frameshow/capture_session_device.cpp
        src/frameshow/capture_session_internal.cpp
        src/frameshow/capture_session_preview.cpp
    )
    target_include_directories(mmltk_live_capture
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(mmltk_live_capture
        PUBLIC
            mmltk_build_options
            mmltk_rfdetr_cuda_ops
            CUDA::cudart
    )
    target_compile_options(mmltk_live_capture
        PRIVATE
            -Wall -Wextra -Wpedantic
    )
    set_target_properties(mmltk_live_capture PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    if(MMLTK_RFDETR_LIVE_CAPTURE_ENABLED)
        add_library(mmltk_live_runtime STATIC
            src/live/live_video_ingress.cpp
            src/live/live_frame_fanout.cpp
            src/live/manual_overlay_document.cpp
            src/live/live_analyzer_worker.cpp
            src/live/live_manual_overlay_worker.cpp
            src/live/live_compositor.cpp
            src/live/live_session_controller.cpp
        )
        target_include_directories(mmltk_live_runtime
            PUBLIC
                ${CMAKE_SOURCE_DIR}/include
            PRIVATE
                ${CMAKE_SOURCE_DIR}/src
        )
        target_link_libraries(mmltk_live_runtime
            PUBLIC
                mmltk_build_options
                mmltk_live_capture
                CUDA::cudart
            PRIVATE
                mmltk_logging
        )
        mmltk_enable_torch(mmltk_live_runtime PRIVATE)
        target_compile_options(mmltk_live_runtime
            PRIVATE
                -Wall -Wextra -Wpedantic
        )
        set_target_properties(mmltk_live_runtime PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
    endif()

    add_library(mmltk_export_onnx_io STATIC
        src/rfdetr/onnx_model_io.cpp
    )
    target_include_directories(mmltk_export_onnx_io
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(mmltk_export_onnx_io
        PUBLIC
            mmltk_build_options
            ONNX::onnx
            ${MMLTK_PROTOBUF_LIBRARY_TARGET}
    )
    mmltk_prefer_system_protobuf_headers(mmltk_export_onnx_io PRIVATE)
    target_compile_definitions(mmltk_export_onnx_io PRIVATE ONNX_ML=1)
    set_target_properties(mmltk_export_onnx_io PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_library(mmltk_export_onnx STATIC
        src/rfdetr/onnx_lowering.cpp
        src/rfdetr/onnx_model_proto_write.cpp
    )
    target_include_directories(mmltk_export_onnx
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_include_directories(mmltk_export_onnx
        SYSTEM PRIVATE
            $<TARGET_PROPERTY:ONNX::onnx,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_link_libraries(mmltk_export_onnx
        PUBLIC
            mmltk_build_options
    )
    mmltk_prefer_system_protobuf_headers(mmltk_export_onnx PRIVATE)
    mmltk_enable_torch(mmltk_export_onnx PUBLIC)
    target_compile_definitions(mmltk_export_onnx PRIVATE ONNX_ML=1)
    set_target_properties(mmltk_export_onnx PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    add_library(mmltk_rfdetr_onnx_lowering ALIAS mmltk_export_onnx)

    add_library(mmltk_export_onnx_simplify STATIC
        src/rfdetr/onnx_simplify.cpp
    )
    target_include_directories(mmltk_export_onnx_simplify
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(mmltk_export_onnx_simplify
        PUBLIC
            mmltk_build_options
            mmltk_logging
            ONNX::onnx
            ${MMLTK_PROTOBUF_LIBRARY_TARGET}
    )
    mmltk_prefer_system_protobuf_headers(mmltk_export_onnx_simplify PRIVATE)
    target_compile_definitions(mmltk_export_onnx_simplify PRIVATE ONNX_ML=1)
    set_target_properties(mmltk_export_onnx_simplify PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    add_library(mmltk_rfdetr_onnx_simplify ALIAS mmltk_export_onnx_simplify)

    add_library(mmltk_onnx_model_info STATIC
        src/rfdetr/onnx_model_info.cpp
    )
    target_include_directories(mmltk_onnx_model_info
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(mmltk_onnx_model_info
        PUBLIC
            mmltk_build_options
            mmltk_export_onnx_io
            ONNX::onnx
            ${MMLTK_PROTOBUF_LIBRARY_TARGET}
    )
    mmltk_prefer_system_protobuf_headers(mmltk_onnx_model_info PRIVATE)
    target_compile_definitions(mmltk_onnx_model_info PRIVATE ONNX_ML=1)
    set_target_properties(mmltk_onnx_model_info PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_executable(mmltk_rfdetr_onnx_simplify_tool
        src/rfdetr/onnx_simplify_main.cpp
    )
    target_include_directories(mmltk_rfdetr_onnx_simplify_tool
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(mmltk_rfdetr_onnx_simplify_tool
        PRIVATE
            mmltk_build_options
            mmltk_logging
            mmltk_export_onnx_io
            mmltk_export_onnx_simplify
            ONNX::onnx
            ${MMLTK_PROTOBUF_LIBRARY_TARGET}
    )
    set_target_properties(mmltk_rfdetr_onnx_simplify_tool PROPERTIES
        OUTPUT_NAME "mmltk-rfdetr-onnx-simplify"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_executable(mmltk_rfdetr_onnx_info_tool
        src/rfdetr/onnx_model_info_main.cpp
    )
    target_include_directories(mmltk_rfdetr_onnx_info_tool
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(mmltk_rfdetr_onnx_info_tool
        PRIVATE
            mmltk_build_options
            mmltk_logging
            mmltk_onnx_model_info
            ONNX::onnx
            ${MMLTK_PROTOBUF_LIBRARY_TARGET}
    )
    set_target_properties(mmltk_rfdetr_onnx_info_tool PROPERTIES
        OUTPUT_NAME "mmltk-rfdetr-onnx-info"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_library(mmltk_backend_onnx_deps INTERFACE)
    target_link_libraries(mmltk_backend_onnx_deps
        INTERFACE
            ${ONNXRUNTIME_LIBRARY}
    )
    add_library(mmltk_backend_tensorrt_deps INTERFACE)
    target_link_libraries(mmltk_backend_tensorrt_deps
        INTERFACE
            ${TENSORRT_NVINFER_LIBRARY}
            ${TENSORRT_NVONNXPARSER_LIBRARY}
    )
    add_library(mmltk_rfdetr_runtime_link_deps INTERFACE)
    target_link_options(mmltk_rfdetr_runtime_link_deps INTERFACE LINKER:--no-as-needed)
    target_link_libraries(mmltk_rfdetr_runtime_link_deps
        INTERFACE
            mmltk_backend_onnx_deps
            mmltk_backend_tensorrt_deps
    )
    if(MMLTK_TORCH_EXTRA_LIBRARIES)
        target_link_libraries(mmltk_rfdetr_runtime_link_deps
            INTERFACE
                ${MMLTK_TORCH_EXTRA_LIBRARIES}
        )
    endif()

    add_library(mmltk_backend_onnx STATIC
        src/rfdetr/inference/backend_onnx.cpp
    )
    target_include_directories(mmltk_backend_onnx
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_include_directories(mmltk_backend_onnx
        SYSTEM PRIVATE
            ${ONNXRUNTIME_INCLUDE_DIR}
    )
    target_link_libraries(mmltk_backend_onnx
        PUBLIC
            mmltk_core
            mmltk_build_options
        PRIVATE
            mmltk_backend_onnx_deps
    )
    mmltk_enable_torch(mmltk_backend_onnx PRIVATE)
    set_target_properties(mmltk_backend_onnx PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_library(mmltk_backend_tensorrt STATIC
        src/rfdetr/inference/backend_tensorrt.cpp
    )
    target_include_directories(mmltk_backend_tensorrt
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_include_directories(mmltk_backend_tensorrt
        SYSTEM PRIVATE
            ${TENSORRT_INCLUDE_DIR}
    )
    target_link_libraries(mmltk_backend_tensorrt
        PUBLIC
            mmltk_core
            mmltk_build_options
        PRIVATE
            mmltk_backend_tensorrt_deps
    )
    mmltk_enable_torch(mmltk_backend_tensorrt PRIVATE)
    set_target_properties(mmltk_backend_tensorrt PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_library(mmltk_rfdetr_torch STATIC
        src/rfdetr/checkpoint.cpp
        src/rfdetr/detection_ops.cpp
        src/rfdetr/draw.cpp
        src/rfdetr/layers/model.cpp
        src/rfdetr/layers/model_components.cpp
        src/rfdetr/layers/modules.cpp
        src/rfdetr/inference/postprocess.cpp
        src/rfdetr/checkpoint/python_checkpoint_bridge.cpp
        src/rfdetr/target_builder.cpp
        src/rfdetr/training/native_optimizer.cpp
        src/rfdetr/training/train.cpp
    )
    target_include_directories(mmltk_rfdetr_torch
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
    )
    target_include_directories(mmltk_rfdetr_torch
        SYSTEM PRIVATE
            $<TARGET_PROPERTY:ONNX::onnx,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_link_libraries(mmltk_rfdetr_torch
        PUBLIC
            mmltk_core
            mmltk_build_options
            mmltk_rfdetr_core
            mmltk_export_onnx
            mmltk_logging
        PRIVATE
            mmltk_rfdetr_lsap
            mmltk_rfdetr_cuda_ops
    )
    mmltk_prefer_system_protobuf_headers(mmltk_rfdetr_torch PRIVATE)
    mmltk_enable_torch(mmltk_rfdetr_torch PUBLIC)
    if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
        mmltk_enable_python_checkpoint_loader(mmltk_rfdetr_torch PRIVATE)
    endif()
    target_compile_definitions(mmltk_rfdetr_torch PRIVATE
        MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER=$<IF:$<BOOL:${BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER}>,1,0>
        MMLTK_RFDETR_LIVE_CAPTURE=$<IF:$<BOOL:${MMLTK_RFDETR_LIVE_CAPTURE_ENABLED}>,1,0>
        ONNX_ML=1
    )
    set_target_properties(mmltk_rfdetr_torch PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )

    add_library(mmltk_rfdetr_runtime STATIC
        src/rfdetr/cli/cli.cpp
        src/rfdetr/cli/distributed_train_launcher.cpp
        src/rfdetr/cli/module.cpp
        src/rfdetr/inference/evaluator.cpp
        src/rfdetr/inference/live_predict.cpp
        src/rfdetr/inference/predict.cpp
        src/rfdetr/core/runtime.cpp
        src/rfdetr/workflow_requests.cpp
        src/rfdetr/inference/validate.cpp
        src/rfdetr/inference/validate_workers.cpp
    )
    target_include_directories(mmltk_rfdetr_runtime
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
    )
    target_link_libraries(mmltk_rfdetr_runtime
        PUBLIC
            mmltk_core
            mmltk_build_options
            mmltk_logging
        PRIVATE
            mmltk_rfdetr_torch
            mmltk_backend_onnx
            mmltk_backend_tensorrt
            cli11
    )
    mmltk_enable_torch(mmltk_rfdetr_runtime PRIVATE)
    if(MMLTK_RFDETR_LIVE_CAPTURE_ENABLED)
        target_link_libraries(mmltk_rfdetr_runtime PRIVATE mmltk_live_capture)
    endif()
    target_compile_definitions(mmltk_rfdetr_runtime PRIVATE
        MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER=$<IF:$<BOOL:${BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER}>,1,0>
        MMLTK_RFDETR_LIVE_CAPTURE=$<IF:$<BOOL:${MMLTK_RFDETR_LIVE_CAPTURE_ENABLED}>,1,0>
    )
    set_target_properties(mmltk_rfdetr_runtime PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )
    
    add_library(mmltk_rfdetr_native SHARED
        src/rfdetr/native_library_anchor.cpp
    )
    target_include_directories(mmltk_rfdetr_native
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(mmltk_rfdetr_native
        PUBLIC
            mmltk_core
            mmltk_build_options
            mmltk_rfdetr_core
            mmltk_rfdetr_torch
        PRIVATE
            -Wl,--whole-archive
            mmltk_rfdetr_runtime
            -Wl,--no-whole-archive
            mmltk_rfdetr_runtime_link_deps
    )
    target_compile_definitions(mmltk_rfdetr_native PUBLIC
        MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER=$<IF:$<BOOL:${BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER}>,1,0>
        MMLTK_RFDETR_LIVE_CAPTURE=$<IF:$<BOOL:${MMLTK_RFDETR_LIVE_CAPTURE_ENABLED}>,1,0>
    )
    set_target_properties(mmltk_rfdetr_native PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )
    target_link_options(mmltk_rfdetr_native PRIVATE LINKER:--no-as-needed)
    add_library(mmltk_rfdetr ALIAS mmltk_rfdetr_native)
    target_compile_definitions(mmltk_cli PRIVATE MMLTK_BUILD_RFDETR_NATIVE=1)
    target_link_libraries(mmltk_cli PRIVATE
        mmltk_rfdetr_runtime
        mmltk_rfdetr_native
        mmltk_rfdetr_runtime_link_deps
    )
    add_dependencies(mmltk_cli mmltk_rfdetr_onnx_info_tool)
    set_target_properties(mmltk_cli PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )
    mmltk_enable_native_runtime_rpath(mmltk_rfdetr_native)
else()
    target_compile_definitions(mmltk_cli PRIVATE MMLTK_BUILD_RFDETR_NATIVE=0)
endif()

mmltk_enable_native_runtime_rpath(mmltk_cli)

if(BUILD_MMLTK_GUI)
    if(NOT BUILD_RFDETR_NATIVE)
        message(FATAL_ERROR "BUILD_MMLTK_GUI requires BUILD_RFDETR_NATIVE=ON.")
    endif()
    if(NOT MMLTK_NEEDS_PYTHON)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()
    set(MMLTK_GUI_VAST_BRIDGE_SOURCE "${CMAKE_SOURCE_DIR}/utilities/vast_bridge.py")
    if(NOT EXISTS "${MMLTK_GUI_VAST_BRIDGE_SOURCE}")
        message(FATAL_ERROR
            "Missing Vast GUI bridge script: ${MMLTK_GUI_VAST_BRIDGE_SOURCE}")
    endif()

    find_package(OpenGL REQUIRED)
    find_package(glfw3 REQUIRED)
    if(TARGET glfw)
        set(MMLTK_GLFW_TARGET glfw)
    elseif(TARGET glfw3::glfw)
        set(MMLTK_GLFW_TARGET glfw3::glfw)
    else()
        message(FATAL_ERROR
            "glfw3 was found, but CMake did not expose a supported imported target.")
    endif()

    if(MMLTK_IMGUI_SOURCE_DIR)
        get_filename_component(_mmltk_imgui_source_dir "${MMLTK_IMGUI_SOURCE_DIR}" REALPATH)
        if(NOT EXISTS "${_mmltk_imgui_source_dir}/imgui.cpp")
            message(FATAL_ERROR
                "MMLTK_IMGUI_SOURCE_DIR must point at a Dear ImGui checkout containing imgui.cpp.")
        endif()
    else()
        message(FATAL_ERROR
            "BUILD_MMLTK_GUI requires MMLTK_IMGUI_SOURCE_DIR. "
            "Prepare Dear ImGui before configuring instead of fetching it during configure.")
    endif()

    add_library(mmltk_imgui STATIC
        ${_mmltk_imgui_source_dir}/imgui.cpp
        ${_mmltk_imgui_source_dir}/imgui_draw.cpp
        ${_mmltk_imgui_source_dir}/imgui_tables.cpp
        ${_mmltk_imgui_source_dir}/imgui_widgets.cpp
        ${_mmltk_imgui_source_dir}/misc/cpp/imgui_stdlib.cpp
        ${_mmltk_imgui_source_dir}/backends/imgui_impl_glfw.cpp
        ${_mmltk_imgui_source_dir}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(mmltk_imgui
        PUBLIC
            ${_mmltk_imgui_source_dir}
            ${_mmltk_imgui_source_dir}/backends
    )
    target_link_libraries(mmltk_imgui
        PUBLIC
            mmltk_build_options
            ${MMLTK_GLFW_TARGET}
            OpenGL::GL
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(mmltk_imgui PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-conversion -Wno-null-dereference>
        )
    endif()

    add_library(mmltk_gui_module STATIC
        src/gui/annotation/controller.cpp
        src/gui/annotation/editor.cpp
        src/gui/annotation/editor_history.cpp
        src/gui/annotation/save_tab.cpp
        src/gui/annotation/setup_tab.cpp
        src/gui/annotation/sidebar.cpp
        src/gui/annotation/sidebar_actions.cpp
        src/gui/annotation/document/document.cpp
        src/gui/annotation/document/edit.cpp
        src/gui/annotation/document/types.cpp
        src/gui/annotation/render/renderer.cpp
        src/gui/annotation/session.cpp
        src/gui/annotation/tools/default_tools.cpp
        src/gui/annotation/tools/tool_helpers.cpp
        src/gui/annotation/tools/spline_tool_helpers.cpp
        src/gui/annotation/tools/skeleton_tool_helpers.cpp
        src/gui/annotation/tools/spline_tool.cpp
        src/gui/annotation/tools/skeleton_tool.cpp
        src/gui/annotation/tools/tool_manager.cpp
        src/gui/annotation/workspace/canvas.cpp
        src/gui/annotation/workspace/interaction.cpp
        src/gui/annotation/workspace/model.cpp
        src/gui/annotation_core.cpp
        src/gui/annotation_core_preview.cpp
        src/gui/annotation_core_persistence.cpp
        src/gui/activity_pane_presenter.cpp
        src/gui/app.cpp
        src/gui/app_options.cpp
        src/gui/canvas_layers.cpp
        src/gui/cli_seed.cpp
        src/gui/cuda_gl_interop_surface_core.cpp
        src/gui/default_state.cpp
        src/gui/model_input_ui.cpp
        src/gui/file_picker.cpp
        src/gui/gui_settings.cpp
        src/gui/local_train_controller.cpp
        src/gui/live_predict_controller.cpp
        src/gui/live_session_utils.cpp
        src/gui/live_preview_texture.cpp
        src/gui/preview_interaction_overlay.cpp
        src/gui/preview_rect_drag.cpp
        src/gui/rfdetr_module.cpp
        src/gui/rfdetr_workflows.cpp
        src/gui/source_runtime.cpp
        src/gui/source_selection.cpp
        src/gui/still_image_preview.cpp
        src/gui/train_command.cpp
        src/gui/train_process_runtime.cpp
        src/gui/ui_style.cpp
        src/gui/vast_query_controller.cpp
        src/gui/vast_runtime.cpp
    )
    target_include_directories(mmltk_gui_module PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(mmltk_gui_module
        PUBLIC
            mmltk_build_options
            mmltk_imgui
            mmltk_core
            mmltk_runtime
            mmltk_rfdetr_native
            mmltk_logging
        PRIVATE
            cli11
            nlohmann_json
    )
    target_compile_definitions(mmltk_gui_module PRIVATE
        MMLTK_GUI_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
        MMLTK_GUI_VAST_BRIDGE_SOURCE="${MMLTK_GUI_VAST_BRIDGE_SOURCE}"
        MMLTK_GUI_ASSET_ROOT_SOURCE="${CMAKE_SOURCE_DIR}/src/gui/res"
    )
    mmltk_enable_torch(mmltk_gui_module PRIVATE)
    if(MMLTK_RFDETR_LIVE_CAPTURE_ENABLED)
        target_link_libraries(mmltk_gui_module PRIVATE
            mmltk_live_capture
            mmltk_live_runtime
        )
    endif()
    set_target_properties(mmltk_gui_module PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )

    add_executable(mmltk_gui
        src/gui/main.cpp
    )
    target_link_libraries(mmltk_gui PRIVATE
        mmltk_gui_module
        mmltk_rfdetr_runtime_link_deps
        mmltk_logging
    )
    set_target_properties(mmltk_gui PROPERTIES
        OUTPUT_NAME "mmltk-gui"
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    )
    mmltk_enable_native_runtime_rpath(mmltk_gui)

endif()

set_target_properties(mmltk_core PROPERTIES OUTPUT_NAME "mmltk_core")
set_target_properties(mmltk_cli PROPERTIES OUTPUT_NAME "mmltk")

install(TARGETS mmltk_logging mmltk_core mmltk_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(BUILD_RFDETR_NATIVE)
    install(TARGETS mmltk_rfdetr_native
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(TARGETS mmltk_rfdetr_onnx_simplify_tool
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(TARGETS mmltk_rfdetr_onnx_info_tool
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(BUILD_MMLTK_GUI)
    install(TARGETS mmltk_gui
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/gui/res/fonts/"
        DESTINATION "${MMLTK_INSTALL_FONTDIR}"
    )
    install(FILES "${MMLTK_GUI_VAST_BRIDGE_SOURCE}"
        DESTINATION "${MMLTK_INSTALL_PYTHONDIR}"
        RENAME "vast_bridge.py"
    )
endif()

if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
    install(FILES "${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}"
        DESTINATION "${MMLTK_INSTALL_PYTHONDIR}"
        RENAME "rfdetr_checkpoint_bridge.py"
    )
endif()

if(BUILD_RFDETR_NATIVE)
    file(GLOB MMLTK_ONNX_RUNTIME_LIBS
        "${MMLTK_ONNX_ROOT}/lib/libonnx*.so*"
    )
    if(MMLTK_ONNX_RUNTIME_LIBS)
        install(FILES ${MMLTK_ONNX_RUNTIME_LIBS}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()
    file(GLOB MMLTK_ONNXRUNTIME_RUNTIME_LIBS
        "${MMLTK_ONNXRUNTIME_ROOT}/lib/libonnxruntime*.so*"
    )
    if(MMLTK_ONNXRUNTIME_RUNTIME_LIBS)
        install(FILES ${MMLTK_ONNXRUNTIME_RUNTIME_LIBS}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()
endif()

# ---- Tests ----
option(BUILD_TESTS "Build tests and profile runners" OFF)
if(BUILD_TESTS)
    enable_testing()
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "Install Catch2 documentation" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "Install Catch2 extras" FORCE)
    set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "Build Catch2 development extras" FORCE)
    set(CATCH_BUILD_TESTING OFF CACHE BOOL "Build Catch2 self tests" FORCE)
    set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "Build Catch2 examples" FORCE)
    set(CATCH_BUILD_EXTRA_TESTS OFF CACHE BOOL "Build Catch2 extra tests" FORCE)
    set(CATCH_BUILD_BENCHMARKS OFF CACHE BOOL "Build Catch2 benchmarks" FORCE)
    set(CATCH_CONFIG_NO_THREAD_SAFE_ASSERTIONS ON CACHE BOOL
        "Force-disable Catch2 thread-safe assertions; keep test assertions on the fast default path" FORCE)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/catch2/generated-includes/catch2")
    add_subdirectory(
        ${CMAKE_SOURCE_DIR}/third_party/catch2
        ${CMAKE_BINARY_DIR}/third_party/catch2
        EXCLUDE_FROM_ALL
    )
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/third_party/catch2/extras)
    include(Catch)

    add_library(mmltk_catch_main STATIC
        tests/support/catch_main.cpp
    )
    target_include_directories(mmltk_catch_main PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/support
    )
    target_link_libraries(mmltk_catch_main PUBLIC
        Catch2::Catch2
        mmltk_logging
    )
    set_target_properties(mmltk_catch_main PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_library(mmltk_test_support STATIC
        tests/support/test_fixture.cpp
    )
    target_include_directories(mmltk_test_support PUBLIC
        ${CMAKE_SOURCE_DIR}/tests/support
    )
    target_link_libraries(mmltk_test_support PUBLIC
        stb
        mmltk_build_options
    )

    add_library(mmltk_test_headers INTERFACE)
    target_include_directories(mmltk_test_headers INTERFACE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/tests/support
        ${CMAKE_SOURCE_DIR}/tests/model/rfdetr
    )

    add_executable(mmltk_tests_core
        tests/core/async_runtime.test.cpp
        tests/core/cli_test_bundles.test.cpp
        tests/core/compile_progress.test.cpp
        tests/core/image_resize.test.cpp
        tests/core/live_crop_state.test.cpp
        tests/core/model_registry.test.cpp
        tests/core/roundtrip.test.cpp
        tests/core/vendored_helpers.test.cpp
        tests/core/worker_pool.test.cpp
    )
    target_include_directories(mmltk_tests_core PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
    )
    target_link_libraries(mmltk_tests_core PRIVATE
        Catch2::Catch2
        mmltk_catch_main
        mmltk_build_options
        mmltk_core
        mmltk_runtime
        mmltk_test_headers
        mmltk_test_support
    )
    target_compile_definitions(mmltk_tests_core PRIVATE
        MMLTK_TEST_MMLTK_CLI_PATH="$<TARGET_FILE:mmltk_cli>"
    )
    add_dependencies(mmltk_tests_core mmltk_cli)
    set_target_properties(mmltk_tests_core PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    catch_discover_tests(
        mmltk_tests_core
        TEST_PREFIX "core::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )

    set(MMLTK_GUI_TEST_SOURCES
        tests/gui/annotation_document.test.cpp
        tests/gui/annotation_save_tab.test.cpp
        tests/gui/annotation_workspace_canvas.test.cpp
        src/gui/annotation/controller.cpp
        src/gui/annotation/editor.cpp
        src/gui/annotation/editor_history.cpp
        src/gui/annotation/document/document.cpp
        src/gui/annotation/document/edit.cpp
        src/gui/annotation/document/types.cpp
        src/gui/annotation/render/renderer.cpp
        src/gui/annotation/session.cpp
        src/gui/annotation/tools/default_tools.cpp
        src/gui/annotation/tools/tool_helpers.cpp
        src/gui/annotation/tools/spline_tool_helpers.cpp
        src/gui/annotation/tools/skeleton_tool_helpers.cpp
        src/gui/annotation/tools/spline_tool.cpp
        src/gui/annotation/tools/skeleton_tool.cpp
        src/gui/annotation/tools/tool_manager.cpp
        src/gui/annotation/workspace/canvas.cpp
        src/gui/annotation/workspace/interaction.cpp
        src/gui/annotation/workspace/model.cpp
        src/gui/annotation_core_preview.cpp
        src/gui/annotation_core_persistence.cpp
        tests/gui/app_options.test.cpp
        tests/gui/live_session_utils.test.cpp
        tests/gui/live_overlay_runtime.test.cpp
        tests/gui/settings.test.cpp
        tests/gui/subprocess_utils.test.cpp
        tests/gui/vast_runtime.test.cpp
        src/gui/annotation_core.cpp
        src/gui/app_options.cpp
        src/gui/canvas_layers.cpp
        src/gui/cuda_gl_interop_surface_core.cpp
        src/gui/gui_settings.cpp
        src/gui/live_session_utils.cpp
        src/gui/vast_query_controller.cpp
        src/gui/vast_runtime.cpp
        src/live/manual_overlay_document.cpp
    )
    if(MMLTK_NEEDS_TORCH)
        list(APPEND MMLTK_GUI_TEST_SOURCES
            tests/gui/annotation_core.test.cpp
            tests/gui/canvas_layers.test.cpp
            tests/gui/preview_rect_drag.test.cpp
            tests/gui/train_command.test.cpp
            src/gui/preview_rect_drag.cpp
            src/gui/train_command.cpp
        )
    endif()
    if(BUILD_RFDETR_NATIVE)
        list(APPEND MMLTK_GUI_TEST_SOURCES
            tests/gui/local_train_runtime.test.cpp
            tests/gui/rfdetr_workflows.test.cpp
            tests/gui/cli_seed.test.cpp
            src/gui/cli_seed.cpp
            src/gui/default_state.cpp
            src/gui/local_train_controller.cpp
            src/gui/rfdetr_workflows.cpp
            src/rfdetr/workflow_requests.cpp
            src/gui/train_process_runtime.cpp
        )
    endif()

    add_executable(mmltk_tests_gui ${MMLTK_GUI_TEST_SOURCES})
    target_include_directories(mmltk_tests_gui PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(mmltk_tests_gui PRIVATE
        Catch2::Catch2
        mmltk_catch_main
        mmltk_build_options
        mmltk_core
        mmltk_runtime
        mmltk_rfdetr_core
        mmltk_test_headers
        cli11
        nlohmann_json
        CUDA::cudart
    )
    if(TARGET mmltk_imgui)
        target_link_libraries(mmltk_tests_gui PRIVATE
            mmltk_imgui
        )
    endif()
    if(MMLTK_NEEDS_TORCH)
        mmltk_enable_torch(mmltk_tests_gui PRIVATE)
    endif()
    if(BUILD_RFDETR_NATIVE)
        target_link_libraries(mmltk_tests_gui PRIVATE
            mmltk_rfdetr_torch
        )
    endif()
    set_target_properties(mmltk_tests_gui PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    catch_discover_tests(
        mmltk_tests_gui
        TEST_PREFIX "gui::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )

    install(TARGETS mmltk_tests_core mmltk_tests_gui
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    add_executable(mmltk_profile_runner
        tests/profile_runner.cpp
    )
    target_include_directories(mmltk_profile_runner PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(mmltk_profile_runner PRIVATE
        mmltk_core
        mmltk_build_options
        mmltk_test_headers
        mmltk_test_support
    )

    if(BUILD_RFDETR_NATIVE)
        set(MMLTK_RFDETR_TEST_SOURCES
            tests/model/rfdetr/weight_catalog.test.cpp
            tests/model/rfdetr/cli_aliases.test.cpp
            tests/model/rfdetr/eval_sample_writer.test.cpp
            tests/model/rfdetr/cuda_utils.test.cpp
            tests/model/rfdetr/model_trainability.test.cpp
            tests/model/rfdetr/modules.test.cpp
            tests/model/rfdetr/native_ops.test.cpp
            tests/model/rfdetr/native_optimizer.test.cpp
            tests/model/rfdetr/onnx_lowering.test.cpp
            tests/model/rfdetr/prediction_json.test.cpp
            tests/model/rfdetr/public_headers.test.cpp
            tests/model/rfdetr/runtime_shared.test.cpp
            tests/model/rfdetr/target_builder.test.cpp
        )
        if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
            list(APPEND MMLTK_RFDETR_TEST_SOURCES
                tests/model/rfdetr/checkpoint.test.cpp
                tests/model/rfdetr/checkpoint_parity.test.cpp
                tests/model/rfdetr/native_integration.test.cpp
            )
        endif()
        add_executable(mmltk_tests_model_rfdetr ${MMLTK_RFDETR_TEST_SOURCES})
        target_include_directories(mmltk_tests_model_rfdetr PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
        )
        target_link_libraries(mmltk_tests_model_rfdetr PRIVATE
            Catch2::Catch2
            mmltk_catch_main
            mmltk_core
            mmltk_build_options
            mmltk_rfdetr_native
            mmltk_test_headers
            mmltk_test_support
        )
        target_compile_definitions(mmltk_tests_model_rfdetr PRIVATE
            MMLTK_TEST_MMLTK_CLI_PATH="$<TARGET_FILE:mmltk_cli>"
        )
        add_dependencies(mmltk_tests_model_rfdetr mmltk_cli)
        mmltk_prefer_system_protobuf_headers(mmltk_tests_model_rfdetr PRIVATE)
        mmltk_enable_torch(mmltk_tests_model_rfdetr PRIVATE)
        set_target_properties(mmltk_tests_model_rfdetr PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        mmltk_enable_native_runtime_rpath(mmltk_tests_model_rfdetr)
        catch_discover_tests(
            mmltk_tests_model_rfdetr
            TEST_SPEC "~[optin]"
            TEST_PREFIX "rfdetr::"
            DISCOVERY_MODE PRE_TEST
            ADD_TAGS_AS_LABELS
        )
        install(TARGETS mmltk_tests_model_rfdetr
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )

        add_executable(mmltk_rfdetr_profile_runner
            tests/rfdetr_profile_runner.cpp
        )
        target_include_directories(mmltk_rfdetr_profile_runner PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_link_libraries(mmltk_rfdetr_profile_runner PRIVATE
            mmltk_core
            mmltk_build_options
            mmltk_rfdetr_native
            mmltk_test_headers
        )
        mmltk_enable_torch(mmltk_rfdetr_profile_runner PRIVATE)
        set_target_properties(mmltk_rfdetr_profile_runner PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        mmltk_enable_native_runtime_rpath(mmltk_rfdetr_profile_runner)

        add_executable(mmltk_rfdetr_train_profile_runner
            tests/rfdetr_train_profile_runner.cpp
        )
        target_include_directories(mmltk_rfdetr_train_profile_runner PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_link_libraries(mmltk_rfdetr_train_profile_runner PRIVATE
            mmltk_core
            mmltk_build_options
            mmltk_rfdetr_native
            mmltk_test_headers
            mmltk_test_support
        )
        mmltk_enable_torch(mmltk_rfdetr_train_profile_runner PRIVATE)
        set_target_properties(mmltk_rfdetr_train_profile_runner PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
        mmltk_enable_native_runtime_rpath(mmltk_rfdetr_train_profile_runner)
    endif()
endif()
