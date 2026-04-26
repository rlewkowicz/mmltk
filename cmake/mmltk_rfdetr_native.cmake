option(BUILD_RFDETR_NATIVE "Build native RF-DETR validation backends" ON)
option(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER
    "Enable embedded Python parsing of upstream RF-DETR .pth checkpoints"
    ${BUILD_RFDETR_NATIVE})

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

if(MMLTK_NEEDS_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE
        "${CMAKE_SOURCE_DIR}/utilities/rfdetr_checkpoint_bridge.py")
    if(NOT EXISTS "${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}")
        message(FATAL_ERROR
            "Missing RF-DETR Python checkpoint bridge: ${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}")
    endif()
endif()

function(mmltk_configure_rfdetr_native)
    mmltk_pin_system_protobuf()
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
    mmltk_resolve_torch_library_dir(TORCH_LIBRARY_DIR)
    get_filename_component(MMLTK_TORCH_LIBRARY_DIR_REAL "${TORCH_LIBRARY_DIR}" REALPATH)
    if(MMLTK_TORCH_ROOT)
        get_filename_component(MMLTK_TORCH_ROOT_REAL "${MMLTK_TORCH_ROOT}" REALPATH)
    else()
        set(MMLTK_TORCH_ROOT_REAL "")
    endif()
    separate_arguments(TORCH_CXX_FLAGS_LIST NATIVE_COMMAND "${TORCH_CXX_FLAGS}")
    set(MMLTK_TORCH_EXTRA_LIBRARIES "")
    mmltk_find_nccl_library(MMLTK_NCCL_LIBRARY)
    if(MMLTK_NCCL_LIBRARY)
        list(APPEND MMLTK_TORCH_EXTRA_LIBRARIES "${MMLTK_NCCL_LIBRARY}")
    elseif(EXISTS "${TORCH_LIBRARY_DIR}/libtorch_cuda.so")
        message(FATAL_ERROR
            "Torch resolved libtorch_cuda.so from ${TORCH_LIBRARY_DIR}, but libnccl.so was not found. "
            "Install NCCL runtime libraries or expose them on the library search path before configuring.")
    endif()
    target_compile_options(mmltk_core PUBLIC ${TORCH_CXX_FLAGS_LIST})
    target_compile_options(mmltk_cli PRIVATE ${TORCH_CXX_FLAGS_LIST})

    target_sources(mmltk_core PRIVATE
        src/dataset/compile/dataset_compiler_cuda.cu
    )
    set_target_properties(mmltk_core PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
    )

    mmltk_find_onnxruntime(ONNXRUNTIME_INCLUDE_DIR ONNXRUNTIME_LIBRARY ONNXRUNTIME_LIBRARY_DIR)
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR
            "ONNX Runtime C++ GPU libraries were not found. Install the official onnxruntime GPU package "
            "and set MMLTK_ONNXRUNTIME_ROOT before building.")
    endif()
    mmltk_find_tensorrt(TENSORRT_INCLUDE_DIR TENSORRT_NVINFER_LIBRARY TENSORRT_NVONNXPARSER_LIBRARY)
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
        set_source_files_properties(src/rfdetr/layers/ms_deform_attn_op.cpp PROPERTIES
            COMPILE_OPTIONS "-Wno-stringop-overflow;-Wno-null-dereference;-Wno-array-bounds")
    endif()
    set_target_properties(mmltk_rfdetr_cuda_ops PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        CUDA_SEPARABLE_COMPILATION ON
        INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
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
        PRIVATE
            CUDA::cudart
            nlohmann_json
    )
    mmltk_prefer_system_protobuf_headers(mmltk_export_onnx PRIVATE)
    mmltk_enable_torch(mmltk_export_onnx PUBLIC)
    target_compile_definitions(mmltk_export_onnx PRIVATE ONNX_ML=1)
    set_target_properties(mmltk_export_onnx PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    mmltk_configure_target_pch(mmltk_export_onnx
        HEADER src/pch/rfdetr_native.hpp)
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
    mmltk_configure_target_pch(mmltk_rfdetr_torch
        HEADER src/pch/rfdetr_native.hpp)

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
    mmltk_configure_target_pch(mmltk_rfdetr_runtime
        HEADER src/pch/rfdetr_native.hpp)

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
    set(MMLTK_RFDETR_LIVE_CAPTURE_ENABLED ON PARENT_SCOPE)
endfunction()
