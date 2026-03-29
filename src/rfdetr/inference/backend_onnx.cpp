#include "rfdetr/backends.h"

#include "profile_utils.h"
#include "rfdetr/cuda_utils.h"

#include <c10/cuda/CUDAGuard.h>
#include <onnxruntime_cxx_api.h>

#include <cuda_runtime.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastloader::rfdetr {

namespace {

std::string ort_type_name(ONNXTensorElementDataType dtype) {
    switch (dtype) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
        return "float32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
        return "int64";
    default:
        return "unsupported";
    }
}

Ort::Env& ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "fastloader_rfdetr");
    return env;
}

class OnnxBackend final : public InferenceBackend {
public:
    OnnxBackend(const std::filesystem::path& model_path, int device_id)
        : model_path_(model_path),
          device_id_(device_id),
          session_options_(),
          allocator_() {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.onnx.construct");
        ensure_cuda_ok(cudaSetDevice(device_id_), "cudaSetDevice for ONNX backend");
        ensure_cuda_ok(fastloader::cuda_stream_create_with_highest_priority(&stream_, cudaStreamNonBlocking),
                       "cudaStreamCreateWithPriority for ONNX backend");
        session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_options_.SetIntraOpNumThreads(1);
        session_options_.SetInterOpNumThreads(1);

        OrtCUDAProviderOptions cuda_options{};
        cuda_options.device_id = device_id_;
        cuda_options.do_copy_in_default_stream = 0;
        cuda_options.has_user_compute_stream = 1;
        cuda_options.user_compute_stream = stream_;
        session_options_.AppendExecutionProvider_CUDA(cuda_options);

        try {
            session_ = std::make_unique<Ort::Session>(
                ort_env(), model_path_.string().c_str(), session_options_);
        } catch (const std::exception& error) {
            throw std::runtime_error("failed to create ONNX Runtime session: " + std::string(error.what()));
        }

        size_t input_count = 0;
        try {
            input_count = session_->GetInputCount();
        } catch (const std::exception& error) {
            throw std::runtime_error("failed to query ONNX input count: " + std::string(error.what()));
        }
        if (input_count != 1) {
            throw std::runtime_error("RF-DETR ONNX runtime expects exactly one input tensor");
        }
        size_t output_count = 0;
        try {
            output_count = session_->GetOutputCount();
        } catch (const std::exception& error) {
            throw std::runtime_error("failed to query ONNX output count: " + std::string(error.what()));
        }
        if (output_count < 2) {
            throw std::runtime_error("RF-DETR ONNX runtime expects at least two outputs");
        }

        std::string input_name_value;
        try {
            auto input_name = session_->GetInputNameAllocated(0, allocator_);
            input_name_value = input_name.get();
        } catch (const std::exception& error) {
            throw std::runtime_error("failed to read ONNX input name: " + std::string(error.what()));
        }
        input_name_ = std::move(input_name_value);
        std::vector<int64_t> input_shape;
        std::string input_dtype;
        try {
            const auto input_type_info = session_->GetInputTypeInfo(0);
            const auto input_type = input_type_info.GetTensorTypeAndShapeInfo();
            input_shape = input_type.GetShape();
            input_dtype = ort_type_name(input_type.GetElementType());
        } catch (const std::exception& error) {
            throw std::runtime_error("failed to read ONNX input type info: " + std::string(error.what()));
        }
        info_.backend = "onnx";
        info_.model_path = model_path_.string();
        info_.input = TensorInfo{input_name_, std::move(input_shape), std::move(input_dtype)};

        output_names_owned_.reserve(output_count);
        for (size_t output_index = 0; output_index < output_count; ++output_index) {
            std::string output_name_value;
            try {
                auto output_name = session_->GetOutputNameAllocated(output_index, allocator_);
                output_name_value = output_name.get();
            } catch (const std::exception& error) {
                throw std::runtime_error("failed to read ONNX output name at index " + std::to_string(output_index) +
                                         ": " + error.what());
            }
            output_names_owned_.push_back(std::move(output_name_value));
            try {
                const auto output_type_info = session_->GetOutputTypeInfo(output_index);
                const auto output_type = output_type_info.GetTensorTypeAndShapeInfo();
                info_.outputs.push_back(TensorInfo{
                    output_names_owned_.back(),
                    output_type.GetShape(),
                    ort_type_name(output_type.GetElementType()),
                });
            } catch (const std::exception& error) {
                throw std::runtime_error("failed to read ONNX output tensor metadata at index " +
                                         std::to_string(output_index) + ": " + error.what());
            }
        }
        infer_output_layout();
    }

    ~OnnxBackend() override {
        if (stream_ != nullptr) {
            cudaStreamDestroy(stream_);
        }
    }

    const ModelInfo& info() const override { return info_; }
    void* stream() const override { return stream_; }

    OutputTensors run(const torch::Tensor& normalized_input) override {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.onnx.run");
        if (!normalized_input.is_cuda() || !normalized_input.is_contiguous()) {
            throw std::runtime_error("ONNX backend input must be contiguous CUDA tensor");
        }

        c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
        const auto shape = std::vector<int64_t>(normalized_input.sizes().begin(), normalized_input.sizes().end());
        Ort::MemoryInfo device_memory("Cuda", OrtArenaAllocator, device_id_, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            device_memory,
            const_cast<float*>(normalized_input.data_ptr<float>()),
            normalized_input.numel(),
            shape.data(),
            shape.size());

        Ort::IoBinding binding(*session_);
        binding.BindInput(input_name_.c_str(), input_tensor);
        Ort::MemoryInfo output_memory("Cuda", OrtArenaAllocator, device_id_, OrtMemTypeDefault);
        for (const std::string& output_name : output_names_owned_) {
            binding.BindOutput(output_name.c_str(), output_memory);
        }
        session_->Run(Ort::RunOptions{nullptr}, binding);
        last_outputs_ = binding.GetOutputValues();
        binding.ClearBoundInputs();
        binding.ClearBoundOutputs();

        return OutputTensors{
            wrap_output(last_outputs_.at(logits_output_index_)),
            wrap_output(last_outputs_.at(boxes_output_index_)),
            info_.has_masks ? std::optional<torch::Tensor>(wrap_output(last_outputs_.at(masks_output_index_)))
                            : std::nullopt,
        };
    }

private:
    void infer_output_layout() {
        boxes_output_index_ = output_names_owned_.size();
        logits_output_index_ = output_names_owned_.size();
        masks_output_index_ = output_names_owned_.size();

        for (size_t output_index = 0; output_index < info_.outputs.size(); ++output_index) {
            const auto& output = info_.outputs[output_index];
            if (output.shape.size() == 3 && output.shape.back() == 4) {
                boxes_output_index_ = output_index;
                info_.num_queries = output.shape[1];
            } else if (output.shape.size() == 3) {
                logits_output_index_ = output_index;
                info_.num_classes = output.shape[2];
            } else if (output.shape.size() == 4) {
                masks_output_index_ = output_index;
                info_.has_masks = true;
            }
        }
        if (boxes_output_index_ >= info_.outputs.size() || logits_output_index_ >= info_.outputs.size()) {
            throw std::runtime_error("failed to identify RF-DETR ONNX output tensors");
        }
    }

    torch::Tensor wrap_output(Ort::Value& value) const {
        auto shape = value.GetTensorTypeAndShapeInfo().GetShape();
        return torch::from_blob(
            value.GetTensorMutableData<float>(),
            shape,
            torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device_id_));
    }

    std::filesystem::path model_path_;
    int device_id_ = 0;
    Ort::SessionOptions session_options_;
    Ort::AllocatorWithDefaultOptions allocator_;
    std::unique_ptr<Ort::Session> session_;
    cudaStream_t stream_ = nullptr;
    std::string input_name_;
    std::vector<std::string> output_names_owned_;
    std::vector<Ort::Value> last_outputs_;
    size_t boxes_output_index_ = 0;
    size_t logits_output_index_ = 0;
    size_t masks_output_index_ = 0;
    ModelInfo info_;
};

} // namespace

std::unique_ptr<InferenceBackend> make_onnx_backend(const std::filesystem::path& model_path,
                                                    int device_id) {
    return std::make_unique<OnnxBackend>(model_path, device_id);
}

std::vector<std::unique_ptr<InferenceBackend>> make_onnx_backend_lanes(
    const std::filesystem::path& model_path,
    int device_id,
    int lane_count) {
    if (lane_count <= 0) {
        throw std::runtime_error("ONNX backend lanes must be greater than zero");
    }
    std::vector<std::unique_ptr<InferenceBackend>> backends;
    backends.reserve(static_cast<size_t>(lane_count));
    for (int lane = 0; lane < lane_count; ++lane) {
        backends.push_back(make_onnx_backend(model_path, device_id));
    }
    return backends;
}

} // namespace fastloader::rfdetr
