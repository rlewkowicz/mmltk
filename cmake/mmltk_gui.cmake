set(MMLTK_GUI_ANNOTATION_SOURCES
    src/gui/annotation/document/document.cpp
    src/gui/annotation/document/edit.cpp
    src/gui/annotation/document/types.cpp
    src/gui/annotation/render/renderer.cpp
    src/gui/annotation/session.cpp
    src/gui/annotation/tools/default_tools.cpp
    src/gui/annotation/tools/tool_helpers.cpp
    src/gui/annotation/tools/spline_tool_helpers.cpp
    src/gui/annotation/tools/skeleton_tool_helpers.cpp
    src/gui/annotation/tools/spline_tool.cpp
    src/gui/annotation/tools/skeleton_tool.cpp
    src/gui/annotation/tools/tool_manager.cpp
    src/gui/annotation/workspace/canvas.cpp
    src/gui/annotation/workspace/interaction.cpp
    src/gui/annotation/workspace/model.cpp
)

option(BUILD_MMLTK_BROWSER_HOST
    "Build the native browser runtime RF-DETR desktop shell" OFF)

if(BUILD_MMLTK_BROWSER_HOST)
    if(NOT BUILD_RFDETR_NATIVE)
        message(FATAL_ERROR
            "BUILD_MMLTK_BROWSER_HOST requires BUILD_RFDETR_NATIVE=ON.")
    endif()
    if(BUILD_MMLTK_BROWSER_HOST AND NOT BUILD_MMLTK_BROWSER_APP)
        message(FATAL_ERROR
            "BUILD_MMLTK_BROWSER_HOST requires BUILD_MMLTK_BROWSER_APP=ON.")
    endif()
    if(NOT MMLTK_NEEDS_PYTHON)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()
    set(MMLTK_GUI_RUNTIME_LINK_LIBS "")
    set(MMLTK_GUI_RUNTIME_SOURCES
        src/gui/browser_runtime_entry.cpp
    )
    set(MMLTK_CEF_WEBGPU_RUNTIME "auto" CACHE STRING
        "CEF Linux WebGPU runtime selection for the browser host")
    set_property(CACHE MMLTK_CEF_WEBGPU_RUNTIME PROPERTY STRINGS
        vulkan
        opengles
        auto)
    option(MMLTK_CEF_ENABLE_UNSAFE_WEBGPU
        "Enable Chromium's unsafe WebGPU switch for the CEF browser host"
        ON)
    option(MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU
        "Request Chromium's high-performance GPU selection for the CEF browser host"
        OFF)
    mmltk_require_pinned_cef_distribution_root(MMLTK_CEF_ROOT)
    get_property(_mmltk_enabled_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    if(NOT "C" IN_LIST _mmltk_enabled_languages)
        enable_language(C)
    endif()
    set(CEF_ROOT "${MMLTK_CEF_ROOT}")
    mmltk_classify_pinned_cef_distribution_root(
        "${MMLTK_CEF_ROOT}"
        MMLTK_CEF_BINARY_ROOT
        MMLTK_CEF_RESOURCES_ROOT)
    list(PREPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
    find_package(CEF REQUIRED)
    add_library(mmltk_cef_shared SHARED IMPORTED GLOBAL)
    set_target_properties(mmltk_cef_shared PROPERTIES
        IMPORTED_LOCATION "${MMLTK_CEF_BINARY_ROOT}/libcef.so"
    )
    add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}"
        "${CMAKE_BINARY_DIR}/cef/libcef_dll_wrapper")
    message(STATUS
        "Configuring CEF browser host with WebGPU runtime `${MMLTK_CEF_WEBGPU_RUNTIME}`")
    add_library(usockets STATIC
        third_party/uSockets/src/bsd.c
        third_party/uSockets/src/context.c
        third_party/uSockets/src/eventing/epoll_kqueue.c
        third_party/uSockets/src/loop.c
        third_party/uSockets/src/socket.c
        third_party/uSockets/src/udp.c
    )
    target_include_directories(usockets SYSTEM PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party/uSockets/src
    )
    target_compile_definitions(usockets PUBLIC
        LIBUS_NO_SSL
        LIBUS_USE_EPOLL
    )
    add_library(uwebsockets INTERFACE)
    target_include_directories(uwebsockets SYSTEM INTERFACE
        ${CMAKE_SOURCE_DIR}/third_party/uWebSockets/src
        ${CMAKE_SOURCE_DIR}/third_party/uSockets/src
    )
    target_compile_definitions(uwebsockets INTERFACE
        UWS_NO_ZLIB
        LIBUS_NO_SSL
        LIBUS_USE_EPOLL
    )
    target_link_libraries(uwebsockets INTERFACE usockets)
    list(APPEND MMLTK_GUI_RUNTIME_LINK_LIBS
        mmltk_cef_shared
        libcef_dll_wrapper
        uwebsockets
        ${CMAKE_DL_LIBS}
    )
    list(APPEND MMLTK_GUI_RUNTIME_SOURCES
        src/gui/browser_runtime_cef.cpp
        src/gui/browser_websocket_server.cpp
        src/gui/cef_workspace_gpu_bridge.cpp
        src/gui/cef_signal_restore.cpp
        src/gui/cef_subprocess_app.cpp
        src/gui/cef_views/cef_app_runner.cpp
        src/gui/cef_views/cef_browser_shell.cpp
        src/gui/cef_views/cef_browser_view_delegate.cpp
        src/gui/cef_views/cef_window_delegate.cpp
    )
    set(MMLTK_GUI_VAST_BRIDGE_SOURCE "${CMAKE_SOURCE_DIR}/utilities/vast_bridge.py")
    if(NOT EXISTS "${MMLTK_GUI_VAST_BRIDGE_SOURCE}")
        message(FATAL_ERROR
            "Missing Vast GUI bridge script: ${MMLTK_GUI_VAST_BRIDGE_SOURCE}")
    endif()

    set(MMLTK_GUI_BROWSER_HOST_SOURCES
        src/browser/host_api.cpp
        src/gui/annotation/controller.cpp
        src/gui/annotation/editor.cpp
        src/gui/annotation/editor_history.cpp
        src/gui/annotation/sidebar_actions.cpp
        ${MMLTK_GUI_ANNOTATION_SOURCES}
        src/gui/annotation_core.cpp
        src/gui/annotation_core_preview.cpp
        src/gui/annotation_core_persistence.cpp
        src/gui/app_browser_host.cpp
        src/gui/app_options.cpp
        src/gui/browser_host_adapters.cpp
        src/gui/browser_retained_frame_registry.cpp
        src/gui/browser_workspace_surface_bridge.cpp
        src/gui/canvas_layers.cpp
        src/gui/cli_seed.cpp
        src/gui/default_state.cpp
        src/gui/file_picker_runtime.cpp
        src/gui/gui_settings.cpp
        src/gui/live_predict_controller.cpp
        src/gui/live_session_utils.cpp
        src/gui/local_train_controller.cpp
        src/gui/preview_rect_drag.cpp
        src/gui/remote_train_controller.cpp
        src/gui/rfdetr_module.cpp
        src/gui/rfdetr_workflows.cpp
        src/gui/source_runtime.cpp
        src/gui/source_selection_runtime.cpp
        src/gui/still_image_preview.cpp
        src/gui/train_command.cpp
        src/gui/train_process_runtime.cpp
        src/gui/vast_query_controller.cpp
        src/gui/vast_runtime.cpp
    )

    add_library(mmltk_gui_browser_host_module STATIC
        ${MMLTK_GUI_BROWSER_HOST_SOURCES}
    )
    mmltk_target_private_src_dir(mmltk_gui_browser_host_module)
    target_link_libraries(mmltk_gui_browser_host_module
        PUBLIC
            mmltk_build_options
            mmltk_core
            mmltk_runtime
            mmltk_rfdetr_native
            mmltk_logging
            mmltk_browser_api_contract
        PRIVATE
            cli11
            nlohmann_json
    )
    target_compile_definitions(mmltk_gui_browser_host_module PRIVATE
        MMLTK_GUI_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
        MMLTK_GUI_VAST_BRIDGE_SOURCE="${MMLTK_GUI_VAST_BRIDGE_SOURCE}"
    )
    mmltk_enable_torch(mmltk_gui_browser_host_module PRIVATE)
    if(MMLTK_RFDETR_LIVE_CAPTURE_ENABLED)
        target_link_libraries(mmltk_gui_browser_host_module PRIVATE
            mmltk_live_capture
            mmltk_live_runtime
        )
    endif()
    mmltk_target_cuda_architectures(mmltk_gui_browser_host_module)
    mmltk_configure_target_pch(mmltk_gui_browser_host_module
        HEADER src/pch/gui_browser_host.hpp)

    add_executable(mmltk_gui
        src/gui/main.cpp
        ${MMLTK_GUI_RUNTIME_SOURCES}
    )
    mmltk_target_private_src_dir(mmltk_gui)
    target_include_directories(mmltk_gui SYSTEM PRIVATE
        "${MMLTK_CEF_ROOT}"
    )
    target_compile_definitions(mmltk_gui PRIVATE
        MMLTK_CEF_RUNTIME_ROOT_SOURCE="${MMLTK_CEF_ROOT}"
        MMLTK_CEF_WEBGPU_RUNTIME="${MMLTK_CEF_WEBGPU_RUNTIME}"
        MMLTK_CEF_ENABLE_UNSAFE_WEBGPU=$<IF:$<BOOL:${MMLTK_CEF_ENABLE_UNSAFE_WEBGPU}>,1,0>
        MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU=$<IF:$<BOOL:${MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU}>,1,0>
    )
    target_compile_options(mmltk_gui PRIVATE
        $<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:CXX>>:-Wno-shadow>
    )
    target_link_libraries(mmltk_gui PRIVATE
        mmltk_gui_browser_host_module
        mmltk_rfdetr_runtime_link_deps
        mmltk_logging
        ${MMLTK_GUI_RUNTIME_LINK_LIBS}
    )
    if(DEFINED MMLTK_BROWSER_APP_DIST_DIR)
        target_compile_definitions(mmltk_gui PRIVATE
            MMLTK_BROWSER_APP_ASSET_ROOT_SOURCE="${MMLTK_BROWSER_APP_DIST_DIR}"
        )
    endif()
    mmltk_configure_target_pch(mmltk_gui
        HEADER src/pch/gui_browser_host.hpp)
    set_target_properties(mmltk_gui PROPERTIES
        ENABLE_EXPORTS ON
        OUTPUT_NAME "mmltk-browser-host"
    )
    add_dependencies(mmltk_gui mmltk_browser_app)
    mmltk_target_cuda_architectures(mmltk_gui)
    mmltk_enable_native_runtime_rpath(mmltk_gui)
    mmltk_target_append_rpath(mmltk_gui BUILD_RPATH
        "${MMLTK_CEF_BINARY_ROOT}")
    mmltk_target_append_rpath(mmltk_gui INSTALL_RPATH
        "\$ORIGIN/../${MMLTK_INSTALL_CEF_RUNTIMEDIR}/Debug")

    mmltk_install_target(mmltk_gui)

    # Minimal CEF subprocess binary. CEF spawns one of these for every GPU,
    # renderer, utility, and zygote process; pointing browser_subprocess_path
    # at this stripped helper keeps the heavy host runtime (CUDA, PyTorch)
    # out of those subprocesses, where it would otherwise hang the GPU process
    # initialization on NVIDIA Linux.
    add_executable(mmltk_browser_helper
        src/browser/host_api.cpp
        src/gui/browser_helper_main.cpp
        src/gui/cef_workspace_gpu_bridge.cpp
        src/gui/cef_subprocess_app.cpp
    )
    mmltk_target_project_dirs(mmltk_browser_helper)
    target_include_directories(mmltk_browser_helper SYSTEM PRIVATE
        "${MMLTK_CEF_ROOT}"
        ${CUDAToolkit_INCLUDE_DIRS}
    )
    target_compile_definitions(mmltk_browser_helper PRIVATE
        MMLTK_CEF_WEBGPU_RUNTIME="${MMLTK_CEF_WEBGPU_RUNTIME}"
        MMLTK_CEF_ENABLE_UNSAFE_WEBGPU=$<IF:$<BOOL:${MMLTK_CEF_ENABLE_UNSAFE_WEBGPU}>,1,0>
        MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU=$<IF:$<BOOL:${MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU}>,1,0>
    )
    target_compile_options(mmltk_browser_helper PRIVATE
        $<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:CXX>>:-Wno-shadow>
    )
    target_link_libraries(mmltk_browser_helper PRIVATE
        mmltk_cef_shared
        libcef_dll_wrapper
        mmltk_browser_api_contract
        mmltk_logging
        nlohmann_json
        spdlog
        ${CMAKE_DL_LIBS}
    )
    set_target_properties(mmltk_browser_helper PROPERTIES
        OUTPUT_NAME "mmltk-browser-helper"
    )
    mmltk_enable_native_runtime_rpath(mmltk_browser_helper)
    mmltk_target_append_rpath(mmltk_browser_helper BUILD_RPATH
        "${MMLTK_CEF_BINARY_ROOT}")
    mmltk_target_append_rpath(mmltk_browser_helper INSTALL_RPATH
        "\$ORIGIN/../${MMLTK_INSTALL_CEF_RUNTIMEDIR}/Debug")
    add_dependencies(mmltk_gui mmltk_browser_helper)
    mmltk_install_target(mmltk_browser_helper)

    mmltk_install_file("${MMLTK_GUI_VAST_BRIDGE_SOURCE}" "${MMLTK_INSTALL_PYTHONDIR}"
        RENAME "vast_bridge.py")
    mmltk_install_pinned_cef_runtime("${MMLTK_CEF_ROOT}"
        "${MMLTK_INSTALL_CEF_RUNTIMEDIR}")
endif()
