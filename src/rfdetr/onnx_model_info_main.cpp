#include "rfdetr/onnx_model_info.h"
#include "rfdetr/onnx_tool_args.h"
#include "mmltk_logging.h"

#include <sstream>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string shape_to_string(const std::vector<int64_t>& shape) {
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

void log_line(const std::string& message) {
    mmltk::logging::logger("rfdetr.onnx_info")->info("{}", message);
}

void print_model_metadata(const mmltk::rfdetr::ModelInfo& info) {
    log_line("model[" + info.backend + "]: path=" + info.model_path + " input=" + info.input.name + " " +
             shape_to_string(info.input.shape) + " outputs=" + std::to_string(info.outputs.size()) +
             " queries=" + std::to_string(info.num_queries) + " classes=" + std::to_string(info.num_classes) +
             " dataset_images=0 dataset_classes=0");
    for (const auto& output : info.outputs) {
        log_line("  output: " + output.name + " " + shape_to_string(output.shape) + " " + output.dtype);
    }
}

void report_onnx_info_error(std::string_view message) noexcept {
    try {
        mmltk::logging::logger("rfdetr.onnx_info")->error("{}", message);
        return;
    } catch (...) {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
        return;
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const mmltk::rfdetr::SingleOnnxToolArgs parsed = mmltk::rfdetr::parse_single_onnx_tool_args(
            argc,
            argv,
            "usage: mmltk-rfdetr-onnx-info [--log-level LEVEL] [--log-file PATH] [--log-dir PATH] MODEL.onnx");
        mmltk::logging::initialize(
            mmltk::logging::merge(mmltk::logging::config_from_env("mmltk-rfdetr-onnx-info"),
                                  parsed.logging));
        print_model_metadata(mmltk::rfdetr::load_onnx_model_info(parsed.model_path));
        return 0;
    } catch (const std::exception& error) {
        report_onnx_info_error(std::string("mmltk rfdetr onnx info error: ") + error.what());
        return 1;
    }
}
