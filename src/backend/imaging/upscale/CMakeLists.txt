include(MmltkComponent)

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
