#include "rfdetr/backends.h"

#include "profile_utils.h"
#include "mmltk_logging.h"
#include "rfdetr/backends_internal.h"
#include "rfdetr/torch_cuda_utils.h"

#include <c10/cuda/CUDAGuard.h>

#include <NvInfer.h>
#include <NvOnnxParser.h>

#include <cuda_runtime.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <vector>

namespace mmltk::rfdetr {

namespace {

std::mutex& tensorrt_log_sink_mutex() {
    static std::mutex mutex;
    return mutex;
}

TensorRtLogSink& tensorrt_log_sink() {
    static TensorRtLogSink sink;
    return sink;
}

TensorRtLogSink exchange_tensorrt_log_sink(TensorRtLogSink sink) {
    std::lock_guard<std::mutex> lock(tensorrt_log_sink_mutex());
    TensorRtLogSink previous = std::move(tensorrt_log_sink());
    tensorrt_log_sink() = std::move(sink);
    return previous;
}

void emit_tensorrt_log_line(const std::string& line) {
    if (line.empty()) {
        return;
    }
    mmltk::logging::logger("rfdetr.tensorrt")->info("{}", line);

    TensorRtLogSink sink;
    {
        std::lock_guard<std::mutex> lock(tensorrt_log_sink_mutex());
        sink = tensorrt_log_sink();
    }
    if (sink) {
        sink(line);
    }
}

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

torch::ScalarType trt_dtype_to_torch_scalar_type(nvinfer1::DataType dtype) {
    switch (dtype) {
        case nvinfer1::DataType::kFLOAT:
            return torch::kFloat32;
        case nvinfer1::DataType::kHALF:
            return torch::kFloat16;
        case nvinfer1::DataType::kINT32:
            return torch::kInt32;
        case nvinfer1::DataType::kINT64:
            return torch::kInt64;
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
    return static_cast<size_t>(std::accumulate(shape.begin(), shape.end(), int64_t{1}, std::multiplies<>()));
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

std::string format_onnx_parser_errors(const nvonnxparser::IParser& parser) {
    const int error_count = parser.getNbErrors();
    if (error_count <= 0) {
        return {};
    }

    std::ostringstream message;
    for (int index = 0; index < error_count; ++index) {
        const nvonnxparser::IParserError* error = parser.getError(index);
        if (error == nullptr) {
            continue;
        }
        if (message.tellp() > 0) {
            message << '\n';
        }
        message << "[" << index << "] code=" << static_cast<int>(error->code())
                << " op=" << (error->nodeOperator() ? error->nodeOperator() : "<unknown>")
                << " node=" << (error->nodeName() ? error->nodeName() : "<unnamed>")
                << " desc=" << (error->desc() ? error->desc() : "<no description>");
    }
    return message.str();
}

std::string format_dims(const nvinfer1::Dims& dims) {
    std::ostringstream message;
    message << "[";
    for (int axis = 0; axis < dims.nbDims; ++axis) {
        if (axis > 0) {
            message << "x";
        }
        message << dims.d[axis];
    }
    message << "]";
    return message.str();
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
    explicit TensorRtLogger(Severity threshold) : threshold_(static_cast<int>(threshold)) {}

    void set_threshold(Severity threshold) noexcept {
        threshold_.store(static_cast<int>(threshold), std::memory_order_relaxed);
    }

    [[nodiscard]] Severity threshold() const noexcept {
        return static_cast<Severity>(threshold_.load(std::memory_order_relaxed));
    }

    void log(Severity severity, const char* message) noexcept override {
        if (severity > threshold()) {
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
        emit_tensorrt_log_line(std::string(prefix) + " " + (message ? message : ""));
    }

   private:
    std::atomic<int> threshold_;
};

TensorRtLogger& tensor_rt_logger() {
    static TensorRtLogger logger(nvinfer1::ILogger::Severity::kWARNING);
    return logger;
}

class TensorRtLoggerThresholdScope {
   public:
    explicit TensorRtLoggerThresholdScope(nvinfer1::ILogger::Severity threshold)
        : previous_(tensor_rt_logger().threshold()) {
        tensor_rt_logger().set_threshold(threshold);
    }

    ~TensorRtLoggerThresholdScope() {
        tensor_rt_logger().set_threshold(previous_);
    }

   private:
    nvinfer1::ILogger::Severity previous_;
};

class TensorRtBuildProgressMonitor final : public nvinfer1::IProgressMonitor {
   public:
    void phaseStart(char const* phase_name, char const* parent_phase, int32_t nb_steps) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = phase_name ? phase_name : "<unnamed>";
        phase_steps_[key] = nb_steps;
        std::ostringstream message;
        message << "[trt:build] phase start: ";
        if (parent_phase != nullptr && parent_phase[0] != '\0') {
            message << parent_phase << " -> ";
        }
        message << key << " steps=" << nb_steps;
        emit_tensorrt_log_line(message.str());
    }

    bool stepComplete(char const* phase_name, int32_t step) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = phase_name ? phase_name : "<unnamed>";
        const auto found = phase_steps_.find(key);
        const int32_t total_steps = found == phase_steps_.end() ? 0 : found->second;
        std::ostringstream message;
        message << "[trt:build] phase step: " << key << " " << (step + 1);
        if (total_steps > 0) {
            message << "/" << total_steps;
        }
        emit_tensorrt_log_line(message.str());
        return true;
    }

    void phaseFinish(char const* phase_name) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = phase_name ? phase_name : "<unnamed>";
        phase_steps_.erase(key);
        emit_tensorrt_log_line("[trt:build] phase finish: " + key);
    }

   private:
    std::mutex mutex_;
    std::unordered_map<std::string, int32_t> phase_steps_;
};

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
    TensorRtSharedState(std::filesystem::path model_path, int device_id, bool allow_fp16,
                        std::filesystem::path save_engine_path)
        : model_path_(std::move(model_path)),
          device_id_(device_id),
          allow_fp16_(allow_fp16),
          save_engine_path_(std::move(save_engine_path)) {
        MMLTK_PROFILE_SCOPE("rfdetr.native.tensorrt.shared_construct");
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

    [[nodiscard]] const ModelInfo& info() const {
        return info_;
    }
    [[nodiscard]] nvinfer1::ICudaEngine& engine() const {
        return *engine_;
    }
    [[nodiscard]] int device_id() const {
        return device_id_;
    }
    [[nodiscard]] size_t boxes_output_index() const {
        return boxes_output_index_;
    }
    [[nodiscard]] size_t logits_output_index() const {
        return logits_output_index_;
    }
    [[nodiscard]] size_t masks_output_index() const {
        return masks_output_index_;
    }

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
        TensorRtLoggerThresholdScope logger_scope(nvinfer1::ILogger::Severity::kVERBOSE);
        TensorRtUnique<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(tensor_rt_logger()));
        if (builder == nullptr) {
            throw std::runtime_error("failed to create TensorRT builder");
        }
        TensorRtUnique<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0U));
        TensorRtUnique<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        if (network == nullptr || config == nullptr) {
            throw std::runtime_error("failed to create TensorRT build objects");
        }
        TensorRtUnique<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, tensor_rt_logger()));
        if (parser == nullptr) {
            throw std::runtime_error("failed to create TensorRT ONNX parser");
        }
        emit_tensorrt_log_line("[trt:build] parsing ONNX: " + onnx_path.string());
        if (!parser->parseFromFile(onnx_path.string().c_str(),
                                   static_cast<int>(nvinfer1::ILogger::Severity::kVERBOSE))) {
            const std::string diagnostics = format_onnx_parser_errors(*parser);
            throw std::runtime_error(diagnostics.empty()
                                         ? "failed to parse RF-DETR ONNX file for TensorRT"
                                         : "failed to parse RF-DETR ONNX file for TensorRT:\n" + diagnostics);
        }
        {
            std::ostringstream message;
            message << "[trt:build] parsed network: layers=" << network->getNbLayers()
                    << " inputs=" << network->getNbInputs() << " outputs=" << network->getNbOutputs();
            emit_tensorrt_log_line(message.str());
        }
        for (int index = 0; index < network->getNbInputs(); ++index) {
            const nvinfer1::ITensor* tensor = network->getInput(index);
            if (tensor == nullptr) {
                continue;
            }
            emit_tensorrt_log_line(std::string("[trt:build] input[") + std::to_string(index) + "] " +
                                   (tensor->getName() ? tensor->getName() : "<unnamed>") + " dtype=" +
                                   trt_dtype_name(tensor->getType()) + " dims=" + format_dims(tensor->getDimensions()));
        }
        for (int index = 0; index < network->getNbOutputs(); ++index) {
            const nvinfer1::ITensor* tensor = network->getOutput(index);
            if (tensor == nullptr) {
                continue;
            }
            emit_tensorrt_log_line(std::string("[trt:build] output[") + std::to_string(index) + "] " +
                                   (tensor->getName() ? tensor->getName() : "<unnamed>") + " dtype=" +
                                   trt_dtype_name(tensor->getType()) + " dims=" + format_dims(tensor->getDimensions()));
        }
        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 1ULL << 30);
        config->setProfilingVerbosity(nvinfer1::ProfilingVerbosity::kDETAILED);
        TensorRtBuildProgressMonitor progress_monitor;
        config->setProgressMonitor(&progress_monitor);
        if (allow_fp16_) {
            set_fp16_builder_flag(*config);
        }
        {
            std::ostringstream message;
            message << "[trt:build] config: fp16=" << (allow_fp16_ ? "on" : "off")
                    << " workspace_bytes=" << (1ULL << 30) << " opt_level=" << config->getBuilderOptimizationLevel()
                    << " profiling_verbosity=detailed";
            emit_tensorrt_log_line(message.str());
        }

        emit_tensorrt_log_line("[trt:build] building serialized engine...");
        serialized_engine_.reset(builder->buildSerializedNetwork(*network, *config));
        if (serialized_engine_ == nullptr) {
            throw std::runtime_error("TensorRT buildSerializedNetwork failed");
        }
        emit_tensorrt_log_line("[trt:build] serialized engine bytes=" + std::to_string(serialized_engine_->size()));
        if (!save_engine_path_.empty()) {
            emit_tensorrt_log_line("[trt:build] writing engine: " + save_engine_path_.string());
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
        emit_tensorrt_log_line("[trt:build] deserializing engine...");
        engine_.reset(runtime_->deserializeCudaEngine(serialized_engine_->data(), serialized_engine_->size()));
        if (engine_ == nullptr) {
            throw std::runtime_error("failed to deserialize TensorRT engine");
        }
        emit_tensorrt_log_line("[trt:build] engine ready");
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
        MMLTK_PROFILE_SCOPE("rfdetr.native.tensorrt.construct");
        ensure_cuda_ok(cudaSetDevice(shared_state_->device_id()), "cudaSetDevice for TensorRT backend lane");
        ensure_cuda_ok(mmltk::cuda_stream_create_with_highest_priority(&stream_, cudaStreamNonBlocking),
                       "cudaStreamCreateWithPriority for TensorRT backend lane");
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

    [[nodiscard]] const ModelInfo& info() const override {
        return shared_state_->info();
    }
    [[nodiscard]] void* stream() const override {
        return stream_;
    }

    [[nodiscard]] std::vector<std::unique_ptr<InferenceBackend>> make_lanes(int count) const override {
        if (count <= 0) {
            throw std::runtime_error("TensorRT backend lanes must be greater than zero");
        }
        std::vector<std::unique_ptr<InferenceBackend>> lanes;
        lanes.reserve(static_cast<size_t>(count));
        for (int lane = 0; lane < count; ++lane) {
            lanes.push_back(std::make_unique<TensorRtBackend>(shared_state_));
        }
        return lanes;
    }

    OutputTensors run(const torch::Tensor& normalized_input) override {
        MMLTK_PROFILE_SCOPE("rfdetr.native.tensorrt.run");
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
            const std::string missing =
                join_tensor_names(missing_tensor_names.data(), missing_count > 0 ? missing_count : infer_result);
            throw std::runtime_error(missing.empty()
                                         ? "TensorRT input shapes or addresses remain unspecified"
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
        const size_t bytes =
            element_count(shape) * trt_dtype_size(shared_state_->engine().getTensorDataType(tensor_name.c_str()));
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
        const auto dtype = shared_state_->engine().getTensorDataType(output.name.c_str());
        return torch::from_blob(output_buffers_.at(output.name), shape,
                                torch::TensorOptions()
                                    .dtype(trt_dtype_to_torch_scalar_type(dtype))
                                    .device(torch::kCUDA, shared_state_->device_id()));
    }

    std::shared_ptr<TensorRtSharedState> shared_state_;
    TensorRtUnique<nvinfer1::IExecutionContext> context_{nullptr};
    cudaStream_t stream_ = nullptr;
    std::unordered_map<std::string, void*> output_buffers_;
    std::unordered_map<std::string, size_t> output_buffer_bytes_;
};

}  // namespace

ScopedTensorRtLogSink::ScopedTensorRtLogSink(TensorRtLogSink sink)
    : previous_sink_(exchange_tensorrt_log_sink(std::move(sink))) {}

ScopedTensorRtLogSink::~ScopedTensorRtLogSink() {
    exchange_tensorrt_log_sink(std::move(previous_sink_));
}

void InferenceBackend::save_engine(const std::filesystem::path&) {
    throw std::runtime_error("this backend does not support saving TensorRT engines");
}

std::unique_ptr<InferenceBackend> make_tensorrt_backend(const std::filesystem::path& model_path, int device_id,
                                                        bool allow_fp16,
                                                        const std::filesystem::path& save_engine_path) {
    auto backends = make_tensorrt_backend_lanes(model_path, device_id, allow_fp16, 1, save_engine_path);
    auto backend = std::move(backends.front());
    return backend;
}

std::vector<std::unique_ptr<InferenceBackend>> make_tensorrt_backend_lanes(
    const std::filesystem::path& model_path, int device_id, bool allow_fp16, int lane_count,
    const std::filesystem::path& save_engine_path) {
    if (lane_count <= 0) {
        throw std::runtime_error("TensorRT backend lanes must be greater than zero");
    }
    auto shared_state = std::make_shared<TensorRtSharedState>(model_path, device_id, allow_fp16, save_engine_path);
    std::vector<std::unique_ptr<InferenceBackend>> backends;
    backends.reserve(static_cast<size_t>(lane_count));
    for (int lane = 0; lane < lane_count; ++lane) {
        backends.push_back(std::make_unique<TensorRtBackend>(shared_state));
    }
    return backends;
}

}  // namespace mmltk::rfdetr
