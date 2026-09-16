include(MmltkComponent)

add_executable(mmltk_entrypoints_cli)
add_executable(mmltk::entrypoints::cli ALIAS mmltk_entrypoints_cli)
mmltk_configure_component(mmltk_entrypoints_cli PRIVATE_DECLARATIONS)
mmltk_register_retained_module_importers(
    mmltk_entrypoints_cli cli.cpp)
mmltk_register_private_headers(
    mmltk_entrypoints_cli cli_output.h)
target_include_directories(mmltk_entrypoints_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
)
target_link_libraries(mmltk_entrypoints_cli PRIVATE
    mmltk::backend::data
    mmltk::common::logging
    mmltk::common::system
    mmltk::controller::contracts
    mmltk::controller::direct_services
    mmltk::frameworks::reflection
)
if(BUILD_RFDETR_NATIVE)
    mmltk_register_retained_module_importers(
        mmltk_entrypoints_cli rfdetr_cli.cpp)
    target_compile_definitions(mmltk_entrypoints_cli PRIVATE
        MMLTK_BUILD_RFDETR_NATIVE=1
    )
    target_link_libraries(mmltk_entrypoints_cli PRIVATE
        mmltk::backend::models::rfdetr::contract
        mmltk::backend::models::rfdetr::core
        mmltk::backend::models::rfdetr::export
        mmltk::backend::models::rfdetr::inference
        mmltk::backend::models::rfdetr::training
    )
else()
    target_compile_definitions(mmltk_entrypoints_cli PRIVATE
        MMLTK_BUILD_RFDETR_NATIVE=0
    )
endif()

mmltk_enable_native_runtime_rpath(mmltk_entrypoints_cli)
set_target_properties(mmltk_entrypoints_cli PROPERTIES OUTPUT_NAME "mmltk")
install(TARGETS mmltk_entrypoints_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

function(mmltk_add_entrypoints_cli_tests)
    add_executable(mmltk_entrypoints_cli_tests EXCLUDE_FROM_ALL)
    mmltk_register_retained_module_importers(
        mmltk_entrypoints_cli_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/cli_runtime.test.cpp
    )
    mmltk_configure_component(
        mmltk_entrypoints_cli_tests PRIVATE_DECLARATIONS)
    target_compile_definitions(mmltk_entrypoints_cli_tests PRIVATE
        MMLTK_TEST_MMLTK_CLI_PATH="$<TARGET_FILE:mmltk_entrypoints_cli>"
    )
    target_link_libraries(mmltk_entrypoints_cli_tests PRIVATE
        mmltk_test_support
        mmltk::frameworks::reflection
    )
    add_dependencies(
        mmltk_entrypoints_cli_tests mmltk_entrypoints_cli)
    catch_discover_tests(
        mmltk_entrypoints_cli_tests
        TEST_PREFIX "entrypoints::cli::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
