include(MmltkComponent)

add_library(mmltk_shiftlut_onnx_ops STATIC)
mmltk_configure_component(mmltk_shiftlut_onnx_ops PRIVATE_DECLARATIONS)
mmltk_register_ordinary_sources(mmltk_shiftlut_onnx_ops
    shiftlut_onnx_ops.cpp shiftlut_lookup_cuda.cu)
mmltk_register_public_headers(mmltk_shiftlut_onnx_ops
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES detail/shiftlut_onnx_ops.h)
target_include_directories(mmltk_shiftlut_onnx_ops PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mmltk_shiftlut_onnx_ops PUBLIC
    CUDA::cudart mmltk::vendor::onnxruntime)
set_target_properties(mmltk_shiftlut_onnx_ops PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_SEPARABLE_COMPILATION ON)
mmltk_register_header_isolation(mmltk_shiftlut_onnx_ops)

find_package(ONNX CONFIG REQUIRED PATHS "${MMLTK_ONNX_ROOT}" NO_DEFAULT_PATH)
find_package(Protobuf REQUIRED)
add_executable(mmltk_shiftlut_model_generator)
mmltk_configure_component(mmltk_shiftlut_model_generator PRIVATE_DECLARATIONS)
mmltk_register_ordinary_sources(mmltk_shiftlut_model_generator shiftlut_model_generator.cpp)
target_compile_definitions(mmltk_shiftlut_model_generator PRIVATE ONNX_ML=1)
target_link_libraries(mmltk_shiftlut_model_generator PRIVATE
    ONNX::onnx "${Protobuf_LIBRARY}" mmltk_shiftlut_onnx_ops)
mmltk_enable_native_runtime_rpath(mmltk_shiftlut_model_generator)

add_library(mmltk_backend_imaging_upscale STATIC)
add_library(
    mmltk::backend::imaging::upscale
    ALIAS mmltk_backend_imaging_upscale
)
mmltk_configure_component(
    mmltk_backend_imaging_upscale PUBLIC_DECLARATIONS)

mmltk_register_retained_module_providers(
    mmltk_backend_imaging_upscale image_upscaler.cppm)
mmltk_register_retained_module_implementation_units(
    mmltk_backend_imaging_upscale
    image_upscaler.cpp
    image_upscaler_onnx.cpp
    image_upscaler_runtime.cpp
    image_upscaler_tensorrt.cpp
)
mmltk_register_ordinary_sources(
    mmltk_backend_imaging_upscale
    image_upscaler_cuda.cu
    image_upscaler_nis.cu)
mmltk_register_private_headers(
    mmltk_backend_imaging_upscale
    detail/image_upscaler_cuda.h
    detail/image_upscaler_internal.h
    detail/image_upscaler_request_tile.h
    detail/image_upscaler_nis.h
    detail/image_upscaler_nis_coefficients.h
)
mmltk_register_public_headers(mmltk_backend_imaging_upscale
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES upscale_execution.h)
target_sources(mmltk_backend_imaging_upscale PRIVATE
    export_shiftlut_lut_onnx.py
    assets/ShiftLUT_fp32.onnx
    assets/RealPLKSR_fp16.onnx
)
target_include_directories(mmltk_backend_imaging_upscale PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mmltk_backend_imaging_upscale
    PRIVATE
        mmltk::backend::ml::runtime
        CUDA::cudart
        mmltk::backend::data
        mmltk::common::logging
        mmltk::common::system
        mmltk::common::types
        mmltk::frameworks::gpu
        mmltk::vendor::onnxruntime
        mmltk::vendor::tensorrt_nvinfer
        mmltk_shiftlut_onnx_ops
)
mmltk_link_declarations(mmltk_backend_imaging_upscale PUBLIC
    mmltk::frameworks::gpu)
# Ordinary declarations are checked by the ordinary C++ isolation target.
# The device-only implementation header belongs to NVCC's source set below.
mmltk_register_header_isolation(mmltk_backend_imaging_upscale)
mmltk_register_private_headers(mmltk_backend_imaging_upscale
    detail/image_upscaler_nis_device.cuh)
set_target_properties(mmltk_backend_imaging_upscale PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_SEPARABLE_COMPILATION ON
)

install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/ShiftLUT_fp32.onnx"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/RealPLKSR_fp16.onnx"
    DESTINATION "models"
)

function(mmltk_add_backend_imaging_upscale_tests)
    add_executable(mmltk_backend_imaging_upscale_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_backend_imaging_upscale_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_backend_imaging_upscale_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/shiftlut_lookup.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/shiftlut_reference.cu)
    mmltk_register_private_headers(mmltk_backend_imaging_upscale_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/shiftlut_reference.h)
    set_target_properties(mmltk_backend_imaging_upscale_tests PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_RESOLVE_DEVICE_SYMBOLS ON CUDA_SEPARABLE_COMPILATION ON)
    target_link_libraries(mmltk_backend_imaging_upscale_tests PRIVATE
        mmltk_acceptance_support mmltk::backend::imaging::upscale
        mmltk_shiftlut_onnx_ops CUDA::cudart)
    target_compile_definitions(mmltk_backend_imaging_upscale_tests PRIVATE
        MMLTK_TEST_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
    catch_discover_tests(mmltk_backend_imaging_upscale_tests
        TEST_PREFIX "backend::imaging::upscale::"
        DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()
