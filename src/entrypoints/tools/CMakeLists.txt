include(MmltkComponent)

if(BUILD_RFDETR_NATIVE)
    add_library(mmltk_entrypoints_tools_support STATIC)
    mmltk_configure_component(
        mmltk_entrypoints_tools_support PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_entrypoints_tools_support detail/onnx_tool_main.cpp)
    mmltk_register_private_headers(
        mmltk_entrypoints_tools_support detail/onnx_tool_main.h)
    target_include_directories(mmltk_entrypoints_tools_support PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(mmltk_entrypoints_tools_support PRIVATE
        mmltk::common::logging
    )

    add_executable(mmltk_entrypoints_tools_onnx_info)
    mmltk_configure_component(
        mmltk_entrypoints_tools_onnx_info PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_entrypoints_tools_onnx_info onnx_model_info_main.cpp)
    target_link_libraries(mmltk_entrypoints_tools_onnx_info PRIVATE
        mmltk::backend::models::rfdetr::export
        mmltk::common::logging
        mmltk_entrypoints_tools_support
    )
    set_target_properties(mmltk_entrypoints_tools_onnx_info PROPERTIES
        OUTPUT_NAME "mmltk-rfdetr-onnx-info"
    )
    mmltk_enable_native_runtime_rpath(
        mmltk_entrypoints_tools_onnx_info)

    add_executable(mmltk_entrypoints_tools_onnx_simplify)
    mmltk_configure_component(
        mmltk_entrypoints_tools_onnx_simplify PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_entrypoints_tools_onnx_simplify onnx_simplify_main.cpp)
    target_link_libraries(mmltk_entrypoints_tools_onnx_simplify PRIVATE
        mmltk::backend::models::rfdetr::export
        mmltk::common::logging
        mmltk_entrypoints_tools_support
    )
    set_target_properties(
        mmltk_entrypoints_tools_onnx_simplify PROPERTIES
        OUTPUT_NAME "mmltk-rfdetr-onnx-simplify"
    )
    mmltk_enable_native_runtime_rpath(
        mmltk_entrypoints_tools_onnx_simplify)

    function(mmltk_add_entrypoints_tools_tests)
        add_executable(mmltk_entrypoints_tools_tests EXCLUDE_FROM_ALL)
        mmltk_register_retained_module_importers(
            mmltk_entrypoints_tools_tests
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/onnx_tool_main.test.cpp)
        mmltk_configure_component(
            mmltk_entrypoints_tools_tests PRIVATE_DECLARATIONS)
        target_include_directories(mmltk_entrypoints_tools_tests PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
        target_link_libraries(mmltk_entrypoints_tools_tests PRIVATE
            mmltk_test_support
            mmltk::common::logging
            mmltk_entrypoints_tools_support)
        catch_discover_tests(mmltk_entrypoints_tools_tests
            TEST_PREFIX "entrypoints::tools::"
            DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
    endfunction()

    install(TARGETS
        mmltk_entrypoints_tools_onnx_info
        mmltk_entrypoints_tools_onnx_simplify
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
