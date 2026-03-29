#pragma once

#include <ATen/core/ScalarType.h>
#include <ATen/core/Tensor.h>
#include <torch/csrc/jit/ir/ir.h>

#include <memory>
#include <unordered_map>

namespace fastloader::rfdetr {

constexpr int kSupportedOnnxExportOpsetVersion = 19;
using OnnxInitializerMap = std::unordered_map<std::string, at::Tensor>;

int onnx_tensor_data_type(at::ScalarType scalar_type);
void validate_supported_onnx_export_opset(int opset_version);
void lower_graph_for_onnx_export(
    const std::shared_ptr<torch::jit::Graph>& graph,
    const OnnxInitializerMap* initializers = nullptr);

} // namespace fastloader::rfdetr
