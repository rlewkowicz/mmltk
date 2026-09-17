include(MmltkComponent)

add_library(mmltk_controller_live_system STATIC)
add_library(mmltk::controller::live_system ALIAS
    mmltk_controller_live_system)
mmltk_configure_component(
    mmltk_controller_live_system PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_live_system
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES live_system.h)
mmltk_register_ordinary_sources(
    mmltk_controller_live_system
    native_live_algorithm.cpp
    live_receiver_copy.cpp
    live_system.cpp)
mmltk_register_private_headers(
    mmltk_controller_live_system live_receiver_copy.h)
target_link_libraries(mmltk_controller_live_system
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
    PRIVATE
        mmltk::controller::visual_runtime
        mmltk::backend::media::capture
        mmltk::backend::media::live)
mmltk_link_declarations(mmltk_controller_live_system PUBLIC
    mmltk::controller::visual_runtime)
mmltk_register_header_isolation(mmltk_controller_live_system)

add_library(mmltk_controller_live_test_support INTERFACE)
mmltk_register_public_headers(mmltk_controller_live_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/support/live_system_fixture.h)
target_link_libraries(mmltk_controller_live_test_support INTERFACE mmltk_controller_visual_fixture mmltk::controller::live_system)

function(mmltk_add_controller_live_tests)
    add_executable(mmltk_controller_live_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_controller_live_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_controller_live_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/live_system.test.cpp)
    target_link_libraries(mmltk_controller_live_tests PRIVATE
        mmltk_controller_live_test_support )
    catch_discover_tests(mmltk_controller_live_tests
        TEST_PREFIX "controller::live::" DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()
