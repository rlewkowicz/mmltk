#include "detail/onnx_tool_main.h"
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
import mmltk.common.logging.mmltk_logging;
namespace mmltk::entrypoints::tools {
int run_onnx_tool_main(const int argc, char** argv, const OnnxToolMainConfig& config, const OnnxToolOperation operation) {
    const auto component = config.error_prefix.ends_with(": ") ? config.error_prefix.substr(0U, config.error_prefix.size() - 2U) : config.error_prefix;
    try {
        auto logging_config = mmltk::common::logging::config_from_env(std::string(config.application_name));
        logging_config = mmltk::common::logging::merge(std::move(logging_config), mmltk::common::logging::scan_cli_overrides(argc, argv));
        std::vector<std::string_view> positionals;
        positionals.reserve(1U);
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--help" || argument == "-h") {
                std::puts(std::string(config.usage).c_str());
                return 0;
            }
            if (argument == "--log-level" || argument == "--log-file" || argument == "--log-dir") {
                if (++index >= argc) throw std::invalid_argument("logging option requires a value");
                continue;
            }
            if (argument.starts_with("--log-level=") || argument.starts_with("--log-file=") || argument.starts_with("--log-dir=")) { continue; }
            positionals.push_back(argument);
        }
        if (positionals.size() != 1U) throw std::invalid_argument(std::string(config.usage));
        mmltk::common::logging::initialize(logging_config);
        operation(std::filesystem::path(positionals.front()));
        return 0;
    } catch (const std::exception& error) {
        mmltk::common::logging::report_fatal(component, error.what(), std::nullopt, config.logger_name);
        return 1;
    } catch (...) {
        mmltk::common::logging::report_fatal(component, "unknown exception", std::nullopt, config.logger_name);
        return 1;
    }
}
}  // namespace mmltk::entrypoints::tools
