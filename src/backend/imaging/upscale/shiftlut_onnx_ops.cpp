#include "detail/shiftlut_onnx_ops.h"
#include "detail/shiftlut_lookup_cuda.h"
#include "detail/shiftlut_model_format.h"
#include <onnxruntime_cxx_api.h>

#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mmltk::backend::imaging::upscale::shiftlut {
namespace {

void checked(cudaError_t status, const char* context) {
    if (status != cudaSuccess) throw std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status));
}

// ORT destroys kernels only after their session's submitted work has settled.
// One allocation owns immutable tables followed by both ping-pong stage images.
class Storage final {
   public:
    void Install(const float* tables) {
        if (data_ != nullptr) return;
        checked(cudaGetDevice(&device_), "bind ShiftLUT allocation");
        checked(cudaMalloc(reinterpret_cast<void**>(&data_), resident_bytes(decision_capacity_)),
                "allocate resident ShiftLUT tables and stages");
        if (allocation_counter_ != nullptr) ++*allocation_counter_;
        checked(cudaMemcpy(data_, tables, kTableBytes, cudaMemcpyHostToDevice), "install immutable ShiftLUT tables");
    }
    ~Storage() {
        if (data_ != nullptr && Release() != cudaSuccess) std::terminate();
    }
    cudaError_t Release() noexcept {
        if (data_ == nullptr) return cudaSuccess;
        int previous = 0;
        const auto device_status = cudaGetDevice(&previous);
        if (device_status != cudaSuccess) return device_status;
        const auto bound = cudaSetDevice(device_);
        if (bound != cudaSuccess) return bound;
        const auto released = cudaFree(data_);
        if (released == cudaSuccess) data_ = nullptr;
        const auto restored = cudaSetDevice(previous);
        return released == cudaSuccess ? restored : released;
    }
    Storage(std::size_t decision_capacity, std::uint64_t* allocation_counter)
        : decision_capacity_(decision_capacity), allocation_counter_(allocation_counter) {
        if (decision_capacity > kMaximumTilePixels) throw std::invalid_argument("invalid ShiftLUT diagnostic capacity");
    }
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    float* data() noexcept { return data_; }
    std::int8_t* Decisions(std::size_t pixels) noexcept {
        if (decision_capacity_ == 0 || pixels > decision_capacity_ || data_ == nullptr) return nullptr;
        return reinterpret_cast<std::int8_t*>(data_ + kTableElements) + kDecisionStorageOffset;
    }
    cudaError_t ReadDecisions(std::span<std::int8_t> target) noexcept {
        if (target.empty() || target.size() > decision_elements(decision_capacity_) || data_ == nullptr) return cudaErrorInvalidValue;
        return cudaMemcpy(target.data(), Decisions(1), target.size_bytes(), cudaMemcpyDeviceToHost);
    }

   private:
    std::size_t decision_capacity_ = 0;
    std::uint64_t* allocation_counter_ = nullptr;
    int device_ = 0;
    float* data_ = nullptr;
};

class Kernel final {
   public:
    Kernel(const OrtApi& api, const OrtKernelInfo* info, Storage& storage) : storage_(storage) {
        int constant = 0;
        const OrtValue* value = nullptr;
        Ort::ThrowOnError(api.KernelInfoGetConstantInput_tensor(info, 1, &constant, &value));
        if (!constant || value == nullptr) throw std::invalid_argument("ShiftLUT tables must be an immutable initializer");
        const Ort::ConstValue table{value};
        const auto shape = table.GetTensorTypeAndShapeInfo();
        if (shape.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            shape.GetShape() != std::vector<std::int64_t>{static_cast<std::int64_t>(kTableElements)})
            throw std::invalid_argument("invalid ShiftLUT v1 table layout");
        const auto* data = table.GetTensorData<float>();
        validate_tables(std::as_bytes(std::span{data, kTableElements}));
        storage_.Install(data);
    }

    OrtStatusPtr ComputeV2(OrtKernelContext* raw) noexcept {
        try {
            Ort::KernelContext context{raw};
            const auto input = context.GetInput(0);
            const auto type = input.GetTensorTypeAndShapeInfo();
            std::array<std::int64_t, 4> shape{};
            if (type.GetDimensionsCount() != shape.size()) throw std::invalid_argument("ShiftLUT input must be NCHW");
            Ort::ThrowOnError(Ort::GetApi().GetDimensions(type, shape.data(), shape.size()));
            if (shape[0] != 1 || shape[1] != kRgbChannels || shape[2] < 1 || shape[3] < 1 || shape[2] > kMaximumTileExtent ||
                shape[3] > kMaximumTileExtent)
                throw std::invalid_argument("ShiftLUT requires one RGB tile with extent at most 256");
            const auto height = static_cast<std::uint32_t>(shape[2]);
            const auto width = static_cast<std::uint32_t>(shape[3]);
            shape[2] *= kScale;
            shape[3] *= kScale;
            auto output = context.GetOutput(0, shape.data(), shape.size());
            auto* first = reinterpret_cast<std::int8_t*>(storage_.data() + kTableElements);
            checked(enqueue(input.GetTensorData<float>(), storage_.data(), first, first + kScratchElements,
                            output.GetTensorMutableData<float>(), height, width, static_cast<cudaStream_t>(context.GetGPUComputeStream()),
                            storage_.Decisions(static_cast<std::size_t>(height) * width)),
                    "submit ShiftLUT stages");
            return nullptr;
        } catch (const std::exception& error) { return Ort::GetApi().CreateStatus(ORT_RUNTIME_EXCEPTION, error.what()); } catch (...) {
            return Ort::GetApi().CreateStatus(ORT_RUNTIME_EXCEPTION, "unknown ShiftLUT failure");
        }
    }

   private:
    Storage& storage_;
};

struct Operator final : Ort::CustomOpBase<Operator, Kernel, true> {
    explicit Operator(Storage& storage) : storage_(storage) {
        start_ver_ = kVersion;
        end_ver_ = kVersion;
    }
    const char* GetName() const noexcept { return kOperator; }
    const char* GetExecutionProviderType() const noexcept { return "CUDAExecutionProvider"; }
    std::size_t GetInputTypeCount() const noexcept { return 2; }
    std::size_t GetOutputTypeCount() const noexcept { return 1; }
    ONNXTensorElementDataType GetInputType(std::size_t) const noexcept { return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT; }
    ONNXTensorElementDataType GetOutputType(std::size_t) const noexcept { return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT; }
    OrtMemType GetInputMemoryType(std::size_t index) const noexcept { return index == 1 ? OrtMemTypeCPUInput : OrtMemTypeDefault; }
    OrtStatusPtr CreateKernelV2(const OrtApi& api, const OrtKernelInfo* info, void** result) const noexcept {
        try {
            *result = new Kernel(api, info, storage_);
            return nullptr;
        } catch (const std::exception& error) { return api.CreateStatus(ORT_RUNTIME_EXCEPTION, error.what()); } catch (...) {
            return api.CreateStatus(ORT_RUNTIME_EXCEPTION, "unknown ShiftLUT initialization failure");
        }
    }

   private:
    Storage& storage_;
};

}  // namespace

struct Operators::Impl {
    Storage storage;
    Operator operation{storage};
    Ort::CustomOpDomain domain{kDomain};
    Impl(std::size_t decision_capacity, std::uint64_t* allocation_counter) : storage(decision_capacity, allocation_counter) {
        domain.Add(&operation);
    }
};

Operators::Operators(std::size_t decision_capacity, std::uint64_t* allocation_counter)
    : impl_(std::make_unique<Impl>(decision_capacity, allocation_counter)) {}
Operators::~Operators() = default;
void Operators::Register(Ort::SessionOptions& options) { options.Add(impl_->domain); }
cudaError_t Operators::Release() noexcept { return impl_->storage.Release(); }
cudaError_t Operators::ReadDecisions(std::span<std::int8_t> target) noexcept { return impl_->storage.ReadDecisions(target); }
void configure_verification_session(Operators& operators, Ort::SessionOptions& options, const int device, const bool enable_cuda_graph,
                                    const cudaStream_t stream) {
    operators.Register(options);
    Ort::CUDAProviderOptions cuda;
    cuda.Update(std::unordered_map<std::string, std::string>{
        {"device_id", std::to_string(device)},
        {"enable_cuda_graph", enable_cuda_graph ? "1" : "0"},
        {"use_tf32", "0"},
    });
    cuda.UpdateWithValue("user_compute_stream", stream);
    options.AppendExecutionProvider_CUDA_V2(*cuda);
}
}  // namespace mmltk::backend::imaging::upscale::shiftlut
