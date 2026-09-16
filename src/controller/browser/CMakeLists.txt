include(MmltkComponent)

add_library(mmltk_controller_browser_protocol_v17 STATIC)
add_library(mmltk::controller::browser::protocol_v17 ALIAS
    mmltk_controller_browser_protocol_v17)
mmltk_configure_component(
    mmltk_controller_browser_protocol_v17 PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_browser_protocol_v17
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        application_stable_identity.h
        application_schema.h
        application_materializer.h
        application_event_publisher.h
        client_record.h)
mmltk_register_ordinary_sources(
    mmltk_controller_browser_protocol_v17
    client_record.cpp)
target_include_directories(mmltk_controller_browser_protocol_v17 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mmltk_controller_browser_protocol_v17
    PUBLIC
        mmltk::common::types
        mmltk::controller::application_systems_projection
        mmltk::frameworks::reflection
        mmltk::frameworks::serialization)
mmltk_register_header_isolation(mmltk_controller_browser_protocol_v17)
mmltk_register_header_include_orders(mmltk_controller_browser_protocol_v17
    src/controller/presentation/visual_system_types.h
    src/controller/browser/application_schema.h
    [=[static_assert(mmltk::controller::browser::ApplicationSchema<mmltk::controller::ApplicationSystems>::BootstrapPayloadBudget() > 0U);]=])

add_library(mmltk_controller_browser_direct STATIC)
add_library(mmltk::controller::browser::direct ALIAS
    mmltk_controller_browser_direct)
mmltk_configure_component(
    mmltk_controller_browser_direct PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_browser_direct
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES application_browser_host.h)
mmltk_register_ordinary_sources(
    mmltk_controller_browser_direct application_browser_host.cpp)
target_include_directories(mmltk_controller_browser_direct PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mmltk_controller_browser_direct
    PUBLIC
        mmltk::controller::browser::protocol_v17
        mmltk::controller::direct_services
        mmltk::frameworks::transport
    PRIVATE
        mmltk::controller::application_runtime)
mmltk_register_header_isolation(mmltk_controller_browser_direct)

set(MMLTK_WORKSPACE_GRAPHICS_ABI
    "${CMAKE_BINARY_DIR}/generated/frontend/iced/workspace_graphics_abi.rs"
    CACHE INTERNAL "Canonical generated native graphics layout")
set(MMLTK_APPLICATION_BINDINGS
    "${CMAKE_BINARY_DIR}/generated/frontend/iced/application_bindings.rs"
    CACHE INTERNAL "Canonical generated Rust application bindings")
set(MMLTK_BROWSER_PROTOCOL_MARKER
    "${CMAKE_BINARY_DIR}/generated/frontend/iced/browser_protocol.marker"
    CACHE INTERNAL "Canonical compiled native browser protocol marker")
set(MMLTK_BROWSER_PROTOCOL_V17_CLIENT_FIXTURE
    "${CMAKE_BINARY_DIR}/generated/frontend/iced/protocol_v17_client_records.hex"
    CACHE INTERNAL "Protocol-17 Rust/native client record fixtures")
set(MMLTK_BROWSER_PROTOCOL_V17_SERVER_FIXTURE
    "${CMAKE_BINARY_DIR}/generated/frontend/iced/protocol_v17_server_records.cbor"
    CACHE INTERNAL "Native protocol-17 server record fixture")

add_executable(mmltk_application_binding_generator EXCLUDE_FROM_ALL)
mmltk_register_ordinary_sources(
    mmltk_application_binding_generator application_binding_generator.cpp)
mmltk_register_private_headers(
    mmltk_application_binding_generator application_outer_routing_emitter.h application_visual_projection_emitter.h application_workspace_abi_emitter.h)
mmltk_configure_component(
    mmltk_application_binding_generator PRIVATE_DECLARATIONS)
target_link_libraries(
    mmltk_application_binding_generator
    PRIVATE
        mmltk::controller::application_systems_projection
        mmltk::frameworks::serialization)
add_custom_command(
    OUTPUT
        "${MMLTK_APPLICATION_BINDINGS}"
        "${MMLTK_BROWSER_PROTOCOL_MARKER}"
        "${MMLTK_WORKSPACE_GRAPHICS_ABI}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/generated/frontend/iced"
    COMMAND "$<TARGET_FILE:mmltk_application_binding_generator>"
            "${MMLTK_APPLICATION_BINDINGS}"
            "${MMLTK_BROWSER_PROTOCOL_MARKER}"
            "${MMLTK_WORKSPACE_GRAPHICS_ABI}"
    DEPENDS
        mmltk_application_binding_generator
        application_stable_identity.h
        application_schema.h
        application_outer_routing_emitter.h
        application_visual_projection_emitter.h
        application_workspace_abi_emitter.h
        ${CMAKE_SOURCE_DIR}/src/controller/presentation/detail/workspace_surface_import_abi.h
        ${CMAKE_SOURCE_DIR}/src/controller/presentation/detail/workspace_frame_signal.h
        ${CMAKE_SOURCE_DIR}/src/controller/presentation/workspace_presentation_types.h
        ${CMAKE_SOURCE_DIR}/src/frameworks/gpu/image_workspace.h
        application_materializer.h
        ${CMAKE_SOURCE_DIR}/src/controller/contracts/application_systems.h
    VERBATIM)
add_custom_target(mmltk_generate_application_bindings
    DEPENDS
        "${MMLTK_APPLICATION_BINDINGS}"
        "${MMLTK_BROWSER_PROTOCOL_MARKER}"
        "${MMLTK_WORKSPACE_GRAPHICS_ABI}")

add_executable(mmltk_controller_browser_protocol_v17_fixture EXCLUDE_FROM_ALL)
mmltk_register_ordinary_sources(
    mmltk_controller_browser_protocol_v17_fixture
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_v17_server_fixture.cpp)
mmltk_configure_component(
    mmltk_controller_browser_protocol_v17_fixture PRIVATE_DECLARATIONS)
mmltk_register_private_headers(
    mmltk_controller_browser_protocol_v17_fixture
    tests/annotation_wire_fixture.h)
target_link_libraries(
    mmltk_controller_browser_protocol_v17_fixture
    PRIVATE
        mmltk::controller::application_systems_projection
        mmltk::controller::browser::protocol_v17
        mmltk::frameworks::serialization)
add_custom_command(
    OUTPUT "${MMLTK_BROWSER_PROTOCOL_V17_SERVER_FIXTURE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/generated/frontend/iced"
    COMMAND "$<TARGET_FILE:mmltk_controller_browser_protocol_v17_fixture>"
            "${MMLTK_BROWSER_PROTOCOL_V17_SERVER_FIXTURE}"
    DEPENDS
        mmltk_controller_browser_protocol_v17_fixture
        "${MMLTK_APPLICATION_BINDINGS}"
    VERBATIM)
add_custom_target(mmltk_controller_browser_protocol_v17_boundary
    DEPENDS
        "${MMLTK_APPLICATION_BINDINGS}"
        "${MMLTK_BROWSER_PROTOCOL_V17_SERVER_FIXTURE}")

add_library(mmltk_controller_browser_test_support INTERFACE)
mmltk_register_public_headers(mmltk_controller_browser_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/support/websocket_test_utils.hpp)
target_link_libraries(mmltk_controller_browser_test_support INTERFACE mmltk_cxx26_dialect)

function(mmltk_add_controller_browser_tests)
    add_executable(mmltk_controller_browser_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_controller_browser_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/application_browser_contract.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/application_browser_host.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/client_record.test.cpp
    )
    mmltk_register_private_headers(
        mmltk_controller_browser_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/annotation_wire_fixture.h
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/application_outer_routing_emitter.h
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/application_visual_projection_emitter.h
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/application_workspace_abi_emitter.h)
    mmltk_configure_component(
        mmltk_controller_browser_tests PRIVATE_DECLARATIONS)
    target_include_directories(mmltk_controller_browser_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests
    )
    target_compile_definitions(mmltk_controller_browser_tests PRIVATE
        MMLTK_PROTOCOL_V17_CLIENT_FIXTURE_PATH="${MMLTK_BROWSER_PROTOCOL_V17_CLIENT_FIXTURE}"
    )
    target_link_libraries(mmltk_controller_browser_tests PRIVATE
        mmltk_frameworks_transport_test_support
        mmltk_controller_browser_test_support
        mmltk_controller_annotation_test_support
        mmltk_test_support
        mmltk::controller::browser::direct
        mmltk::controller::browser::protocol_v17
        mmltk::controller::presentation_system
        mmltk::frameworks::reflection
        mmltk::frameworks::serialization
    )
    add_dependencies(
        mmltk_controller_browser_tests
        mmltk_controller_browser_protocol_v17_boundary)
    if(TARGET mmltk_frontend_iced_protocol_v17_client_fixture)
        add_dependencies(
            mmltk_controller_browser_tests
            mmltk_frontend_iced_protocol_v17_client_fixture)
    endif()
    catch_discover_tests(
        mmltk_controller_browser_tests
        TEST_PREFIX "controller::browser::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
