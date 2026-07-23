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
    "Build the Firefox-hosted iced RF-DETR desktop shell" OFF)
set(MMLTK_INSTALL_FIREFOX_RUNTIMEDIR
    "${CMAKE_INSTALL_LIBDIR}/mmltk/firefox" CACHE STRING
    "Install-relative directory for the owned Firefox runtime")

if(BUILD_MMLTK_BROWSER_HOST)
    if(NOT BUILD_RFDETR_NATIVE)
        message(FATAL_ERROR "BUILD_MMLTK_BROWSER_HOST requires BUILD_RFDETR_NATIVE=ON.")
    endif()
    if(NOT BUILD_MMLTK_BROWSER_APP)
        message(FATAL_ERROR "BUILD_MMLTK_BROWSER_HOST requires BUILD_MMLTK_BROWSER_APP=ON.")
    endif()
    if(NOT MMLTK_NEEDS_PYTHON)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()
    find_package(CURL REQUIRED)

    get_property(_mmltk_enabled_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    if(NOT "C" IN_LIST _mmltk_enabled_languages)
        enable_language(C)
    endif()
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

    set(MMLTK_GUI_VAST_BRIDGE_SOURCE "${CMAKE_SOURCE_DIR}/utilities/vast_bridge.py")
    if(NOT EXISTS "${MMLTK_GUI_VAST_BRIDGE_SOURCE}")
        message(FATAL_ERROR "Missing Vast GUI bridge script: ${MMLTK_GUI_VAST_BRIDGE_SOURCE}")
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
        src/gui/browser_artifact_controller.cpp
        src/gui/app_options.cpp
        src/gui/browser_host_adapters.cpp
        src/gui/workspace_surface_broker.cpp
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
            CURL::libcurl
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
    add_dependencies(mmltk_gui_browser_host_module mmltk_browser_app_contract)

    add_executable(mmltk_gui
        src/gui/main.cpp
        src/gui/browser_runtime_entry.cpp
        src/gui/browser_runtime_firefox.cpp
        src/gui/browser_app_server.cpp
    )
    mmltk_target_private_src_dir(mmltk_gui)
    target_compile_definitions(mmltk_gui PRIVATE
        MMLTK_FIREFOX_RUNTIME_ROOT_SOURCE="${MMLTK_FIREFOX_RUNTIME_ROOT}"
        MMLTK_FIREFOX_RUNTIME_INSTALL_RELATIVE="${MMLTK_INSTALL_FIREFOX_RUNTIMEDIR}"
        MMLTK_BROWSER_APP_ASSET_ROOT_SOURCE="${MMLTK_BROWSER_APP_DIST_DIR}"
    )
    target_compile_options(mmltk_gui PRIVATE
        $<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:CXX>>:-Wno-shadow>
    )
    target_link_libraries(mmltk_gui PRIVATE
        mmltk_gui_browser_host_module
        mmltk_rfdetr_runtime_link_deps
        mmltk_logging
        uwebsockets
        ${CMAKE_DL_LIBS}
    )
    mmltk_configure_target_pch(mmltk_gui
        HEADER src/pch/gui_browser_host.hpp)
    set_target_properties(mmltk_gui PROPERTIES
        ENABLE_EXPORTS ON
        OUTPUT_NAME "mmltk-browser-host"
    )
    mmltk_target_cuda_architectures(mmltk_gui)
    mmltk_enable_native_runtime_rpath(mmltk_gui)
    mmltk_install_target(mmltk_gui)

    string(CONCAT _mmltk_firefox_install_validation
        "set(_mmltk_firefox_runtime_root [==[${MMLTK_FIREFOX_RUNTIME_ROOT}]==])\n"
        "foreach(_mmltk_firefox_runtime_file IN ITEMS\n"
        "    firefox firefox-bin libxul.so libmozgtk.so libmozwayland.so\n"
        "    application.ini platform.ini omni.ja browser/omni.ja)\n"
        "  if(NOT EXISTS \"\${_mmltk_firefox_runtime_root}/\${_mmltk_firefox_runtime_file}\")\n"
        "    message(FATAL_ERROR\n"
        "      \"Incomplete owned Firefox package: missing \"\n"
        "      \"\${_mmltk_firefox_runtime_root}/\${_mmltk_firefox_runtime_file}\")\n"
        "  endif()\n"
        "endforeach()\n"
    )
    install(CODE "${_mmltk_firefox_install_validation}"
        COMPONENT mmltk_firefox_runtime)
    install(DIRECTORY "${MMLTK_FIREFOX_RUNTIME_ROOT}/"
        DESTINATION "${MMLTK_INSTALL_FIREFOX_RUNTIMEDIR}"
        USE_SOURCE_PERMISSIONS
        COMPONENT mmltk_firefox_runtime
    )
    mmltk_install_file("${MMLTK_GUI_VAST_BRIDGE_SOURCE}" "${MMLTK_INSTALL_PYTHONDIR}"
        RENAME "vast_bridge.py")
endif()
