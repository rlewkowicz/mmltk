#include "rfdetr/onnx_model_io.h"

#include <fstream>
#include <stdexcept>

namespace mmltk::rfdetr {

std::runtime_error onnx_model_io_error(const char* message, const std::filesystem::path& path) {
    return std::runtime_error(std::string(message) + ": " + path.string());
}

ONNX_NAMESPACE::ModelProto load_onnx_model(const std::filesystem::path& model_path) {
    ONNX_NAMESPACE::ModelProto model;
    std::ifstream input(model_path, std::ios::binary);
    if (!input.is_open()) {
        throw onnx_model_io_error("failed to open ONNX model", model_path);
    }
    if (!model.ParseFromIstream(&input)) {
        throw onnx_model_io_error("failed to parse ONNX model", model_path);
    }
    return model;
}

void write_onnx_model(const ONNX_NAMESPACE::ModelProto& model, const std::filesystem::path& output_path) {
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw onnx_model_io_error("failed to open ONNX model for writing", output_path);
    }
    if (!model.SerializeToOstream(&output)) {
        throw onnx_model_io_error("failed to write ONNX model", output_path);
    }
}

} // namespace mmltk::rfdetr
