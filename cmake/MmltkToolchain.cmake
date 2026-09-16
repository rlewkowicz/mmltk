include_guard(GLOBAL)

include(GNUInstallDirs)

include(CheckPIESupported)
check_pie_supported(OUTPUT_VARIABLE MMLTK_PIE_CHECK_OUTPUT LANGUAGES C CXX)
if(NOT CMAKE_C_LINK_PIE_SUPPORTED OR NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
    message(FATAL_ERROR "mmltk requires PIE-capable C and C++ linkers: ${MMLTK_PIE_CHECK_OUTPUT}")
endif()

set(CMAKE_INSTALL_BINDIR "bin" CACHE STRING "Install subdirectory for executables" FORCE)
set(CMAKE_INSTALL_LIBDIR "lib" CACHE STRING "Install subdirectory for libraries" FORCE)
set(CMAKE_INSTALL_DATADIR "share" CACHE STRING "Install subdirectory for shared data" FORCE)

set(MMLTK_GCC16_PREFIX "/opt/gcc-16.2")
set(MMLTK_GCC16_RUNTIME_LIBDIR "${MMLTK_GCC16_PREFIX}/lib64")

function(mmltk_require_exact_gcc16 compiler language expected_path)
    get_filename_component(_mmltk_expected_compiler "${expected_path}" REALPATH)
    get_filename_component(_mmltk_selected_compiler "${compiler}" REALPATH)
    if(NOT _mmltk_selected_compiler STREQUAL _mmltk_expected_compiler)
        message(FATAL_ERROR
            "mmltk requires ${language} compiler ${expected_path}, but selected ${compiler}")
    endif()
    execute_process(
        COMMAND "${compiler}" -dumpfullversion -dumpversion
        RESULT_VARIABLE _mmltk_gcc_version_result
        OUTPUT_VARIABLE _mmltk_gcc_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _mmltk_gcc_version_result EQUAL 0 OR
       NOT _mmltk_gcc_version STREQUAL "16.2.0")
        message(FATAL_ERROR
            "mmltk requires GNU ${language} compiler 16.2.0 at ${expected_path}, "
            "but ${compiler} reports '${_mmltk_gcc_version}'")
    endif()
endfunction()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "mmltk core host C and C++ compilation requires GNU GCC 16.2")
endif()
mmltk_require_exact_gcc16("${CMAKE_C_COMPILER}" C "${MMLTK_GCC16_PREFIX}/bin/gcc")
mmltk_require_exact_gcc16("${CMAKE_CXX_COMPILER}" CXX "${MMLTK_GCC16_PREFIX}/bin/g++")

set(CMAKE_CUDA_HOST_COMPILER "${MMLTK_GCC16_PREFIX}/bin/g++" CACHE FILEPATH
    "Pinned CUDA host compiler")
mmltk_require_exact_gcc16("${CMAKE_CUDA_HOST_COMPILER}" "CUDA host CXX"
    "${MMLTK_GCC16_PREFIX}/bin/g++")

# CUDA compiler identification and all subsequent CUDA commands use this policy.
set(CMAKE_CUDA_STANDARD 23 CACHE STRING "Required CUDA C++ dialect" FORCE)
set(CMAKE_CUDA_STANDARD_REQUIRED ON CACHE BOOL "Require CUDA C++23" FORCE)
set(CMAKE_CUDA_EXTENSIONS OFF CACHE BOOL "Disable CUDA language extensions" FORCE)
if(NOT " ${CMAKE_CUDA_FLAGS} " MATCHES " --allow-unsupported-compiler ")
    string(APPEND CMAKE_CUDA_FLAGS " --allow-unsupported-compiler")
endif()
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS}" CACHE STRING "Configured CUDA flags" FORCE)

function(mmltk_resolve_gcc16_runtime_library output_var library_name)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${library_name}"
        RESULT_VARIABLE _mmltk_runtime_library_result
        OUTPUT_VARIABLE _mmltk_runtime_library
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _mmltk_runtime_library_result EQUAL 0 OR
       _mmltk_runtime_library STREQUAL "${library_name}" OR
       NOT EXISTS "${_mmltk_runtime_library}")
        message(FATAL_ERROR
            "GCC 16.2 could not resolve ${library_name} from ${CMAKE_CXX_COMPILER}")
    endif()
    get_filename_component(_mmltk_runtime_library "${_mmltk_runtime_library}" REALPATH)
    file(RELATIVE_PATH _mmltk_runtime_library_relative
        "${MMLTK_GCC16_PREFIX}" "${_mmltk_runtime_library}")
    if(_mmltk_runtime_library_relative MATCHES "^\\.\\.")
        message(FATAL_ERROR
            "GCC 16.2 resolved ${library_name} outside ${MMLTK_GCC16_PREFIX}: "
            "${_mmltk_runtime_library}")
    endif()
    set(${output_var} "${_mmltk_runtime_library}" PARENT_SCOPE)
endfunction()

mmltk_resolve_gcc16_runtime_library(MMLTK_GCC16_LIBSTDCXX_RUNTIME libstdc++.so.6)
mmltk_resolve_gcc16_runtime_library(MMLTK_GCC16_LIBGCC_RUNTIME libgcc_s.so.1)

file(READ "${CMAKE_SOURCE_DIR}/docker/nvidia-payload.json" _mmltk_nvidia_manifest)
foreach(_mmltk_root IN ITEMS cuda torch python native tensorrt)
    string(JSON MMLTK_NVIDIA_${_mmltk_root}_ROOT GET "${_mmltk_nvidia_manifest}" "${_mmltk_root}_root")
endforeach()
string(JSON _mmltk_cuda_version GET "${_mmltk_nvidia_manifest}" versions cuda)
string(REGEX REPLACE "\\.[0-9]+$" "" _mmltk_cuda_version "${_mmltk_cuda_version}")
set(MMLTK_CUDA_VERSION "${_mmltk_cuda_version}" CACHE STRING "Pinned CUDA toolkit version" FORCE)
set(MMLTK_CUDA_TOOLKIT_ROOT "${MMLTK_NVIDIA_cuda_ROOT}" CACHE PATH "Normalized donor CUDA root" FORCE)
string(JSON _mmltk_library_count LENGTH "${_mmltk_nvidia_manifest}" library_order)
math(EXPR _mmltk_library_last "${_mmltk_library_count} - 1")
set(MMLTK_NVIDIA_LIBRARY_DIRECTORIES "")
foreach(_mmltk_index RANGE ${_mmltk_library_last})
    string(JSON _mmltk_root_key GET "${_mmltk_nvidia_manifest}" library_order ${_mmltk_index})
    string(JSON _mmltk_root GET "${_mmltk_nvidia_manifest}" "${_mmltk_root_key}")
    if(_mmltk_root_key STREQUAL "cuda_root")
        list(APPEND MMLTK_NVIDIA_LIBRARY_DIRECTORIES "${_mmltk_root}/lib64")
    else()
        list(APPEND MMLTK_NVIDIA_LIBRARY_DIRECTORIES "${_mmltk_root}/lib")
    endif()
endforeach()
set(MMLTK_DEFAULT_CUDA_ARCHITECTURES "86;87;89;90;100;103;110;120;121")
set(MMLTK_CUDA_ARCHITECTURES "${MMLTK_DEFAULT_CUDA_ARCHITECTURES}" CACHE STRING
    "CUDA architectures for mmltk CUDA targets")
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

find_program(MMLTK_CCACHE_PROGRAM NAMES ccache REQUIRED)
foreach(MMLTK_COMPILER_LAUNCHER_LANGUAGE IN ITEMS C CUDA)
    set(CMAKE_${MMLTK_COMPILER_LAUNCHER_LANGUAGE}_COMPILER_LAUNCHER
        "${MMLTK_CCACHE_PROGRAM}" CACHE FILEPATH
        "Required ${MMLTK_COMPILER_LAUNCHER_LANGUAGE} compiler launcher for mmltk builds" FORCE)
endforeach()
set(MMLTK_CXX_COMPILER_LAUNCHER
    "${CMAKE_SOURCE_DIR}/tools/module_safe_compiler_launcher.sh")
if(NOT IS_EXECUTABLE "${MMLTK_CXX_COMPILER_LAUNCHER}")
    message(FATAL_ERROR
        "Missing executable module-safe C++ compiler launcher: "
        "${MMLTK_CXX_COMPILER_LAUNCHER}")
endif()
set(CMAKE_CXX_COMPILER_LAUNCHER
    "${MMLTK_CXX_COMPILER_LAUNCHER}" CACHE FILEPATH
    "C++ compiler launcher that bypasses ccache for named-module compilations" FORCE)
message(STATUS
    "Using ccache for ordinary compilation and direct compiler execution for C++ module BMIs")

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

set(MMLTK_HOST_SECTION_OPTIONS -ffunction-sections -fdata-sections)
set(MMLTK_COMMON_HOST_COMPILE_OPTIONS
    -march=native
    -mtune=native
    -fno-semantic-interposition
    ${MMLTK_HOST_SECTION_OPTIONS})
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|amd64|AMD64)$")
    list(APPEND MMLTK_COMMON_HOST_COMPILE_OPTIONS
        -mssse3
        -msse4.2
        -mavx2
        -mfma)
endif()
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
set(MMLTK_DEV_SANITIZER_OPTIONS "")
if(MMLTK_DEV_ENABLE_ASAN AND MMLTK_DEV_ENABLE_TSAN)
    message(FATAL_ERROR "MMLTK_DEV_ENABLE_ASAN and MMLTK_DEV_ENABLE_TSAN cannot both be enabled")
endif()
if(MMLTK_DEV_ENABLE_ASAN)
    list(APPEND MMLTK_DEV_SANITIZER_OPTIONS -fsanitize=address)
endif()
if(MMLTK_DEV_ENABLE_TSAN)
    list(APPEND MMLTK_DEV_SANITIZER_OPTIONS -fsanitize=thread)
endif()
if(MMLTK_DEV_ENABLE_UBSAN)
    list(APPEND MMLTK_DEV_SANITIZER_OPTIONS -fsanitize=undefined)
endif()
if(MMLTK_DEV_SANITIZER_OPTIONS)
    list(APPEND MMLTK_DEV_SANITIZER_OPTIONS -fno-sanitize-recover=all)
endif()
set(MMLTK_RELEASE_HOST_COMPILE_OPTIONS
    -O3
    -DNDEBUG
    -fomit-frame-pointer
    -fno-plt
    -fhardened
    -Werror=hardened
    ${MMLTK_COMMON_HOST_COMPILE_OPTIONS})
# GCC 16.2 crashes in DWARF enum emission for C++26 reflection modules at
# debug levels 2 and 3. Level 1 retains line/function attribution for Dev,
# sanitizer, debugger, and static-analysis graphs without entering that
# compiler defect.
set(MMLTK_DEV_DEBUG_OPTION -g1)
set(MMLTK_DEV_HOST_COMPILE_OPTIONS
    -O2
    ${MMLTK_DEV_DEBUG_OPTION}
    -fno-omit-frame-pointer
    -fno-optimize-sibling-calls
    -D_GLIBCXX_ASSERTIONS
    -D_FORTIFY_SOURCE=3
    ${MMLTK_COMMON_HOST_COMPILE_OPTIONS}
    ${MMLTK_DEV_SANITIZER_OPTIONS})
if(MMLTK_TIME_TRACE_FLAGS)
    list(APPEND MMLTK_RELEASE_HOST_COMPILE_OPTIONS
        "${MMLTK_TIME_TRACE_FLAGS}")
    list(APPEND MMLTK_DEV_HOST_COMPILE_OPTIONS
        "${MMLTK_TIME_TRACE_FLAGS}")
endif()

# CUDA host code shares the ordinary --gc-sections link. A standard-library
# COMDAT can retain an otherwise unused CUDA archive member after LTO; separate
# host sections let the linker discard its unrelated functions and references.
list(JOIN MMLTK_HOST_SECTION_OPTIONS "," MMLTK_CUDA_HOST_SECTION_OPTIONS)
set(MMLTK_COMMON_CUDA_COMPILE_OPTIONS
    "-Xcompiler=${MMLTK_CUDA_HOST_SECTION_OPTIONS}"
    "-Xcompiler=${MMLTK_HOST_LINKER_FLAGS}")
set(MMLTK_RELEASE_CUDA_COMPILE_OPTIONS
    -O3
    -DNDEBUG
    ${MMLTK_COMMON_CUDA_COMPILE_OPTIONS})
set(MMLTK_DEV_CUDA_COMPILE_OPTIONS
    -O2
    -g
    -lineinfo
    -Xcompiler=-fno-omit-frame-pointer
    ${MMLTK_COMMON_CUDA_COMPILE_OPTIONS})
if(MMLTK_TIME_TRACE_FLAGS)
    list(APPEND MMLTK_RELEASE_CUDA_COMPILE_OPTIONS
        "-Xcompiler=${MMLTK_TIME_TRACE_FLAGS}")
    list(APPEND MMLTK_DEV_CUDA_COMPILE_OPTIONS
        "-Xcompiler=${MMLTK_TIME_TRACE_FLAGS}")
endif()

set(MMLTK_COMMON_LINK_OPTIONS
    -Wl,-O2
    -Wl,--as-needed
    "${MMLTK_HOST_LINKER_FLAGS}")
set(MMLTK_RELEASE_LINK_OPTIONS
    ${MMLTK_COMMON_LINK_OPTIONS}
    -Wl,--gc-sections
    -Wl,-z,relro,-z,now)
set(MMLTK_DEV_LINK_OPTIONS
    ${MMLTK_COMMON_LINK_OPTIONS}
    -Wl,--gc-sections
    ${MMLTK_DEV_SANITIZER_OPTIONS})

set(MMLTK_ANALYSIS_IPO_ENABLED OFF)
if(MMLTK_ENABLE_TIME_TRACE)
    set(MMLTK_RELEASE_IPO_ENABLED OFF)
    message(STATUS "IPO disabled for time-trace builds")
else()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT MMLTK_IPO_SUPPORTED OUTPUT MMLTK_IPO_OUTPUT LANGUAGES C CXX)
    if(MMLTK_IPO_SUPPORTED)
        set(MMLTK_RELEASE_IPO_ENABLED ON)
        if(MMLTK_STATIC_ANALYSIS_BUILD)
            set(MMLTK_ANALYSIS_IPO_ENABLED ON)
        endif()
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

function(_mmltk_apply_linux_cxx_target_properties target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot configure unknown target `${target}`")
    endif()

    get_target_property(_mmltk_target_type "${target}" TYPE)
    if(_mmltk_target_type STREQUAL "INTERFACE_LIBRARY")
        message(FATAL_ERROR
            "Concrete C++ configuration requires a non-interface target")
    endif()

    set_target_properties("${target}" PROPERTIES
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
        COMPILE_WARNING_AS_ERROR "${MMLTK_WARNINGS_AS_ERRORS}"
        LINK_WARNING_AS_ERROR "${MMLTK_WARNINGS_AS_ERRORS}"
        INTERPROCEDURAL_OPTIMIZATION_DEV "${MMLTK_ANALYSIS_IPO_ENABLED}"
        INTERPROCEDURAL_OPTIMIZATION_RELEASE "${MMLTK_RELEASE_IPO_ENABLED}"
    )
    if(MMLTK_WARNINGS_AS_ERRORS)
        set_property(TARGET "${target}" APPEND PROPERTY LINK_OPTIONS
            "$<DEVICE_LINK:-Xnvlink=-Werror>")
    endif()

    if(_mmltk_target_type STREQUAL "EXECUTABLE" OR
       _mmltk_target_type STREQUAL "SHARED_LIBRARY" OR
       _mmltk_target_type STREQUAL "MODULE_LIBRARY")
        foreach(_mmltk_option IN LISTS MMLTK_RELEASE_LINK_OPTIONS)
            target_link_options("${target}" PRIVATE
                "$<$<CONFIG:Release>:${_mmltk_option}>")
        endforeach()
        foreach(_mmltk_option IN LISTS MMLTK_DEV_LINK_OPTIONS)
            target_link_options("${target}" PRIVATE
                "$<$<CONFIG:Dev>:${_mmltk_option}>")
        endforeach()
        target_link_options("${target}" PRIVATE
            "$<$<LINK_LANGUAGE:C,CXX>:$<HOST_LINK:-L${MMLTK_GCC16_RUNTIME_LIBDIR}>>")
        set_target_properties("${target}" PROPERTIES
            BUILD_RPATH "${MMLTK_GCC16_RUNTIME_LIBDIR}"
            INSTALL_RPATH "${MMLTK_GCC16_RUNTIME_LIBDIR}")
    endif()
endfunction()
