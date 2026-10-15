include(MmltkComponent)
add_library(mmltk_backend_data_catalog STATIC)
add_library(mmltk::backend::data::catalog ALIAS mmltk_backend_data_catalog)
mmltk_configure_component(mmltk_backend_data_catalog PUBLIC_DECLARATIONS)
mmltk_register_public_headers(mmltk_backend_data_catalog
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES class_catalog.h coco_catalog.h keypoint_schema.h keypoint_visibility.h)
mmltk_register_ordinary_sources(mmltk_backend_data_catalog class_catalog.cpp keypoint_schema.cpp)
target_link_libraries(mmltk_backend_data_catalog PUBLIC mmltk::frameworks::reflection)

mmltk_register_header_isolation(mmltk_backend_data_catalog)

add_library(mmltk_backend_data_catalog_test_support INTERFACE)
mmltk_register_public_headers(mmltk_backend_data_catalog_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/keypoint_schema_fixture.h)
target_link_libraries(mmltk_backend_data_catalog_test_support INTERFACE
    mmltk::backend::data::catalog)
mmltk_register_header_isolation(mmltk_backend_data_catalog_test_support)
