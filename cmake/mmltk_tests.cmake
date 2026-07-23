option(BUILD_TESTS "Build tests and profile runners" OFF)

if(BUILD_TESTS)
    enable_testing()
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "Install Catch2 documentation" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "Install Catch2 extras" FORCE)
    set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "Build Catch2 development extras" FORCE)
    set(CATCH_BUILD_TESTING OFF CACHE BOOL "Build Catch2 self tests" FORCE)
    set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "Build Catch2 examples" FORCE)
    set(CATCH_BUILD_EXTRA_TESTS OFF CACHE BOOL "Build Catch2 extra tests" FORCE)
    set(CATCH_BUILD_BENCHMARKS OFF CACHE BOOL "Build Catch2 benchmarks" FORCE)
    set(CATCH_CONFIG_NO_THREAD_SAFE_ASSERTIONS ON CACHE BOOL
        "Force-disable Catch2 thread-safe assertions; keep test assertions on the fast default path" FORCE)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/catch2/generated-includes/catch2")
    add_subdirectory(
        ${CMAKE_SOURCE_DIR}/third_party/catch2
        ${CMAKE_BINARY_DIR}/third_party/catch2
        EXCLUDE_FROM_ALL
    )
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/third_party/catch2/extras)
    include(Catch)

    add_library(mmltk_catch_main STATIC EXCLUDE_FROM_ALL
        tests/support/catch_main.cpp
    )
    mmltk_target_project_dirs(mmltk_catch_main)
    target_include_directories(mmltk_catch_main PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/support
    )
    target_link_libraries(mmltk_catch_main PUBLIC
        Catch2::Catch2
        mmltk_logging
    )

    add_library(mmltk_test_support STATIC EXCLUDE_FROM_ALL
        tests/support/test_fixture.cpp
    )
    target_include_directories(mmltk_test_support PUBLIC
        ${CMAKE_SOURCE_DIR}/tests/support
    )
    target_link_libraries(mmltk_test_support PUBLIC
        stb
        mmltk_build_options
    )

    add_library(mmltk_test_headers INTERFACE EXCLUDE_FROM_ALL)
    target_include_directories(mmltk_test_headers INTERFACE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/tests/support
        ${CMAKE_SOURCE_DIR}/tests/model/rfdetr
    )

    set(MMLTK_TEST_CORE_BASE_LINK_LIBS
        Catch2::Catch2
        mmltk_catch_main
        mmltk_build_options
        mmltk_core
        mmltk_runtime
    )
    set(MMLTK_RFDETR_MODEL_TEST_BASE_LINK_LIBS
        mmltk_core
        mmltk_build_options
        mmltk_rfdetr_native
        mmltk_test_headers
    )

    add_executable(mmltk_tests_core EXCLUDE_FROM_ALL
        tests/core/async_runtime.test.cpp
        tests/core/cli_runtime.test.cpp
        tests/core/compile_progress.test.cpp
        tests/core/image_resize.test.cpp
        tests/core/live_crop_state.test.cpp
        tests/core/live_slot_ring.test.cpp
        tests/core/model_registry.test.cpp
        tests/core/roundtrip.test.cpp
        tests/core/vendored_helpers.test.cpp
        tests/core/worker_pool.test.cpp
    )
    mmltk_target_project_dirs(mmltk_tests_core)
    target_include_directories(mmltk_tests_core PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
    )
    target_link_libraries(mmltk_tests_core PRIVATE
        ${MMLTK_TEST_CORE_BASE_LINK_LIBS}
        mmltk_test_headers
        mmltk_test_support
    )
    target_compile_definitions(mmltk_tests_core PRIVATE
        MMLTK_TEST_MMLTK_CLI_PATH="$<TARGET_FILE:mmltk_cli>"
    )
    add_dependencies(mmltk_tests_core mmltk_cli)
    mmltk_configure_target_pch(mmltk_tests_core
        HEADER src/pch/tests_core.hpp)
    catch_discover_tests(
        mmltk_tests_core
        TEST_PREFIX "core::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )

    set(MMLTK_GUI_TEST_SOURCES
        tests/gui/browser_host_api.test.cpp
        tests/gui/browser_runtime_dispatch.test.cpp
        src/browser/host_api.cpp
        src/gui/browser_host_adapters.cpp
        tests/gui/annotation_document.test.cpp
        tests/gui/annotation_save_tab.test.cpp
        tests/gui/annotation_workspace_canvas.test.cpp
        src/gui/annotation/controller.cpp
        src/gui/annotation/editor.cpp
        src/gui/annotation/editor_history.cpp
        ${MMLTK_GUI_ANNOTATION_SOURCES}
        src/gui/annotation_core_preview.cpp
        src/gui/annotation_core_persistence.cpp
        tests/gui/app_options.test.cpp
        tests/gui/live_source_lease_ring.test.cpp
        tests/gui/live_session_utils.test.cpp
        tests/gui/live_overlay_runtime.test.cpp
        tests/gui/settings.test.cpp
        tests/gui/subprocess_utils.test.cpp
        tests/gui/vast_runtime.test.cpp
        src/gui/annotation_core.cpp
        src/gui/app_options.cpp
        src/gui/canvas_layers.cpp
        src/gui/gui_settings.cpp
        src/gui/live_session_utils.cpp
        src/gui/vast_query_controller.cpp
        src/gui/vast_runtime.cpp
        src/live/manual_overlay_document.cpp
    )
    if(MMLTK_NEEDS_TORCH)
        list(APPEND MMLTK_GUI_TEST_SOURCES
            tests/gui/annotation_core.test.cpp
            tests/gui/canvas_layers.test.cpp
            tests/gui/preview_rect_drag.test.cpp
            tests/gui/train_command.test.cpp
            src/gui/preview_rect_drag.cpp
            src/gui/train_command.cpp
        )
    endif()
    if(BUILD_RFDETR_NATIVE)
        list(APPEND MMLTK_GUI_TEST_SOURCES
            tests/gui/local_train_runtime.test.cpp
            tests/gui/rfdetr_workflows.test.cpp
            tests/gui/cli_seed.test.cpp
            src/gui/cli_seed.cpp
            src/gui/default_state.cpp
            src/gui/local_train_controller.cpp
            src/gui/rfdetr_workflows.cpp
            src/rfdetr/workflow_requests.cpp
            src/gui/train_process_runtime.cpp
        )
    endif()

    add_executable(mmltk_tests_gui EXCLUDE_FROM_ALL ${MMLTK_GUI_TEST_SOURCES})
    if(TARGET mmltk_browser_app_contract)
        add_dependencies(mmltk_tests_gui mmltk_browser_app_contract)
    endif()
    mmltk_target_project_dirs(mmltk_tests_gui)
    target_link_libraries(mmltk_tests_gui PRIVATE
        ${MMLTK_TEST_CORE_BASE_LINK_LIBS}
        mmltk_rfdetr_core
        mmltk_browser_api_contract
        mmltk_test_headers
        cli11
        nlohmann_json
        CUDA::cudart
    )
    if(MMLTK_NEEDS_TORCH)
        mmltk_enable_torch(mmltk_tests_gui PRIVATE)
    endif()
    if(BUILD_RFDETR_NATIVE)
        target_link_libraries(mmltk_tests_gui PRIVATE
            mmltk_rfdetr_torch
        )
    endif()
    if(MMLTK_RFDETR_LIVE_CAPTURE_ENABLED)
        target_link_libraries(mmltk_tests_gui PRIVATE
            mmltk_live_runtime
        )
    endif()
    target_compile_definitions(mmltk_tests_gui PRIVATE
        MMLTK_RFDETR_LIVE_CAPTURE=$<IF:$<BOOL:${MMLTK_RFDETR_LIVE_CAPTURE_ENABLED}>,1,0>
    )
    mmltk_configure_target_pch(mmltk_tests_gui
        HEADER src/pch/tests_native.hpp)
    catch_discover_tests(
        mmltk_tests_gui
        TEST_PREFIX "gui::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )

    add_executable(mmltk_profile_runner EXCLUDE_FROM_ALL
        tests/profile_runner.cpp
    )
    mmltk_target_private_src_dir(mmltk_profile_runner)
    target_link_libraries(mmltk_profile_runner PRIVATE
        mmltk_core
        mmltk_build_options
        mmltk_test_headers
        mmltk_test_support
    )

    if(BUILD_RFDETR_NATIVE)
        set(MMLTK_RFDETR_TEST_SOURCES
            tests/model/rfdetr/weight_catalog.test.cpp
            tests/model/rfdetr/cli_aliases.test.cpp
            tests/model/rfdetr/eval_sample_writer.test.cpp
            tests/model/rfdetr/cuda_utils.test.cpp
            tests/model/rfdetr/model_trainability.test.cpp
            tests/model/rfdetr/modules.test.cpp
            tests/model/rfdetr/native_ops.test.cpp
            tests/model/rfdetr/native_optimizer.test.cpp
            tests/model/rfdetr/onnx_lowering.test.cpp
            tests/model/rfdetr/prediction_json.test.cpp
            tests/model/rfdetr/public_headers.test.cpp
            tests/model/rfdetr/runtime_shared.test.cpp
            tests/model/rfdetr/target_builder.test.cpp
        )
        if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
            list(APPEND MMLTK_RFDETR_TEST_SOURCES
                tests/model/rfdetr/checkpoint.test.cpp
                tests/model/rfdetr/checkpoint_parity.test.cpp
                tests/model/rfdetr/native_integration.test.cpp
            )
        endif()
        add_executable(mmltk_tests_model_rfdetr EXCLUDE_FROM_ALL
            ${MMLTK_RFDETR_TEST_SOURCES})
        mmltk_target_project_dirs(mmltk_tests_model_rfdetr)
        target_include_directories(mmltk_tests_model_rfdetr PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
        )
        target_link_libraries(mmltk_tests_model_rfdetr PRIVATE
            ${MMLTK_RFDETR_MODEL_TEST_BASE_LINK_LIBS}
            Catch2::Catch2
            mmltk_catch_main
            mmltk_test_support
        )
        target_compile_definitions(mmltk_tests_model_rfdetr PRIVATE
            MMLTK_TEST_MMLTK_CLI_PATH="$<TARGET_FILE:mmltk_cli>"
        )
        add_dependencies(mmltk_tests_model_rfdetr mmltk_cli)
        mmltk_prefer_system_protobuf_headers(mmltk_tests_model_rfdetr PRIVATE)
        mmltk_enable_torch(mmltk_tests_model_rfdetr PRIVATE)
        mmltk_target_cuda_architectures(mmltk_tests_model_rfdetr)
        mmltk_enable_native_runtime_rpath(mmltk_tests_model_rfdetr)
        mmltk_configure_target_pch(mmltk_tests_model_rfdetr
            HEADER src/pch/tests_native.hpp)
        catch_discover_tests(
            mmltk_tests_model_rfdetr
            TEST_SPEC "~[optin]"
            TEST_PREFIX "rfdetr::"
            DISCOVERY_MODE PRE_TEST
            ADD_TAGS_AS_LABELS
        )
        add_executable(mmltk_rfdetr_profile_runner EXCLUDE_FROM_ALL
            tests/rfdetr_profile_runner.cpp
        )
        mmltk_target_private_src_dir(mmltk_rfdetr_profile_runner)
        target_link_libraries(mmltk_rfdetr_profile_runner PRIVATE
            ${MMLTK_RFDETR_MODEL_TEST_BASE_LINK_LIBS}
        )
        mmltk_enable_torch(mmltk_rfdetr_profile_runner PRIVATE)
        mmltk_target_cuda_architectures(mmltk_rfdetr_profile_runner)
        mmltk_enable_native_runtime_rpath(mmltk_rfdetr_profile_runner)

        add_executable(mmltk_rfdetr_train_profile_runner EXCLUDE_FROM_ALL
            tests/rfdetr_train_profile_runner.cpp
        )
        mmltk_target_private_src_dir(mmltk_rfdetr_train_profile_runner)
        target_link_libraries(mmltk_rfdetr_train_profile_runner PRIVATE
            ${MMLTK_RFDETR_MODEL_TEST_BASE_LINK_LIBS}
            mmltk_test_support
        )
        mmltk_enable_torch(mmltk_rfdetr_train_profile_runner PRIVATE)
        mmltk_target_cuda_architectures(mmltk_rfdetr_train_profile_runner)
        mmltk_enable_native_runtime_rpath(mmltk_rfdetr_train_profile_runner)
    endif()
endif()
