include(MmltkComponent)

add_library(mmltk_controller_annotation_system STATIC)
add_library(mmltk::controller::annotation_system ALIAS
    mmltk_controller_annotation_system)
mmltk_configure_component(
    mmltk_controller_annotation_system PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_annotation_system
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        annotation_system.h)
mmltk_register_ordinary_sources(
    mmltk_controller_annotation_system
    annotation_mask.cpp
    annotation_document.cpp
    annotation_reducer.cpp
    annotation_system.cpp)
mmltk_register_private_headers(
    mmltk_controller_annotation_system
    detail/annotation_mask.h
    detail/annotation_document.h
    detail/annotation_render_state.h
    detail/annotation_atomic_save.h)
mmltk_register_retained_module_importers(
    mmltk_controller_annotation_system
    native_annotation_algorithm.cpp)
target_link_libraries(mmltk_controller_annotation_system
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::contracts
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
    PRIVATE
        mmltk::controller::visual_runtime
        mmltk::backend::imaging::raster
        stb)
mmltk_link_declarations(mmltk_controller_annotation_system PUBLIC
    mmltk::controller::visual_runtime)
mmltk_register_header_isolation(mmltk_controller_annotation_system)

function(mmltk_add_controller_annotation_tests)
    add_executable(mmltk_controller_annotation_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_controller_annotation_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(
        mmltk_controller_annotation_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/annotation_document.test.cpp)
    target_link_libraries(mmltk_controller_annotation_tests PRIVATE
        mmltk_acceptance_support
        mmltk::controller::annotation_system)
    catch_discover_tests(
        mmltk_controller_annotation_tests
        TEST_PREFIX "controller::annotation::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS)
endfunction()
