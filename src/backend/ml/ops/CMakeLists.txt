include(MmltkComponent)

add_library(mmltk_backend_ml_ops STATIC)
add_library(mmltk::backend::ml::ops ALIAS mmltk_backend_ml_ops)
mmltk_configure_component(mmltk_backend_ml_ops PUBLIC_DECLARATIONS)
mmltk_register_ordinary_sources(mmltk_backend_ml_ops
    box_iou.cu)
target_include_directories(mmltk_backend_ml_ops PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mmltk_backend_ml_ops
    PRIVATE
        mmltk::frameworks::gpu
        CUDA::cudart
)
mmltk_enable_torch(mmltk_backend_ml_ops PUBLIC)
set_target_properties(mmltk_backend_ml_ops PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_SEPARABLE_COMPILATION ON
)

mmltk_register_public_headers(mmltk_backend_ml_ops BASE_DIRS ${CMAKE_SOURCE_DIR} FILES box_iou.h)
