include(CheckCXXSourceRuns)
include(MmltkComponent)

option(BUILD_TESTS "Build tests and profile runners" OFF)
if(NOT BUILD_TESTS)
    return()
endif()

check_cxx_source_runs([=[
    #include <cstdint>
    #include <csignal>
    #include <linux/sched.h>
    #include <poll.h>
    #include <sys/syscall.h>
    #include <sys/wait.h>
    #include <unistd.h>

    int main() {
        int pidfd = -1;
        clone_args arguments{};
        arguments.flags = CLONE_PIDFD;
        arguments.pidfd = reinterpret_cast<std::uintptr_t>(&pidfd);
        arguments.exit_signal = SIGCHLD;
        const auto child = static_cast<pid_t>(
            ::syscall(SYS_clone3, &arguments, sizeof(arguments)));
        if (child == 0) ::_exit(0);
        if (child < 0 || pidfd < 0) return 1;
        pollfd ready{.fd = pidfd, .events = POLLIN, .revents = 0};
        const timespec timeout{.tv_sec = 1, .tv_nsec = 0};
        const int ready_count = ::ppoll(&ready, 1U, &timeout, nullptr);
        if (ready_count != 1 || (ready.revents & POLLIN) == 0) {
            static_cast<void>(
                ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0U));
            siginfo_t ignored{};
            static_cast<void>(::waitid(
                P_PIDFD, static_cast<id_t>(pidfd), &ignored, WEXITED));
            static_cast<void>(::close(pidfd));
            return 2;
        }
        siginfo_t terminal{};
        const int wait_result = ::waitid(
            P_PIDFD, static_cast<id_t>(pidfd), &terminal, WEXITED);
        static_cast<void>(::close(pidfd));
        return wait_result == 0 && terminal.si_pid == child &&
                       terminal.si_code == CLD_EXITED &&
                       terminal.si_status == 0
                   ? 0
                   : 3;
    }
]=] MMLTK_HAVE_ATOMIC_PIDFD_TEST_SUPPORT)
if(NOT MMLTK_HAVE_ATOMIC_PIDFD_TEST_SUPPORT)
    message(FATAL_ERROR
        "BUILD_TESTS requires Linux kernel 5.4 or newer with "
        "clone3(CLONE_PIDFD), pollable pidfds, and waitid(P_PIDFD); "
        "the build/test container seccomp profile must allow clone3.")
endif()

enable_testing()
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "Install Catch2 documentation" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "Install Catch2 extras" FORCE)
set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "Build Catch2 development extras" FORCE)
set(CATCH_BUILD_TESTING OFF CACHE BOOL "Build Catch2 self tests" FORCE)
set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "Build Catch2 examples" FORCE)
set(CATCH_BUILD_EXTRA_TESTS OFF CACHE BOOL "Build Catch2 extra tests" FORCE)
set(CATCH_BUILD_BENCHMARKS OFF CACHE BOOL "Build Catch2 benchmarks" FORCE)
set(CATCH_CONFIG_NO_THREAD_SAFE_ASSERTIONS ON CACHE BOOL
    "Force-disable Catch2 thread-safe assertions" FORCE)
file(MAKE_DIRECTORY
    "${CMAKE_BINARY_DIR}/third_party/catch2/generated-includes/catch2")
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/third_party/catch2
    ${CMAKE_BINARY_DIR}/third_party/catch2
    EXCLUDE_FROM_ALL
)
set_target_properties(Catch2 Catch2WithMain PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/third_party/catch2/extras)
include(Catch)

add_library(mmltk_acceptance_support STATIC EXCLUDE_FROM_ALL)
mmltk_configure_component(
    mmltk_acceptance_support PRIVATE_DECLARATIONS)
mmltk_register_retained_module_importers(
    mmltk_acceptance_support
    tests/catch_main.cpp
    tests/filesystem_test_utils.cpp
    tests/profile_runner_common.cpp)
mmltk_register_private_headers(
    mmltk_acceptance_support
    tests/async_test_utils.hpp
    tests/catch2_compat.hpp
    tests/cli_option_table.h
    tests/error_expectation_test_utils.hpp
    tests/filesystem_test_utils.hpp
    tests/linux_process_test_utils.hpp
    tests/parse_cli_integer.h
    tests/profile_runner_common.h
    tests/require_test_utils.hpp
    tests/subprocess_test_utils.hpp
    tests/websocket_test_utils.hpp
    tests/workspace_surface_socket_test_utils.hpp)
target_include_directories(mmltk_acceptance_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(mmltk_acceptance_support PUBLIC
    Catch2::Catch2
    mmltk::common::io
    mmltk::common::logging
    stb
)

add_executable(mmltk_acceptance EXCLUDE_FROM_ALL)
add_executable(mmltk::acceptance ALIAS mmltk_acceptance)
mmltk_configure_component(mmltk_acceptance PRIVATE_DECLARATIONS)
mmltk_register_ordinary_sources(
    mmltk_acceptance tests/vendored_helpers.test.cpp)
target_include_directories(mmltk_acceptance PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
)
target_compile_definitions(mmltk_acceptance PRIVATE
    MMLTK_TEST_MMLTK_CLI_PATH="$<TARGET_FILE:mmltk_entrypoints_cli>"
    MMLTK_TEST_MMLTK_GUI_LAUNCHER="/opt/mmltk/bin/mmltk-browser-host"
    MMLTK_TEST_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
add_dependencies(mmltk_acceptance mmltk_entrypoints_cli)
target_link_libraries(mmltk_acceptance PRIVATE
    mmltk_acceptance_support
    mmltk::common::logging
)
if(BUILD_RFDETR_NATIVE)
    mmltk_register_retained_module_importers(
        mmltk_acceptance
        tests/compiled_dataset_explore.test.cpp
    )
    mmltk_register_ordinary_sources(
        mmltk_acceptance
        tests/compile_progress_presentation.test.cpp
        tests/direct_browser_runtime.test.cpp)
    target_link_libraries(mmltk_acceptance PRIVATE
        mmltk::backend::data
        mmltk::backend::imaging::explore
        mmltk::common::io
        mmltk::controller::browser::direct
        mmltk::controller::contracts
        mmltk::controller::direct_services
        mmltk::controller::shell
        mmltk::controller::data_compute_systems
        mmltk_backend_data_test_support
        mmltk_backend_imaging_explore_test_support
        CUDA::cudart
        nlohmann_json
    )

    add_executable(mmltk_workspace_wayland_integration EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_workspace_wayland_integration PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_workspace_wayland_integration
        tests/workspace_wayland_integration.test.cpp)
    target_compile_definitions(mmltk_workspace_wayland_integration PRIVATE
        MMLTK_TEST_MMLTK_GUI_LAUNCHER="/opt/mmltk/bin/mmltk-browser-host")
    target_link_libraries(mmltk_workspace_wayland_integration PRIVATE
        mmltk_acceptance_support
        mmltk::backend::data
        mmltk::common::io
        mmltk::controller::contracts
        mmltk::controller::direct_services
        mmltk_backend_data_test_support
        CUDA::cuda_driver
        nlohmann_json)
endif()
catch_discover_tests(
    mmltk_acceptance
    TEST_PREFIX "acceptance::"
    DISCOVERY_MODE PRE_TEST
    ADD_TAGS_AS_LABELS
)

mmltk_add_common_system_tests()
mmltk_add_common_concurrency_tests()
mmltk_add_frameworks_gpu_tests()
mmltk_add_frameworks_process_tests()
mmltk_add_frameworks_serialization_tests()
mmltk_add_frameworks_transport_tests()
mmltk_add_backend_data_tests()
mmltk_add_backend_models_rfdetr_contract_tests()
mmltk_add_backend_models_catalog_tests()
mmltk_add_entrypoints_cli_tests()

if(BUILD_RFDETR_NATIVE)
    mmltk_add_backend_ml_cuda_tests()
    mmltk_add_backend_ml_layers_tests()
    mmltk_add_backend_imaging_annotation_tests()
    mmltk_add_backend_imaging_explore_tests()
    mmltk_add_backend_imaging_upscale_tests()
    mmltk_add_backend_media_live_tests()
    mmltk_add_controller_presentation_tests()
    mmltk_add_controller_annotation_tests()
    mmltk_add_backend_models_rfdetr_core_tests()
    mmltk_add_backend_models_rfdetr_augmentation_tests()
    mmltk_add_backend_models_rfdetr_training_tests()
    mmltk_add_backend_models_rfdetr_inference_tests()
    mmltk_add_backend_models_rfdetr_export_tests()
    mmltk_add_entrypoints_tools_tests()
endif()

if(TARGET mmltk_controller_shell)
    mmltk_add_controller_shell_tests()
    mmltk_add_entrypoints_desktop_tests()
    mmltk_add_controller_services_tests()
    mmltk_add_controller_browser_tests()
endif()
if(TARGET mmltk_controller_data_compute_systems)
    mmltk_add_controller_subsystems_system_tests()
endif()
