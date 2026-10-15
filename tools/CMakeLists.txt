cmake_minimum_required(VERSION 4.4.3)
if(NOT CMAKE_VERSION VERSION_EQUAL "4.4.3")
    message(FATAL_ERROR "Native tools require exactly CMake 4.4.3")
endif()

include(cmake/StdModules.cmake)
include(cmake/Toolchain.cmake)
project(mmltk_tools LANGUAGES CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(MMLTK_TOOLS_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${MMLTK_TOOLS_REPOSITORY}/build/tools" CACHE PATH "Published native tools" FORCE)
endif()

option(MMLTK_TOOLS_BUILD_DRIVER "Build the standalone driver" ON)
option(MMLTK_TOOLS_OPTIMIZE_SIZE "Optimize standalone Release executables for size" ON)
set(MMLTK_TOOLS_HOST_LIBRARY_DIR "build/tools/lib" CACHE STRING "Runtime library directory relative to the host executable")
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
        support/cpp_text.cpp
        support/cpp_support.cpp
        support/cpp_digest.cpp)
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
        support/cpp_linux.cpp
        support/cpp_locale.cpp
        support/cpp_shared_library.cpp
        support/cpp_time.cpp
        cleanup/cpd_patterns.cpp
        cleanup/declaration_patterns.cpp
        cleanup/review_patterns.cpp
        cleanup/audit_includes.cpp
        maintenance/audit_test_inventory.cpp
        profiling/build_time_trace.cpp
        maintenance/check_cargo_dupes.cpp
        toolchain/check_toolchain_invariants.cpp
        profiling/collect_time_traces.cpp
        toolchain/configure_cmake_graph.cpp
        diagnostics/diagnose_benchmark_image.cpp
        diagnostics/diagnose_dataset_io.cpp
        diagnostics/diagnose_gpu_environment.cpp
        diagnostics/diagnose_native_link.cpp
        diagnostics/diagnose_native_process.cpp
        diagnostics/diagnose_native_symbols.cpp
        diagnostics/diagnose_nvidia_payload.cpp
        cleanup/format_declarations.cpp
        cleanup/generate_cleanup_json.cpp
        wayland/headless_compositor.cpp
        logging/log_query.cpp
        driver/mmltk_container_commands.cpp
        toolchain/module_safe_compiler_launcher.cpp
        maintenance/run_codex_actionplan.cpp
        toolchain/run_static_analysis.cpp
        profiling/run_traced_command.cpp
        maintenance/run_uncomment.cpp
        toolchain/runtime_package.cpp
        driver/tool_command.cpp
        tests/cpp_test_support.cpp
        tests/audit_includes.test.cpp
        tests/format_declarations.test.cpp
        tests/generate_cleanup_json.test.cpp
        tests/review_candidates.test.cpp
        tests/test_headless_compositor.cpp
    PRIVATE FILE_SET internal_modules TYPE CXX_MODULES FILES
        logging/log_query_internal.cpp
        tests/test_log_query.cpp)
    target_sources(mmltk_tool_modules PRIVATE logging/log_query_runtime.cpp)
    target_link_libraries(mmltk_tool_modules PUBLIC mmltk_tool_support
        Boost::headers tomlplusplus::tomlplusplus ICU::uc ICU::i18n PkgConfig::PCRE2 LibArchive::LibArchive ${CMAKE_DL_LIBS})
    # CUDA, Vulkan and NUMA contribute declarations only. Runtime observation
    # opens their libraries in the selected diagnostic's existing child role.
    target_include_directories(mmltk_tool_modules SYSTEM PRIVATE
        "${MMLTK_TOOLS_CUDA_INCLUDE_DIR}"
        "${MMLTK_TOOLS_NUMA_INCLUDE_DIR}"
        "${MMLTK_TOOLS_VULKAN_INCLUDE_DIR}")
    add_library(mmltk_tool_dispatch STATIC)
    mmltk_tools_configure_target(mmltk_tool_dispatch)
    target_sources(mmltk_tool_dispatch PUBLIC FILE_SET CXX_MODULES FILES driver/tool_dispatch.cpp)
    target_link_libraries(mmltk_tool_dispatch PUBLIC mmltk_tool_modules)

    add_executable(mmltk mmltk.cpp)
    mmltk_tools_configure_target(mmltk)
    target_link_libraries(mmltk PRIVATE mmltk_tool_dispatch)
    mmltk_tools_install_runtime(mmltk)

    # Fixture objects share the production module archive but are referenced
    # only by this runner, so the release driver contains no fixture code.
    add_executable(mmltk_tool_tests tests/tool_main.cpp)
    mmltk_tools_configure_target(mmltk_tool_tests)
    target_sources(mmltk_tool_tests PRIVATE FILE_SET CXX_MODULES FILES tests/tool_dispatch.cpp)
    target_link_libraries(mmltk_tool_tests PRIVATE mmltk_tool_dispatch)
    set_target_properties(mmltk_tool_tests PROPERTIES OUTPUT_NAME mmltk-tests)
    mmltk_tools_install_runtime(mmltk_tool_tests)
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
        "${CMAKE_CURRENT_SOURCE_DIR}/oracle/generate_rfdetr_upstream_oracle.cpp"
        mmltk.tools.generate_rfdetr_upstream_oracle mmltk_tool_generate_rfdetr_upstream_oracle)
    target_sources(mmltk_rfdetr_upstream_oracle PRIVATE oracle/native_catalog.cpp)
    target_include_directories(mmltk_rfdetr_upstream_oracle PRIVATE
        "${MMLTK_TOOLS_REPOSITORY}"
        "${MMLTK_TOOLS_REPOSITORY}/src/frameworks/reflection/include")
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
    target_sources(mmltk_headless_weston_seat PRIVATE FILE_SET CXX_MODULES FILES wayland/headless_weston_seat.cpp)
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
