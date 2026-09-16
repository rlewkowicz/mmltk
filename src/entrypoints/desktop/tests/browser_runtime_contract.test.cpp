#include <catch2/catch_test_macros.hpp>
#include "src/entrypoints/desktop/browser_runtime_options.h"
#include "src/controller/services/firefox_process_owner.h"
#include "src/controller/shell/application_shell.h"
namespace {
namespace services = mmltk::controller::services;
namespace shell = mmltk::controller::shell;
TEST_CASE("desktop runtime success follows Firefox and shell results") {
    const services::FirefoxProcessLifecycle firefox{.terminal = services::FirefoxProcessTerminal::Exited, .status = 0};
    CHECK(services::browser_runtime_exit_status(firefox, true) == 0);
    CHECK(services::browser_runtime_exit_status(firefox, false) != 0);
}
TEST_CASE("desktop shell configuration carries presentation settings") {
    shell::ApplicationShellConfig configuration;
    configuration.presentation.cuda_device_index = 0;
    CHECK(configuration.presentation.cuda_device_index == 0);
}
}  // namespace
TEST_CASE("desktop NUMA override is reflected, validated and materialized") {
    const std::array<std::string_view, 5> arguments{"--device-id", "2", "--numa-node", "3", "--gdrcopy"};
    const auto configuration = mmltk::entrypoints::desktop::parse_browser_runtime_options(arguments);
    CHECK(configuration.presentation.cuda_device_index == 2);
    CHECK(configuration.presentation.numa_node == 3);
    CHECK_FALSE(configuration.h2d_dataloader);
    CHECK(mmltk::entrypoints::desktop::parse_browser_runtime_options({}).h2d_dataloader);
    const std::array<std::string_view, 1> removed{"--h2d-dataloader"};
    CHECK_THROWS(mmltk::entrypoints::desktop::parse_browser_runtime_options(removed));
    const std::array<std::string_view, 2> invalid{"--numa-node", "-2"};
    CHECK_THROWS(mmltk::entrypoints::desktop::parse_browser_runtime_options(invalid));
}
TEST_CASE("ordinary shell and native presentation configuration disable acceptance perturbation") {
    const shell::ApplicationShellConfig shell_configuration;
    const mmltk::controller::PresentationNativeConfiguration native_configuration;
    CHECK_FALSE(shell_configuration.pending_supersession_acceptance);
    CHECK_FALSE(native_configuration.pending_supersession_acceptance);
    CHECK_FALSE(shell_configuration.completion_acceptance);
    CHECK_FALSE(native_configuration.completion_acceptance);
}
