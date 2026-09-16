include(MmltkComponent)

find_package(ZLIB REQUIRED)

add_library(mmltk_frameworks_transport STATIC)
add_library(mmltk::frameworks::transport ALIAS mmltk_frameworks_transport)
mmltk_configure_component(mmltk_frameworks_transport PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_frameworks_transport
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        browser_record_ring.h
        browser_server.h)
mmltk_register_ordinary_sources(
    mmltk_frameworks_transport
    browser_record_ring.cpp
    browser_server.cpp)
target_sources(mmltk_frameworks_transport PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/uSockets/src/bsd.c
    ${CMAKE_SOURCE_DIR}/third_party/uSockets/src/context.c
    ${CMAKE_SOURCE_DIR}/third_party/uSockets/src/eventing/epoll_kqueue.c
    ${CMAKE_SOURCE_DIR}/third_party/uSockets/src/loop.c
    ${CMAKE_SOURCE_DIR}/third_party/uSockets/src/socket.c
    ${CMAKE_SOURCE_DIR}/third_party/uSockets/src/udp.c)
mmltk_register_private_headers(
    mmltk_frameworks_transport
    browser_server_lifecycle.h)

target_include_directories(mmltk_frameworks_transport SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/uWebSockets/src
    ${CMAKE_SOURCE_DIR}/third_party/uSockets/src
)
target_compile_definitions(mmltk_frameworks_transport PRIVATE
    _GNU_SOURCE
    LIBUS_NO_SSL
    LIBUS_USE_EPOLL
)

target_link_libraries(mmltk_frameworks_transport
    PUBLIC
        mmltk::common::concurrency
        mmltk::common::io
        mmltk::common::logging
        mmltk::common::types
        mmltk::frameworks::serialization
    PRIVATE
        ZLIB::ZLIB
)

function(mmltk_add_frameworks_transport_tests)
    add_library(mmltk_frameworks_transport_test_support STATIC EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_frameworks_transport_test_support PUBLIC_DECLARATIONS)
    mmltk_register_public_headers(mmltk_frameworks_transport_test_support
        BASE_DIRS ${CMAKE_SOURCE_DIR}
        FILES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/support/browser_assets.h)
    mmltk_register_ordinary_sources(mmltk_frameworks_transport_test_support
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/support/browser_assets.cpp)
    target_link_libraries(mmltk_frameworks_transport_test_support PUBLIC mmltk_test_support)
    add_executable(mmltk_frameworks_transport_tests EXCLUDE_FROM_ALL)
    mmltk_register_retained_module_importers(
        mmltk_frameworks_transport_tests
        ${CMAKE_SOURCE_DIR}/src/frameworks/transport/tests/browser_server.test.cpp
        ${CMAKE_SOURCE_DIR}/src/frameworks/transport/tests/browser_record_ring.test.cpp
    )
    mmltk_configure_component(
        mmltk_frameworks_transport_tests PRIVATE_DECLARATIONS)
    target_include_directories(mmltk_frameworks_transport_tests
        SYSTEM PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/uSockets/src
    )
    target_link_libraries(mmltk_frameworks_transport_tests PRIVATE
        mmltk_frameworks_transport_test_support
        mmltk_test_support
        mmltk::frameworks::transport
    )
    catch_discover_tests(
        mmltk_frameworks_transport_tests
        TEST_PREFIX "frameworks::transport::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
