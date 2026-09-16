include(MmltkComponent)

include("${CMAKE_CURRENT_LIST_DIR}/firefox_runtime_build.cmake")

option(BUILD_MMLTK_BROWSER_HOST
    "Build the Firefox-hosted iced RF-DETR desktop shell" OFF)
set(MMLTK_INSTALL_FIREFOX_RUNTIMEDIR
    "${CMAKE_INSTALL_LIBDIR}/mmltk/firefox" CACHE STRING
    "Install-relative directory for the owned Firefox runtime")

add_library(mmltk_entrypoints_desktop INTERFACE)
add_library(mmltk::entrypoints::desktop ALIAS
    mmltk_entrypoints_desktop)
target_link_libraries(mmltk_entrypoints_desktop INTERFACE
    mmltk::common::io
    mmltk::controller::direct_services
    mmltk::controller::shell
    mmltk::frontend::iced
)

if(BUILD_MMLTK_BROWSER_HOST OR BUILD_TESTS)
    add_executable(mmltk_entrypoints_desktop_host)
    if(NOT BUILD_MMLTK_BROWSER_HOST)
        set_target_properties(mmltk_entrypoints_desktop_host PROPERTIES
            EXCLUDE_FROM_ALL TRUE)
    endif()
    mmltk_configure_component(
        mmltk_entrypoints_desktop_host PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_entrypoints_desktop_host browser_runtime_entry.cpp)
    mmltk_register_private_headers(mmltk_entrypoints_desktop_host browser_runtime_options.h)
    target_include_directories(mmltk_entrypoints_desktop_host
        SYSTEM PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/uWebSockets/src
            ${CMAKE_SOURCE_DIR}/third_party/uSockets/src
    )
    target_compile_definitions(mmltk_entrypoints_desktop_host PRIVATE
        MMLTK_FIREFOX_RUNTIME_ROOT="${CMAKE_INSTALL_FULL_LIBDIR}/mmltk/firefox"
        MMLTK_BROWSER_APP_ASSET_ROOT="${CMAKE_INSTALL_FULL_DATADIR}/mmltk/browser-app"
    )
    target_link_libraries(mmltk_entrypoints_desktop_host PRIVATE
        mmltk::common::logging
        mmltk::controller::direct_services
        mmltk::controller::shell
    )
    set_target_properties(mmltk_entrypoints_desktop_host PROPERTIES
        OUTPUT_NAME "mmltk-browser-host")
    add_dependencies(mmltk_entrypoints_desktop_host mmltk_entrypoints_cli)
    mmltk_enable_native_runtime_rpath(
        mmltk_entrypoints_desktop_host)
endif()

if(BUILD_MMLTK_BROWSER_HOST)
    if(NOT BUILD_MMLTK_BROWSER_APP)
        message(FATAL_ERROR
            "BUILD_MMLTK_BROWSER_HOST requires BUILD_MMLTK_BROWSER_APP for its packaged page assets.")
    endif()
    if(NOT BUILD_MMLTK_FIREFOX_RUNTIME)
        message(FATAL_ERROR
            "BUILD_MMLTK_BROWSER_HOST requires BUILD_MMLTK_FIREFOX_RUNTIME for its owned browser process.")
    endif()

    add_dependencies(mmltk_entrypoints_desktop_host
        mmltk_firefox_runtime
        mmltk_frontend_iced_bundle
        mmltk_frontend_iced_check
    )
    mmltk_install_target(mmltk_entrypoints_desktop_host)
    install(DIRECTORY "${MMLTK_FIREFOX_RUNTIME_ROOT}/"
        DESTINATION "${MMLTK_INSTALL_FIREFOX_RUNTIMEDIR}"
        USE_SOURCE_PERMISSIONS)
endif()

function(mmltk_add_entrypoints_desktop_tests)
    set(_mmltk_desktop_test_asset_root
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/assets")
    set(_mmltk_desktop_fixture_root
        "${CMAKE_CURRENT_BINARY_DIR}/entrypoints-desktop-fixtures")
    set(_mmltk_desktop_fake_firefox_root
        "${_mmltk_desktop_fixture_root}/fake-firefox")
    set(_mmltk_desktop_invalid_firefox_root
        "${_mmltk_desktop_test_asset_root}/index.html")

    add_executable(mmltk_entrypoints_desktop_child_fixture EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_entrypoints_desktop_child_fixture
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/browser_runtime_child_fixture.cpp
    )
    mmltk_register_private_headers(
        mmltk_entrypoints_desktop_child_fixture
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/browser_runtime_fixture_args.hpp)
    mmltk_configure_component(
        mmltk_entrypoints_desktop_child_fixture PRIVATE_DECLARATIONS)

    add_executable(mmltk_entrypoints_desktop_fake_firefox_fixture
        EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_entrypoints_desktop_fake_firefox_fixture
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/browser_runtime_entry_fake_firefox.cpp
    )
    mmltk_configure_component(
        mmltk_entrypoints_desktop_fake_firefox_fixture PRIVATE_DECLARATIONS)
    set_target_properties(
        mmltk_entrypoints_desktop_fake_firefox_fixture PROPERTIES
        OUTPUT_NAME firefox
        RUNTIME_OUTPUT_DIRECTORY
            "${_mmltk_desktop_fake_firefox_root}"
    )

    add_custom_target(
        mmltk_entrypoints_desktop_entry_healthy_fixture
        DEPENDS
            mmltk_entrypoints_desktop_host
            mmltk_entrypoints_desktop_fake_firefox_fixture
    )
    add_custom_target(
        mmltk_entrypoints_desktop_entry_refusal_fixture
        DEPENDS mmltk_entrypoints_desktop_host
    )

    add_executable(mmltk_entrypoints_desktop_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_entrypoints_desktop_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/browser_runtime_contract.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/browser_runtime_entry.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/firefox_process_owner.test.cpp
    )
    mmltk_configure_component(
        mmltk_entrypoints_desktop_tests PRIVATE_DECLARATIONS)
    target_include_directories(mmltk_entrypoints_desktop_tests PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
        ${CMAKE_SOURCE_DIR}/src/controller/shell/tests
    )
    target_compile_definitions(mmltk_entrypoints_desktop_tests PRIVATE
        MMLTK_BROWSER_RUNTIME_CHILD_FIXTURE="$<TARGET_FILE:mmltk_entrypoints_desktop_child_fixture>"
        MMLTK_BROWSER_RUNTIME_ENTRY_FIXTURE="$<TARGET_FILE:mmltk_entrypoints_desktop_host>"
        MMLTK_BROWSER_RUNTIME_ENTRY_ASSET_ROOT="${_mmltk_desktop_test_asset_root}"
        MMLTK_BROWSER_RUNTIME_ENTRY_FAKE_FIREFOX_ROOT="${_mmltk_desktop_fake_firefox_root}"
        MMLTK_BROWSER_RUNTIME_ENTRY_INVALID_FIREFOX_ROOT="${_mmltk_desktop_invalid_firefox_root}"
    )
    target_link_libraries(mmltk_entrypoints_desktop_tests PRIVATE
        mmltk_acceptance_support
        mmltk::controller::direct_services
        mmltk::controller::shell
        mmltk::entrypoints::desktop
    )
    add_dependencies(mmltk_entrypoints_desktop_tests
        mmltk_entrypoints_desktop_child_fixture
        mmltk_entrypoints_desktop_entry_healthy_fixture
        mmltk_entrypoints_desktop_entry_refusal_fixture
    )
    catch_discover_tests(
        mmltk_entrypoints_desktop_tests
        TEST_PREFIX "entrypoints::desktop::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
