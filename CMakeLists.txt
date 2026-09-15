cmake_minimum_required(VERSION 4.4.3)

if(NOT CMAKE_VERSION VERSION_EQUAL "4.4.3")
    message(FATAL_ERROR
        "mmltk requires exactly CMake 4.4.3; resolved ${CMAKE_VERSION}")
endif()

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if(POLICY CMP0138)
    cmake_policy(SET CMP0138 NEW)
endif()

project(mmltk LANGUAGES C CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MmltkOptions)
include(MmltkToolchain)
include(MmltkComponent)
include(MmltkSourceRegistration)

add_library(mmltk_cxx26_dialect INTERFACE)
# The CXX feature changes only C++ translation units; CUDA inherits C++23.
target_compile_features(mmltk_cxx26_dialect INTERFACE cxx_std_26)
target_compile_options(mmltk_cxx26_dialect INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-freflection>)

add_library(stb STATIC
    third_party/stb/stb_image.cpp
    third_party/stb/stb_image_write.cpp
)
mmltk_configure_foreign_target(stb)
target_include_directories(stb SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include")

set(MMLTK_AVIR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/avir")
if(NOT EXISTS "${MMLTK_AVIR_SOURCE_DIR}/avir.h")
    message(FATAL_ERROR
        "Missing vendored AVIR checkout at ${MMLTK_AVIR_SOURCE_DIR}")
endif()
add_library(avir INTERFACE)
target_include_directories(avir SYSTEM INTERFACE
    "${MMLTK_AVIR_SOURCE_DIR}")

set(LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB ON CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_WITH_RPMALLOC OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_WITH_ISAL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/rapidgzip EXCLUDE_FROM_ALL)

if(BUILD_RFDETR_NATIVE)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR
       CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        set(_mmltk_cuda_architectures "${MMLTK_CUDA_ARCHITECTURES}")
        if(_mmltk_cuda_architectures STREQUAL "")
            set(_mmltk_cuda_architectures
                "${MMLTK_DEFAULT_CUDA_ARCHITECTURES}")
        endif()
        set(CMAKE_CUDA_ARCHITECTURES
            "${_mmltk_cuda_architectures}" CACHE STRING
            "CUDA architectures" FORCE)
    endif()
    enable_language(CUDA)
endif()

add_subdirectory(src/common/math)
add_subdirectory(src/common/types)
add_subdirectory(src/common/system)
add_subdirectory(src/common/concurrency)
add_subdirectory(src/common/io)
add_subdirectory(src/common/logging)
add_subdirectory(src/frameworks/process)
add_subdirectory(src/frameworks/reflection)
add_subdirectory(src/frameworks/serialization)
add_subdirectory(src/frameworks/transport)
include(MmltkGdrCopy)
add_subdirectory(src/frameworks/gpu)
add_subdirectory(src/backend/data/catalog)
add_subdirectory(src/backend/data)

if(BUILD_RFDETR_NATIVE)
    add_subdirectory(src/backend/ml/torch)
    add_subdirectory(src/backend/ml/cuda)
    add_subdirectory(src/backend/ml/layers)
    add_subdirectory(src/backend/ml/ops)
endif()
add_subdirectory(src/backend/ml/runtime)
add_subdirectory(src/backend/models/rfdetr/contract)
add_subdirectory(src/backend/models/catalog)

if(BUILD_RFDETR_NATIVE)
    add_subdirectory(src/backend/imaging/annotation)
    add_subdirectory(src/backend/imaging/explore)
    add_subdirectory(src/backend/imaging/raster)
    add_subdirectory(src/backend/models/rfdetr/core)
    add_subdirectory(src/backend/models/rfdetr/augmentation)
    add_subdirectory(src/backend/models/rfdetr/export)
    add_subdirectory(src/backend/media/video)
    add_subdirectory(src/backend/models/rfdetr/inference)
    add_subdirectory(src/backend/models/rfdetr/training)
    add_subdirectory(src/backend/imaging/upscale)
    add_subdirectory(src/backend/media/capture)
    add_subdirectory(src/backend/media/live)
endif()

add_subdirectory(src/controller/contracts)
add_subdirectory(src/controller/services)

if(BUILD_RFDETR_NATIVE)
    add_subdirectory(src/controller/subsystems/system)
    add_subdirectory(src/controller/subsystems/annotation)
    add_subdirectory(src/controller/subsystems/explore)
    add_subdirectory(src/controller/subsystems/live)
    add_subdirectory(src/controller/subsystems/upscale)
endif()

add_subdirectory(src/controller/presentation)
if(BUILD_RFDETR_NATIVE)
    add_subdirectory(src/controller)
endif()
add_subdirectory(src/controller/browser)
if(BUILD_RFDETR_NATIVE)
    add_subdirectory(src/controller/shell)
endif()
add_subdirectory(src/frontend/iced)
add_subdirectory(src/entrypoints/desktop)
add_subdirectory(src/entrypoints/cli)
add_subdirectory(src/entrypoints/tools)
add_subdirectory(src/acceptance)

# Target registration is authoritative for generated first-party sources too.
get_property(_mmltk_first_party_targets GLOBAL PROPERTY
    MMLTK_CONFIGURED_CONCRETE_TARGETS)
string(REPLACE ";" "\n" _mmltk_first_party_targets "${_mmltk_first_party_targets}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/mmltk-first-party-targets.txt"
    CONTENT "${_mmltk_first_party_targets}\n")

install(FILES
    "${MMLTK_GCC16_LIBSTDCXX_RUNTIME}"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/gcc-16.2"
    RENAME "libstdc++.so.6")
install(FILES
    "${MMLTK_GCC16_LIBGCC_RUNTIME}"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/gcc-16.2"
    RENAME "libgcc_s.so.1")
