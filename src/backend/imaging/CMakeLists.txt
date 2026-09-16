include(MmltkComponent)
add_library(mmltk_backend_imaging_primitives INTERFACE)
add_library(mmltk::backend::imaging::primitives ALIAS mmltk_backend_imaging_primitives)
mmltk_register_public_headers(mmltk_backend_imaging_primitives
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES sampling.h raster/class_palette.h)
target_link_libraries(mmltk_backend_imaging_primitives INTERFACE mmltk_cxx26_dialect CUDA::cudart)
add_subdirectory(resample)
