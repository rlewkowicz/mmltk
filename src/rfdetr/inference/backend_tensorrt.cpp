#include "rfdetr/backends.h"

#include "profile_utils.h"
#include "rfdetr/backends_internal.h"
#include "rfdetr/cuda_utils.h"

#include <c10/cuda/CUDAGuard.h>

#include <NvInfer.h>
#include <NvOnnxParser.h>

#include <cuda_runtime.h>

#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace fastloader::rfdetr {

namespace {

std::string trt_dtype_name(nvinfer1::DataType dtype) {
    switch (dtype) {
    case nvinfer1::DataType::kFLOAT:
        return "float32";
    case nvinfer1::DataType::kHALF:
        return "float16";
    case nvinfer1::DataType::kINT32:
        return "int32";
    case nvinfer1::DataType::kINT64:
        return "int64";
    default:
        return "unsupported";
    }
}

size_t trt_dtype_size(nvinfer1::DataType dtype) {
    switch (dtype) {
    case nvinfer1::DataType::kFLOAT:
        return sizeof(float);
    case nvinfer1::DataType::kHALF:
        return sizeof(uint16_t);
    case nvinfer1::DataType::kINT32:
        return sizeof(int32_t);
    case nvinfer1::DataType::kINT64:
        return sizeof(int64_t);
    default:
        throw std::runtime_error("unsupported TensorRT tensor dtype");
    }
}

std::vector<int64_t> dims_to_shape(const nvinfer1::Dims& dims) {
    std::vector<int64_t> shape;
    shape.reserve(static_cast<size_t>(dims.nbDims));
    for (int axis = 0; axis < dims.nbDims; ++axis) {
        shape.push_back(static_cast<int64_t>(dims.d[axis]));
    }
    return shape;
}

nvinfer1::Dims tensor_sizes_to_dims(const torch::Tensor& tensor) {
    nvinfer1::Dims dims{};
    if (tensor.dim() > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error("tensor rank exceeds TensorRT dimension range");
    }
    dims.nbDims = static_cast<int32_t>(tensor.dim());
    for (int axis = 0; axis < dims.nbDims; ++axis) {
        dims.d[axis] = static_cast<int32_t>(tensor.size(axis));
    }
    return dims;
}

size_t element_count(const std::vector<int64_t>& shape) {
    if (shape.empty()) {
        return 0;
    }
    return static_cast<size_t>(std::accumulate(
        shape.begin(), shape.end(), int64_t{1}, std::multiplies<int64_t>()));
}

std::streamsize checked_streamsize(size_t value, const char* context) {
    if (value > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error(std::string(context) + ": size exceeds std::streamsize range");
    }
    return static_cast<std::streamsize>(value);
}

std::string join_tensor_names(char const* const* tensor_names, int32_t count) {
    std::string joined;
    for (int32_t index = 0; index < count; ++index) {
        if (tensor_names[index] == nullptr || tensor_names[index][0] == '\0') {
            continue;
        }
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += tensor_names[index];
    }
    return joined;
}

void set_fp16_builder_flag(nvinfer1::IBuilderConfig& config) {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    config.setFlag(nvinfer1::BuilderFlag::kFP16);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

class TensorRtLogger final : public nvinfer1::ILogger {
public:
    explicit TensorRtLogger(Severity threshold) : threshold_(threshold) {}

    void log(Severity severity, const char* message) noexcept override {
        if (severity > threshold_) {
            return;
        }
        const char* prefix = "[trt]";
        switch (severity) {
        case Severity::kINTERNAL_ERROR:
        case Severity::kERROR:
            prefix = "[trt:error]";
            break;
        case Severity::kWARNING:
            prefix = "[trt:warn]";
            break;
        case Severity::kINFO:
            break;
        case Severity::kVERBOSE:
            prefix = "[trt:verbose]";
            break;
        default:
            break;
        }
        std::fprintf(stderr, "%s %s\n", prefix, message);
    }

private:
    Severity threshold_;
};

TensorRtLogger& tensor_rt_logger() {
    static TensorRtLogger logger(nvinfer1::ILogger::Severity::kWARNING);
    return logger;
}

template <typename T>
struct TensorRtDestroy {
    void operator()(T* ptr) const noexcept {
        delete ptr;
    }
};

template <typename T>
using TensorRtUnique = std::unique_ptr<T, TensorRtDestroy<T>>;

class TensorRtSharedState {
public:
    TensorRtSharedState(const std::filesystem::path& model_path,
                        int device_id,
                        bool allow_fp16,
                        const std::filesystem::path& save_engine_path)
        : model_path_(model_path),
          device_id_(device_id),
          allow_fp16_(allow_fp16),
          save_engine_path_(save_engine_path) {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.tensorrt.shared_construct");
        ensure_cuda_ok(cudaSetDevice(device_id_), "cudaSetDevice for TensorRT backend");
        if (has_extension(model_path_, ".engine")) {
            load_engine(read_binary(model_path_));
        } else if (has_extension(model_path_, ".onnx")) {
            build_engine_from_onnx(model_path_);
        } else {
            throw std::runtime_error("TensorRT backend expects a .engine or .onnx path");
        }
        initialize_tensor_info();
    }

    const ModelInfo& info() const { return info_; }
    nvinfer1::ICudaEngine& engine() const { return *engine_; }
    int device_id() const { return device_id_; }
    size_t boxes_output_index() const { return boxes_output_index_; }
    size_t logits_output_index() const { return logits_output_index_; }
    size_t masks_output_index() const { return masks_output_index_; }

    void save_engine(const std::filesystem::path& path) const {
        if (serialized_engine_ == nullptr) {
            throw std::runtime_error("no serialized TensorRT engine is available to save");
        }
        std::ofstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            throw std::runtime_error("failed to open TensorRT engine output path: " + path.string());
        }
        stream.write(static_cast<const char*>(serialized_engine_->data()),
                     checked_streamsize(serialized_engine_->size(), "TensorRT engine write"));
    }

private:
    std::vector<char> read_binary(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            throw std::runtime_error("failed to open TensorRT engine: " + path.string());
        }
        stream.seekg(0, std::ios::end);
        const auto size = static_cast<size_t>(stream.tellg());
        stream.seekg(0, std::ios::beg);
        std::vector<char> bytes(size);
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return bytes;
    }

    void load_engine(const std::vector<char>& bytes) {
        runtime_.reset(nvinfer1::createInferRuntime(tensor_rt_logger()));
        if (runtime_ == nullptr) {
            throw std::runtime_error("failed to create TensorRT runtime");
        }
        engine_.reset(runtime_->deserializeCudaEngine(bytes.data(), bytes.size()));
        if (engine_ == nullptr) {
            throw std::runtime_error("failed to deserialize TensorRT engine");
        }
    }

    void build_engine_from_onnx(const std::filesystem::path& onnx_path) {
        TensorRtUnique<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(tensor_rt_logger()));
        if (builder == nullptr) {
            throw std::runtime_error("failed to create TensorRT builder");
        }
        TensorRtUnique<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0U));
        TensorRtUnique<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        if (network == nullptr || config == nullptr) {
            throw std::runtime_error("failed to create TensorRT build objects");
        }
        TensorRtUnique<nvonnxparser::IParser> parser(
            nvonnxparser::createParser(*network, tensor_rt_logger()));
        if (parser == nullptr) {
            throw std::runtime_error("failed to create TensorRT ONNX parser");
        }
        if (!parser->parseFromFile(
                onnx_path.string().c_str(),
                static_cast<int>(nvinfer1::ILogger::Severity::kWARNING))) {
            throw std::runtime_error("failed to parse RF-DETR ONNX file for TensorRT");
        }
        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 1ULL << 30);
        if (allow_fp16_) {
            // TensorRT 10.12 deprecates kFP16 in favor of strongly typed networks.
            // Our ONNX parser flow still relies on builder-selected mixed precision,
            // so keep the behavior and suppress the warning locally until the export
            // path is upgraded to emit a strongly typed FP16 network.
            set_fp16_builder_flag(*config);
        }

        serialized_engine_.reset(builder->buildSerializedNetwork(*network, *config));
        if (serialized_engine_ == nullptr) {
            throw std::runtime_error("TensorRT buildSerializedNetwork failed");
        }
        if (!save_engine_path_.empty()) {
            std::ofstream stream(save_engine_path_, std::ios::binary);
            if (!stream.is_open()) {
                throw std::runtime_error("failed to open TensorRT save path: " + save_engine_path_.string());
            }
            stream.write(static_cast<const char*>(serialized_engine_->data()),
                         checked_streamsize(serialized_engine_->size(), "TensorRT serialized engine write"));
        }
        runtime_.reset(nvinfer1::createInferRuntime(tensor_rt_logger()));
        if (runtime_ == nullptr) {
            throw std::runtime_error("failed to create TensorRT runtime");
        }
        engine_.reset(runtime_->deserializeCudaEngine(serialized_engine_->data(), serialized_engine_->size()));
        if (engine_ == nullptr) {
            throw std::runtime_error("failed to deserialize TensorRT engine");
        }
    }

    void initialize_tensor_info() {
        info_.backend = "tensorrt";
        info_.model_path = model_path_.string();
        const int tensor_count = engine_->getNbIOTensors();
        if (tensor_count <= 0) {
            throw std::runtime_error("TensorRT engine exposes no I/O tensors");
        }

        boxes_output_index_ = static_cast<size_t>(tensor_count);
        logits_output_index_ = static_cast<size_t>(tensor_count);
        masks_output_index_ = static_cast<size_t>(tensor_count);

        for (int tensor_index = 0; tensor_index < tensor_count; ++tensor_index) {
            const char* tensor_name = engine_->getIOTensorName(tensor_index);
            const auto mode = engine_->getTensorIOMode(tensor_name);
            const auto shape = dims_to_shape(engine_->getTensorShape(tensor_name));
            const auto dtype = trt_dtype_name(engine_->getTensorDataType(tensor_name));
            if (mode == nvinfer1::TensorIOMode::kINPUT) {
                info_.input = TensorInfo{tensor_name, shape, dtype};
            } else {
                info_.outputs.push_back(TensorInfo{tensor_name, shape, dtype});
            }
        }

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
            throw std::runtime_error("failed to identify RF-DETR TensorRT outputs");
        }
    }

    std::filesystem::path model_path_;
    int device_id_ = 0;
    bool allow_fp16_ = true;
    std::filesystem::path save_engine_path_;
    TensorRtUnique<nvinfer1::IRuntime> runtime_{nullptr};
    TensorRtUnique<nvinfer1::ICudaEngine> engine_{nullptr};
    TensorRtUnique<nvinfer1::IHostMemory> serialized_engine_{nullptr};
    size_t boxes_output_index_ = 0;
    size_t logits_output_index_ = 0;
    size_t masks_output_index_ = 0;
    ModelInfo info_;
};

class TensorRtBackend final : public InferenceBackend {
public:
    explicit TensorRtBackend(std::shared_ptr<TensorRtSharedState> shared_state)
        : shared_state_(std::move(shared_state)) {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.tensorrt.construct");
        ensure_cuda_ok(cudaSetDevice(shared_state_->device_id()), "cudaSetDevice for TensorRT backend lane");
        ensure_cuda_ok(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking),
                       "cudaStreamCreateWithFlags for TensorRT backend lane");
        context_.reset(shared_state_->engine().createExecutionContext());
        if (context_ == nullptr) {
            throw std::runtime_error("failed to create TensorRT execution context");
        }
    }

    ~TensorRtBackend() override {
        for (auto& item : output_buffers_) {
            if (item.second != nullptr) {
                cudaFree(item.second);
            }
        }
        if (stream_ != nullptr) {
            cudaStreamDestroy(stream_);
        }
    }

    const ModelInfo& info() const override { return shared_state_->info(); }
    void* stream() const override { return stream_; }

    OutputTensors run(const torch::Tensor& normalized_input) override {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.tensorrt.run");
        if (!normalized_input.is_cuda() || !normalized_input.is_contiguous()) {
            throw std::runtime_error("TensorRT backend input must be contiguous CUDA tensor");
        }

        c10::cuda::CUDAGuard device_guard(checked_device_index(shared_state_->device_id()));
        const auto input_dims = tensor_sizes_to_dims(normalized_input);
        if (!context_->setInputShape(info().input.name.c_str(), input_dims)) {
            throw std::runtime_error("TensorRT failed to set RF-DETR input shape");
        }
        if (!context_->setInputTensorAddress(info().input.name.c_str(), normalized_input.data_ptr())) {
            throw std::runtime_error("TensorRT failed to bind input tensor");
        }
        const int32_t infer_result = context_->inferShapes(0, nullptr);
        if (infer_result != 0) {
            if (infer_result < 0) {
                throw std::runtime_error("TensorRT failed to infer RF-DETR shapes");
            }
            std::vector<char const*> missing_tensor_names(static_cast<size_t>(infer_result), nullptr);
            const int32_t missing_count = context_->inferShapes(infer_result, missing_tensor_names.data());
            const std::string missing = join_tensor_names(
                missing_tensor_names.data(), missing_count > 0 ? missing_count : infer_result);
            throw std::runtime_error(
                missing.empty() ? "TensorRT input shapes or addresses remain unspecified"
                                : "TensorRT input shapes or addresses remain unspecified: " + missing);
        }

        for (const TensorInfo& output : info().outputs) {
            reserve_output_buffer(output.name);
            if (!context_->setOutputTensorAddress(output.name.c_str(), output_buffers_.at(output.name))) {
                throw std::runtime_error("TensorRT failed to bind output tensor: " + output.name);
            }
        }

        if (!context_->enqueueV3(stream_)) {
            throw std::runtime_error("TensorRT enqueueV3 failed");
        }

        return OutputTensors{
            wrap_output(info().outputs.at(shared_state_->logits_output_index())),
            wrap_output(info().outputs.at(shared_state_->boxes_output_index())),
            info().has_masks
                ? std::optional<torch::Tensor>(wrap_output(info().outputs.at(shared_state_->masks_output_index())))
                : std::nullopt,
        };
    }

    void save_engine(const std::filesystem::path& path) override {
        shared_state_->save_engine(path);
    }

private:
    void reserve_output_buffer(const std::string& tensor_name) {
        const auto shape = dims_to_shape(context_->getTensorShape(tensor_name.c_str()));
        const size_t bytes = element_count(shape) *
                             trt_dtype_size(shared_state_->engine().getTensorDataType(tensor_name.c_str()));
        void*& buffer = output_buffers_[tensor_name];
        size_t& capacity = output_buffer_bytes_[tensor_name];
        if (bytes <= capacity && buffer != nullptr) {
            return;
        }
        if (buffer != nullptr) {
            cudaFree(buffer);
            buffer = nullptr;
        }
        ensure_cuda_ok(cudaMalloc(&buffer, bytes), "cudaMalloc TensorRT output buffer");
        capacity = bytes;
    }

    torch::Tensor wrap_output(const TensorInfo& output) {
        const auto shape = dims_to_shape(context_->getTensorShape(output.name.c_str()));
        return torch::from_blob(
            output_buffers_.at(output.name),
            shape,
            torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, shared_state_->device_id()));
    }

    std::shared_ptr<TensorRtSharedState> shared_state_;
    TensorRtUnique<nvinfer1::IExecutionContext> context_{nullptr};
    cudaStream_t stream_ = nullptr;
    std::unordered_map<std::string, void*> output_buffers_;
    std::unordered_map<std::string, size_t> output_buffer_bytes_;
};

} // namespace

void InferenceBackend::save_engine(const std::filesystem::path&) {
    throw std::runtime_error("this backend does not support saving TensorRT engines");
}

std::unique_ptr<InferenceBackend> make_tensorrt_backend(const std::filesystem::path& model_path,
                                                        int device_id,
                                                        bool allow_fp16,
                                                        const std::filesystem::path& save_engine_path) {
    auto backends = make_tensorrt_backend_lanes(model_path, device_id, allow_fp16, 1, save_engine_path);
    auto backend = std::move(backends.front());
    return backend;
}

std::vector<std::unique_ptr<InferenceBackend>> make_tensorrt_backend_lanes(
    const std::filesystem::path& model_path,
    int device_id,
    bool allow_fp16,
    int lane_count,
    const std::filesystem::path& save_engine_path) {
    if (lane_count <= 0) {
        throw std::runtime_error("TensorRT backend lanes must be greater than zero");
    }
    auto shared_state = std::make_shared<TensorRtSharedState>(
        model_path,
        device_id,
        allow_fp16,
        save_engine_path);
    std::vector<std::unique_ptr<InferenceBackend>> backends;
    backends.reserve(static_cast<size_t>(lane_count));
    for (int lane = 0; lane < lane_count; ++lane) {
        backends.push_back(std::make_unique<TensorRtBackend>(shared_state));
    }
    return backends;
}

} // namespace fastloader::rfdetr
