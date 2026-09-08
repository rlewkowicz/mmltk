module;
#include <cuda_runtime.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "detail/image_upscaler_cuda.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/image_failure.h"
#include "upscale_execution.h"

module mmltk.backend.imaging.upscale.image_upscaler;


import mmltk.common.logging.mmltk_logging;

#include "detail/image_upscaler_internal.h"
#include "detail/image_upscaler_request_tile.h"

namespace mmltk::backend::imaging::upscale {

using mmltk::frameworks::gpu::ensure_cuda_ok;

namespace {

constexpr std::array<std::int64_t, 4U> kInputShape{1, 3, kImageUpscalerInputExtent, kImageUpscalerInputExtent};
constexpr std::array<std::int64_t, 4U> kOutputShape{1, 3, kImageUpscalerOutputExtent, kImageUpscalerOutputExtent};

[[nodiscard]] Ort::Env& upscaler_ort_environment() {
    static Ort::Env environment(ORT_LOGGING_LEVEL_ERROR, "mmltk_backend_imaging_upscale");
    return environment;
}

void validate_shape(const std::vector<std::int64_t>& actual, const std::array<std::int64_t, 4U>& expected, const std::string& context) {
    if (actual.size() != expected.size()) { throw std::runtime_error(context + " must be a rank-four NCHW tensor"); }
    for (std::size_t axis = 0U; axis < actual.size(); ++axis) {
        if (actual[axis] > 0 && actual[axis] != expected[axis]) {
            throw std::runtime_error(context + " does not support the fixed upscaler tile shape");
        }
    }
}

class OnnxImageUpscalerRuntime final
    : public TiledImageUpscalerRuntimeAdapter<OnnxImageUpscalerRuntime, ImageUpscalerBackend::OnnxRuntime> {
   public:
    [[nodiscard]] bool graph_replay() const noexcept override { return graph_enabled_ && inference_runs_ >= 3U; }
    OnnxImageUpscalerRuntime(const ImageUpscalerDescriptor& descriptor, const std::filesystem::path& model_path, const int device_id,
                             const ImageUpscalerExecutionCheckpoint& checkpoint)
        : TiledImageUpscalerRuntimeAdapter(descriptor, device_id, checkpoint),
          model_path_(model_path),
          input_elements_(checked_upscaler_elements(kImageUpscalerInputExtent, kImageUpscalerInputExtent, 3U)),
          output_elements_(checked_upscaler_elements(kImageUpscalerOutputExtent, kImageUpscalerOutputExtent, 3U)) {}

    ImageUpscalerOutcome ActivateBackend(ImageUpscalerCurrent current) {
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        const auto& descriptor = this->descriptor();
        const auto device_id = this->device_id();
        bool diagnostics_enabled = false;
        mmltk::common::logging::trace([&](auto&) { diagnostics_enabled = true; });
        std::size_t free_before = 0U;
        std::size_t total_before = 0U;
        if (diagnostics_enabled) { (void)cudaMemGetInfo(&free_before, &total_before); }
            ensure_cuda_ok(cudaSetDevice(device_id), "cudaSetDevice for ONNX Image upscaler");
            ensure_cuda_ok(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreate for ONNX Image upscaler");
            Checkpoint(ImageUpscalerExecutionStage::StreamCreated);
            if (!current()) return ImageUpscalerOutcome::Cancelled;
            ensure_cuda_ok(cudaEventCreateWithFlags(&source_ready_, cudaEventDisableTiming), "cudaEventCreate for ONNX upscaler source");
            Checkpoint(ImageUpscalerExecutionStage::EventCreated);
            if (!current()) return ImageUpscalerOutcome::Cancelled;
            ensure_cuda_ok(cudaEventCreateWithFlags(&completion_, cudaEventDisableTiming), "cudaEventCreate for ONNX upscaler completion");
            Checkpoint(ImageUpscalerExecutionStage::EventCreated);
            if (!current()) return ImageUpscalerOutcome::Cancelled;

            const char* reference = std::getenv("MMLTK_UPSCALE_ONNX_REFERENCE");
            const bool reference_run = reference != nullptr && std::string_view(reference) == "1";
            graph_enabled_ = !reference_run;
            try {
                create_session(graph_enabled_);
            } catch (const Ort::Exception& error) {
                const std::string_view detail{error.what()};
                if (!graph_enabled_ || !graph_ineligible(detail)) throw;
                // ORT validates complete CUDA partitioning/control-flow eligibility at
                // session construction. Rebuild once without capture for that model.
                graph_enabled_ = false;
                mmltk::common::logging::trace([&](auto& logger) {
                    logger.trace("event=image_upscaler_onnx_graph_unavailable device={} model={} detail={}", device_id, descriptor.label,
                                 error.what());
                });
                if (!current()) return ImageUpscalerOutcome::Cancelled;
                create_session(false);
            }
            if (!reference_run) run_options_.AddConfigEntry("disable_synchronize_execution_providers", "1");
            validate_model();
            Checkpoint(ImageUpscalerExecutionStage::ContextCreated);
            if (!current()) return ImageUpscalerOutcome::Cancelled;

            input_.ensure(input_elements_, "cudaMalloc for ONNX upscaler FP32 input");
            Checkpoint(ImageUpscalerExecutionStage::BuffersAllocated);
            if (!current()) return ImageUpscalerOutcome::Cancelled;
            output_.ensure(output_elements_, "cudaMalloc for ONNX upscaler output");
            Checkpoint(ImageUpscalerExecutionStage::BuffersAllocated);
            if (!current()) return ImageUpscalerOutcome::Cancelled;
            device_memory_ = std::make_unique<Ort::MemoryInfo>("Cuda", OrtArenaAllocator, device_id, OrtMemTypeDefault);
            input_value_ = std::make_unique<Ort::Value>(
                Ort::Value::CreateTensor<float>(*device_memory_, input_.data(), input_elements_, kInputShape.data(), kInputShape.size()));
            output_value_ = std::make_unique<Ort::Value>(Ort::Value::CreateTensor<float>(*device_memory_, output_.data(), output_elements_,
                                                                                         kOutputShape.data(), kOutputShape.size()));
            bind_tensors();
            // All activation-owned objects are installed. Withdrawal here is
            // handled by the resident execution path, not partial cleanup.
            Checkpoint(ImageUpscalerExecutionStage::BindingsReady);
            mmltk::common::logging::trace([&](auto& logger) {
                std::size_t free_after = 0U;
                std::size_t total_after = 0U;
                (void)cudaMemGetInfo(&free_after, &total_after);
                logger.trace(
                    "event=image_upscaler_onnx_runtime_allocated device={} model={} model_sha256={} tf32={} "
                    "input_buffer_bytes={} output_buffer_bytes={} input_buffer={} output_buffer={} graph={} free_before_bytes={} "
                    "free_after_bytes={} "
                    "total_device_bytes={} observed_device_allocation_bytes={}",
                    device_id, descriptor.label, descriptor.sha256, descriptor.allow_tf32, input_elements_ * sizeof(float),
                    output_elements_ * sizeof(float), reinterpret_cast<std::uintptr_t>(input_.data()),
                    reinterpret_cast<std::uintptr_t>(output_.data()), graph_enabled_, free_before, free_after,
                    total_after != 0U ? total_after : total_before, free_before > free_after ? free_before - free_after : 0U);
            });
        return ImageUpscalerOutcome::Completed;
    }

    ~OnnxImageUpscalerRuntime() override {
        if (stream_ != nullptr || source_ready_ != nullptr || completion_ != nullptr || abandoned_capture_ != nullptr
            || session_ || binding_ || input_value_ || output_value_ || device_memory_)
            std::terminate();
    }

   private:
    friend class TiledImageUpscalerRuntimeAdapter<OnnxImageUpscalerRuntime, ImageUpscalerBackend::OnnxRuntime>;

    void create_session(const bool graph) {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        Ort::CUDAProviderOptions cuda_options;
        cuda_options.Update(std::unordered_map<std::string, std::string>{
            {"device_id", std::to_string(device_id())},
            {"do_copy_in_default_stream", "1"},
            {"use_tf32", descriptor().allow_tf32 ? "1" : "0"},
            {"enable_cuda_graph", graph ? "1" : "0"},
        });
        cuda_options.UpdateWithValue("user_compute_stream", stream_);
        options.AppendExecutionProvider_CUDA_V2(*cuda_options);
        session_ = std::make_unique<Ort::Session>(upscaler_ort_environment(), model_path_.string().c_str(), options);
    }

    void bind_tensors() {
        auto binding = std::make_unique<Ort::IoBinding>(*session_);
        binding->BindInput(descriptor().input_name, *input_value_);
        binding->BindOutput(descriptor().output_name, *output_value_);
        binding_ = std::move(binding);
    }

    [[nodiscard]] static bool graph_ineligible(const std::string_view detail) noexcept {
        return detail.find("graph capture feature") != std::string_view::npos ||
               detail.find("CUDA Graph feature") != std::string_view::npos;
    }

    [[nodiscard]] static bool first_capture_rejected(const std::string_view detail) noexcept {
        if (graph_ineligible(detail) || detail.find("CUDAGraph::CaptureEnd: graph_ is NULL") != std::string_view::npos ||
            (detail.find("Graph capture did not complete after ") != std::string_view::npos &&
             detail.find(" internal runs for CUDAExecutionProvider") != std::string_view::npos))
            return true;
        // ORT 1.27.1 CudaCall emits the numeric CUDA status before its expression.
        // Decode only capture restrictions, never OOM or arbitrary CUDA failures.
        constexpr std::string_view prefix = "CUDA failure ";
        const auto start = detail.find(prefix);
        if (start == std::string_view::npos) return false;
        const auto code_text = detail.substr(start + prefix.size());
        int code = 0;
        const auto parsed = std::from_chars(code_text.data(), code_text.data() + code_text.size(), code);
        if (parsed.ec != std::errc{} || parsed.ptr == code_text.data() + code_text.size() || *parsed.ptr != ':') return false;
        switch (static_cast<cudaError_t>(code)) {
            case cudaErrorStreamCaptureUnsupported:
            case cudaErrorStreamCaptureInvalidated:
            case cudaErrorStreamCaptureMerge:
            case cudaErrorStreamCaptureUnmatched:
            case cudaErrorStreamCaptureUnjoined:
            case cudaErrorStreamCaptureIsolation:
            case cudaErrorStreamCaptureImplicit:
            case cudaErrorStreamCaptureWrongThread:
            case cudaErrorCapturedEvent:
                return true;
            case cudaErrorNotSupported:
                return detail.find("expr=cudaStreamBeginCapture(") != std::string_view::npos ||
                       detail.find("expr=cudaGraphInstantiate(") != std::string_view::npos ||
                       detail.find("expr=cudaGraphLaunch(") != std::string_view::npos;
            default:
                return false;
        }
    }

    [[nodiscard]] cudaError_t settle_stream() noexcept {
        return settle_upscaler_stream(stream_, abandoned_capture_);
    }
    [[nodiscard]] cudaError_t SettleBackend() noexcept {
        cleanup_.Record(settle_stream(), "settle ONNX provider stream");
        return cleanup_.status();
    }

    ImageUpscalerOutcome run_tile(ImageUpscalerCurrent current) {
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        try {
            session_->Run(run_options_, *binding_);
            Checkpoint(ImageUpscalerExecutionStage::WarmSubmitted);
        } catch (const Ort::Exception& error) {
            if (!graph_enabled_ || inference_submitted_ || !first_capture_rejected(error.what())) throw;
            ensure_cuda_ok(settle_stream(), "settle rejected ONNX CUDA graph capture");
            if (abandoned_capture_ != nullptr) {
                ensure_cuda_ok(cudaGraphDestroy(abandoned_capture_), "destroy rejected ONNX CUDA graph");
                abandoned_capture_ = nullptr;
            }
            // Only provider-owned state is replaced. The stream, input/output
            // allocations, OrtValues and current tile remain unchanged.
            if (!current()) return ImageUpscalerOutcome::Cancelled;
            binding_.reset();
            session_.reset();
            graph_enabled_ = false;
            create_session(false);
            validate_model();
            bind_tensors();
            if (!current()) return ImageUpscalerOutcome::Cancelled;
            mmltk::common::logging::trace([&](auto& logger) {
                logger.trace("event=image_upscaler_onnx_first_capture_fallback device={} model={} detail={}", device_id(),
                             descriptor().label, error.what());
            });
            // One ordinary retry; errors here are outside the graph catch path.
            session_->Run(run_options_, *binding_);
        }
        inference_submitted_ = true;
        if (inference_runs_ < 3U) ++inference_runs_;
        return ImageUpscalerOutcome::Completed;
    }

    struct CompletionTiming final {
        std::atomic_bool pending{false};
        std::chrono::steady_clock::time_point admitted;
        std::uint64_t sequence = 0U;
        int device = -1;
        std::string_view model;
        bool first_inference = false;
    } timing_;

    static void CUDART_CB completed(void* data) noexcept {
        auto& timing = *static_cast<CompletionTiming*>(data);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - timing.admitted).count();
        mmltk::common::logging::trace([&](auto& logger) {
            logger.trace(
                "event=image_upscaler_onnx_gpu_completed device={} model={} sequence={} first_inference={} enqueue_to_completion_us={}",
                timing.device, timing.model, timing.sequence, timing.first_inference, elapsed);
        });
        timing.pending.store(false, std::memory_order_release);
    }

    [[nodiscard]] cudaError_t ReleaseBackend() noexcept {
        // Session/provider, binding and buffers all remain owned if stream
        // settlement fails; no destructor may race the provider's work.
        if (!cleanup_.Record(settle_stream(), "settle ONNX provider stream")) return cleanup_.status();
        if (abandoned_capture_ != nullptr &&
            cleanup_.Record(cudaGraphDestroy(abandoned_capture_), "destroy abandoned ONNX capture"))
            abandoned_capture_ = nullptr;
        if (abandoned_capture_ == nullptr) {
        binding_.reset();
        output_value_.reset();
        input_value_.reset();
        device_memory_.reset();
        session_.reset();
        CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::ContextReleased);
        static_cast<void>(input_.Release(&cleanup_));
        CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::BufferReleased);
        static_cast<void>(output_.Release(&cleanup_));
        CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::BufferReleased);
        }
        if (completion_ != nullptr) {
            if (cleanup_.Record(cudaEventDestroy(completion_), "destroy ONNX completion")) completion_ = nullptr;
            CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::EventDestroyed);
        }
        if (source_ready_ != nullptr) {
            if (cleanup_.Record(cudaEventDestroy(source_ready_), "destroy ONNX source fence")) source_ready_ = nullptr;
            CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::EventDestroyed);
        }
        if (stream_ != nullptr) {
            if (cleanup_.Record(cudaStreamDestroy(stream_), "destroy ONNX stream")) stream_ = nullptr;
            CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::StreamDestroyed);
        }
        return cleanup_.status();
    }

    void validate_model() const {
        if (session_->GetInputCount() != 1U || session_->GetOutputCount() != 1U) {
            throw std::runtime_error(std::string(descriptor().label) + " ONNX model must have exactly one input and one output");
        }
        Ort::AllocatorWithDefaultOptions allocator;
        const auto input_name = session_->GetInputNameAllocated(0U, allocator);
        const auto output_name = session_->GetOutputNameAllocated(0U, allocator);
        if (std::string_view(descriptor().input_name) != input_name.get() ||
            std::string_view(descriptor().output_name) != output_name.get()) {
            throw std::runtime_error(std::string(descriptor().label) + " ONNX tensor names do not match the upscaler contract");
        }
        const auto input_type = session_->GetInputTypeInfo(0U);
        const auto output_type = session_->GetOutputTypeInfo(0U);
        const auto input = input_type.GetTensorTypeAndShapeInfo();
        const auto output = output_type.GetTensorTypeAndShapeInfo();
        if (input.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            output.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error(std::string(descriptor().label) +
                                     " ONNX boundary tensor types do not match the FP32 upscaler contract: input=" +
                                     std::to_string(static_cast<int>(input.GetElementType())) +
                                     ", output=" + std::to_string(static_cast<int>(output.GetElementType())));
        }
        validate_shape(input.GetShape(), kInputShape, std::string(descriptor().label) + " ONNX input");
        validate_shape(output.GetShape(), kOutputShape, std::string(descriptor().label) + " ONNX output");
    }

    [[nodiscard]] bool submit_tiles(const ImageUpscalerRequest& request, const cudaStream_t consumer_stream,
                                    const std::uint32_t restored_width, const std::uint32_t restored_height) {
        if (!request.current()) return false;
        ensure_cuda_ok(cudaEventRecord(source_ready_, consumer_stream), "cudaEventRecord for ONNX upscaler source");
        ensure_cuda_ok(cudaStreamWaitEvent(stream_, source_ready_, 0U), "cudaStreamWaitEvent for ONNX upscaler source");
        const bool completed = submit_tiles_after_source(request, restored_width, restored_height);
        ensure_cuda_ok(cudaStreamWaitEvent(consumer_stream, completion_, 0U), "cudaStreamWaitEvent for ONNX upscaler completion");
        return completed;
    }

    [[nodiscard]] bool submit_tiles_after_source(const ImageUpscalerRequest& request, const std::uint32_t restored_width,
                                                 const std::uint32_t restored_height) {
        bool trace = false;
        mmltk::common::logging::trace([&](auto&) { trace = true; });
        const bool timed = trace && !timing_.pending.exchange(true, std::memory_order_acq_rel);
        if (timed) {
            timing_.admitted = std::chrono::steady_clock::now();
            timing_.device = device_id();
            timing_.model = descriptor().label;
            timing_.first_inference = !inference_submitted_;
            ++timing_.sequence;
        }
        const std::uint32_t core_extent = kImageUpscalerInputExtent - descriptor().halo * 2U;
        for (std::uint32_t y = 0U; y < request.crop_height; y += core_extent) {
            for (std::uint32_t x = 0U; x < request.crop_width; x += core_extent) {
                if (!request.current()) {
                    if (timed) timing_.pending.store(false, std::memory_order_release);
                    ensure_cuda_ok(cudaEventRecord(completion_, stream_), "complete cancelled ONNX tiles");
                    return false;
                }
                const image_upscaler_cuda::Tile tile = image_upscaler_cuda::prepare_request_tile(
                    request, x, y, core_extent, descriptor().kind, descriptor().halo, input_.data(), stream_);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch ONNX upscaler tile preparation");
                Checkpoint(ImageUpscalerExecutionStage::TilePrepared);
                if (!request.current()) {
                    if (timed) timing_.pending.store(false, std::memory_order_release);
                    ensure_cuda_ok(cudaEventRecord(completion_, stream_), "complete cancelled ONNX preparation");
                    return false;
                }
                // The fixed tensors and all preparation/stitching use this owned
                // stream. ORT replays its captured tile only after the new input
                // is ready; no tensor is resized or rebound during its lifetime.
                if (run_tile(request.current) == ImageUpscalerOutcome::Cancelled || !request.current()) {
                    if (timed) timing_.pending.store(false, std::memory_order_release);
                    ensure_cuda_ok(cudaEventRecord(completion_, stream_), "complete cancelled ONNX inference");
                    return false;
                }
                image_upscaler_cuda::stitch_request_tile(output_.data(), tile, descriptor().kind, descriptor().halo, restored_pixels(),
                                                         restored_width, restored_height, stream_);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch ONNX upscaler tile composition");
            }
        }
        if (timed) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - timing_.admitted).count();
            mmltk::common::logging::trace([&](auto& logger) {
                logger.trace(
                    "event=image_upscaler_onnx_tiles_submitted device={} model={} sequence={} first_inference={} graph={} width={} "
                    "height={} tiles={} host_enqueue_us={} restored={} restored_bytes={} input={} output={} binding={}",
                    device_id(), descriptor().label, timing_.sequence, timing_.first_inference, graph_enabled_, request.crop_width,
                    request.crop_height,
                    ((request.crop_width + core_extent - 1U) / core_extent) * ((request.crop_height + core_extent - 1U) / core_extent),
                    elapsed, reinterpret_cast<std::uintptr_t>(restored_pixels()),
                    static_cast<std::size_t>(restored_width) * restored_height * 3U * sizeof(float),
                    reinterpret_cast<std::uintptr_t>(input_.data()), reinterpret_cast<std::uintptr_t>(output_.data()),
                    reinterpret_cast<std::uintptr_t>(binding_.get()));
            });
            ensure_cuda_ok(cudaLaunchHostFunc(stream_, completed, &timing_), "enqueue ONNX upscaler completion diagnostic");
        }
        ensure_cuda_ok(cudaEventRecord(completion_, stream_), "cudaEventRecord for ONNX upscaler completion");
        return true;
    }

    std::filesystem::path model_path_;
    UpscalerCleanup cleanup_;
    std::size_t input_elements_ = 0U;
    std::size_t output_elements_ = 0U;
    cudaStream_t stream_ = nullptr;
    cudaGraph_t abandoned_capture_ = nullptr;
    cudaEvent_t source_ready_ = nullptr;
    cudaEvent_t completion_ = nullptr;
    UpscalerFloatBuffer input_;
    UpscalerFloatBuffer output_;
    Ort::RunOptions run_options_;
    bool graph_enabled_ = false;
    bool inference_submitted_ = false;
    std::uint32_t inference_runs_ = 0U;
    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<Ort::MemoryInfo> device_memory_;
    std::unique_ptr<Ort::Value> input_value_;
    std::unique_ptr<Ort::Value> output_value_;
    std::unique_ptr<Ort::IoBinding> binding_;
};

}  // namespace

std::shared_ptr<ImageUpscalerRuntime> make_onnx_upscaler_runtime(const ImageUpscalerDescriptor& descriptor,
    const std::filesystem::path& model_path, const int device_id, const ImageUpscalerExecutionCheckpoint& checkpoint) {
    // The environment must precede every ORT object, including member RunOptions.
    static_cast<void>(upscaler_ort_environment());
    return std::make_shared<OnnxImageUpscalerRuntime>(descriptor, model_path, device_id, checkpoint);
}

}  // namespace mmltk::backend::imaging::upscale
