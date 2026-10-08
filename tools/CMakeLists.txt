cmake_minimum_required(VERSION 4.4.3)
if(NOT CMAKE_VERSION VERSION_EQUAL "4.4.3")
    message(FATAL_ERROR "Native tools require exactly CMake 4.4.3")
endif()

include(cmake/Toolchain.cmake)
project(mmltk_tools LANGUAGES CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(MMLTK_TOOLS_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${MMLTK_TOOLS_REPOSITORY}/build/tools" CACHE PATH "Published native tools" FORCE)
endif()

option(MMLTK_TOOLS_BUILD_DRIVER "Build the standalone driver and native fixtures" ON)
option(MMLTK_TOOLS_BUILD_ORACLE "Build the oracle in the pinned CPython donor environment" OFF)
option(MMLTK_TOOLS_BUILD_WESTON_SEAT "Build the Weston 13 validation plugin" OFF)
include(cmake/RuntimeClosure.cmake)

# Support and consumers are registered explicitly. No source discovery or
# parallel module inventory participates in this graph.
if(MMLTK_TOOLS_BUILD_DRIVER OR MMLTK_TOOLS_BUILD_ORACLE)
    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    add_library(mmltk_tool_support STATIC)
    mmltk_tools_configure_target(mmltk_tool_support)
    target_sources(mmltk_tool_support PUBLIC FILE_SET CXX_MODULES FILES
        cpp_text.cpp
        cpp_support.cpp
        cpp_digest.cpp)
    target_include_directories(mmltk_tool_support SYSTEM PUBLIC
        "${MMLTK_TOOLS_REPOSITORY}/third_party/json/include")
    target_link_libraries(mmltk_tool_support PUBLIC Threads::Threads OpenSSL::Crypto)
endif()

if(MMLTK_TOOLS_BUILD_DRIVER)
    find_package(LibArchive REQUIRED)
    find_package(Boost REQUIRED)
    find_package(tomlplusplus CONFIG REQUIRED)
    find_package(ICU REQUIRED COMPONENTS uc i18n)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)
    find_path(MMLTK_TOOLS_NUMA_INCLUDE_DIR numaif.h REQUIRED)
    find_path(MMLTK_TOOLS_VULKAN_INCLUDE_DIR vulkan/vulkan_core.h REQUIRED)
    file(READ "${MMLTK_TOOLS_REPOSITORY}/docker/nvidia-payload.json" _payload)
    string(JSON _cuda_root GET "${_payload}" cuda_root)
    find_path(MMLTK_TOOLS_CUDA_INCLUDE_DIR cuda.h HINTS "$ENV{MMLTK_TOOLS_CUDA_INCLUDE_DIR}" "${_cuda_root}/include" REQUIRED)

    add_library(mmltk_tool_modules STATIC)
    mmltk_tools_configure_target(mmltk_tool_modules)
    target_sources(mmltk_tool_modules PUBLIC FILE_SET CXX_MODULES FILES
        cpp_linux.cpp
        cpp_locale.cpp
        cpp_shared_library.cpp
        cpp_time.cpp
        cleanup/cpd_patterns.cpp
        cleanup/declaration_patterns.cpp
        cleanup/review_patterns.cpp
        audit_includes.cpp
        audit_test_inventory.cpp
        build_time_trace.cpp
        check_cargo_dupes.cpp
        check_toolchain_invariants.cpp
        collect_time_traces.cpp
        configure_cmake_graph.cpp
        diagnose_benchmark_image.cpp
        diagnose_dataset_io.cpp
        diagnose_gpu_environment.cpp
        diagnose_native_link.cpp
        diagnose_native_process.cpp
        diagnose_native_symbols.cpp
        diagnose_nvidia_payload.cpp
        format_declarations.cpp
        generate_cleanup_json.cpp
        headless_compositor.cpp
        log_query.cpp
        mmltk_container_commands.cpp
        module_safe_compiler_launcher.cpp
        run_codex_actionplan.cpp
        run_static_analysis.cpp
        run_traced_command.cpp
        run_uncomment.cpp
        runtime_package.cpp
        tool_command.cpp
        tests/cpp_test_support.cpp
        tests/audit_includes.test.cpp
        tests/format_declarations.test.cpp
        tests/generate_cleanup_json.test.cpp
        tests/review_candidates.test.cpp
        tests/test_headless_compositor.cpp
    PRIVATE FILE_SET internal_modules TYPE CXX_MODULES FILES
        log_query_internal.cpp
        tests/test_log_query.cpp)
    target_sources(mmltk_tool_modules PRIVATE tool_dispatch.cpp log_query_runtime.cpp)
    target_link_libraries(mmltk_tool_modules PUBLIC mmltk_tool_support
        Boost::headers tomlplusplus::tomlplusplus ICU::uc ICU::i18n PkgConfig::PCRE2 LibArchive::LibArchive ${CMAKE_DL_LIBS})
    # CUDA, Vulkan and NUMA contribute declarations only. Runtime observation
    # opens their libraries in the selected diagnostic's existing child role.
    target_include_directories(mmltk_tool_modules SYSTEM PRIVATE
        "${MMLTK_TOOLS_CUDA_INCLUDE_DIR}"
        "${MMLTK_TOOLS_NUMA_INCLUDE_DIR}"
        "${MMLTK_TOOLS_VULKAN_INCLUDE_DIR}")
    add_executable(mmltk mmltk.cpp)
    mmltk_tools_configure_target(mmltk)
    target_link_libraries(mmltk PRIVATE mmltk_tool_modules)
    mmltk_tools_install_runtime(mmltk)
endif()

if(MMLTK_TOOLS_BUILD_ORACLE)
    # This is configured only in the pinned donor image. It is never linked
    # into the host driver, and importing an ordinary tool cannot start Python.
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Embed)
    find_path(MMLTK_TOOLS_PYBIND11_INCLUDE_DIR pybind11/embed.h
        HINTS /usr/local/lib/python3.12/dist-packages/torch/include
        REQUIRED)
    add_executable(mmltk_rfdetr_upstream_oracle)
    mmltk_tools_register_process(mmltk_rfdetr_upstream_oracle
        "${CMAKE_CURRENT_SOURCE_DIR}/generate_rfdetr_upstream_oracle.cpp"
        mmltk.tools.generate_rfdetr_upstream_oracle mmltk_tool_generate_rfdetr_upstream_oracle)
    target_include_directories(mmltk_rfdetr_upstream_oracle SYSTEM PRIVATE "${MMLTK_TOOLS_PYBIND11_INCLUDE_DIR}")
    target_link_libraries(mmltk_rfdetr_upstream_oracle PRIVATE mmltk_tool_support Python3::Python)
    set_target_properties(mmltk_rfdetr_upstream_oracle PROPERTIES OUTPUT_NAME mmltk-rfdetr-upstream-oracle)
    mmltk_tools_install_runtime(mmltk_rfdetr_upstream_oracle)
endif()

if(MMLTK_TOOLS_BUILD_WESTON_SEAT)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WESTON REQUIRED IMPORTED_TARGET libweston-13 wayland-server)
    add_library(mmltk_headless_weston_seat MODULE)
    mmltk_tools_configure_target(mmltk_headless_weston_seat)
    target_sources(mmltk_headless_weston_seat PRIVATE FILE_SET CXX_MODULES FILES headless_weston_seat.cpp)
    target_link_libraries(mmltk_headless_weston_seat PRIVATE PkgConfig::WESTON)
    target_link_options(mmltk_headless_weston_seat PRIVATE -Wl,-z,defs)
    mmltk_tools_add_entry_adapter(mmltk_headless_weston_seat WESTON
        MODULE mmltk.tools.headless_weston_seat FUNCTION mmltk_headless_weston_seat)
    set_target_properties(mmltk_headless_weston_seat PROPERTIES PREFIX "" OUTPUT_NAME "mmltk-headless-seat")
    install(TARGETS mmltk_headless_weston_seat LIBRARY DESTINATION lib/weston COMPONENT Runtime)
endif()

# Product-dependent modules are registered by their owners using
# mmltk_tools_register_process from cmake/Toolchain.cmake. They never enter the
# standalone driver or acquire its ordinary external dependency closure.
