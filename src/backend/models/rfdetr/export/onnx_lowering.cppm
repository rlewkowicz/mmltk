module;
#include <cstdint>
#include <string>
#include "src/backend/ml/cuda/tensor_readback.h"
export module mmltk.backend.models.rfdetr.model_export.onnx_lowering;
export namespace mmltk::backend::models::rfdetr {
constexpr int kSupportedOnnxExportOpsetVersion = 19;
enum class OnnxTensorElementType : std::uint8_t {
 Float32,
 Float16,
 BFloat16,
 Float64,
 Bool,
 UInt8,
 Int8,
 Int16,
 Int32,
 Int64,
};
using OnnxInitializerLookup = const void* (*)(const void*, const std::string&) noexcept;
struct OnnxLoweringRequest final {
 void* graph = nullptr;
 const void* initializer_context = nullptr;
 OnnxInitializerLookup find_initializer = nullptr;
 mmltk::backend::ml::cuda::TensorReadbackBuffers* readback = nullptr;
};
int onnx_tensor_data_type(OnnxTensorElementType element_type);
void validate_supported_onnx_export_opset(int opset_version);
void lower_graph_for_onnx_export(const OnnxLoweringRequest& request);
}  // namespace mmltk::backend::models::rfdetr
