include(MmltkComponent)

add_library(mmltk_backend_imaging_raster STATIC)
add_library(
    mmltk::backend::imaging::raster
    ALIAS mmltk_backend_imaging_raster
)
mmltk_configure_component(
    mmltk_backend_imaging_raster PUBLIC_DECLARATIONS)

mmltk_register_retained_module_providers(
    mmltk_backend_imaging_raster raster.cppm)
mmltk_register_retained_module_implementation_units(
    mmltk_backend_imaging_raster raster.cpp)
mmltk_register_ordinary_sources(
    mmltk_backend_imaging_raster
    chw_image.cu
    raster_cuda.cu
    mask_pack_cuda.cu)
mmltk_register_public_headers(mmltk_backend_imaging_raster BASE_DIRS ${CMAKE_SOURCE_DIR} FILES chw_image.h)
mmltk_register_private_headers(
    mmltk_backend_imaging_raster
    detail/mask_pack_cuda_abi.h
    detail/raster_color.h
    detail/raster_cuda_abi.h
)
target_include_directories(mmltk_backend_imaging_raster PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mmltk_backend_imaging_raster
    PRIVATE
        CUDA::cudart
        mmltk::backend::ml::cuda
)
set_target_properties(mmltk_backend_imaging_raster PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
)
