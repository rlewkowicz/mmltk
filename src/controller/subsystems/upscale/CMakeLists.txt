include(MmltkComponent)

add_library(mmltk_controller_upscale_system STATIC)
add_library(mmltk::controller::upscale_system ALIAS
    mmltk_controller_upscale_system)
mmltk_configure_component(
    mmltk_controller_upscale_system PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_upscale_system
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES upscale_system.h)
mmltk_register_retained_module_importers(
    mmltk_controller_upscale_system upscale_system.cpp)
target_link_libraries(mmltk_controller_upscale_system
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
    PRIVATE
        mmltk::controller::visual_runtime
        mmltk::backend::imaging::raster
        mmltk::backend::imaging::upscale)
mmltk_link_declarations(mmltk_controller_upscale_system PUBLIC
    mmltk::controller::visual_runtime
    mmltk::backend::imaging::upscale)
mmltk_register_header_isolation(mmltk_controller_upscale_system)

add_library(mmltk_controller_upscale_test_support INTERFACE)
mmltk_register_public_headers(mmltk_controller_upscale_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/support/upscale_system_fixture.h)
target_link_libraries(mmltk_controller_upscale_test_support INTERFACE mmltk_controller_visual_fixture mmltk::controller::upscale_system )

function(mmltk_add_controller_upscale_tests)
    add_executable(mmltk_controller_upscale_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_controller_upscale_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_controller_upscale_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/upscale_system.test.cpp)
    target_link_libraries(mmltk_controller_upscale_tests PRIVATE
        mmltk_controller_upscale_test_support mmltk::backend::ml::runtime mmltk_controller_explore_test_support
        mmltk::vendor::onnxruntime
        mmltk::controller::data_compute_systems)
    catch_discover_tests(mmltk_controller_upscale_tests
        TEST_PREFIX "controller::upscale::" DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()

mmltk_register_header_isolation(mmltk_controller_upscale_test_support)
