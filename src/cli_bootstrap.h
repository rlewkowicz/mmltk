#pragma once

#include "CLI11.hpp"

#include <string>

namespace mmltk::cli_support {

struct LoggingOptions {
    std::string level;
    std::string log_file;
    std::string log_dir;
};

inline void add_logging_options(CLI::App& app, LoggingOptions& options) {
    app.add_option("--log-level",
                   options.level,
                   "Logging level (trace, debug, info, warn, error, critical, off)");
    app.add_option("--log-file", options.log_file, "Explicit log file path")
        ->type_name("PATH");
    app.add_option("--log-dir", options.log_dir, "Log directory for the default rotating file sink")
        ->type_name("PATH");
}

} // namespace mmltk::cli_support
