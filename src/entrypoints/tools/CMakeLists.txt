include(MmltkComponent)
include("${CMAKE_SOURCE_DIR}/tools/cmake/Toolchain.cmake")

add_executable(mmltk_diagnose_compiled_sample EXCLUDE_FROM_ALL)
mmltk_configure_component(mmltk_diagnose_compiled_sample PRIVATE_DECLARATIONS)
mmltk_tools_register_process(mmltk_diagnose_compiled_sample
    "${CMAKE_SOURCE_DIR}/tools/diagnostics/diagnose_compiled_sample.cpp"
    mmltk.tools.diagnose_compiled_sample mmltk_tool_diagnose_compiled_sample)
target_link_libraries(mmltk_diagnose_compiled_sample PRIVATE
    mmltk::backend::data
    mmltk::backend::data::catalog
    mmltk::common::concurrency
    mmltk::common::system
    mmltk::common::io
    nlohmann_json)
mmltk_enable_native_runtime_rpath(mmltk_diagnose_compiled_sample)

add_executable(mmltk_diagnose_coconut_mask EXCLUDE_FROM_ALL)
mmltk_configure_component(mmltk_diagnose_coconut_mask PRIVATE_DECLARATIONS)
mmltk_tools_register_process(mmltk_diagnose_coconut_mask
    "${CMAKE_SOURCE_DIR}/tools/diagnostics/diagnose_coconut_mask.cpp"
    mmltk.tools.diagnose_coconut_mask mmltk_tool_diagnose_coconut_mask)
target_link_libraries(mmltk_diagnose_coconut_mask PRIVATE
    Arrow::arrow_static
    Parquet::parquet_static
    nlohmann_json
    stb)
mmltk_enable_native_runtime_rpath(mmltk_diagnose_coconut_mask)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/benchmark-image-diagnostics.env"
    CONTENT "MMLTK_COMPILED_SAMPLE_DIAGNOSTIC=$<TARGET_FILE:mmltk_diagnose_compiled_sample>\nMMLTK_COCONUT_MASK_DIAGNOSTIC=$<TARGET_FILE:mmltk_diagnose_coconut_mask>\n")

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
        mmltk_register_ordinary_sources(mmltk_entrypoints_tools_tests
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/benchmark_image_diagnostic.test.cpp)
        mmltk_configure_component(
            mmltk_entrypoints_tools_tests PRIVATE_DECLARATIONS)
        target_include_directories(mmltk_entrypoints_tools_tests PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
        target_link_libraries(mmltk_entrypoints_tools_tests PRIVATE
            mmltk_test_support
            mmltk::backend::data
            Arrow::arrow_static
            Parquet::parquet_static
            nlohmann_json
            stb
            mmltk::common::logging
            mmltk_entrypoints_tools_support)
        target_compile_definitions(mmltk_entrypoints_tools_tests PRIVATE
            MMLTK_TEST_COMPILED_DIAGNOSTIC="$<TARGET_FILE:mmltk_diagnose_compiled_sample>"
            MMLTK_TEST_MASK_DIAGNOSTIC="$<TARGET_FILE:mmltk_diagnose_coconut_mask>"
            MMLTK_TEST_BENCHMARK_DIAGNOSTIC="${MMLTK_NATIVE_TOOLS_EXECUTABLE}")
        add_dependencies(mmltk_entrypoints_tools_tests
            mmltk_diagnose_compiled_sample mmltk_diagnose_coconut_mask)
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
