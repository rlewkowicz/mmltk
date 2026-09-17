include(MmltkComponent)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(mmltk_controller_direct_services STATIC)
add_library(mmltk::controller::direct_services ALIAS mmltk_controller_direct_services)
mmltk_configure_component(mmltk_controller_direct_services PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_direct_services
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        artifact_store.h
        diagnostics_client.h
        file_dialog_catalog.h
        file_dialog_system.h
        settings_system.h
        file_dialog_client.h
        file_dialog_types.h
        firefox_process_observation.h
        firefox_process_owner.h
        persistence_types.h
        runtime_diagnostics.h
        runtime_diagnostic_span.h
        settings_location.h
        settings_store.h
        train_command.h
        train_process_client.h
        train_run_store.h
        vast_client.h
        vast_provider_owner.h)
mmltk_register_ordinary_sources(
    mmltk_controller_direct_services
    artifact_store.cpp
    diagnostics_client.cpp
    file_dialog_catalog.cpp
    file_dialog_system.cpp
    settings_system.cpp
    file_dialog_client.cpp
    firefox_process_owner.cpp
    runtime_diagnostics.cpp
    settings_store.cpp
    train_command.cpp
    train_process_client.cpp
    train_run_store.cpp
    vast_client.cpp
    vast_provider_owner.cpp
    vast_runtime.cpp)
target_include_directories(mmltk_controller_direct_services PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mmltk_controller_direct_services
    PUBLIC
        mmltk::backend::data
        mmltk::backend::models::rfdetr::contract
        mmltk::common::io
        mmltk::common::types
        mmltk::controller::runtime
        mmltk::controller::application_boundary
        mmltk::controller::contracts
        mmltk::frameworks::process
        mmltk::frameworks::reflection
        mmltk::frameworks::serialization
    PRIVATE
        CURL::libcurl
        OpenSSL::Crypto
        mmltk::common::concurrency
        mmltk::common::logging
        mmltk::common::system
        mmltk::controller::browser::schema
        nlohmann_json)

mmltk_register_header_isolation(mmltk_controller_direct_services)

set(MMLTK_VAST_BRIDGE_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/vast_bridge.py"
    CACHE INTERNAL "Controller-owned Vast bridge")

add_library(mmltk_controller_settings_test_support INTERFACE)
mmltk_register_public_headers(mmltk_controller_settings_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/support/settings_test_fixture.h)
target_link_libraries(mmltk_controller_settings_test_support INTERFACE
    mmltk::controller::direct_services mmltk_test_support)
mmltk_register_header_isolation(mmltk_controller_settings_test_support)

function(mmltk_add_controller_services_tests)
    add_executable(mmltk_controller_services_file_dialog_fixture
        EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_controller_services_file_dialog_fixture
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/file_dialog_process_fixture.cpp
    )
    mmltk_configure_component(
        mmltk_controller_services_file_dialog_fixture PRIVATE_DECLARATIONS)
    target_link_libraries(
        mmltk_controller_services_file_dialog_fixture
        PRIVATE mmltk::controller::direct_services
    )

    set(mmltk_controller_services_ordinary_test_sources
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/application_services.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/settings.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/settings_persistence.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/vast_runtime.test.cpp
    )
    if(TARGET mmltk_backend_models_rfdetr_training)
        list(APPEND mmltk_controller_services_ordinary_test_sources
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/train_command.test.cpp
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/train_run_store.test.cpp)
    endif()
    add_executable(mmltk_controller_services_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_controller_services_tests
        ${mmltk_controller_services_ordinary_test_sources})
    mmltk_register_private_headers(mmltk_controller_services_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/support/diagnostics_client_test_access.h)
    mmltk_configure_component(
        mmltk_controller_services_tests PRIVATE_DECLARATIONS)
    target_include_directories(mmltk_controller_services_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_definitions(mmltk_controller_services_tests PRIVATE
        "MMLTK_FILE_DIALOG_PROCESS_FIXTURE=$<TARGET_FILE:mmltk_controller_services_file_dialog_fixture>"
    )
    target_link_libraries(mmltk_controller_services_tests PRIVATE
        mmltk_controller_settings_test_support
        mmltk_test_support
        mmltk::backend::data
        mmltk::backend::models::rfdetr::contract
        mmltk::common::io
        mmltk::controller::contracts
        mmltk::controller::data_compute_systems
        mmltk::controller::direct_services
        mmltk::frameworks::reflection
        mmltk::frameworks::serialization
        mmltk_backend_data_test_support
        nlohmann_json
    )
    add_dependencies(mmltk_controller_services_tests
        mmltk_controller_services_file_dialog_fixture)
    if(TARGET mmltk_backend_models_rfdetr_training)
        target_link_libraries(mmltk_controller_services_tests PRIVATE
            mmltk::backend::models::rfdetr::training)
    endif()
    catch_discover_tests(
        mmltk_controller_services_tests
        TEST_PREFIX "controller::services::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()

# Called after the desktop factory has registered the physical child fixture.
function(mmltk_add_controller_firefox_process_tests)
    add_executable(mmltk_controller_firefox_process_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_controller_firefox_process_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_controller_firefox_process_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/firefox_process_owner.test.cpp)
    target_compile_definitions(mmltk_controller_firefox_process_tests PRIVATE
        "MMLTK_BROWSER_RUNTIME_CHILD_FIXTURE=$<TARGET_FILE:mmltk_entrypoints_desktop_child_fixture>")
    target_link_libraries(mmltk_controller_firefox_process_tests PRIVATE
        mmltk_test_support mmltk::controller::direct_services)
    add_dependencies(mmltk_controller_firefox_process_tests mmltk_entrypoints_desktop_child_fixture)
    catch_discover_tests(mmltk_controller_firefox_process_tests
        TEST_PREFIX "controller::firefox-process::" DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()
