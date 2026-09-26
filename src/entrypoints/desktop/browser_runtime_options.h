#pragma once
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include "src/controller/shell/application_shell.h"
#include "src/frameworks/reflection/cli_declarations.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::entrypoints::desktop {
using BrowserPresentationConfig = mmltk::controller::shell::ApplicationShellConfig::PresentationConfig;
struct BrowserRuntimeConfig final {
 BrowserPresentationConfig presentation{};
 bool h2d_dataloader = true;
};
MMLTK_REFLECT_FIELDS(BrowserRuntimeConfig)
using BrowserPresentation = mmltk::frameworks::reflection::CliScope<BrowserRuntimeConfig>::within<&BrowserRuntimeConfig::presentation>;
inline constexpr std::array kBrowserExecutionOptions{
 MMLTK_CLI_NAMED(BrowserPresentation, cuda_device_index, "--device-id", "CUDA-visible visual device", "Execution"),
 MMLTK_CLI_OPTION(BrowserPresentation, numa_node, "GPU-local NUMA node (-1 selects automatic locality)", "Execution"),
 mmltk::frameworks::reflection::negative_flag<BrowserRuntimeConfig, &BrowserRuntimeConfig::h2d_dataloader>("--gdrcopy", "Use GDRCopy for compiled-image loading", "Execution")};
[[nodiscard]] inline BrowserRuntimeConfig parse_browser_runtime_options(std::span<const std::string_view> arguments) {
 auto parsed = mmltk::frameworks::reflection::parse<BrowserRuntimeConfig>(arguments, kBrowserExecutionOptions);
 if (!parsed) throw parsed.error();
 return std::move(parsed->request);
}
}  // namespace mmltk::entrypoints::desktop
