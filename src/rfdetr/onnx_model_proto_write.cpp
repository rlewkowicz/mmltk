#include "rfdetr/onnx_model_io.h"

#include <torch/csrc/jit/serialization/export.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace mmltk::rfdetr {

namespace {

std::runtime_error onnx_proto_write_error(const char* message, const std::filesystem::path& path) {
    const std::string path_string = path.string();
    std::string error_message;
    error_message.reserve(std::strlen(message) + 2U + path_string.size());
    error_message.append(message);
    error_message.append(": ");
    error_message.append(path_string);
    return std::runtime_error(error_message);
}

}  

std::string serialize_onnx_model_proto(const std::shared_ptr<ONNX_NAMESPACE::ModelProto>& model_proto) {
    if (model_proto == nullptr) {
        throw std::runtime_error("torch::jit::export_onnx returned a null ONNX model");
    }
    return torch::jit::serialize_model_proto_to_string(model_proto);
}

void write_onnx_model_proto(const std::shared_ptr<ONNX_NAMESPACE::ModelProto>& model_proto,
                            const std::filesystem::path& output_path) {
    const std::string serialized = serialize_onnx_model_proto(model_proto);
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        throw onnx_proto_write_error("failed to open ONNX output file", output_path);
    }
    out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    out.close();
}

}  
