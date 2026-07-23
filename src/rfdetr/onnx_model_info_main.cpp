#include "rfdetr/onnx_model_info.h"
#include "rfdetr/onnx_tool_shared.h"
#include "mmltk_logging.h"

#include <string>

namespace {

void log_line(const std::string& message) {
    mmltk::logging::logger("rfdetr.onnx_info")->info("{}", message);
}

void print_model_metadata(const mmltk::rfdetr::ModelInfo& info) {
    log_line("model[" + info.backend + "]: path=" + info.model_path + " input=" + info.input.name + " " +
             mmltk::rfdetr::format_shape(info.input.shape) + " outputs=" + std::to_string(info.outputs.size()) +
             " queries=" + std::to_string(info.num_queries) + " classes=" + std::to_string(info.num_classes) +
             " dataset_images=0 dataset_classes=0");
    for (const auto& output : info.outputs) {
        log_line("  output: " + output.name + " " + mmltk::rfdetr::format_shape(output.shape) + " " + output.dtype);
    }
}

}  

int main(int argc, char** argv) {
    return mmltk::rfdetr::run_single_onnx_tool_main(
        argc, argv, "usage: mmltk-rfdetr-onnx-info [--log-level LEVEL] [--log-file PATH] [--log-dir PATH] MODEL.onnx",
        "mmltk-rfdetr-onnx-info", "rfdetr.onnx_info", "mmltk rfdetr onnx info error: ",
        [](const std::string& model_path) { print_model_metadata(mmltk::rfdetr::load_onnx_model_info(model_path)); });
}
