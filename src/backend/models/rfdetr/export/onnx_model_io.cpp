module;
#ifndef ONNX_NAMESPACE
#define ONNX_NAMESPACE mmltk_onnx
#endif

#include <onnx/checker.h>
#include <onnx/onnx_pb.h>
#include <onnx/shape_inference/implementation.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/backend/models/rfdetr/core/model_info.h"

module mmltk.backend.models.rfdetr.model_export;

import :onnx_model_info;
import :onnx_simplify;
import mmltk.common.logging.mmltk_logging;

namespace mmltk::backend::models::rfdetr {
namespace {

[[nodiscard]] std::runtime_error onnx_model_io_error(const char* message, const std::filesystem::path& path) {
    return std::runtime_error(std::string(message) + ": " + path.string());
}

[[nodiscard]] ONNX_NAMESPACE::ModelProto load_onnx_model(const std::filesystem::path& model_path) {
    ONNX_NAMESPACE::ModelProto model;
    std::ifstream input(model_path, std::ios::binary);
    if (!input.is_open()) { throw onnx_model_io_error("failed to open ONNX model", model_path); }
    if (!model.ParseFromIstream(&input)) { throw onnx_model_io_error("failed to parse ONNX model", model_path); }
    return model;
}

void write_onnx_model(const ONNX_NAMESPACE::ModelProto& model, const std::filesystem::path& output_path) {
    if (!output_path.parent_path().empty()) { std::filesystem::create_directories(output_path.parent_path()); }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) { throw onnx_model_io_error("failed to open ONNX model for writing", output_path); }
    if (!model.SerializeToOstream(&output)) { throw onnx_model_io_error("failed to write ONNX model", output_path); }
}

[[nodiscard]] std::string onnx_tensor_dtype_name(const int elem_type) {
    using DataType = ONNX_NAMESPACE::TensorProto_DataType;
    switch (static_cast<DataType>(elem_type)) {
        case DataType::TensorProto_DataType_FLOAT:
            return "float32";
        case DataType::TensorProto_DataType_FLOAT16:
            return "float16";
        case DataType::TensorProto_DataType_DOUBLE:
            return "float64";
        case DataType::TensorProto_DataType_INT8:
            return "int8";
        case DataType::TensorProto_DataType_INT16:
            return "int16";
        case DataType::TensorProto_DataType_INT32:
            return "int32";
        case DataType::TensorProto_DataType_INT64:
            return "int64";
        case DataType::TensorProto_DataType_UINT8:
            return "uint8";
        case DataType::TensorProto_DataType_UINT16:
            return "uint16";
        case DataType::TensorProto_DataType_BOOL:
            return "bool";
        case DataType::TensorProto_DataType_BFLOAT16:
            return "bfloat16";
        default:
            return "unknown";
    }
}

[[nodiscard]] TensorInfo tensor_info_from_value_info(const ONNX_NAMESPACE::ValueInfoProto& value_info) {
    TensorInfo info;
    info.name = value_info.name();
    if (!value_info.has_type() || !value_info.type().has_tensor_type()) {
        info.dtype = "unknown";
        return info;
    }

    const auto& tensor_type = value_info.type().tensor_type();
    info.dtype = onnx_tensor_dtype_name(tensor_type.elem_type());
    if (!tensor_type.has_shape()) { return info; }

    info.shape.reserve(static_cast<std::size_t>(tensor_type.shape().dim_size()));
    for (const auto& dimension : tensor_type.shape().dim()) {
        info.shape.push_back(dimension.has_dim_value() ? dimension.dim_value() : -1);
    }
    return info;
}

void run_onnx_simplify(ONNX_NAMESPACE::ModelProto& model) {
    try {
        mmltk::common::logging::info([](auto& logger) { logger.info("onnx: checking exported model..."); });
        ONNX_NAMESPACE::checker::check_model(model);
        mmltk::common::logging::info([](auto& logger) { logger.info("onnx: running shape inference..."); });
        ONNX_NAMESPACE::shape_inference::InferShapes(model, ONNX_NAMESPACE::OpSchemaRegistry::Instance());
        mmltk::common::logging::info([](auto& logger) { logger.info("onnx: shape inference complete"); });
    } catch (const ONNX_NAMESPACE::checker::ValidationError& error) {
        throw std::runtime_error(std::string("RF-DETR ONNX simplify failed: ONNX checker "
                                             "rejected the model: ") +
                                 error.what());
    } catch (const ONNX_NAMESPACE::InferenceError& error) {
        throw std::runtime_error(std::string("RF-DETR ONNX simplify failed: ONNX shape inference "
                                             "rejected the model: ") +
                                 error.what());
    } catch (const std::exception& error) { throw std::runtime_error(std::string("RF-DETR ONNX simplify failed: ") + error.what()); }
}

}  // namespace

void write_onnx_model_bytes(const std::string_view serialized_model, const std::filesystem::path& output_path) {
    if (serialized_model.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("serialized ONNX model exceeds stream capacity");
    }
    if (!output_path.parent_path().empty()) { std::filesystem::create_directories(output_path.parent_path()); }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) { throw onnx_model_io_error("failed to open ONNX output file", output_path); }
    output.write(serialized_model.data(), static_cast<std::streamsize>(serialized_model.size()));
    if (!output) { throw onnx_model_io_error("failed to write ONNX output file", output_path); }
}

ModelInfo load_onnx_model_info(const std::filesystem::path& model_path) {
    const ONNX_NAMESPACE::ModelProto model = load_onnx_model(model_path);
    const auto& graph = model.graph();
    if (graph.input_size() == 0) { throw std::runtime_error("ONNX model has no graph inputs: " + model_path.string()); }

    std::set<std::string> initializer_names;
    for (const auto& initializer : graph.initializer()) {
        initializer_names.insert(initializer.name());
    }

    const ONNX_NAMESPACE::ValueInfoProto* selected_input = nullptr;
    for (const auto& input : graph.input()) {
        if (!initializer_names.contains(input.name())) {
            selected_input = &input;
            break;
        }
    }
    if (selected_input == nullptr) { selected_input = &graph.input(0); }

    ModelInfo info;
    info.backend = "onnx";
    info.model_path = model_path.string();
    info.input = tensor_info_from_value_info(*selected_input);
    info.outputs.reserve(static_cast<std::size_t>(graph.output_size()));
    for (const auto& output : graph.output()) {
        info.outputs.push_back(tensor_info_from_value_info(output));
    }
    infer_rfdetr_output_layout(info);
    return info;
}

void simplify_onnx_model_file(const std::filesystem::path& model_path) {
    auto model = load_onnx_model(model_path);
    run_onnx_simplify(model);
    write_onnx_model(model, model_path);
}

}  // namespace mmltk::backend::models::rfdetr
