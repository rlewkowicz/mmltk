include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
get_filename_component(MMLTK_CUDA_TOOLKIT_ROOT "${MMLTK_CUDA_TOOLKIT_ROOT}" REALPATH)
set(MMLTK_CUDA_NVCC "${MMLTK_CUDA_TOOLKIT_ROOT}/bin/nvcc")
if(NOT EXISTS "${MMLTK_CUDA_NVCC}")
    message(FATAL_ERROR
        "mmltk requires CUDA ${MMLTK_CUDA_VERSION}.x at ${MMLTK_CUDA_TOOLKIT_ROOT}. "
        "Install the toolkit there or override MMLTK_CUDA_TOOLKIT_ROOT.")
endif()

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
option(MMLTK_WARNINGS_AS_ERRORS
    "Treat compiler warnings as build errors for every project target"
    ON)
option(MMLTK_DEV_ENABLE_ASAN
    "Enable AddressSanitizer for host-side Dev C++ builds"
    OFF)
option(MMLTK_DEV_ENABLE_UBSAN
    "Enable UndefinedBehaviorSanitizer for host-side Dev C++ builds"
    OFF)
option(MMLTK_ENABLE_TIME_TRACE
    "Emit host-side Clang -ftime-trace JSON artifacts during compilation"
    OFF)

set(CMAKE_COMPILE_WARNING_AS_ERROR "${MMLTK_WARNINGS_AS_ERRORS}")

find_program(MMLTK_CCACHE_PROGRAM NAMES ccache REQUIRED)
foreach(MMLTK_COMPILER_LAUNCHER_LANGUAGE IN ITEMS C CXX CUDA)
    set(CMAKE_${MMLTK_COMPILER_LAUNCHER_LANGUAGE}_COMPILER_LAUNCHER
        "${MMLTK_CCACHE_PROGRAM}" CACHE FILEPATH
        "Required ${MMLTK_COMPILER_LAUNCHER_LANGUAGE} compiler launcher for mmltk builds" FORCE)
endforeach()
message(STATUS "Using required ccache compiler launcher: ${MMLTK_CCACHE_PROGRAM}")

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "mmltk requires a GNU-compatible host compiler so every native link can use mold")
endif()
find_program(MMLTK_MOLD_PROGRAM NAMES mold REQUIRED)
set(MMLTK_HOST_LINKER_FLAGS "-fuse-ld=mold")
message(STATUS "Using required mold linker: ${MMLTK_MOLD_PROGRAM}")

set(MMLTK_TIME_TRACE_FLAGS "")
if(MMLTK_ENABLE_TIME_TRACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(MMLTK_TIME_TRACE_FLAGS "-ftime-trace")
        message(STATUS "Enabling host-side Clang time trace emission")
    else()
        message(FATAL_ERROR
            "MMLTK_ENABLE_TIME_TRACE=ON requires a Clang host compiler, "
            "but CMAKE_CXX_COMPILER_ID resolved to `${CMAKE_CXX_COMPILER_ID}`")
    endif()
endif()

set(MMLTK_CUDA_HOST_FORWARD_FLAGS "")
if(NOT MMLTK_HOST_LINKER_FLAGS STREQUAL "")
    string(APPEND MMLTK_CUDA_HOST_FORWARD_FLAGS " -Xcompiler=${MMLTK_HOST_LINKER_FLAGS}")
endif()
if(NOT MMLTK_TIME_TRACE_FLAGS STREQUAL "")
    string(APPEND MMLTK_CUDA_HOST_FORWARD_FLAGS " -Xcompiler=${MMLTK_TIME_TRACE_FLAGS}")
endif()

set(MMLTK_X86_SIMD_FLAGS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|amd64|AMD64)$")
    set(MMLTK_X86_SIMD_FLAGS "-mssse3 -msse4.2 -mavx2 -mfma")
endif()
set(MMLTK_COMMON_MACHINE_FLAGS
    "-march=native -mtune=native ${MMLTK_X86_SIMD_FLAGS} -fno-semantic-interposition -ffunction-sections -fdata-sections")
set(MMLTK_PROJECT_CXX_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
)
if(MMLTK_DEV_STRICT_WARNINGS)
    list(APPEND MMLTK_PROJECT_CXX_WARNING_FLAGS
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Woverloaded-virtual
        -Wconversion
        -Wformat=2
        -Wnull-dereference
        -Wimplicit-fallthrough
    )
endif()
set(MMLTK_PROJECT_CUDA_WARNING_FLAGS "")
if(MMLTK_WARNINGS_AS_ERRORS)
    list(APPEND MMLTK_PROJECT_CUDA_WARNING_FLAGS --Werror all-warnings)
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
    "-O3 -DNDEBUG -fomit-frame-pointer -fno-plt ${MMLTK_COMMON_MACHINE_FLAGS} ${MMLTK_TIME_TRACE_FLAGS}")
set(MMLTK_DEV_FLAGS
    "-O2 -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls -D_GLIBCXX_ASSERTIONS -D_FORTIFY_SOURCE=3 ${MMLTK_COMMON_MACHINE_FLAGS} ${MMLTK_DEV_SANITIZER_FLAGS} ${MMLTK_TIME_TRACE_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "${MMLTK_RELEASE_FLAGS}")
set(CMAKE_CXX_FLAGS_DEV "${MMLTK_DEV_FLAGS}")

set(CMAKE_CUDA_FLAGS_RELEASE "-O3 -DNDEBUG${MMLTK_CUDA_HOST_FORWARD_FLAGS}")
set(CMAKE_CUDA_FLAGS_DEV
    "-O2 -g -lineinfo -Xcompiler=-fno-omit-frame-pointer${MMLTK_CUDA_HOST_FORWARD_FLAGS}")

set(MMLTK_COMMON_LINK_FLAGS "-Wl,-O2 -Wl,--as-needed ${MMLTK_HOST_LINKER_FLAGS}")
set(MMLTK_RELEASE_LINK_FLAGS "${MMLTK_COMMON_LINK_FLAGS} -Wl,--gc-sections")
set(MMLTK_DEV_LINK_FLAGS
    "${MMLTK_COMMON_LINK_FLAGS} -Wl,--gc-sections ${MMLTK_DEV_SANITIZER_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${MMLTK_RELEASE_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_RELEASE "${MMLTK_RELEASE_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${MMLTK_RELEASE_LINK_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_DEV "${MMLTK_DEV_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_DEV "${MMLTK_DEV_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_DEV "${MMLTK_DEV_LINK_FLAGS}")

if(MMLTK_ENABLE_TIME_TRACE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
    message(STATUS "IPO disabled for time-trace builds")
else()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT MMLTK_IPO_SUPPORTED OUTPUT MMLTK_IPO_OUTPUT LANGUAGES CXX)
    if(MMLTK_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(FATAL_ERROR "Release IPO is required but unsupported: ${MMLTK_IPO_OUTPUT}")
    endif()
endif()

find_package(CUDAToolkit REQUIRED)
if(NOT "${CUDAToolkit_VERSION}" MATCHES "^${MMLTK_CUDA_VERSION}(\\.|$)")
    message(FATAL_ERROR
        "Expected CUDA ${MMLTK_CUDA_VERSION}.x from ${MMLTK_CUDA_TOOLKIT_ROOT}, "
        "but CMake resolved CUDA ${CUDAToolkit_VERSION} from ${CUDAToolkit_BIN_DIR}.")
endif()
message(STATUS "Using CUDA toolkit ${CUDAToolkit_VERSION} from ${CUDAToolkit_ROOT}")
find_package(Threads REQUIRED)
