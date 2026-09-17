include(MmltkComponent)

add_library(mmltk_controller_explore_system STATIC)
add_library(mmltk::controller::explore_system ALIAS
    mmltk_controller_explore_system)
mmltk_configure_component(
    mmltk_controller_explore_system PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_explore_system
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES explore_system.h)
mmltk_register_ordinary_sources(
    mmltk_controller_explore_system explore_system.cpp native_explore_storage.cpp gallery_thumbnail_cache.cpp gallery_atlas.cpp)
mmltk_register_private_headers(
    mmltk_controller_explore_system native_explore_storage.h detail/gallery_stream.h detail/gallery_thumbnail_cache.h detail/gallery_atlas.h
    detail/gallery_read_scheduler.h detail/gallery_descriptor_storage.h detail/gallery_stream_probe.h
    detail/gallery_payload.h detail/gallery_buffer_family.h detail/gallery_host_allocations.h)
mmltk_register_retained_module_importers(
    mmltk_controller_explore_system native_explore_algorithm.cpp gallery_stream.cpp gallery_read_scheduler.cpp gallery_descriptor_storage.cpp gallery_stream_probe.cpp)
target_link_libraries(mmltk_controller_explore_system
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::contracts
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
    PRIVATE
        mmltk::common::math
        mmltk::frameworks::serialization
        mmltk::controller::visual_runtime
        mmltk::backend::data
        mmltk::backend::imaging::primitives
        mmltk::backend::imaging::explore
        mmltk::backend::models::rfdetr::augmentation
        mmltk::common::concurrency
        mmltk::common::io
        mmltk::common::system
        mmltk::controller::direct_services)
mmltk_link_declarations(mmltk_controller_explore_system PUBLIC
    mmltk::controller::visual_runtime)
mmltk_register_header_isolation(mmltk_controller_explore_system)

add_library(mmltk_controller_explore_test_support INTERFACE)
mmltk_register_public_headers(mmltk_controller_explore_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/support/explore_system_fixture.h)
target_link_libraries(mmltk_controller_explore_test_support INTERFACE mmltk_controller_visual_fixture mmltk::controller::explore_system)

function(mmltk_add_controller_explore_tests)
    add_executable(mmltk_controller_explore_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_controller_explore_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_controller_explore_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/explore_system.test.cpp)
    target_link_libraries(mmltk_controller_explore_tests PRIVATE
        mmltk_controller_explore_test_support )
    catch_discover_tests(mmltk_controller_explore_tests
        TEST_PREFIX "controller::explore::" DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()
