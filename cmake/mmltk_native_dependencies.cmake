set(MMLTK_SYSTEM_LIBRARY_HINT_DIRS
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib
    /usr/local/lib64
)
set(MMLTK_SYSTEM_LIBRARY_HINT_DIRS_WITH_LIB64
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib
    /usr/local/lib64
    /usr/lib64
    /usr/lib
)
set(MMLTK_TENSORRT_INCLUDE_HINT_DIRS
    /usr/include
    /usr/include/x86_64-linux-gnu
    /usr/local/include
    /opt/TensorRT/include
    /opt/tensorrt/include
)
set(MMLTK_TENSORRT_LIBRARY_HINT_DIRS
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib
    /usr/local/lib64
    /opt/TensorRT/lib
    /opt/tensorrt/lib
)
set(MMLTK_ONNXRUNTIME_INCLUDE_HINT_DIRS
    /opt/onnxruntime/include
    /usr/local/include
    /usr/include
)
set(MMLTK_ONNXRUNTIME_LIBRARY_HINT_DIRS
    /opt/onnxruntime/lib
    /usr/local/lib
    /usr/lib/x86_64-linux-gnu
)

function(mmltk_find_path_in_dirs out_var)
    set(options NO_DEFAULT_PATH)
    set(oneValueArgs)
    set(multiValueArgs NAMES HINTS)
    cmake_parse_arguments(MMLTK_FIND_PATH "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT MMLTK_FIND_PATH_NAMES)
        message(FATAL_ERROR "mmltk_find_path_in_dirs requires NAMES")
    endif()
    if(MMLTK_FIND_PATH_NO_DEFAULT_PATH)
        find_path(${out_var}
            NAMES ${MMLTK_FIND_PATH_NAMES}
            HINTS ${MMLTK_FIND_PATH_HINTS}
            NO_DEFAULT_PATH)
    else()
        find_path(${out_var}
            NAMES ${MMLTK_FIND_PATH_NAMES}
            HINTS ${MMLTK_FIND_PATH_HINTS})
    endif()
    set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

function(mmltk_find_library_in_dirs out_var)
    set(options NO_DEFAULT_PATH)
    set(oneValueArgs)
    set(multiValueArgs NAMES HINTS)
    cmake_parse_arguments(MMLTK_FIND_LIBRARY "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT MMLTK_FIND_LIBRARY_NAMES)
        message(FATAL_ERROR "mmltk_find_library_in_dirs requires NAMES")
    endif()
    if(MMLTK_FIND_LIBRARY_NO_DEFAULT_PATH)
        find_library(${out_var}
            NAMES ${MMLTK_FIND_LIBRARY_NAMES}
            HINTS ${MMLTK_FIND_LIBRARY_HINTS}
            NO_DEFAULT_PATH)
    else()
        find_library(${out_var}
            NAMES ${MMLTK_FIND_LIBRARY_NAMES}
            HINTS ${MMLTK_FIND_LIBRARY_HINTS})
    endif()
    set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

function(mmltk_resolve_imported_target_library_dir out_var target)
    set(_mmltk_target_library_dir "")
    foreach(_mmltk_location_property IN ITEMS
            IMPORTED_LOCATION_RELEASE
            IMPORTED_LOCATION_RELWITHDEBINFO
            IMPORTED_LOCATION_DEBUG
            IMPORTED_LOCATION)
        get_target_property(_mmltk_imported_location ${target} ${_mmltk_location_property})
        if(_mmltk_imported_location AND EXISTS "${_mmltk_imported_location}")
            get_filename_component(_mmltk_target_library_dir "${_mmltk_imported_location}" DIRECTORY)
            break()
        endif()
    endforeach()
    set(${out_var} "${_mmltk_target_library_dir}" PARENT_SCOPE)
endfunction()

function(mmltk_resolve_torch_library_dir out_var)
    set(_mmltk_torch_library_dir "")
    if(DEFINED TORCH_INSTALL_PREFIX AND NOT TORCH_INSTALL_PREFIX STREQUAL "")
        get_filename_component(_mmltk_torch_library_dir "${TORCH_INSTALL_PREFIX}/lib" ABSOLUTE)
    elseif(MMLTK_TORCH_ROOT)
        get_filename_component(_mmltk_torch_library_dir "${MMLTK_TORCH_ROOT}/lib" ABSOLUTE)
    elseif(DEFINED Torch_DIR AND NOT Torch_DIR STREQUAL "")
        get_filename_component(_mmltk_torch_library_dir "${Torch_DIR}/../../../lib" ABSOLUTE)
    else()
        foreach(_mmltk_torch_library IN LISTS TORCH_LIBRARIES)
            if(IS_ABSOLUTE "${_mmltk_torch_library}")
                get_filename_component(_mmltk_torch_library_dir "${_mmltk_torch_library}" DIRECTORY)
                break()
            endif()
        endforeach()
    endif()
    if(NOT DEFINED _mmltk_torch_library_dir OR _mmltk_torch_library_dir STREQUAL "" OR
       NOT EXISTS "${_mmltk_torch_library_dir}")
        message(FATAL_ERROR
            "Failed to resolve the LibTorch library directory. "
            "Set MMLTK_TORCH_ROOT or Torch_DIR to your LibTorch install.")
    endif()
    set(${out_var} "${_mmltk_torch_library_dir}" PARENT_SCOPE)
endfunction()

function(mmltk_find_nccl_library out_var)
    if(NOT DEFINED TORCH_LIBRARY_DIR OR TORCH_LIBRARY_DIR STREQUAL "")
        message(FATAL_ERROR "mmltk_find_nccl_library requires TORCH_LIBRARY_DIR")
    endif()
    find_file(${out_var}
        NAMES libnccl.so.2
        HINTS
            "${TORCH_LIBRARY_DIR}/../../nvidia/nccl/lib"
            "${TORCH_LIBRARY_DIR}/../nvidia/nccl/lib"
            "${MMLTK_TORCH_ROOT_REAL}/../../nvidia/nccl/lib"
            "${MMLTK_TORCH_ROOT_REAL}/../nvidia/nccl/lib"
        NO_DEFAULT_PATH)
    if(NOT ${out_var})
        mmltk_find_library_in_dirs(${out_var}
            NAMES nccl libnccl.so.2
            HINTS
                ${MMLTK_SYSTEM_LIBRARY_HINT_DIRS}
                "${MMLTK_CUDA_TOOLKIT_ROOT}/lib64"
                "${MMLTK_CUDA_TOOLKIT_ROOT}/targets/x86_64-linux/lib")
    endif()
    set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

function(mmltk_pin_system_protobuf)
    mmltk_find_path_in_dirs(MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR
        NAMES google/protobuf/message.h
        HINTS
            /usr/include
            /usr/local/include
        NO_DEFAULT_PATH)
    mmltk_find_library_in_dirs(MMLTK_SYSTEM_PROTOBUF_LIBRARY
        NAMES protobuf libprotobuf
        HINTS
            ${MMLTK_SYSTEM_LIBRARY_HINT_DIRS_WITH_LIB64}
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
    set(MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR "${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}" PARENT_SCOPE)
    set(MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR "${MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR}" PARENT_SCOPE)
    set(MMLTK_SYSTEM_PROTOBUF_LIBRARY "${MMLTK_SYSTEM_PROTOBUF_LIBRARY}" PARENT_SCOPE)
endfunction()

function(mmltk_find_onnxruntime out_include_var out_library_var out_library_dir_var)
    mmltk_find_path_in_dirs(${out_include_var}
        NAMES onnxruntime_cxx_api.h
        HINTS
            ${MMLTK_ONNXRUNTIME_INCLUDE_HINT_DIRS}
            ${MMLTK_ONNXRUNTIME_ROOT}/include)
    mmltk_find_library_in_dirs(${out_library_var}
        NAMES onnxruntime
        HINTS
            ${MMLTK_ONNXRUNTIME_LIBRARY_HINT_DIRS}
            ${MMLTK_ONNXRUNTIME_ROOT}/lib)
    if(DEFINED ${out_library_var} AND NOT "${${out_library_var}}" STREQUAL "")
        get_filename_component(_mmltk_onnxruntime_library_dir "${${out_library_var}}" DIRECTORY)
        set(${out_library_dir_var} "${_mmltk_onnxruntime_library_dir}" PARENT_SCOPE)
    else()
        set(${out_library_dir_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(mmltk_find_tensorrt out_include_var out_nvinfer_var out_nvonnxparser_var)
    mmltk_find_path_in_dirs(${out_include_var}
        NAMES NvInfer.h
        HINTS
            ${MMLTK_TENSORRT_INCLUDE_HINT_DIRS})
    mmltk_find_library_in_dirs(${out_nvinfer_var}
        NAMES nvinfer
        HINTS
            ${MMLTK_TENSORRT_LIBRARY_HINT_DIRS})
    mmltk_find_library_in_dirs(${out_nvonnxparser_var}
        NAMES nvonnxparser
        HINTS
            ${MMLTK_TENSORRT_LIBRARY_HINT_DIRS})
endfunction()

function(mmltk_install_native_runtime_libraries onnx_root onnxruntime_root)
    file(GLOB MMLTK_ONNX_RUNTIME_LIBS
        "${onnx_root}/lib/libonnx*.so*")
    if(MMLTK_ONNX_RUNTIME_LIBS)
        install(FILES ${MMLTK_ONNX_RUNTIME_LIBS}
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    file(GLOB MMLTK_ONNXRUNTIME_RUNTIME_LIBS
        "${onnxruntime_root}/lib/libonnxruntime*.so*")
    if(MMLTK_ONNXRUNTIME_RUNTIME_LIBS)
        install(FILES ${MMLTK_ONNXRUNTIME_RUNTIME_LIBS}
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
endfunction()
