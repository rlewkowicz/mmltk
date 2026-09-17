include(MmltkComponent)

add_library(mmltk_common_concurrency STATIC)
add_library(mmltk::common::concurrency ALIAS mmltk_common_concurrency)
mmltk_configure_component(mmltk_common_concurrency PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_common_concurrency
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        event_cancellation.h
        cancellation_observation.h
        parallel_range.h
        worker_pool.h
)
mmltk_register_ordinary_sources(
    mmltk_common_concurrency
    event_cancellation.cpp
    worker_pool.cpp)

target_link_libraries(mmltk_common_concurrency
    PUBLIC
        mmltk::common::io
        mmltk::common::math
        mmltk::common::system
        mmltk::common::types
        Threads::Threads
)

function(mmltk_add_common_concurrency_tests)
    add_executable(mmltk_common_concurrency_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_common_concurrency_tests
        ${CMAKE_SOURCE_DIR}/src/common/concurrency/tests/worker_pool.test.cpp
        ${CMAKE_SOURCE_DIR}/src/common/concurrency/tests/cancellation_observation.test.cpp
    )
    mmltk_configure_component(
        mmltk_common_concurrency_tests PRIVATE_DECLARATIONS)
    target_link_libraries(mmltk_common_concurrency_tests
        PRIVATE
            mmltk_test_support
            mmltk_common_system_test_support
            mmltk::common::concurrency
            mmltk::common::system
    )
    catch_discover_tests(
        mmltk_common_concurrency_tests
        TEST_PREFIX "common::concurrency::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
