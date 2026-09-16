include(MmltkComponent)

add_library(mmltk_frameworks_process STATIC)
add_library(mmltk::frameworks::process ALIAS mmltk_frameworks_process)
mmltk_configure_component(mmltk_frameworks_process PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_frameworks_process
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES subprocess_utils.h
)
mmltk_register_ordinary_sources(mmltk_frameworks_process
    subprocess_utils.cpp)

target_link_libraries(mmltk_frameworks_process
    PUBLIC
        mmltk::common::io
        mmltk::common::system
        mmltk::common::types
)

function(mmltk_add_frameworks_process_tests)
    add_executable(mmltk_frameworks_process_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_frameworks_process_tests
        ${CMAKE_SOURCE_DIR}/src/frameworks/process/tests/subprocess_utils.test.cpp
    )
    mmltk_configure_component(
        mmltk_frameworks_process_tests PRIVATE_DECLARATIONS)
    target_link_libraries(mmltk_frameworks_process_tests PRIVATE
        mmltk_test_support
        mmltk::frameworks::process
    )
    catch_discover_tests(
        mmltk_frameworks_process_tests
        TEST_PREFIX "frameworks::process::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
