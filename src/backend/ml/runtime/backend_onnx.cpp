#include <cuda_runtime.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"

namespace mmltk::backend::ml::runtime {

namespace {

[[nodiscard]] RuntimeElementType runtime_element_type(const ONNXTensorElementDataType value) {
    switch (value) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
            return RuntimeElementType::Float32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
            return RuntimeElementType::Float16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
            return RuntimeElementType::Int32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
            return RuntimeElementType::Int64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
            return RuntimeElementType::Bool;
        default:
            throw std::runtime_error("unsupported ONNX Runtime tensor element type");
    }
}

[[nodiscard]] ONNXTensorElementDataType onnx_element_type(const RuntimeElementType value) {
    switch (value) {
        case RuntimeElementType::Float16:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
        case RuntimeElementType::Float32:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
        case RuntimeElementType::Int32:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
        case RuntimeElementType::Int64:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
        case RuntimeElementType::Bool:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
    }
    throw std::runtime_error("invalid runtime tensor element type");
}

[[nodiscard]] RuntimeShape runtime_shape(const std::vector<std::int64_t>& extents) {
    if (extents.size() > kMaximumRuntimeRank) { throw std::runtime_error("runtime tensor rank exceeds the public bound"); }
    RuntimeShape shape{.rank = static_cast<std::uint8_t>(extents.size())};
    std::copy(extents.begin(), extents.end(), shape.extents.begin());
    return shape;
}

[[nodiscard]] std::span<const std::int64_t> shape_extents(const RuntimeShape& shape) {
    if (shape.rank > kMaximumRuntimeRank) { throw std::invalid_argument("runtime tensor rank exceeds the public bound"); }
    return {shape.extents.data(), shape.rank};
}

Ort::Env& onnx_environment() {
    static Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "mmltk_runtime");
    return environment;
}

class OnnxBindingScope final {
   public:
    explicit OnnxBindingScope(Ort::IoBinding& binding) noexcept : binding_(binding) {}
    ~OnnxBindingScope() noexcept {
        try {
            binding_.ClearBoundInputs();
            binding_.ClearBoundOutputs();
        } catch (...) {}
    }

    OnnxBindingScope(const OnnxBindingScope&) = delete;
    OnnxBindingScope& operator=(const OnnxBindingScope&) = delete;

   private:
    Ort::IoBinding& binding_;
};

class OnnxRuntimeBackend final : public RuntimeBackend {
   public:
    explicit OnnxRuntimeBackend(RuntimeBackendOptions options)
        : RuntimeBackend(options.device, options.command_stream), options_(std::move(options)) {
        if (options_.model_path.empty()) { throw std::invalid_argument("ONNX runtime model path is empty"); }
        session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_options_.SetIntraOpNumThreads(1);
        session_options_.SetInterOpNumThreads(1);
        OrtCUDAProviderOptions cuda_options{};
        cuda_options.device_id = options_.device;
        cuda_options.do_copy_in_default_stream = 1;
        cuda_options.has_user_compute_stream = 1;
        cuda_options.user_compute_stream = reinterpret_cast<cudaStream_t>(cuda_lane().native_stream());
        session_options_.AppendExecutionProvider_CUDA(cuda_options);
        session_ = std::make_unique<Ort::Session>(onnx_environment(), options_.model_path.string().c_str(), session_options_);
        binding_ = std::make_unique<Ort::IoBinding>(*session_);
        ReadModelInfo();
        output_values_.reserve(info_.output_count);
    }

    ~OnnxRuntimeBackend() override { static_cast<void>(Close()); }

    [[nodiscard]] const RuntimeModelInfo& model_info() const noexcept override { return info_; }

    [[nodiscard]] RawSubmission Submit(const RuntimeTensorBuffer& input, const std::span<RuntimeTensorBuffer> outputs,
                                       const RuntimeContinuation continuation) override {
        cuda_lane().Activate();
        OnnxBindingScope binding_scope{*binding_};
        const Ort::MemoryInfo memory("Cuda", OrtArenaAllocator, options_.device, OrtMemTypeDefault);
        const auto input_shape = shape_extents(input.shape);
        Ort::Value input_value = Ort::Value::CreateTensor(memory, input.device_data, input.capacity_bytes, input_shape.data(),
                                                          input_shape.size(), onnx_element_type(input.element_type));
        binding_->BindInput(info_.input.name.c_str(), input_value);

        output_values_.clear();
        for (std::size_t index = 0U; index < outputs.size(); ++index) {
            RuntimeTensorBuffer& output = outputs[index];
            const auto output_shape = shape_extents(output.shape);
            output_values_.push_back(Ort::Value::CreateTensor(memory, output.device_data, output.capacity_bytes, output_shape.data(),
                                                              output_shape.size(), onnx_element_type(output.element_type)));
            binding_->BindOutput(info_.outputs[index].name.c_str(), output_values_.back());
        }
        try {
            session_->Run(Ort::RunOptions{nullptr}, *binding_);
        } catch (...) { cuda_lane().RethrowAfterSynchronization(std::current_exception()); }
        cuda_lane().Record(continuation);
        return cuda_lane().Receipt(outputs.size());
    }

    void ReleaseBackendResources() noexcept override {
        binding_.reset();
        session_.reset();
    }

    [[nodiscard]] std::shared_ptr<RuntimeBackend> MakeLane() const override { return std::make_shared<OnnxRuntimeBackend>(options_); }

    void SaveCompiledModel(const std::filesystem::path&) const override {
        throw std::logic_error("ONNX Runtime does not produce a compiled model artifact");
    }

   private:
    void ReadModelInfo() {
        Ort::AllocatorWithDefaultOptions allocator;
        if (session_->GetInputCount() != 1U) { throw std::runtime_error("runtime backend requires exactly one model input"); }
        const auto input_name = session_->GetInputNameAllocated(0U, allocator);
        const auto input_type = session_->GetInputTypeInfo(0U).GetTensorTypeAndShapeInfo();
        info_.model_path = options_.model_path;
        info_.input = RuntimeTensorDescriptor{
            .name = input_name.get(),
            .shape = runtime_shape(input_type.GetShape()),
            .element_type = runtime_element_type(input_type.GetElementType()),
        };

        info_.output_count = session_->GetOutputCount();
        if (info_.output_count > kMaximumRuntimeOutputs) {
            throw std::runtime_error("model output count exceeds the public runtime bound");
        }
        for (std::size_t index = 0U; index < info_.output_count; ++index) {
            const auto name = session_->GetOutputNameAllocated(index, allocator);
            const auto type = session_->GetOutputTypeInfo(index).GetTensorTypeAndShapeInfo();
            info_.outputs[index] = RuntimeTensorDescriptor{
                .name = name.get(),
                .shape = runtime_shape(type.GetShape()),
                .element_type = runtime_element_type(type.GetElementType()),
            };
        }
    }

    RuntimeBackendOptions options_;
    RuntimeModelInfo info_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<Ort::IoBinding> binding_;
    std::vector<Ort::Value> output_values_;
};

}  // namespace

[[nodiscard]] std::shared_ptr<RuntimeBackend> make_onnx_runtime_backend(const RuntimeBackendOptions& options) {
    return std::make_shared<OnnxRuntimeBackend>(options);
}

}  // namespace mmltk::backend::ml::runtime
