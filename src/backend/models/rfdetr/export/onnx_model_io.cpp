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
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/frameworks/reflection/reflection_metadata.h"

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
    output.close();
    if (!output) throw onnx_model_io_error("failed to finish ONNX model", output_path);
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
    for (const auto& metadata : value_info.metadata_props()) {
        if (metadata.key() != "mmltk.rfdetr.output_role") continue;
        if (info.role != RfdetrOutputRole::Unspecified) throw std::invalid_argument("duplicate ONNX output role metadata");
        info.role = mmltk::frameworks::reflection::enum_from_name<RfdetrOutputRole>(metadata.value());
        if (info.role == RfdetrOutputRole::Unspecified) throw std::invalid_argument("unresolved ONNX output role metadata");
    }
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

void write_onnx_model_bytes(const std::string_view serialized_model, const std::filesystem::path& output_path,
    const ModelClassLayout& layout) {
    ONNX_NAMESPACE::ModelProto model;
    if (serialized_model.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !model.ParseFromArray(serialized_model.data(), static_cast<int>(serialized_model.size())))
        throw std::invalid_argument("invalid serialized ONNX model");
    auto* metadata = model.add_metadata_props();
    metadata->set_key("mmltk.rfdetr.class_layout");
    metadata->set_value(encode_class_layout(layout));
    write_onnx_model(model, output_path);
}

ModelInfo load_onnx_model_info(const std::filesystem::path& model_path, std::span<const RfdetrNamedOutputRole> roles) {
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
    for (const auto& metadata : model.metadata_props()) {
        if (metadata.key() != "mmltk.rfdetr.class_layout") continue;
        if (info.class_layout) throw std::invalid_argument("duplicate ONNX class layout metadata");
        info.class_layout = decode_class_layout(metadata.value());
    }
    apply_rfdetr_output_roles(info, roles);
    static_cast<void>(validate_rfdetr_output_layout(info));
    if (info.class_layout && info.class_layout->slots.size() != static_cast<std::size_t>(info.num_classes))
        throw std::invalid_argument("ONNX class layout disagrees with logits width");
    return info;
}

void simplify_onnx_model_file(const std::filesystem::path& model_path) {
    ClassArtifactPublication publication(model_path);
    auto source_lease = publication.LockPreviousArtifact();
    const auto& descriptor = publication.previous_descriptor();
    const auto roles = descriptor ? descriptor->output_roles : std::vector<RfdetrNamedOutputRole>{};
    const auto admitted = load_onnx_model_info(model_path, roles);
    const auto expected = admit_artifact_class_layout(admitted.num_classes, admitted.class_layout,
        descriptor ? std::span<const ModelClassDescriptor>(&*descriptor, 1) : std::span<const ModelClassDescriptor>{});
    auto model = load_onnx_model(model_path);
    source_lease = {};
    run_onnx_simplify(model);
    write_onnx_model(model, publication.staged_artifact());
    const auto reopened = load_onnx_model_info(publication.staged_artifact(), roles);
    if (admit_artifact_class_layout(reopened.num_classes, reopened.class_layout,
            descriptor ? std::span<const ModelClassDescriptor>(&*descriptor, 1) : std::span<const ModelClassDescriptor>{}) != expected ||
        rfdetr_output_roles(reopened) != rfdetr_output_roles(admitted))
        throw std::runtime_error("ONNX simplification changed admitted class metadata");
    publication.Publish(descriptor);
}

}  // namespace mmltk::backend::models::rfdetr
