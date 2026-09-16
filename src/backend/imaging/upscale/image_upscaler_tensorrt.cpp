// CLEANUP-IGNORE: The TensorRT backend declares its own vendor global fragment and imports for this independent module
// implementation.
module;
#include <NvInfer.h>
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
constexpr std::size_t kLaneCount = 2U;
[[nodiscard]] nvinfer1::ICudaEngine& native_engine(const TensorRtEngine& owner) noexcept {
    return *reinterpret_cast<nvinfer1::ICudaEngine*>(owner.native_engine_handle());
}
class TensorRtExecutionContextDestroy final {
   public:
    void operator()(nvinfer1::IExecutionContext* pointer) const noexcept { delete pointer; }
};
using TensorRtExecutionContext = std::unique_ptr<nvinfer1::IExecutionContext, TensorRtExecutionContextDestroy>;
[[nodiscard]] TensorRtExecutionContext make_tensor_rt_context(nvinfer1::ICudaEngine& engine) {
    return TensorRtExecutionContext{engine.createExecutionContext()};
}
struct TensorRtTileLane {
    TensorRtExecutionContext context;
    UpscalerFloatBuffer input;
    UpscalerFloatBuffer output;
    cudaStream_t stream = nullptr;
    cudaEvent_t completion = nullptr;
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t graph_exec = nullptr;
    bool submitted = false;
    TensorRtTileLane() = default;
    TensorRtTileLane(const TensorRtTileLane&) = delete;
    TensorRtTileLane& operator=(const TensorRtTileLane&) = delete;
    ~TensorRtTileLane() {
        if (context || graph_exec != nullptr || graph != nullptr || completion != nullptr || stream != nullptr) std::terminate();
    }
};
[[nodiscard]] bool shape_matches(const nvinfer1::Dims& actual, const std::array<std::int64_t, 4U>& expected) {
    if (actual.nbDims != static_cast<std::int32_t>(expected.size())) { return false; }
    for (std::int32_t axis = 0; axis < actual.nbDims; ++axis) {
        if (actual.d[axis] > 0 && actual.d[axis] != expected[static_cast<std::size_t>(axis)]) { return false; }
    }
    return true;
}
class TensorRtImageUpscalerRuntime final : public TiledImageUpscalerRuntimeAdapter<TensorRtImageUpscalerRuntime, ImageUpscalerBackend::TensorRt> {
   public:
    [[nodiscard]] bool graph_replay() const noexcept override {
        return std::ranges::all_of(lanes_, [](const auto& lane) { return lane.graph_exec != nullptr; });
    }
    TensorRtImageUpscalerRuntime(const ImageUpscalerDescriptor& descriptor, std::unique_ptr<TensorRtEngine> engine, const int device_id,
                                 const ImageUpscalerExecutionCheckpoint& checkpoint)
        : TiledImageUpscalerRuntimeAdapter(descriptor, device_id, checkpoint), engine_(std::move(engine)) {
        if (!descriptor.tensor_rt_enabled) { throw std::invalid_argument(std::string(descriptor.label) + " does not permit TensorRT"); }
        if (engine_ == nullptr) { throw std::invalid_argument("TensorRT Image upscaler requires an engine"); }
        validate_engine();
    }
    ImageUpscalerOutcome ActivateBackend(ImageUpscalerCurrent current) {
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        const auto& descriptor = this->descriptor();
        const auto device_id = this->device_id();
        ensure_cuda_ok(cudaSetDevice(device_id), "cudaSetDevice for TensorRT Image upscaler");
        ensure_cuda_ok(cudaEventCreateWithFlags(&source_ready_, cudaEventDisableTiming), "cudaEventCreate for TensorRT upscaler source");
        Checkpoint(ImageUpscalerExecutionStage::EventCreated);
        for (TensorRtTileLane& lane : lanes_) {
            if (!current() || initialize_lane(lane, current) == ImageUpscalerOutcome::Cancelled) return ImageUpscalerOutcome::Cancelled;
        }
        mmltk::common::logging::trace([&](auto& logger) {
            const std::uint64_t context_bytes = native_engine(*engine_).getDeviceMemorySizeV2();
            const std::uint64_t lane_buffer_bytes =
                static_cast<std::uint64_t>(checked_upscaler_elements(kImageUpscalerInputExtent, kImageUpscalerInputExtent, 3U) +
                                           checked_upscaler_elements(kImageUpscalerOutputExtent, kImageUpscalerOutputExtent, 3U)) *
                sizeof(float);
            logger.trace(
                "event=image_upscaler_tensorrt_runtime_allocated device={} model={} model_sha256={} lane_count={} "
                "context_device_memory_bytes={} total_context_device_memory_bytes={} lane_buffer_bytes={} "
                "total_lane_buffer_bytes={}",
                device_id, descriptor.label, descriptor.sha256, kLaneCount, context_bytes, context_bytes * kLaneCount, lane_buffer_bytes,
                lane_buffer_bytes * kLaneCount);
        });
        return ImageUpscalerOutcome::Completed;
    }
    ~TensorRtImageUpscalerRuntime() override {
        if (source_ready_ != nullptr || engine_) std::terminate();
    }

   private:
    friend class TiledImageUpscalerRuntimeAdapter<TensorRtImageUpscalerRuntime, ImageUpscalerBackend::TensorRt>;
    [[nodiscard]] cudaError_t SettleBackend() noexcept {
        // Activation is sequential. The newest initialized lane is the only
        // lane that can still own thread-local capture, which must end before
        // CUDA permits synchronization of any earlier lane.
        for (auto lane = lanes_.rbegin(); lane != lanes_.rend(); ++lane)
            cleanup_.Record(settle_upscaler_stream(lane->stream, lane->graph), "settle TensorRT lane");
        return cleanup_.status();
    }
    [[nodiscard]] cudaError_t ReleaseBackend() noexcept {
        bool settled = true;
        for (TensorRtTileLane& lane : lanes_) {
            if (lane.stream != nullptr && !cleanup_.Record(settle_upscaler_stream(lane.stream, lane.graph), "settle TensorRT lane")) {
                settled = false;
                continue;
            }
            if (lane.graph_exec != nullptr) {
                if (cleanup_.Record(cudaGraphExecDestroy(lane.graph_exec), "destroy TensorRT graph executable")) lane.graph_exec = nullptr;
                CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::GraphExecutableDestroyed);
            }
            if (lane.graph != nullptr) {
                if (cleanup_.Record(cudaGraphDestroy(lane.graph), "destroy TensorRT graph")) lane.graph = nullptr;
                CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::GraphDestroyed);
            }
            if (lane.completion != nullptr) {
                if (cleanup_.Record(cudaEventDestroy(lane.completion), "destroy TensorRT completion")) lane.completion = nullptr;
                CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::EventDestroyed);
            }
            // A retained executable still owns references to the context and
            // bound buffers, even though its stream has settled.
            if (lane.graph_exec == nullptr && lane.graph == nullptr) {
                lane.context.reset();
                CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::ContextReleased);
                static_cast<void>(lane.input.Release(&cleanup_));
                CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::BufferReleased);
                static_cast<void>(lane.output.Release(&cleanup_));
                CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::BufferReleased);
            }
            if (lane.stream != nullptr) {
                if (cleanup_.Record(cudaStreamDestroy(lane.stream), "destroy TensorRT lane stream")) lane.stream = nullptr;
                CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::StreamDestroyed);
            }
            lane.submitted = false;
        }
        if (settled && source_ready_ != nullptr) {
            if (cleanup_.Record(cudaEventDestroy(source_ready_), "destroy TensorRT source fence")) source_ready_ = nullptr;
            CleanupCheckpoint(cleanup_, ImageUpscalerExecutionStage::EventDestroyed);
        }
        if (cleanup_.status() == cudaSuccess) engine_.reset();
        return cleanup_.status();
    }
    void validate_engine() const {
        constexpr std::array<std::int64_t, 4U> input_shape{1, 3, kImageUpscalerInputExtent, kImageUpscalerInputExtent};
        constexpr std::array<std::int64_t, 4U> output_shape{1, 3, kImageUpscalerOutputExtent, kImageUpscalerOutputExtent};
        nvinfer1::ICudaEngine& engine = native_engine(*engine_);
        if (engine.getNbIOTensors() != 2 || engine.getTensorIOMode(descriptor().input_name) != nvinfer1::TensorIOMode::kINPUT ||
            engine.getTensorIOMode(descriptor().output_name) != nvinfer1::TensorIOMode::kOUTPUT ||
            engine.getTensorDataType(descriptor().input_name) != nvinfer1::DataType::kFLOAT ||
            engine.getTensorDataType(descriptor().output_name) != nvinfer1::DataType::kFLOAT ||
            !shape_matches(engine.getTensorShape(descriptor().input_name), input_shape) ||
            !shape_matches(engine.getTensorShape(descriptor().output_name), output_shape)) {
            throw mmltk::backend::ml::runtime::TensorRtCacheIntegrityError(std::string(descriptor().label) +
                                                                           " TensorRT engine does not match the fixed FP32 boundary contract");
        }
    }
    ImageUpscalerOutcome initialize_lane(TensorRtTileLane& lane, ImageUpscalerCurrent current) {
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        lane.context = make_tensor_rt_context(native_engine(*engine_));
        engine_->CheckOperation(lane.context != nullptr, "create TensorRT upscaler execution context");
        Checkpoint(ImageUpscalerExecutionStage::ContextCreated);
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        lane.input.ensure(checked_upscaler_elements(kImageUpscalerInputExtent, kImageUpscalerInputExtent, 3U), "cudaMalloc for TensorRT upscaler input");
        Checkpoint(ImageUpscalerExecutionStage::BuffersAllocated);
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        lane.output.ensure(checked_upscaler_elements(kImageUpscalerOutputExtent, kImageUpscalerOutputExtent, 3U), "cudaMalloc for TensorRT upscaler output");
        Checkpoint(ImageUpscalerExecutionStage::BuffersAllocated);
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        ensure_cuda_ok(cudaStreamCreateWithFlags(&lane.stream, cudaStreamNonBlocking), "cudaStreamCreate for TensorRT upscaler lane");
        Checkpoint(ImageUpscalerExecutionStage::StreamCreated);
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        ensure_cuda_ok(cudaEventCreateWithFlags(&lane.completion, cudaEventDisableTiming), "cudaEventCreate for TensorRT upscaler lane");
        Checkpoint(ImageUpscalerExecutionStage::EventCreated);
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        const nvinfer1::Dims4 input_shape{1, 3, static_cast<std::int32_t>(kImageUpscalerInputExtent), static_cast<std::int32_t>(kImageUpscalerInputExtent)};
        if (!lane.context->setInputShape(descriptor().input_name, input_shape) ||
            !lane.context->setInputTensorAddress(descriptor().input_name, lane.input.data())) {
            engine_->CheckOperation(false, "configure TensorRT upscaler input");
        }
        const std::int32_t missing = lane.context->inferShapes(0, nullptr);
        if (missing != 0) { engine_->CheckOperation(false, "resolve TensorRT upscaler fixed tile shapes"); }
        constexpr std::array<std::int64_t, 4U> output_shape{1, 3, kImageUpscalerOutputExtent, kImageUpscalerOutputExtent};
        if (!shape_matches(lane.context->getTensorShape(descriptor().output_name), output_shape) ||
            !lane.context->setOutputTensorAddress(descriptor().output_name, lane.output.data())) {
            engine_->CheckOperation(false, "configure TensorRT upscaler output");
        }
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        ensure_cuda_ok(cudaMemsetAsync(lane.input.data(), 0,
                                       checked_upscaler_elements(kImageUpscalerInputExtent, kImageUpscalerInputExtent, 3U) * sizeof(float), lane.stream),
                       "initialize TensorRT warm input");
        Checkpoint(ImageUpscalerExecutionStage::WarmInputSubmitted);
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        engine_->CheckOperation(lane.context->enqueueV3(lane.stream), "TensorRT first warm inference");
        Checkpoint(ImageUpscalerExecutionStage::WarmSubmitted);
        ensure_cuda_ok(cudaStreamSynchronize(lane.stream), "complete TensorRT first warm inference");
        Checkpoint(ImageUpscalerExecutionStage::WarmSettled);
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        const auto capture = cudaStreamBeginCapture(lane.stream, cudaStreamCaptureModeThreadLocal);
        if (capture == cudaSuccess) {
            Checkpoint(ImageUpscalerExecutionStage::CaptureBegan);
            if (!current()) return ImageUpscalerOutcome::Cancelled;
            const bool captured = lane.context->enqueueV3(lane.stream);
            Checkpoint(ImageUpscalerExecutionStage::CaptureSubmitted);
            const cudaError_t ended = cudaStreamEndCapture(lane.stream, &lane.graph);
            if (ended != cudaErrorStreamCaptureInvalidated) ensure_cuda_ok(ended, "finish TensorRT warm capture");
            Checkpoint(ImageUpscalerExecutionStage::CaptureEnded);
            engine_->CheckOperation(captured, "capture TensorRT warm inference");
            if (!current()) return ImageUpscalerOutcome::Cancelled;
            if (ended == cudaSuccess && lane.graph != nullptr) {
                ensure_cuda_ok(cudaGraphInstantiate(&lane.graph_exec, lane.graph, 0U), "instantiate TensorRT warm graph");
                Checkpoint(ImageUpscalerExecutionStage::GraphInstantiated);
                if (!current()) return ImageUpscalerOutcome::Cancelled;
                ensure_cuda_ok(cudaGraphLaunch(lane.graph_exec, lane.stream), "replay TensorRT warm graph");
                Checkpoint(ImageUpscalerExecutionStage::ReplaySubmitted);
                ensure_cuda_ok(cudaStreamSynchronize(lane.stream), "complete TensorRT warm replay");
                Checkpoint(ImageUpscalerExecutionStage::ReplaySettled);
                // Settlement completes this lane. Demand gates the next lane,
                // not the already completed activation of the final lane.
                return ImageUpscalerOutcome::Completed;
            }
        } else if (capture != cudaErrorStreamCaptureUnsupported) {
            ensure_cuda_ok(capture, "begin TensorRT warm capture");
        }
        if (lane.graph != nullptr) {
            ensure_cuda_ok(cudaGraphDestroy(lane.graph), "destroy unavailable TensorRT warm graph");
            lane.graph = nullptr;
        }
        lane.graph_exec = nullptr;
        return ImageUpscalerOutcome::Completed;
    }
    void await_lane(TensorRtTileLane& lane) {
        if (!lane.submitted) { return; }
        ensure_cuda_ok(cudaStreamWaitEvent(lane.stream, lane.completion, 0U), "cudaStreamWaitEvent for TensorRT image upscaler lane reuse");
        lane.submitted = false;
    }
    void publish_lanes(const cudaStream_t consumer_stream) {
        cudaError_t first_error = cudaSuccess;
        for (TensorRtTileLane& lane : lanes_) {
            if (!lane.submitted) { continue; }
            const cudaError_t status = cudaStreamWaitEvent(consumer_stream, lane.completion, 0U);
            if (first_error == cudaSuccess && status != cudaSuccess) { first_error = status; }
        }
        ensure_cuda_ok(first_error, "cudaStreamWaitEvent for TensorRT upscaler completion");
    }
    void wait_lanes_for_source() {
        for (TensorRtTileLane& lane : lanes_) {
            ensure_cuda_ok(cudaStreamWaitEvent(lane.stream, source_ready_, 0U), "cudaStreamWaitEvent for TensorRT upscaler source");
        }
    }
    [[nodiscard]] bool submit_tiles(const ImageUpscalerRequest& request, const cudaStream_t consumer_stream, const std::uint32_t restored_width,
                                    const std::uint32_t restored_height) {
        if (!request.current()) return false;
        ensure_cuda_ok(cudaEventRecord(source_ready_, consumer_stream), "cudaEventRecord for TensorRT upscaler source");
        wait_lanes_for_source();
        const bool completed = submit_tiles_after_source(request, restored_width, restored_height);
        publish_lanes(consumer_stream);
        return completed;
    }
    [[nodiscard]] bool submit_tiles_after_source(const ImageUpscalerRequest& request, const std::uint32_t restored_width, const std::uint32_t restored_height) {
        const std::uint32_t core_extent = kImageUpscalerInputExtent - descriptor().halo * 2U;
        std::size_t dispatch = 0U;
        for (std::uint32_t y = 0U; y < request.crop_height; y += core_extent) {
            for (std::uint32_t x = 0U; x < request.crop_width; x += core_extent) {
                if (!request.current()) return false;
                TensorRtTileLane& lane = lanes_[dispatch % lanes_.size()];
                await_lane(lane);
                const image_upscaler_cuda::Tile tile =
                    image_upscaler_cuda::prepare_request_tile(request, x, y, core_extent, descriptor().kind, descriptor().halo, lane.input.data(), lane.stream);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch TensorRT upscaler tile preparation");
                Checkpoint(ImageUpscalerExecutionStage::TilePrepared);
                if (!request.current()) return false;
                if (lane.graph_exec != nullptr) {
                    ensure_cuda_ok(cudaGraphLaunch(lane.graph_exec, lane.stream), "cudaGraphLaunch for TensorRT upscaler tile");
                } else if (!lane.context->enqueueV3(lane.stream)) {
                    engine_->CheckOperation(false, "TensorRT upscaler enqueue failed");
                }
                if (!request.current()) return false;
                image_upscaler_cuda::stitch_request_tile(lane.output.data(), tile, descriptor().kind, descriptor().halo, request, restored_width,
                                                         restored_height, lane.stream);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch TensorRT upscaler tile composition");
                ensure_cuda_ok(cudaEventRecord(lane.completion, lane.stream), "cudaEventRecord for TensorRT upscaler tile");
                lane.submitted = true;
                ++dispatch;
            }
        }
        return true;
    }
    std::unique_ptr<TensorRtEngine> engine_;
    UpscalerCleanup cleanup_;
    std::array<TensorRtTileLane, kLaneCount> lanes_;
    cudaEvent_t source_ready_ = nullptr;
};
}  // namespace
std::shared_ptr<ImageUpscalerRuntime> make_tensorrt_upscaler_runtime(const ImageUpscalerDescriptor& descriptor, std::unique_ptr<TensorRtEngine> engine,
                                                                     const int device_id, const ImageUpscalerExecutionCheckpoint& checkpoint) {
    return std::make_shared<TensorRtImageUpscalerRuntime>(descriptor, std::move(engine), device_id, checkpoint);
}
}  // namespace mmltk::backend::imaging::upscale
