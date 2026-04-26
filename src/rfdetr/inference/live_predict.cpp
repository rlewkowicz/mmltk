#include "mmltk/rfdetr/live_predict.h"

#include "mmltk/live/frame_analyzer.h"
#include "mmltk/rfdetr/draw_cuda.h"
#include "mmltk/rfdetr/model.h"

#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/postprocess.h"
#include "rfdetr/predict_runtime_internal.h"
#include "rfdetr/validate_internal.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

namespace {

using Clock = std::chrono::steady_clock;

using predict_internal::describe_backend_execution;
using predict_internal::LiveFrameInputs;
using predict_internal::LiveFrameRenderData;
using predict_internal::LiveRunnerState;
using predict_internal::LiveSplitRenderData;
using predict_internal::destroy_cuda_event;
using predict_internal::live_output_tensors;
using predict_internal::make_live_runner_state;
using predict_internal::run_live_frame_pipeline;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

inline std::int64_t live_query_count(const ResolvedModelArtifacts& artifacts) {
    return artifacts.config.num_select > 0 ? artifacts.config.num_select : artifacts.config.num_queries;
}

struct LiveWeightsRunnerSetup {
    std::shared_ptr<NativeRfDetrModel> model;
    LiveRunnerState state;
    torch::Tensor nested_mask;
};

LiveWeightsRunnerSetup make_live_weights_runner_setup(const ResolvedModelArtifacts& artifacts,
                                                      const LivePredictOptions& options) {
    auto model = predict_internal::load_native_model(artifacts, options.device_id);
    auto state = make_live_runner_state(artifacts, options.device_id, get_high_priority_cuda_stream(options.device_id),
                                        "cudaEventCreateWithFlags for live weights ready event");
    const auto resolution = static_cast<std::int64_t>(artifacts.config.resolution);
    model->optimize_for_inference(1, false, options.compilation_mode);
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    c10::cuda::CUDAStreamGuard stream_guard(state.stream);
    auto nested_mask = torch::zeros({1, resolution, resolution},
                                    torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, options.device_id));
    return LiveWeightsRunnerSetup{std::move(model), std::move(state), std::move(nested_mask)};
}

struct LiveBackendRunnerSetup {
    std::unique_ptr<InferenceBackend> backend;
    LiveRunnerState state;
    std::int64_t num_queries = 300;
    std::size_t category_count = 0;
};

LiveBackendRunnerSetup make_live_backend_runner_setup(const ResolvedModelArtifacts& artifacts,
                                                      const LivePredictOptions& options,
                                                      const std::string& backend_name) {
    auto backend = predict_internal::make_backend(artifacts, backend_name, options.device_id, options.allow_fp16);
    const predict_internal::BackendExecutionSpec spec =
        describe_backend_execution(*backend, artifacts, options.device_id);
    auto state = make_live_runner_state(artifacts, options.device_id, spec.stream,
                                        "cudaEventCreateWithFlags for live backend ready event");
    return LiveBackendRunnerSetup{std::move(backend), std::move(state), spec.num_queries, spec.category_count};
}

template <typename RunModelFn>
LiveFrameRenderData run_live_runner_frame(const LiveFrameInputs& frame, const std::uint32_t split_count,
                                          const std::size_t max_dets_per_image, const float threshold,
                                          const ResolvedModelArtifacts& artifacts, LiveRunnerState& state,
                                          const int device_id, const bool include_masks,
                                          const bool include_status_detections, const std::size_t category_count,
                                          const int num_classes, const std::int64_t max_queries, RunModelFn&& run_model,
                                          const char* runtime_label) {
    return run_live_frame_pipeline(frame, split_count, max_dets_per_image, threshold, category_count, num_classes,
                                   artifacts.config.resolution, state.stream, device_id, state.ready_event,
                                   state.input_gpu, state.mean, state.std, include_masks, include_status_detections,
                                   max_queries, std::forward<RunModelFn>(run_model), runtime_label);
}

class LiveModelRunner {
   public:
    virtual ~LiveModelRunner() = default;

    [[nodiscard]] virtual LiveFrameRenderData run_frame(const LiveFrameInputs& frame, std::uint32_t split_count,
                                                        std::size_t max_dets_per_image, float threshold) = 0;
};

class LiveConfiguredRunner : public LiveModelRunner {
   public:
    ~LiveConfiguredRunner() override {
        destroy_cuda_event(state_.ready_event);
    }

   protected:
    LiveConfiguredRunner(ResolvedModelArtifacts artifacts, const LivePredictOptions& options, LiveRunnerState state)
        : artifacts_(std::move(artifacts)),
          state_(std::move(state)),
          device_id_(options.device_id),
          include_masks_(options.include_masks),
          include_status_detections_(options.include_status_detections) {}

    [[nodiscard]] LiveFrameRenderData run_frame(const LiveFrameInputs& frame, const std::uint32_t split_count,
                                                const std::size_t max_dets_per_image, const float threshold) override {
        return run_live_runner_frame(
            frame, split_count, max_dets_per_image, threshold, artifacts_, state_, device_id_, include_masks_,
            include_status_detections_, category_count(), num_classes(), max_queries(),
            [this]() { return run_model_outputs(); }, runtime_label());
    }

    [[nodiscard]] virtual OutputTensors run_model_outputs() = 0;
    [[nodiscard]] virtual std::size_t category_count() const = 0;
    [[nodiscard]] virtual int num_classes() const = 0;
    [[nodiscard]] virtual std::int64_t max_queries() const = 0;
    [[nodiscard]] virtual const char* runtime_label() const = 0;

    ResolvedModelArtifacts artifacts_;
    LiveRunnerState state_;
    int device_id_ = 0;
    bool include_masks_ = false;
    bool include_status_detections_ = false;
};

class LiveWeightsRunner final : public LiveConfiguredRunner {
   public:
    LiveWeightsRunner(const ResolvedModelArtifacts& artifacts, const LivePredictOptions& options)
        : LiveWeightsRunner(artifacts, options, make_live_weights_runner_setup(artifacts, options)) {}

   private:
    LiveWeightsRunner(ResolvedModelArtifacts artifacts, const LivePredictOptions& options, LiveWeightsRunnerSetup setup)
        : LiveConfiguredRunner(std::move(artifacts), options, std::move(setup.state)),
          model_(std::move(setup.model)),
          nested_mask_(std::move(setup.nested_mask)) {}

    [[nodiscard]] OutputTensors run_model_outputs() override {
        return live_output_tensors(
            model_->forward(NestedTensor{state_.input_gpu, nested_mask_}, nullptr, include_masks_), include_masks_);
    }
    [[nodiscard]] std::size_t category_count() const override {
        return static_cast<std::size_t>(artifacts_.config.num_classes);
    }
    [[nodiscard]] int num_classes() const override {
        return artifacts_.config.num_classes;
    }
    [[nodiscard]] std::int64_t max_queries() const override {
        return live_query_count(artifacts_);
    }
    [[nodiscard]] const char* runtime_label() const override {
        return "weights";
    }

   private:
    std::shared_ptr<NativeRfDetrModel> model_;
    torch::Tensor nested_mask_;
};

class LiveBackendRunner final : public LiveConfiguredRunner {
   public:
    LiveBackendRunner(const ResolvedModelArtifacts& artifacts, const LivePredictOptions& options,
                      const std::string& backend_name)
        : LiveBackendRunner(artifacts, options, make_live_backend_runner_setup(artifacts, options, backend_name)) {}

   private:
    LiveBackendRunner(ResolvedModelArtifacts artifacts, const LivePredictOptions& options, LiveBackendRunnerSetup setup)
        : LiveConfiguredRunner(std::move(artifacts), options, std::move(setup.state)),
          backend_(std::move(setup.backend)),
          num_queries_(setup.num_queries),
          category_count_(setup.category_count) {}

    [[nodiscard]] OutputTensors run_model_outputs() override {
        return backend_->run(state_.input_gpu);
    }
    [[nodiscard]] std::size_t category_count() const override {
        return category_count_;
    }
    [[nodiscard]] int num_classes() const override {
        return static_cast<int>(category_count_);
    }
    [[nodiscard]] std::int64_t max_queries() const override {
        return num_queries_;
    }
    [[nodiscard]] const char* runtime_label() const override {
        return "backend";
    }

   private:
    std::unique_ptr<InferenceBackend> backend_;
    std::int64_t num_queries_ = 300;
    std::size_t category_count_ = 0;
};

class RfDetrLiveFrameAnalyzer final : public mmltk::live::FrameAnalyzer {
   public:
    explicit RfDetrLiveFrameAnalyzer(LivePredictOptions options)
        : options_(std::move(options)),
          artifacts_(resolve_model_artifacts(options_)),
          backend_name_(predict_internal::choose_backend_name(options_.backend, artifacts_)) {
        if (options_.split_count == 0U) {
            throw std::runtime_error("RF-DETR live analyzer requires split_count >= 1");
        }
        LivePredictOptions runner_options = options_;
        runner_options.include_status_detections = true;
        if (backend_name_ == "weights") {
            runner_ = std::make_unique<LiveWeightsRunner>(artifacts_, runner_options);
        } else {
            runner_ = std::make_unique<LiveBackendRunner>(artifacts_, runner_options, backend_name_);
        }
    }

    [[nodiscard]] mmltk::live::AnalyzerResult analyze(const mmltk::live::DetectBundle& bundle) override {
        const LiveFrameInputs frame{
            bundle.frame_id.sequence,
            bundle.capture_ns,
            bundle.ready_ns,
            bundle.ready_event,
            bundle.short_frame,
            static_cast<const std::uint8_t*>(mmltk::live::device_ptr_as_const_void(bundle.data)),
            bundle.dims.pitch_bytes,
            bundle.region,
        };
        LiveFrameRenderData render =
            runner_->run_frame(frame, options_.split_count, options_.max_dets_per_image, options_.threshold);

        mmltk::live::AnalyzerResult result;
        result.frame_id = bundle.frame_id;
        result.capture_ns = bundle.capture_ns;
        result.ready_ns = now_ns();
        result.ready_event = render.ready_event;
        result.producer_stream = render.producer_stream;
        result.splits.reserve(render.splits.size());
        for (LiveSplitRenderData& split : render.splits) {
            mmltk::live::AnalyzerSplitResult converted;
            converted.source_region = split.source_region;
            converted.boxes_xyxy = std::move(split.boxes_xyxy);
            converted.labels_zero_based = std::move(split.labels_zero_based);
            converted.scores = std::move(split.scores);
            converted.colors_rgb = std::move(split.colors_rgb);
            converted.masks = std::move(split.masks);
            converted.detections = std::move(split.detections);
            result.splits.push_back(std::move(converted));
        }
        return result;
    }

    [[nodiscard]] std::string backend_name() const override {
        return backend_name_;
    }

    [[nodiscard]] std::uint32_t model_resolution() const override {
        return artifacts_.config.resolution;
    }

    [[nodiscard]] int num_classes() const override {
        return artifacts_.config.num_classes;
    }

   private:
    LivePredictOptions options_{};
    ResolvedModelArtifacts artifacts_{};
    std::string backend_name_;
    std::unique_ptr<LiveModelRunner> runner_;
};

}  // namespace

bool live_capture_supported() {
#if MMLTK_RFDETR_LIVE_CAPTURE
    return true;
#else
    return false;
#endif
}

std::unique_ptr<mmltk::live::FrameAnalyzer> make_live_rfdetr_frame_analyzer(const LivePredictOptions& options) {
    return std::make_unique<RfDetrLiveFrameAnalyzer>(options);
}

}  // namespace mmltk::rfdetr
