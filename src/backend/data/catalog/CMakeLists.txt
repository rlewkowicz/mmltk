include(MmltkComponent)
add_library(mmltk_backend_data_catalog STATIC)
add_library(mmltk::backend::data::catalog ALIAS mmltk_backend_data_catalog)
mmltk_configure_component(mmltk_backend_data_catalog PUBLIC_DECLARATIONS)
mmltk_register_public_headers(mmltk_backend_data_catalog
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES class_catalog.h coco_catalog.h)
mmltk_register_ordinary_sources(mmltk_backend_data_catalog class_catalog.cpp)
target_link_libraries(mmltk_backend_data_catalog PUBLIC mmltk::frameworks::reflection)
