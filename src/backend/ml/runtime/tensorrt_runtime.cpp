#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include <NvInfer.h>
#include <NvInferVersion.h>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "detail/tensorrt_engine_access.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/image_failure.h"
namespace mmltk::backend::ml::runtime {
static_assert(NV_TENSORRT_MAJOR == 11, "mmltk requires TensorRT 11");
namespace {
class EngineErrors final : public nvinfer1::IErrorRecorder {
public:
 explicit EngineErrors(nvinfer1::ILogger& logger) noexcept : logger_(logger) {}
 int32_t getNbErrors() const noexcept override {
  std::scoped_lock lock(mutex_);
  return count_;
 }
 nvinfer1::ErrorCode getErrorCode(int32_t index) const noexcept override {
  std::scoped_lock lock(mutex_);
  return index >= 0 && index < count_ ? errors_[index] : nvinfer1::ErrorCode::kUNSPECIFIED_ERROR;
 }
 ErrorDesc getErrorDesc(int32_t) const noexcept override { return "TensorRT typed engine error"; }
 bool hasOverflowed() const noexcept override {
  std::scoped_lock lock(mutex_);
  return overflow_;
 }
 void clear() noexcept override {
  std::scoped_lock lock(mutex_);
  count_ = 0;
  overflow_ = false;
 }
 bool reportError(nvinfer1::ErrorCode code, ErrorDesc description) noexcept override {
  {
   std::scoped_lock lock(mutex_);
   if (count_ < static_cast<int32_t>(errors_.size()))
    errors_[count_++] = code;
   else
    overflow_ = true;
  }
  logger_.log(nvinfer1::ILogger::Severity::kERROR, description);
  return code == nvinfer1::ErrorCode::kINTERNAL_ERROR;
 }
 RefCount incRefCount() noexcept override { return ++references_; }
 RefCount decRefCount() noexcept override { return --references_; }
 void Check(bool succeeded, std::string_view operation, bool cache = false) const {
  using Code = nvinfer1::ErrorCode;
  std::scoped_lock lock(mutex_);
  if (succeeded && count_ == 0 && !overflow_) return;
  bool integrity = count_ != 0 && !overflow_;
  bool allocation = false;
  bool physical = false;
  std::exception_ptr reported;
  for (int32_t index = 0; index < count_; ++index) {
   const auto code = errors_[index];
   integrity = integrity && (code == Code::kINVALID_ARGUMENT || code == Code::kINVALID_CONFIG);
   allocation = allocation || code == Code::kFAILED_ALLOCATION;
   physical = physical || code == Code::kINTERNAL_ERROR;
   reported = mmltk::frameworks::gpu::combine_image_failures(reported, std::make_exception_ptr(TensorRtOperationError(static_cast<std::int32_t>(code), operation)));
  }
  const auto cuda = cudaPeekAtLastError();
  if (cuda != cudaSuccess) {
   const mmltk::frameworks::gpu::CudaError failure(cuda, std::string(operation).c_str());
   if (failure.shared_failure()) throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(std::make_exception_ptr(failure), reported);
   throw mmltk::frameworks::gpu::ImageFailure(std::make_exception_ptr(failure), reported);
  }
  if (physical) throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(reported);
  if (allocation) throw mmltk::frameworks::gpu::ImageFailure(std::make_exception_ptr(mmltk::frameworks::gpu::CudaError(cudaErrorMemoryAllocation, std::string(operation).c_str())), reported);
  if (cache && integrity) throw TensorRtCacheIntegrityError(std::string(operation));
  if (reported) std::rethrow_exception(reported);
  throw std::runtime_error(std::string(operation));
 }
 [[nodiscard]] bool cancellation_failure_only() const noexcept {
  std::scoped_lock lock(mutex_);
  return !overflow_ && std::ranges::all_of(std::span{errors_}.first(static_cast<std::size_t>(count_)), [](const auto code) { return code == nvinfer1::ErrorCode::kFAILED_EXECUTION; });
 }

private:
 nvinfer1::ILogger& logger_;
 mutable std::mutex mutex_;
 std::array<nvinfer1::ErrorCode, 32U> errors_{};
 int32_t count_ = 0;
 bool overflow_ = false;
 std::atomic<RefCount> references_{0};
};
void destroy_builder(nvinfer1::IBuilder* pointer) noexcept { delete pointer; }
void destroy_network(nvinfer1::INetworkDefinition* pointer) noexcept { delete pointer; }
void destroy_config(nvinfer1::IBuilderConfig* pointer) noexcept { delete pointer; }
void destroy_parser(nvonnxparser::IParser* pointer) noexcept { delete pointer; }
void destroy_runtime(nvinfer1::IRuntime* pointer) noexcept { delete pointer; }
void destroy_engine(nvinfer1::ICudaEngine* pointer) noexcept { delete pointer; }
void destroy_host_memory(nvinfer1::IHostMemory* pointer) noexcept { delete pointer; }
using BuilderOwner = std::unique_ptr<nvinfer1::IBuilder, decltype(&destroy_builder)>;
using NetworkOwner = std::unique_ptr<nvinfer1::INetworkDefinition, decltype(&destroy_network)>;
using ConfigOwner = std::unique_ptr<nvinfer1::IBuilderConfig, decltype(&destroy_config)>;
using ParserOwner = std::unique_ptr<nvonnxparser::IParser, decltype(&destroy_parser)>;
using RuntimeOwner = std::unique_ptr<nvinfer1::IRuntime, decltype(&destroy_runtime)>;
using EngineOwner = std::unique_ptr<nvinfer1::ICudaEngine, decltype(&destroy_engine)>;
using HostMemoryOwner = std::unique_ptr<nvinfer1::IHostMemory, decltype(&destroy_host_memory)>;
[[nodiscard]] std::string tensor_rt_data_type_name(nvinfer1::DataType data_type);
[[nodiscard]] nvinfer1::ProfilingVerbosity profiling_verbosity(const TensorRtProfilingVerbosity value) noexcept {
 switch (value) {
  case TensorRtProfilingVerbosity::Detailed: return nvinfer1::ProfilingVerbosity::kDETAILED;
  case TensorRtProfilingVerbosity::Disabled: return nvinfer1::ProfilingVerbosity::kNONE;
  case TensorRtProfilingVerbosity::LayerNames:
  default: return nvinfer1::ProfilingVerbosity::kLAYER_NAMES_ONLY;
 }
}
[[nodiscard]] std::streamsize checked_stream_size(const std::size_t size, const std::string_view context) {
 if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) { throw std::overflow_error(std::string(context) + " exceeds std::streamsize"); }
 return static_cast<std::streamsize>(size);
}
[[nodiscard]] std::vector<char> read_binary(const std::filesystem::path& path, const std::string_view context) {
 std::ifstream stream(path, std::ios::binary | std::ios::ate);
 if (!stream.is_open()) { throw std::runtime_error(std::string(context) + " failed to open " + path.string()); }
 const std::streampos end = stream.tellg();
 if (end < 0) { throw std::runtime_error(std::string(context) + " failed to size " + path.string()); }
 const auto size = static_cast<std::size_t>(end);
 std::vector<char> bytes(size);
 stream.seekg(0, std::ios::beg);
 if (size != 0U && !stream.read(bytes.data(), checked_stream_size(size, context))) { throw std::runtime_error(std::string(context) + " failed to read " + path.string()); }
 return bytes;
}
void write_binary(const std::filesystem::path& path, const void* data, const std::size_t size, const std::string_view context) {
 std::ofstream stream(path, std::ios::binary | std::ios::trunc);
 if (!stream.is_open()) { throw std::runtime_error(std::string(context) + " failed to open " + path.string()); }
 if (size != 0U) { stream.write(static_cast<const char*>(data), checked_stream_size(size, context)); }
 if (!stream) { throw std::runtime_error(std::string(context) + " failed to write " + path.string()); }
}
[[nodiscard]] std::string format_dimensions(const nvinfer1::Dims& dimensions) {
 std::ostringstream output;
 output << '[';
 for (std::int32_t axis = 0; axis < dimensions.nbDims; ++axis) {
  if (axis != 0) { output << 'x'; }
  output << dimensions.d[axis];
 }
 output << ']';
 return output.str();
}
[[nodiscard]] nvinfer1::Dims profile_dimensions(const std::vector<std::int64_t>& values, const std::string_view context) {
 if (values.empty() || values.size() > static_cast<std::size_t>(nvinfer1::Dims::MAX_DIMS)) { throw std::invalid_argument(std::string(context) + " has an invalid rank"); }
 nvinfer1::Dims dimensions{};
 dimensions.nbDims = static_cast<std::int32_t>(values.size());
 for (std::size_t axis = 0U; axis < values.size(); ++axis) {
  if (values[axis] <= 0 || values[axis] > std::numeric_limits<std::int32_t>::max()) { throw std::invalid_argument(std::string(context) + " contains an invalid dimension"); }
  dimensions.d[axis] = static_cast<std::int32_t>(values[axis]);
 }
 return dimensions;
}
[[nodiscard]] const nvinfer1::ITensor& profile_input(const nvinfer1::INetworkDefinition& network, const std::string_view name, const std::string_view context) {
 for (std::int32_t index = 0; index < network.getNbInputs(); ++index) {
  const nvinfer1::ITensor* input = network.getInput(index);
  if (input != nullptr && input->getName() != nullptr && name == input->getName()) { return *input; }
 }
 throw std::invalid_argument(std::string(context) + " references unknown input " + std::string(name));
}
void configure_optimization_profiles(nvinfer1::IBuilder& builder, nvinfer1::INetworkDefinition& network, nvinfer1::IBuilderConfig& config, const TensorRtEngineOptions& options) {
 std::unordered_set<std::string> dynamic_inputs;
 for (std::int32_t index = 0; index < network.getNbInputs(); ++index) {
  const nvinfer1::ITensor* input = network.getInput(index);
  if (input == nullptr || input->getName() == nullptr || input->isShapeTensor()) { continue; }
  const nvinfer1::Dims dimensions = input->getDimensions();
  for (std::int32_t axis = 0; axis < dimensions.nbDims; ++axis) {
   if (dimensions.d[axis] < 0) {
    dynamic_inputs.emplace(input->getName());
    break;
   }
  }
 }
 if (options.optimization_profiles.empty()) {
  if (!dynamic_inputs.empty()) { throw std::invalid_argument(options.context + " requires optimization profiles for every dynamic input"); }
  return;
 }
 nvinfer1::IOptimizationProfile* profile = builder.createOptimizationProfile();
 if (profile == nullptr) { throw std::runtime_error(options.context + " failed to create an optimization profile"); }
 std::unordered_set<std::string> configured_inputs;
 for (const TensorRtOptimizationProfile& definition : options.optimization_profiles) {
  if (definition.input_name.empty()) { throw std::invalid_argument(options.context + " optimization profile input name is empty"); }
  if (!configured_inputs.emplace(definition.input_name).second) { throw std::invalid_argument(options.context + " contains duplicate optimization profiles for " + definition.input_name); }
  const nvinfer1::ITensor& input = profile_input(network, definition.input_name, options.context);
  if (input.isShapeTensor()) { throw std::invalid_argument(options.context + " does not support value profiles for shape tensor " + definition.input_name); }
  const nvinfer1::Dims minimum = profile_dimensions(definition.minimum, options.context + " minimum optimization profile");
  const nvinfer1::Dims optimum = profile_dimensions(definition.optimum, options.context + " optimum optimization profile");
  const nvinfer1::Dims maximum = profile_dimensions(definition.maximum, options.context + " maximum optimization profile");
  if (minimum.nbDims != optimum.nbDims || optimum.nbDims != maximum.nbDims || minimum.nbDims != input.getDimensions().nbDims) {
   throw std::invalid_argument(options.context + " optimization profile rank does not match input " + definition.input_name);
  }
  const nvinfer1::Dims network_dimensions = input.getDimensions();
  for (std::int32_t axis = 0; axis < minimum.nbDims; ++axis) {
   if (minimum.d[axis] > optimum.d[axis] || optimum.d[axis] > maximum.d[axis]) {
    throw std::invalid_argument(options.context + " optimization profile ordering is invalid for " + definition.input_name);
   }
   if (network_dimensions.d[axis] > 0 && (minimum.d[axis] != network_dimensions.d[axis] || optimum.d[axis] != network_dimensions.d[axis] || maximum.d[axis] != network_dimensions.d[axis])) {
    throw std::invalid_argument(options.context + " optimization profile changes a static dimension for " + definition.input_name);
   }
  }
  if (!profile->setDimensions(definition.input_name.c_str(), nvinfer1::OptProfileSelector::kMIN, minimum) ||
      !profile->setDimensions(definition.input_name.c_str(), nvinfer1::OptProfileSelector::kOPT, optimum) ||
      !profile->setDimensions(definition.input_name.c_str(), nvinfer1::OptProfileSelector::kMAX, maximum)) {
   throw std::runtime_error(options.context + " failed to configure optimization profile for " + definition.input_name);
  }
 }
 for (const std::string& input_name : dynamic_inputs) {
  if (!configured_inputs.contains(input_name)) { throw std::invalid_argument(options.context + " has no optimization profile for dynamic input " + input_name); }
 }
 if (!profile->isValid() || config.addOptimizationProfile(profile) < 0) { throw std::runtime_error(options.context + " rejected the optimization profile"); }
}
class Logger final : public nvinfer1::ILogger {
public:
 Logger(std::string context, std::function<void(std::string_view)> sink) : context_(std::move(context)), sink_(std::move(sink)) {}
 void set_threshold(const Severity threshold) noexcept { threshold_.store(static_cast<int>(threshold), std::memory_order_relaxed); }
 void log(const Severity severity, const char* message) noexcept override {
  if (severity > static_cast<Severity>(threshold_.load(std::memory_order_relaxed)) || !sink_) { return; }
  try {
   std::string line;
   line.reserve(context_.size() + 32U + (message != nullptr ? std::char_traits<char>::length(message) : 0U));
   line += '[';
   line += context_;
   line += ":trt";
   switch (severity) {
    case Severity::kINTERNAL_ERROR:
    case Severity::kERROR: line += ":error"; break;
    case Severity::kWARNING: line += ":warn"; break;
    case Severity::kVERBOSE: line += ":verbose"; break;
    case Severity::kINFO:
    default: break;
   }
   line += "] ";
   line += message != nullptr ? message : "";
   sink_(line);
  } catch (...) { return; }
 }

private:
 std::string context_;
 std::function<void(std::string_view)> sink_;
 std::atomic<int> threshold_{static_cast<int>(Severity::kWARNING)};
};
class BuildProgressMonitor final : public nvinfer1::IProgressMonitor {
public:
 explicit BuildProgressMonitor(std::function<void(std::string_view)> sink, std::function<bool()> continue_build) : sink_(std::move(sink)), continue_build_(std::move(continue_build)) {}
 void phaseStart(const char* phase_name, const char* parent_phase, const std::int32_t steps) noexcept override {
  if (!sink_) return;
  try {
   std::lock_guard lock(mutex_);
   const std::string phase = phase_name != nullptr ? phase_name : "<unnamed>";
   phase_steps_[phase] = steps;
   if (sink_) {
    std::ostringstream message;
    message << "[trt:build] phase start ";
    if (parent_phase != nullptr && parent_phase[0] != '\0') { message << parent_phase << " -> "; }
    message << phase << " steps=" << steps;
    sink_(message.str());
   }
  } catch (...) { return; }
 }
 bool stepComplete(const char* phase_name, const std::int32_t step) noexcept override {
  try {
   std::lock_guard lock(mutex_);
   if (sink_) {
    const std::string phase = phase_name != nullptr ? phase_name : "<unnamed>";
    const auto found = phase_steps_.find(phase);
    std::ostringstream message;
    message << "[trt:build] phase step " << phase << ' ' << step + 1;
    if (found != phase_steps_.end() && found->second > 0) { message << '/' << found->second; }
    sink_(message.str());
   }
  } catch (...) { return should_continue(); }
  return should_continue();
 }
 void phaseFinish(const char* phase_name) noexcept override {
  if (!sink_) return;
  try {
   std::lock_guard lock(mutex_);
   const std::string phase = phase_name != nullptr ? phase_name : "<unnamed>";
   phase_steps_.erase(phase);
   if (sink_) { sink_("[trt:build] phase finish " + phase); }
  } catch (...) { return; }
 }
 [[nodiscard]] bool cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

private:
 [[nodiscard]] bool should_continue() noexcept {
  if (!continue_build_) { return true; }
  try {
   const bool proceed = continue_build_();
   if (!proceed) cancelled_.store(true, std::memory_order_release);
   return proceed;
  } catch (...) {
   cancelled_.store(true, std::memory_order_release);
   return false;
  }
 }
 std::function<void(std::string_view)> sink_;
 std::function<bool()> continue_build_;
 std::mutex mutex_;
 std::unordered_map<std::string, std::int32_t> phase_steps_;
 std::atomic_bool cancelled_{false};
};
[[nodiscard]] std::string parser_errors(const nvonnxparser::IParser& parser) {
 std::ostringstream output;
 for (std::int32_t index = 0; index < parser.getNbErrors(); ++index) {
  const nvonnxparser::IParserError* error = parser.getError(index);
  if (error == nullptr) { continue; }
  if (output.tellp() > 0) { output << '\n'; }
  output << '[' << index << "] code=" << static_cast<int>(error->code()) << " op=" << (error->nodeOperator() != nullptr ? error->nodeOperator() : "<unknown>")
         << " node=" << (error->nodeName() != nullptr ? error->nodeName() : "<unnamed>") << " desc=" << (error->desc() != nullptr ? error->desc() : "<no description>");
 }
 return output.str();
}
void configure_tf32(nvinfer1::IBuilderConfig& config, const bool allow_tf32) {
 if (!allow_tf32) { config.clearFlag(nvinfer1::BuilderFlag::kTF32); }
}
}  // namespace
struct TensorRtEngine::Impl {
 explicit Impl(std::filesystem::path path, TensorRtEngineOptions engine_options)
     : model_path(std::move(path)), options(std::move(engine_options)), logger(options.context, options.log), errors(logger) {
  if (!admitted()) return;
  const cudaError_t set_device = cudaSetDevice(options.device);
  mmltk::frameworks::gpu::ensure_cuda_ok(set_device, (options.context + " cudaSetDevice").c_str());
  if (!admitted()) return;
  std::string extension = model_path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
  if (extension == ".engine") {
   load_engine(read_binary(model_path, options.context));
  } else if (extension == ".onnx") {
   built_from_onnx = true;
   build_engine();
  } else {
   throw std::invalid_argument(options.context + " expects a .engine or .onnx model");
  }
  options.continue_build = {};
 }
 bool admitted() {
  cancelled = options.continue_build && !options.continue_build();
  return !cancelled;
 }
 void emit(const std::string& message) const {
  if (options.log) { options.log(message); }
 }
 void load_engine(const std::vector<char>& bytes) {
  if (bytes.empty()) throw TensorRtCacheIntegrityError(options.context + " empty TensorRT engine");
  if (!admitted()) return;
  runtime.reset(nvinfer1::createInferRuntime(logger));
  if (runtime == nullptr) { throw std::runtime_error(options.context + " failed to create TensorRT runtime"); }
  runtime->setErrorRecorder(&errors);
  if (!admitted()) return;
  engine.reset(runtime->deserializeCudaEngine(bytes.data(), bytes.size()));
  errors.Check(engine != nullptr, options.context + " deserialize TensorRT engine", true);
 }
 void build_engine() {
  if (!admitted()) return;
  logger.set_threshold(nvinfer1::ILogger::Severity::kVERBOSE);
  BuilderOwner builder(nvinfer1::createInferBuilder(logger), destroy_builder);
  if (builder == nullptr) { throw std::runtime_error(options.context + " failed to create TensorRT builder"); }
  builder->setErrorRecorder(&errors);
  if (!admitted()) return;
  NetworkOwner network(builder->createNetworkV2(0), destroy_network);
  if (network == nullptr) throw std::runtime_error(options.context + " failed to create TensorRT network");
  if (!admitted()) return;
  ConfigOwner config(builder->createBuilderConfig(), destroy_config);
  if (config == nullptr) { throw std::runtime_error(options.context + " failed to create TensorRT build objects"); }
  if (!admitted()) return;
  ParserOwner parser(nvonnxparser::createParser(*network, logger), destroy_parser);
  if (parser == nullptr) { throw std::runtime_error(options.context + " failed to create ONNX parser"); }
  if (!admitted()) return;
  emit("[trt:build] parsing ONNX " + model_path.string());
  if (!parser->parseFromFile(model_path.string().c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kVERBOSE))) {
   const std::string diagnostics = parser_errors(*parser);
   throw std::runtime_error(options.context + " failed to parse ONNX" + (diagnostics.empty() ? std::string{} : ":\n" + diagnostics));
  }
  emit("[trt:build] parsed layers=" + std::to_string(network->getNbLayers()) + " inputs=" + std::to_string(network->getNbInputs()) + " outputs=" + std::to_string(network->getNbOutputs()));
  for (std::int32_t index = 0; index < network->getNbInputs(); ++index) {
   const nvinfer1::ITensor* tensor = network->getInput(index);
   if (tensor != nullptr) {
    emit("[trt:build] input " + std::string(tensor->getName() != nullptr ? tensor->getName() : "<unnamed>") + " dtype=" + tensor_rt_data_type_name(tensor->getType()) +
         " dims=" + format_dimensions(tensor->getDimensions()));
   }
  }
  for (std::int32_t index = 0; index < network->getNbOutputs(); ++index) {
   const nvinfer1::ITensor* tensor = network->getOutput(index);
   if (tensor != nullptr) {
    emit("[trt:build] output " + std::string(tensor->getName() != nullptr ? tensor->getName() : "<unnamed>") + " dtype=" + tensor_rt_data_type_name(tensor->getType()) +
         " dims=" + format_dimensions(tensor->getDimensions()));
   }
  }
  if (!admitted()) return;
  configure_optimization_profiles(*builder, *network, *config, options);
  config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, options.workspace_bytes);
  config->setProfilingVerbosity(profiling_verbosity(options.profiling_verbosity));
  BuildProgressMonitor progress(options.log, options.continue_build);
  config->setProgressMonitor(&progress);
  configure_tf32(*config, options.allow_tf32);
  if (options.log) {
   emit("[trt:build] precision=model-defined automatic_fp16_lowering=not-applied fp16_permission=" + std::string(options.allow_fp16 ? "allowed" : "disabled") +
        " tf32_permission=" + std::string(options.allow_tf32 ? "allowed" : "disabled"));
  }
  emit("[trt:build] building serialized engine");
  if (!admitted()) return;
  serialized.reset(builder->buildSerializedNetwork(*network, *config));
  const auto cuda_status = cudaPeekAtLastError();
  if (progress.cancelled() && cuda_status == cudaSuccess && errors.cancellation_failure_only()) {
   errors.clear();
   cancelled = true;
   serialized.reset();
   return;
  }
  // Inspect reported failures before cancellation: an OOM/context loss
  // concurrent with withdrawal is not a successful cancellation.
  if (errors.getNbErrors() != 0 || errors.hasOverflowed() || cuda_status != cudaSuccess) errors.Check(false, options.context + " build TensorRT engine");
  if (!admitted()) return;
  if (serialized == nullptr) { throw std::runtime_error(options.context + " TensorRT buildSerializedNetwork failed"); }
  emit("[trt:build] serialized bytes=" + std::to_string(serialized->size()));
  if (!options.save_engine_path.empty()) { write_binary(options.save_engine_path, serialized->data(), serialized->size(), options.context); }
  if (!admitted()) return;
  runtime.reset(nvinfer1::createInferRuntime(logger));
  if (runtime == nullptr) { throw std::runtime_error(options.context + " failed to create TensorRT runtime"); }
  runtime->setErrorRecorder(&errors);
  if (!admitted()) return;
  engine.reset(runtime->deserializeCudaEngine(serialized->data(), serialized->size()));
  errors.Check(engine != nullptr, options.context + " deserialize built TensorRT engine");
  emit("[trt:build] engine ready");
  logger.set_threshold(nvinfer1::ILogger::Severity::kWARNING);
 }
 std::filesystem::path model_path;
 TensorRtEngineOptions options;
 Logger logger;
 EngineErrors errors;
 RuntimeOwner runtime{nullptr, destroy_runtime};
 EngineOwner engine{nullptr, destroy_engine};
 HostMemoryOwner serialized{nullptr, destroy_host_memory};
 bool built_from_onnx = false;
 bool cancelled = false;
};
TensorRtEngine::TensorRtEngine(const std::filesystem::path& model_path, TensorRtEngineOptions options) : impl_(std::make_unique<Impl>(model_path, std::move(options))) {}
TensorRtEngine::~TensorRtEngine() = default;
TensorRtEngine::TensorRtEngine(TensorRtEngine&&) noexcept = default;
TensorRtEngine& TensorRtEngine::operator=(TensorRtEngine&&) noexcept = default;
bool TensorRtEngine::cancelled() const noexcept { return impl_->cancelled; }
std::int32_t TensorRtEngine::device() const noexcept { return impl_->options.device; }
nvinfer1::ICudaEngine& detail::TensorRtEngineAccess::Get(const TensorRtEngine& owner) noexcept { return *owner.impl_->engine; }
const std::filesystem::path& TensorRtEngine::model_path() const noexcept { return impl_->model_path; }
bool TensorRtEngine::built_from_onnx() const noexcept { return impl_->built_from_onnx; }
std::uintptr_t TensorRtEngine::native_engine_handle() const noexcept { return reinterpret_cast<std::uintptr_t>(impl_->engine.get()); }
void TensorRtEngine::CheckOperation(const bool succeeded, const std::string_view operation) const { impl_->errors.Check(succeeded, operation); }
void TensorRtEngine::Save(const std::filesystem::path& path) const {
 if (impl_->serialized == nullptr) { throw std::runtime_error(impl_->options.context + " has no serialized engine available to save"); }
 write_binary(path, impl_->serialized->data(), impl_->serialized->size(), impl_->options.context);
}
namespace {
std::string tensor_rt_data_type_name(const nvinfer1::DataType data_type) {
 switch (data_type) {
  case nvinfer1::DataType::kFLOAT: return "float32";
  case nvinfer1::DataType::kHALF: return "float16";
  case nvinfer1::DataType::kINT8: return "int8";
  case nvinfer1::DataType::kINT32: return "int32";
  case nvinfer1::DataType::kBOOL: return "bool";
  case nvinfer1::DataType::kUINT8: return "uint8";
  case nvinfer1::DataType::kFP8: return "float8";
  case nvinfer1::DataType::kBF16: return "bfloat16";
  case nvinfer1::DataType::kINT64: return "int64";
  case nvinfer1::DataType::kINT4: return "int4";
  case nvinfer1::DataType::kFP4: return "float4";
  default: return "unsupported";
 }
}
}  // namespace
}  // namespace mmltk::backend::ml::runtime
