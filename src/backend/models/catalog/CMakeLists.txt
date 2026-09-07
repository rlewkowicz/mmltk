include(MmltkComponent)

add_library(mmltk_backend_models_catalog STATIC)
add_library(
    mmltk::backend::models::catalog
    ALIAS mmltk_backend_models_catalog
)
mmltk_configure_component(
    mmltk_backend_models_catalog PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_backend_models_catalog
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        artifacts.h
        module.h
        model_registry.h)
mmltk_register_ordinary_sources(
    mmltk_backend_models_catalog
    module.cpp
    model_registry.cpp
)
target_link_libraries(mmltk_backend_models_catalog
    PUBLIC
        mmltk::frameworks::reflection
    PRIVATE
        mmltk::backend::models::rfdetr::contract
)

function(mmltk_add_backend_models_catalog_tests)
    add_executable(mmltk_backend_models_catalog_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_models_catalog_tests PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_backend_models_catalog_tests
        ${CMAKE_SOURCE_DIR}/src/backend/models/catalog/tests/model_registry.test.cpp)
    target_link_libraries(mmltk_backend_models_catalog_tests PRIVATE
        mmltk_acceptance_support
        mmltk::backend::models::catalog
    )
    catch_discover_tests(
        mmltk_backend_models_catalog_tests
        TEST_PREFIX "backend::models::catalog::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
