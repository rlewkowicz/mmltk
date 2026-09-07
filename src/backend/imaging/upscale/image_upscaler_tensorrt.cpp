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

module mmltk.backend.imaging.upscale.image_upscaler;

import mmltk.backend.ml.cuda.gpu_quiescence;

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
        if (graph_exec != nullptr) { (void)cudaGraphExecDestroy(graph_exec); }
        if (graph != nullptr) { (void)cudaGraphDestroy(graph); }
        if (completion != nullptr) { (void)cudaEventDestroy(completion); }
        if (stream != nullptr) { (void)cudaStreamDestroy(stream); }
    }
};

[[nodiscard]] bool shape_matches(const nvinfer1::Dims& actual, const std::array<std::int64_t, 4U>& expected) {
    if (actual.nbDims != static_cast<std::int32_t>(expected.size())) { return false; }
    for (std::int32_t axis = 0; axis < actual.nbDims; ++axis) {
        if (actual.d[axis] > 0 && actual.d[axis] != expected[static_cast<std::size_t>(axis)]) { return false; }
    }
    return true;
}

class TensorRtImageUpscalerRuntime final
    : public TiledImageUpscalerRuntimeAdapter<TensorRtImageUpscalerRuntime, ImageUpscalerBackend::TensorRt> {
   public:
    TensorRtImageUpscalerRuntime(const ImageUpscalerDescriptor& descriptor, std::unique_ptr<TensorRtEngine> engine, const int device_id)
        : TiledImageUpscalerRuntimeAdapter(descriptor, device_id), engine_(std::move(engine)) {
        if (!descriptor.tensor_rt_enabled) { throw std::invalid_argument(std::string(descriptor.label) + " does not permit TensorRT"); }
        if (engine_ == nullptr) { throw std::invalid_argument("TensorRT Image upscaler requires an engine"); }
        ensure_cuda_ok(cudaSetDevice(device_id), "cudaSetDevice for TensorRT Image upscaler");
        validate_engine();
        try {
            ensure_cuda_ok(cudaEventCreateWithFlags(&source_ready_, cudaEventDisableTiming),
                           "cudaEventCreate for TensorRT upscaler source");
            for (TensorRtTileLane& lane : lanes_) {
                initialize_lane(lane);
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
                    device_id, descriptor.label, descriptor.sha256, kLaneCount, context_bytes, context_bytes * kLaneCount,
                    lane_buffer_bytes, lane_buffer_bytes * kLaneCount);
            });
        } catch (...) {
            if (source_ready_ != nullptr) {
                (void)cudaEventDestroy(source_ready_);
                source_ready_ = nullptr;
            }
            throw;
        }
    }

    ~TensorRtImageUpscalerRuntime() override { static_cast<void>(ReleaseBackend()); }

   private:
    friend class TiledImageUpscalerRuntimeAdapter<TensorRtImageUpscalerRuntime, ImageUpscalerBackend::TensorRt>;

    [[nodiscard]] cudaError_t ReleaseBackend() noexcept {
        cudaError_t failure = cudaSuccess;
        for (TensorRtTileLane& lane : lanes_) {
            if (lane.stream != nullptr) {
                failure = cudaStreamSynchronize(lane.stream);
                if (failure != cudaSuccess) return failure;
            }
        }
        for (TensorRtTileLane& lane : lanes_) {
            lane.context.reset();
            if (lane.graph_exec != nullptr) {
                failure = cudaGraphExecDestroy(lane.graph_exec);
                if (failure != cudaSuccess) return failure;
                lane.graph_exec = nullptr;
            }
            if (lane.graph != nullptr) {
                failure = cudaGraphDestroy(lane.graph);
                if (failure != cudaSuccess) return failure;
                lane.graph = nullptr;
            }
            if (lane.completion != nullptr) {
                failure = cudaEventDestroy(lane.completion);
                if (failure != cudaSuccess) return failure;
                lane.completion = nullptr;
            }
            failure = lane.input.Release();
            if (failure != cudaSuccess) return failure;
            failure = lane.output.Release();
            if (failure != cudaSuccess) return failure;
            if (lane.stream != nullptr) {
                failure = cudaStreamDestroy(lane.stream);
                if (failure != cudaSuccess) return failure;
                lane.stream = nullptr;
            }
            lane.submitted = false;
        }
        if (source_ready_ != nullptr) {
            failure = cudaEventDestroy(source_ready_);
            if (failure != cudaSuccess) return failure;
            source_ready_ = nullptr;
        }
        engine_.reset();
        return cudaSuccess;
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
            throw std::runtime_error(std::string(descriptor().label) + " TensorRT engine does not match the fixed FP32 boundary contract");
        }
    }

    void initialize_lane(TensorRtTileLane& lane) {
        lane.context = make_tensor_rt_context(native_engine(*engine_));
        if (lane.context == nullptr) {
            throw std::runtime_error(std::string("failed to create ") + std::string(descriptor().label) + " TensorRT execution context");
        }
        lane.input.ensure(checked_upscaler_elements(kImageUpscalerInputExtent, kImageUpscalerInputExtent, 3U),
                          "cudaMalloc for TensorRT upscaler input");
        lane.output.ensure(checked_upscaler_elements(kImageUpscalerOutputExtent, kImageUpscalerOutputExtent, 3U),
                           "cudaMalloc for TensorRT upscaler output");
        ensure_cuda_ok(cudaStreamCreateWithFlags(&lane.stream, cudaStreamNonBlocking), "cudaStreamCreate for TensorRT upscaler lane");
        ensure_cuda_ok(cudaEventCreateWithFlags(&lane.completion, cudaEventDisableTiming), "cudaEventCreate for TensorRT upscaler lane");

        const nvinfer1::Dims4 input_shape{1, 3, static_cast<std::int32_t>(kImageUpscalerInputExtent),
                                          static_cast<std::int32_t>(kImageUpscalerInputExtent)};
        if (!lane.context->setInputShape(descriptor().input_name, input_shape) ||
            !lane.context->setInputTensorAddress(descriptor().input_name, lane.input.data())) {
            throw std::runtime_error(std::string("failed to configure ") + std::string(descriptor().label) + " TensorRT input");
        }
        const std::int32_t missing = lane.context->inferShapes(0, nullptr);
        if (missing != 0) {
            throw std::runtime_error(std::string(descriptor().label) + " TensorRT could not resolve the fixed tile shapes");
        }
        constexpr std::array<std::int64_t, 4U> output_shape{1, 3, kImageUpscalerOutputExtent, kImageUpscalerOutputExtent};
        if (!shape_matches(lane.context->getTensorShape(descriptor().output_name), output_shape) ||
            !lane.context->setOutputTensorAddress(descriptor().output_name, lane.output.data())) {
            throw std::runtime_error(std::string("failed to configure ") + std::string(descriptor().label) + " TensorRT output");
        }
        if (cudaStreamBeginCapture(lane.stream, cudaStreamCaptureModeThreadLocal) == cudaSuccess) {
            const bool captured = lane.context->enqueueV3(lane.stream);
            const cudaError_t ended = cudaStreamEndCapture(lane.stream, &lane.graph);
            if (captured && ended == cudaSuccess && lane.graph != nullptr &&
                cudaGraphInstantiate(&lane.graph_exec, lane.graph, 0U) == cudaSuccess) {
                return;
            }
        }
        (void)cudaGetLastError();
        if (lane.graph != nullptr) {
            (void)cudaGraphDestroy(lane.graph);
            lane.graph = nullptr;
        }
        lane.graph_exec = nullptr;
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

    [[nodiscard]] bool submit_tiles(const ImageUpscalerRequest& request, const cudaStream_t consumer_stream,
                                    const std::uint32_t restored_width, const std::uint32_t restored_height) {
        ensure_cuda_ok(cudaEventRecord(source_ready_, consumer_stream), "cudaEventRecord for TensorRT upscaler source");
        wait_lanes_for_source();
        if (!submit_tiles_after_source(request, restored_width, restored_height)) { return false; }
        publish_lanes(consumer_stream);
        return true;
    }

    [[nodiscard]] bool submit_tiles_after_source(const ImageUpscalerRequest& request, const std::uint32_t restored_width,
                                                 const std::uint32_t restored_height) {
        const std::uint32_t core_extent = kImageUpscalerInputExtent - descriptor().halo * 2U;
        std::size_t dispatch = 0U;
        for (std::uint32_t y = 0U; y < request.crop_height; y += core_extent) {
            for (std::uint32_t x = 0U; x < request.crop_width; x += core_extent) {
                TensorRtTileLane& lane = lanes_[dispatch % lanes_.size()];
                await_lane(lane);
                const image_upscaler_cuda::Tile tile = image_upscaler_cuda::prepare_request_tile(
                    request, x, y, core_extent, descriptor().kind, descriptor().halo, lane.input.data(), lane.stream);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch TensorRT upscaler tile preparation");
                if (lane.graph_exec != nullptr) {
                    ensure_cuda_ok(cudaGraphLaunch(lane.graph_exec, lane.stream), "cudaGraphLaunch for TensorRT upscaler tile");
                } else if (!lane.context->enqueueV3(lane.stream)) {
                    throw std::runtime_error(std::string(descriptor().label) + " TensorRT enqueue failed");
                }
                image_upscaler_cuda::stitch_request_tile(lane.output.data(), tile, descriptor().kind, descriptor().halo, restored_pixels(),
                                                         restored_width, restored_height, lane.stream);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch TensorRT upscaler tile composition");
                ensure_cuda_ok(cudaEventRecord(lane.completion, lane.stream), "cudaEventRecord for TensorRT upscaler tile");
                lane.submitted = true;
                ++dispatch;
            }
        }
        return true;
    }

    std::unique_ptr<TensorRtEngine> engine_;
    std::array<TensorRtTileLane, kLaneCount> lanes_;
    cudaEvent_t source_ready_ = nullptr;
};

}  // namespace

std::shared_ptr<ImageUpscalerRuntime> make_tensorrt_upscaler_runtime(const ImageUpscalerDescriptor& descriptor,
                                                                     std::unique_ptr<TensorRtEngine> engine, const int device_id) {
    return std::make_shared<TensorRtImageUpscalerRuntime>(descriptor, std::move(engine), device_id);
}

}  // namespace mmltk::backend::imaging::upscale
