#include <NvInfer.h>
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include "detail/tensorrt_engine_access.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
namespace mmltk::backend::ml::runtime {
namespace {
void destroy_execution_context(nvinfer1::IExecutionContext* pointer) noexcept { delete pointer; }
using ExecutionContextOwner = std::unique_ptr<nvinfer1::IExecutionContext, decltype(&destroy_execution_context)>;
// CLEANUP-IGNORE: This exhaustive TensorRT-to-runtime conversion owns a distinct external ABI boundary.
[[nodiscard]] RuntimeElementType runtime_element_type(const nvinfer1::DataType value) {
 switch (value) {
  case nvinfer1::DataType::kFLOAT: return RuntimeElementType::Float32;
  case nvinfer1::DataType::kHALF: return RuntimeElementType::Float16;
  case nvinfer1::DataType::kINT32: return RuntimeElementType::Int32;
  case nvinfer1::DataType::kINT64: return RuntimeElementType::Int64;
  case nvinfer1::DataType::kBOOL: return RuntimeElementType::Bool;
  default: throw std::runtime_error("unsupported TensorRT tensor element type");
 }
}
[[nodiscard]] RuntimeShape runtime_shape(const nvinfer1::Dims& dimensions) {
 if (dimensions.nbDims < 0 || dimensions.nbDims > static_cast<std::int32_t>(kMaximumRuntimeRank)) {
  throw std::runtime_error("TensorRT tensor rank exceeds the public bound");
 }
 RuntimeShape shape{.rank = static_cast<std::uint8_t>(dimensions.nbDims)};
 std::copy_n(dimensions.d, dimensions.nbDims, shape.extents.begin());
 return shape;
}
[[nodiscard]] nvinfer1::Dims tensor_dimensions(const RuntimeShape& shape) {
 nvinfer1::Dims dimensions{};
 dimensions.nbDims = shape.rank;
 std::copy_n(shape.extents.begin(), shape.rank, dimensions.d);
 return dimensions;
}
[[nodiscard]] RuntimeModelInfo read_model_info(const TensorRtEngine& owner, const std::filesystem::path& model_path) {
 nvinfer1::ICudaEngine& native = detail::TensorRtEngineAccess::Get(owner);
 RuntimeModelInfo info{};
 info.model_path = model_path;
 for (std::int32_t index = 0; index < native.getNbIOTensors(); ++index) {
  const char* name = native.getIOTensorName(index);
  if (name == nullptr) { throw std::runtime_error("TensorRT exposed an unnamed model tensor"); }
  RuntimeTensorDescriptor descriptor{
   .name = name,
   .shape = runtime_shape(native.getTensorShape(name)),
   .element_type = runtime_element_type(native.getTensorDataType(name)),
  };
  if (native.getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) {
   if (!info.input.name.empty()) { throw std::runtime_error("runtime backend requires exactly one model input"); }
   info.input = std::move(descriptor);
   continue;
  }
  if (info.output_count == kMaximumRuntimeOutputs) { throw std::runtime_error("model output count exceeds the public runtime bound"); }
  info.outputs[info.output_count++] = std::move(descriptor);
 }
 if (info.input.name.empty()) { throw std::runtime_error("runtime backend requires exactly one model input"); }
 return info;
}
[[nodiscard]] TensorRtEngineOptions make_engine_options(const RuntimeBackendOptions& options) {
 TensorRtEngineOptions result{};
 result.device = options.device;
 // Preserve the request permission; TensorRT 11 builds the model's declared types.
 result.allow_fp16 = options.allow_fp16;
 result.save_engine_path = options.save_compiled_model_path;
 return result;
}
class TensorRtSharedState final {
public:
 explicit TensorRtSharedState(const RuntimeBackendOptions& options)
     : engine_(options.model_path, make_engine_options(options)),
       model_info_(read_model_info(engine_, options.model_path)),
       device_(options.device),
       command_stream_(options.command_stream) {}
 [[nodiscard]] const TensorRtEngine& engine() const noexcept { return engine_; }
 [[nodiscard]] const RuntimeModelInfo& model_info() const noexcept { return model_info_; }
 [[nodiscard]] std::int32_t device() const noexcept { return device_; }
 [[nodiscard]] BorrowedCommandStream command_stream() const noexcept { return command_stream_; }
 void Save(const std::filesystem::path& path) const {
  std::lock_guard lock(save_mutex_);
  engine_.Save(path);
 }

private:
 TensorRtEngine engine_;
 RuntimeModelInfo model_info_;
 std::int32_t device_ = -1;
 BorrowedCommandStream command_stream_{};
 mutable std::mutex save_mutex_;
};
class TensorRtRuntimeBackend final : public RuntimeBackend {
public:
 explicit TensorRtRuntimeBackend(std::shared_ptr<const TensorRtSharedState> shared)
     : RuntimeBackend(shared->device(), shared->command_stream()), shared_(std::move(shared)), context_(nullptr, destroy_execution_context) {
  context_.reset(detail::TensorRtEngineAccess::Get(shared_->engine()).createExecutionContext());
  if (context_ == nullptr) throw std::runtime_error("TensorRT failed to create an execution context");
 }
 ~TensorRtRuntimeBackend() override { static_cast<void>(Close()); }
 [[nodiscard]] const RuntimeModelInfo& model_info() const noexcept override { return shared_->model_info(); }
 [[nodiscard]] std::shared_ptr<RuntimeBackend> MakeLane() const override { return std::make_shared<TensorRtRuntimeBackend>(shared_); }
 void SaveCompiledModel(const std::filesystem::path& path) const override { shared_->Save(path); }

private:
 [[nodiscard]] RawSubmission Submit(const RuntimeTensorBuffer& input, const std::span<RuntimeTensorBuffer> outputs,
                                    const RuntimeContinuation continuation) override {
  cuda_lane().Activate();
  const RuntimeModelInfo& info = shared_->model_info();
  if (!context_->setInputShape(info.input.name.c_str(), tensor_dimensions(input.shape)) ||
      !context_->setInputTensorAddress(info.input.name.c_str(), input.device_data)) {
   throw std::runtime_error("TensorRT failed to bind the model input");
  }
  const std::int32_t unresolved = context_->inferShapes(0, nullptr);
  if (unresolved != 0) { throw std::runtime_error("TensorRT model shapes are not fully specified"); }
  std::array<RuntimeShape, kMaximumRuntimeOutputs> resolved_shapes{};
  for (std::size_t index = 0U; index < outputs.size(); ++index) {
   resolved_shapes[index] = runtime_shape(context_->getTensorShape(info.outputs[index].name.c_str()));
   static_cast<void>(validate_runtime_tensor_buffer(info.outputs[index], outputs[index], &resolved_shapes[index]));
  }
  for (std::size_t index = 0U; index < outputs.size(); ++index) {
   outputs[index].shape = resolved_shapes[index];
   if (!context_->setOutputTensorAddress(info.outputs[index].name.c_str(), outputs[index].device_data)) {
    throw std::runtime_error("TensorRT failed to bind a model output");
   }
  }
  if (!context_->enqueueV3(reinterpret_cast<cudaStream_t>(cuda_lane().native_stream()))) {
   cuda_lane().Synchronize();
   throw std::runtime_error("TensorRT enqueueV3 failed");
  }
  cuda_lane().Record(continuation);
  return cuda_lane().Receipt(outputs.size());
 }
 void ReleaseBackendResources() noexcept override { context_.reset(); }
 std::shared_ptr<const TensorRtSharedState> shared_;
 ExecutionContextOwner context_;
};
}  // namespace
[[nodiscard]] std::shared_ptr<RuntimeBackend> make_runtime_backend(const RuntimeBackendOptions& options) {
 switch (options.kind) {
  case RuntimeBackendKind::Onnx: return make_onnx_runtime_backend(options);
  case RuntimeBackendKind::TensorRt: return std::make_shared<TensorRtRuntimeBackend>(std::make_shared<const TensorRtSharedState>(options));
 }
 throw std::invalid_argument("invalid runtime backend kind");
}
}  // namespace mmltk::backend::ml::runtime
