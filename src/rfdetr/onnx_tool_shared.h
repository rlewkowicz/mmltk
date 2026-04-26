#pragma once

#include "mmltk_logging.h"
#include "rfdetr/onnx_tool_args.h"

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

inline std::string format_shape(const std::vector<int64_t>& shape) {
    std::ostringstream stream;
    stream << "[";
    for (size_t index = 0; index < shape.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << shape[index];
    }
    stream << "]";
    return stream.str();
}

inline void report_onnx_tool_error(std::string_view logger_name, std::string_view message) noexcept {
    try {
        mmltk::logging::logger(std::string(logger_name))->error("{}", message);
    } catch (...) {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    }
}

template <typename RunFn>
int run_single_onnx_tool_main(const int argc, char** argv, std::string_view usage, std::string_view app_name,
                              std::string_view logger_name, std::string_view error_prefix, RunFn&& run_fn) {
    try {
        const SingleOnnxToolArgs parsed = parse_single_onnx_tool_args(argc, argv, std::string(usage));
        mmltk::logging::initialize(
            mmltk::logging::merge(mmltk::logging::config_from_env(std::string(app_name)), parsed.logging));
        std::forward<RunFn>(run_fn)(parsed.model_path);
        return 0;
    } catch (const std::exception& error) {
        report_onnx_tool_error(logger_name, std::string(error_prefix) + error.what());
        return 1;
    }
}

}  // namespace mmltk::rfdetr
