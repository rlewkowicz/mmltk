include(MmltkComponent)

set(MMLTK_TORCH_ROOT "${MMLTK_NVIDIA_torch_ROOT}" CACHE PATH
    "Torch installation prefix containing share/cmake/Torch")
set(MMLTK_INSTALL_TORCH_ROOT "${MMLTK_NVIDIA_torch_ROOT}" CACHE PATH
    "Runtime Torch root used for installed mmltk RPATHs")
set(MMLTK_DEFAULT_TORCH_CUDA_ARCH_LIST
    "8.6 8.7 8.9 9.0 10.0 10.3 11.0 12.0 12.1")
set(MMLTK_TORCH_CUDA_ARCH_LIST
    "${MMLTK_DEFAULT_TORCH_CUDA_ARCH_LIST}" CACHE STRING
    "Default TORCH_CUDA_ARCH_LIST for backend ML LibTorch consumers")

if(MMLTK_TORCH_CUDA_ARCH_LIST)
    set(TORCH_CUDA_ARCH_LIST "${MMLTK_TORCH_CUDA_ARCH_LIST}" CACHE STRING
        "Pinned CUDA architectures for LibTorch consumers" FORCE)
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
# Torch's transitive NVIDIA discovery uses the same normalized donor root.
list(PREPEND CMAKE_PREFIX_PATH ${MMLTK_NVIDIA_native_ROOT})
set(CUDNN_ROOT ${MMLTK_NVIDIA_native_ROOT} CACHE PATH "Normalized donor cuDNN root" FORCE)
set(CUSPARSELT_ROOT ${MMLTK_NVIDIA_native_ROOT} CACHE PATH "Normalized donor cuSPARSELt root" FORCE)

set(_mmltk_saved_cuda_architectures "")
if(DEFINED CMAKE_CUDA_ARCHITECTURES)
    if(NOT CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        set(_mmltk_saved_cuda_architectures
            "${CMAKE_CUDA_ARCHITECTURES}")
    endif()
    unset(CMAKE_CUDA_ARCHITECTURES CACHE)
    unset(CMAKE_CUDA_ARCHITECTURES)
endif()
find_package(Torch REQUIRED CONFIG
    PATHS "${MMLTK_TORCH_ROOT}/share/cmake/Torch" NO_DEFAULT_PATH)
get_filename_component(_mmltk_torch_prefix "${TORCH_INSTALL_PREFIX}" REALPATH)
get_filename_component(_mmltk_expected_torch_prefix "${MMLTK_TORCH_ROOT}" REALPATH)
if(NOT _mmltk_torch_prefix STREQUAL _mmltk_expected_torch_prefix)
    message(FATAL_ERROR
        "Torch must resolve from ${MMLTK_TORCH_ROOT}, found ${TORCH_INSTALL_PREFIX}")
endif()
if(NOT _mmltk_saved_cuda_architectures STREQUAL "")
    set(CMAKE_CUDA_ARCHITECTURES
        "${_mmltk_saved_cuda_architectures}" CACHE STRING
        "CUDA architectures" FORCE)
endif()

get_filename_component(_mmltk_torch_library_dir_real
    "${MMLTK_TORCH_ROOT}/lib" REALPATH)
set(_mmltk_torch_library_dir "${_mmltk_torch_library_dir_real}")
find_library(_mmltk_nccl_library
    NAMES nccl libnccl.so.2
    PATHS ${MMLTK_NVIDIA_native_ROOT}/lib
    NO_DEFAULT_PATH)
if(NOT _mmltk_nccl_library AND
   EXISTS "${_mmltk_torch_library_dir}/libtorch_cuda.so")
    message(FATAL_ERROR
        "LibTorch provides CUDA support, but libnccl.so was not found")
endif()

separate_arguments(
    _mmltk_torch_compile_options NATIVE_COMMAND "${TORCH_CXX_FLAGS}")
add_library(mmltk_backend_ml_torch_dependencies INTERFACE)
add_library(mmltk::backend::ml::torch_dependencies
    ALIAS mmltk_backend_ml_torch_dependencies)
target_include_directories(
    mmltk_backend_ml_torch_dependencies SYSTEM INTERFACE
    ${MMLTK_NVIDIA_native_ROOT}/include
    ${TORCH_INCLUDE_DIRS})
target_compile_options(
    mmltk_backend_ml_torch_dependencies INTERFACE
    ${_mmltk_torch_compile_options})
target_link_libraries(
    mmltk_backend_ml_torch_dependencies INTERFACE
    stdc++
    ${TORCH_LIBRARIES})
if(_mmltk_nccl_library)
    target_link_libraries(
        mmltk_backend_ml_torch_dependencies INTERFACE
        "${_mmltk_nccl_library}")
endif()
mmltk_register_runtime_library_directory(
    "${_mmltk_torch_library_dir_real}"
    INSTALL_DIRECTORY "${MMLTK_INSTALL_TORCH_ROOT}/lib")

mmltk_register_runtime_library_directory(
    ${MMLTK_NVIDIA_native_ROOT}/lib INSTALL_DIRECTORY ${MMLTK_NVIDIA_native_ROOT}/lib)

add_library(mmltk_backend_ml_torch STATIC)
add_library(mmltk::backend::ml::torch ALIAS mmltk_backend_ml_torch)
mmltk_configure_component(
    mmltk_backend_ml_torch PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_backend_ml_torch
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES archive.h scalar_type.h)
mmltk_register_ordinary_sources(
    mmltk_backend_ml_torch torch_toolchain_check.cpp)
target_link_libraries(mmltk_backend_ml_torch
    PUBLIC
        mmltk::backend::ml::torch_dependencies
)

function(mmltk_enable_torch target visibility)
    target_link_libraries("${target}" "${visibility}"
        mmltk::backend::ml::torch)
endfunction()
