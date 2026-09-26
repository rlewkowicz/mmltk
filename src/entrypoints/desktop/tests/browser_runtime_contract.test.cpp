#include <catch2/catch_test_macros.hpp>
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include "src/entrypoints/desktop/browser_runtime_options.h"
#include "src/controller/services/firefox_process_owner.h"
#include "src/controller/shell/application_shell.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
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
TEST_CASE("desktop scoped options retain public spellings help and emission order") {
 namespace desktop = mmltk::entrypoints::desktop;
 namespace reflection = mmltk::frameworks::reflection;
 const auto configuration = desktop::parse_browser_runtime_options(std::array<std::string_view, 3>{"--device-id=2", "--numa-node=3", "--gdrcopy"});
 std::vector<std::string> emitted;
 reflection::emit(emitted, configuration, desktop::kBrowserExecutionOptions);
 CHECK((emitted == std::vector<std::string>{"--device-id", "2", "--numa-node", "3", "--gdrcopy"}));
 CHECK(reflection::help("desktop [options]", "Desktop", desktop::kBrowserExecutionOptions) ==
       "Desktop\nUsage: desktop [options]\nExecution"
       "\n  --device-id  CUDA-visible visual device"
       "\n  --numa-node  GPU-local NUMA node (-1 selects automatic locality)"
       "\n  --gdrcopy  Use GDRCopy for compiled-image loading");
 for (const std::string_view spelling : {"--cuda-device-index", "--no-numa-node", "--h2d-dataloader"}) {
  CHECK_THROWS(desktop::parse_browser_runtime_options(std::array{spelling}));
 }
}
