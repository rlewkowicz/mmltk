#include "rfdetr/onnx_model_info.h"
#include "rfdetr/onnx_model_io.h"

#include <set>
#include <stdexcept>
#include <string>

namespace mmltk::rfdetr {

namespace {

std::string onnx_tensor_dtype_name(int elem_type) {
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

TensorInfo tensor_info_from_value_info(const ONNX_NAMESPACE::ValueInfoProto& value_info) {
    TensorInfo info;
    info.name = value_info.name();
    if (!value_info.has_type() || !value_info.type().has_tensor_type()) {
        info.dtype = "unknown";
        return info;
    }

    const auto& tensor_type = value_info.type().tensor_type();
    info.dtype = onnx_tensor_dtype_name(tensor_type.elem_type());
    if (!tensor_type.has_shape()) {
        return info;
    }

    info.shape.reserve(static_cast<size_t>(tensor_type.shape().dim_size()));
    for (const auto& dim : tensor_type.shape().dim()) {
        info.shape.push_back(dim.has_dim_value() ? dim.dim_value() : -1);
    }
    return info;
}

void infer_rfdetr_output_layout(ModelInfo& info) {
    for (const TensorInfo& output : info.outputs) {
        if (output.shape.size() == 3 && output.shape.back() == 4) {
            info.num_queries = output.shape[1];
        } else if (output.shape.size() == 3) {
            info.num_classes = output.shape[2];
        } else if (output.shape.size() == 4) {
            info.has_masks = true;
        }
    }
}

}  // namespace

ModelInfo load_onnx_model_info(const std::filesystem::path& model_path) {
    const ONNX_NAMESPACE::ModelProto model = load_onnx_model(model_path);

    const auto& graph = model.graph();
    if (graph.input_size() == 0) {
        throw std::runtime_error("ONNX model has no graph inputs: " + model_path.string());
    }

    std::set<std::string> initializer_names;
    for (const auto& initializer : graph.initializer()) {
        initializer_names.insert(initializer.name());
    }

    const ONNX_NAMESPACE::ValueInfoProto* selected_input = nullptr;
    for (const auto& input : graph.input()) {
        if (initializer_names.find(input.name()) == initializer_names.end()) {
            selected_input = &input;
            break;
        }
    }
    if (selected_input == nullptr) {
        selected_input = &graph.input(0);
    }

    ModelInfo info;
    info.backend = "onnx";
    info.model_path = model_path.string();
    info.input = tensor_info_from_value_info(*selected_input);
    info.outputs.reserve(static_cast<size_t>(graph.output_size()));
    for (const auto& output : graph.output()) {
        info.outputs.push_back(tensor_info_from_value_info(output));
    }
    infer_rfdetr_output_layout(info);
    return info;
}

}  // namespace mmltk::rfdetr
