#include "fastloader/rfdetr/live_predict.h"
#include "fastloader/rfdetr/draw_cuda.h"

#include "rfdetr/cuda_utils.h"
#include "rfdetr/live_preprocess.h"
#include "rfdetr/predict_runtime_internal.h"
#include "rfdetr/postprocess.h"
#include "rfdetr/validate_internal.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if FASTLOADER_RFDETR_LIVE_CAPTURE
#include <frameshow/capture_session.hpp>
#include <frameshow/capture_types.hpp>
#include <frameshow/status.hpp>
#endif

namespace fastloader::rfdetr {

namespace {

using Clock = std::chrono::steady_clock;

void log_live_predict_error_to_stderr(const std::string& message) {
    if (message.empty()) {
        return;
    }
    std::fprintf(stderr, "fastloader live predict error: %s\n", message.c_str());
    std::fflush(stderr);
}

struct SplitRegion {
    uint32_t x = 0;
    uint32_t width = 0;
};

std::vector<SplitRegion> build_horizontal_splits(uint32_t total_width, uint32_t requested_splits) {
    std::vector<SplitRegion> splits;
    if (total_width == 0U) {
        return splits;
    }

    const uint32_t split_count = std::max<uint32_t>(1U, std::min(requested_splits, total_width));
    splits.reserve(split_count);

    const uint32_t base_width = total_width / split_count;
    const uint32_t remainder = total_width % split_count;
    uint32_t x = 0;
    for (uint32_t split_index = 0; split_index < split_count; ++split_index) {
        const uint32_t width = base_width + (split_index < remainder ? 1U : 0U);
        if (width == 0U) {
            continue;
        }
        SplitRegion split;
        split.x = x;
        split.width = width;
        splits.push_back(split);
        x += width;
    }
    return splits;
}

int live_image_id(uint64_t frame_id) {
    return frame_id > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(frame_id);
}

std::vector<Prediction> filter_threshold(std::vector<Prediction>&& predictions, float threshold) {
    predictions.erase(
        std::remove_if(predictions.begin(),
                       predictions.end(),
                       [threshold](const Prediction& prediction) { return prediction.score < threshold; }),
        predictions.end());
    return std::move(predictions);
}

std::string format_live_split_context(uint64_t frame_id,
                                      uint32_t split_index,
                                      std::size_t src_pitch_bytes,
                                      uint32_t src_width,
                                      uint32_t src_height,
                                      uint32_t dst_width,
                                      uint32_t dst_height) {
    return "frame_id=" + std::to_string(frame_id) +
           " split_index=" + std::to_string(split_index) +
           " src=" + std::to_string(src_width) + "x" + std::to_string(src_height) +
           " src_pitch_bytes=" + std::to_string(src_pitch_bytes) +
           " dst=" + std::to_string(dst_width) + "x" + std::to_string(dst_height);
}

LiveCaptureRegion make_split_region(const LiveCaptureRegion& frame_region,
                                    const SplitRegion& split) {
    return LiveCaptureRegion{
        frame_region.x + split.x,
        frame_region.y,
        split.width,
        frame_region.height,
    };
}

struct LiveFrameInputs {
    uint64_t frame_id = 0;
    uint64_t capture_ns = 0;
    uint64_t ready_ns = 0;
    cudaEvent_t ready_event = nullptr;
    bool short_frame = false;
    const std::uint8_t* data = nullptr;
    std::size_t pitch_bytes = 0;
    LiveCaptureRegion region{};
};

struct LiveSplitRenderData {
    LiveSplitPrediction prediction;
    torch::Tensor boxes_xyxy;
    torch::Tensor labels_zero_based;
    torch::Tensor colors_rgb;
    torch::Tensor masks;
};

struct LiveFrameRenderData {
    std::vector<LiveSplitRenderData> splits;
    cudaEvent_t ready_event = nullptr;
    cudaStream_t producer_stream = nullptr;
};

OutputTensors live_output_tensors(const ModelOutputs& outputs, bool include_masks) {
    OutputTensors tensors = predict_internal::to_output_tensors(outputs);
    if (!include_masks) {
        tensors.pred_masks.reset();
    }
    return tensors;
}

class LiveModelRunner {
public:
    virtual ~LiveModelRunner() = default;

    virtual LiveFrameRenderData run_frame(const LiveFrameInputs& frame,
                                          uint32_t split_count,
                                          size_t max_dets_per_image,
                                          float threshold) = 0;
};

struct LiveOverlaySelection {
    torch::Tensor boxes_xyxy;
    torch::Tensor labels_zero_based;
    torch::Tensor colors_rgb;
    torch::Tensor masks;
};

LiveOverlaySelection select_live_overlay_for_preview(const TensorMap& result,
                                                     size_t category_count,
                                                     size_t max_dets_per_image,
                                                     float threshold,
                                                     int num_classes,
                                                     uint32_t split_width,
                                                     uint32_t split_height,
                                                     cudaStream_t stream) {
    const auto score_it = result.find("scores");
    const auto label_it = result.find("labels");
    const auto box_it = result.find("boxes");
    if (score_it == result.end() || label_it == result.end() || box_it == result.end()) {
        throw std::runtime_error("live preview overlay requires scores, labels, and boxes");
    }

    torch::Tensor scores = score_it->second.reshape({-1});
    torch::Tensor labels = label_it->second.reshape({-1}).to(torch::kInt64);
    torch::Tensor boxes = box_it->second;
    if (boxes.dim() != 2 || boxes.size(1) != 4) {
        throw std::runtime_error("live preview overlay requires boxes shaped [num_predictions,4]");
    }

    torch::Tensor valid = torch::logical_and(
        labels.ge(0),
        labels.lt(static_cast<int64_t>(category_count)));
    if (threshold > 0.0f) {
        valid = torch::logical_and(valid, scores.ge(threshold));
    }

    torch::Tensor indices = torch::nonzero(valid).flatten();
    const int64_t selected_count =
        std::min<int64_t>(indices.size(0), static_cast<int64_t>(max_dets_per_image));
    if (selected_count <= 0) {
        return {};
    }
    if (indices.size(0) > selected_count) {
        indices = indices.narrow(0, 0, selected_count).contiguous();
    }

    LiveOverlaySelection selection;
    selection.boxes_xyxy = boxes.index_select(0, indices).to(torch::kFloat32).contiguous();
    selection.boxes_xyxy.select(1, 0).clamp_(0.0, static_cast<double>(split_width));
    selection.boxes_xyxy.select(1, 1).clamp_(0.0, static_cast<double>(split_height));
    selection.boxes_xyxy.select(1, 2).clamp_(0.0, static_cast<double>(split_width));
    selection.boxes_xyxy.select(1, 3).clamp_(0.0, static_cast<double>(split_height));
    selection.labels_zero_based = labels.index_select(0, indices).to(torch::kInt32).contiguous();
    torch::Tensor valid_boxes = torch::logical_and(
        selection.boxes_xyxy.select(1, 2).gt(selection.boxes_xyxy.select(1, 0)),
        selection.boxes_xyxy.select(1, 3).gt(selection.boxes_xyxy.select(1, 1)));
    torch::Tensor valid_box_indices = torch::nonzero(valid_boxes).flatten();
    if (valid_box_indices.numel() == 0) {
        return {};
    }
    if (valid_box_indices.size(0) != selection.labels_zero_based.size(0)) {
        selection.boxes_xyxy = selection.boxes_xyxy.index_select(0, valid_box_indices).contiguous();
        selection.labels_zero_based = selection.labels_zero_based.index_select(0, valid_box_indices).contiguous();
        indices = indices.index_select(0, valid_box_indices).contiguous();
    }
    selection.colors_rgb = torch::empty(
        {selection.labels_zero_based.size(0), 3},
        torch::TensorOptions()
            .dtype(torch::kUInt8)
            .device(selection.labels_zero_based.device()));
    launch_build_instance_colors_from_zero_based_labels(
        selection.labels_zero_based.data_ptr<int>(),
        static_cast<std::size_t>(selection.labels_zero_based.size(0)),
        num_classes,
        selection.colors_rgb.data_ptr<std::uint8_t>(),
        stream);
    ensure_cuda_ok(cudaPeekAtLastError(), "live preview color generation");

    const auto mask_it = result.find("masks");
    if (mask_it != result.end()) {
        torch::Tensor masks = mask_it->second;
        if (masks.dim() == 4 && masks.size(1) == 1) {
            masks = masks.squeeze(1);
        }
        if (masks.dim() != 3) {
            throw std::runtime_error("live preview masks must be [num_predictions,height,width]");
        }
        selection.masks = masks.index_select(0, indices).to(torch::kBool).contiguous();
    }

    return selection;
}

class LiveWeightsRunner final : public LiveModelRunner {
public:
    LiveWeightsRunner(const ResolvedModelArtifacts& artifacts, const LivePredictOptions& options)
        : artifacts_(artifacts),
          model_(predict_internal::load_native_model(artifacts, options.device_id)),
          stream_(get_high_priority_cuda_stream(options.device_id)),
          device_id_(options.device_id),
          include_masks_(options.include_masks),
          include_status_detections_(options.include_status_detections) {
        const int64_t resolution = artifacts_.config.resolution;
        model_->optimize_for_inference(1, false, options.compilation_mode);
        {
            c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
            c10::cuda::CUDAStreamGuard stream_guard(stream_);
            auto normalization = predict_internal::initialize_normalization_tensors(device_id_, stream_);
            mean_ = std::move(normalization.first);
            std_ = std::move(normalization.second);
            input_gpu_ = torch::empty(
                {1, 3, resolution, resolution},
                torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device_id_));
            nested_mask_ = torch::zeros(
                {1, resolution, resolution},
                torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, device_id_));
            ensure_cuda_ok(cudaEventCreateWithFlags(&ready_event_, cudaEventDisableTiming),
                           "cudaEventCreateWithFlags for live weights ready event");
        }
    }

    ~LiveWeightsRunner() override {
        if (ready_event_ != nullptr) {
            cudaEventDestroy(ready_event_);
        }
    }

    LiveFrameRenderData run_frame(const LiveFrameInputs& frame,
                                  uint32_t split_count,
                                  size_t max_dets_per_image,
                                  float threshold) override {
        LiveFrameRenderData out;
        const std::vector<SplitRegion> splits = build_horizontal_splits(frame.region.width, split_count);
        if (splits.empty()) {
            throw std::runtime_error("RF-DETR live predict split list is empty");
        }

        const cudaStream_t cuda_stream = reinterpret_cast<cudaStream_t>(stream_.stream());
        ensure_cuda_ok(cudaStreamWaitEvent(cuda_stream, frame.ready_event, 0),
                       "cudaStreamWaitEvent for live weights frame");

        c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
        torch::NoGradGuard no_grad;
        out.splits.reserve(splits.size());
        for (size_t split_index = 0; split_index < splits.size(); ++split_index) {
            const SplitRegion split = splits[split_index];
            const LiveCaptureRegion split_region = make_split_region(frame.region, split);
            const auto* split_ptr = frame.data + static_cast<std::size_t>(split.x) * 3U;
            std::vector<TensorMap> outputs;
            LiveSplitRenderData split_render;
            split_render.prediction.split_index = static_cast<uint32_t>(split_index);
            split_render.prediction.source_region = split_region;
            {
                c10::cuda::CUDAStreamGuard stream_guard(stream_);
                if (const char* arg_error = validate_bgr_split_to_planar_float_args(
                        frame.pitch_bytes,
                        split.width,
                        frame.region.height,
                        static_cast<uint32_t>(artifacts_.config.resolution),
                        static_cast<uint32_t>(artifacts_.config.resolution),
                        cuda_stream)) {
                    throw std::runtime_error(
                        "RF-DETR live weights preprocess arguments are invalid: " +
                        format_live_split_context(frame.frame_id,
                                                  static_cast<uint32_t>(split_index),
                                                  frame.pitch_bytes,
                                                  split.width,
                                                  frame.region.height,
                                                  static_cast<uint32_t>(artifacts_.config.resolution),
                                                  static_cast<uint32_t>(artifacts_.config.resolution)) +
                        " reason=" + arg_error);
                }
                ensure_cuda_ok(
                    launch_bgr_split_to_planar_float(split_ptr,
                                                     frame.pitch_bytes,
                                                     split.width,
                                                     frame.region.height,
                                                     input_gpu_.data_ptr<float>(),
                                                     static_cast<uint32_t>(artifacts_.config.resolution),
                                                     static_cast<uint32_t>(artifacts_.config.resolution),
                                                     cuda_stream),
                    ("launch_bgr_split_to_planar_float for live weights frame: " +
                     format_live_split_context(frame.frame_id,
                                               static_cast<uint32_t>(split_index),
                                               frame.pitch_bytes,
                                               split.width,
                                               frame.region.height,
                                               static_cast<uint32_t>(artifacts_.config.resolution),
                                               static_cast<uint32_t>(artifacts_.config.resolution)))
                        .c_str());
                input_gpu_.sub_(mean_).div_(std_);
                const ModelOutputs raw_outputs = model_->forward(NestedTensor{input_gpu_, nested_mask_},
                                                                 nullptr,
                                                                 include_masks_);
                outputs = postprocess_outputs_fixed_size(
                    live_output_tensors(raw_outputs, include_masks_),
                    split_region.height,
                    split_region.width,
                    artifacts_.config.num_select > 0 ? artifacts_.config.num_select : artifacts_.config.num_queries);
                ensure_cuda_ok(cudaPeekAtLastError(), "live weights postprocess");
                if (include_status_detections_) {
                    const int image_id = live_image_id(frame.frame_id);
                    split_render.prediction.detections = filter_threshold(
                        result_to_predictions(
                            image_id,
                            outputs.front(),
                            static_cast<size_t>(artifacts_.config.num_classes),
                            max_dets_per_image),
                        threshold);
                }
                LiveOverlaySelection overlay = select_live_overlay_for_preview(
                    outputs.front(),
                    static_cast<size_t>(artifacts_.config.num_classes),
                    max_dets_per_image,
                    threshold,
                    artifacts_.config.num_classes,
                    split_region.width,
                    split_region.height,
                    cuda_stream);
                split_render.boxes_xyxy = std::move(overlay.boxes_xyxy);
                split_render.labels_zero_based = std::move(overlay.labels_zero_based);
                split_render.colors_rgb = std::move(overlay.colors_rgb);
                split_render.masks = std::move(overlay.masks);
            }
            out.splits.push_back(std::move(split_render));
        }
        ensure_cuda_ok(cudaEventRecord(ready_event_, cuda_stream),
                       "cudaEventRecord for live weights frame");
        out.ready_event = ready_event_;
        out.producer_stream = cuda_stream;
        return out;
    }

private:
    ResolvedModelArtifacts artifacts_;
    std::shared_ptr<NativeRfDetrModel> model_;
    c10::cuda::CUDAStream stream_;
    int device_id_ = 0;
    torch::Tensor mean_;
    torch::Tensor std_;
    torch::Tensor input_gpu_;
    torch::Tensor nested_mask_;
    bool include_masks_ = false;
    bool include_status_detections_ = false;
    cudaEvent_t ready_event_ = nullptr;
};

class LiveBackendRunner final : public LiveModelRunner {
public:
    LiveBackendRunner(const ResolvedModelArtifacts& artifacts,
                      const LivePredictOptions& options,
                      const std::string& backend_name)
        : artifacts_(artifacts),
          backend_(predict_internal::make_backend(artifacts, backend_name, options.device_id, options.allow_fp16)),
          stream_(validate_detail::backend_cuda_stream(*backend_, options.device_id)),
          device_id_(options.device_id),
          num_queries_(backend_->info().num_queries > 0 ? backend_->info().num_queries : 300),
          category_count_(backend_->info().num_classes > 0 ? static_cast<size_t>(backend_->info().num_classes)
                                                           : static_cast<size_t>(artifacts_.config.num_classes)),
          include_masks_(options.include_masks),
          include_status_detections_(options.include_status_detections) {
        const int64_t resolution = artifacts_.config.resolution;
        {
            c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
            c10::cuda::CUDAStreamGuard stream_guard(stream_);
            auto normalization = predict_internal::initialize_normalization_tensors(device_id_, stream_);
            mean_ = std::move(normalization.first);
            std_ = std::move(normalization.second);
            input_gpu_ = torch::empty(
                {1, 3, resolution, resolution},
                torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device_id_));
            ensure_cuda_ok(cudaEventCreateWithFlags(&ready_event_, cudaEventDisableTiming),
                           "cudaEventCreateWithFlags for live backend ready event");
        }
    }

    ~LiveBackendRunner() override {
        if (ready_event_ != nullptr) {
            cudaEventDestroy(ready_event_);
        }
    }

    LiveFrameRenderData run_frame(const LiveFrameInputs& frame,
                                  uint32_t split_count,
                                  size_t max_dets_per_image,
                                  float threshold) override {
        LiveFrameRenderData out;
        const std::vector<SplitRegion> splits = build_horizontal_splits(frame.region.width, split_count);
        if (splits.empty()) {
            throw std::runtime_error("RF-DETR live predict split list is empty");
        }

        const cudaStream_t cuda_stream = reinterpret_cast<cudaStream_t>(stream_.stream());
        ensure_cuda_ok(cudaStreamWaitEvent(cuda_stream, frame.ready_event, 0),
                       "cudaStreamWaitEvent for live backend frame");

        c10::cuda::CUDAGuard device_guard(checked_device_index(device_id_));
        torch::NoGradGuard no_grad;
        out.splits.reserve(splits.size());
        for (size_t split_index = 0; split_index < splits.size(); ++split_index) {
            const SplitRegion split = splits[split_index];
            const LiveCaptureRegion split_region = make_split_region(frame.region, split);
            const auto* split_ptr = frame.data + static_cast<std::size_t>(split.x) * 3U;
            std::vector<TensorMap> outputs;
            LiveSplitRenderData split_render;
            split_render.prediction.split_index = static_cast<uint32_t>(split_index);
            split_render.prediction.source_region = split_region;
            {
                c10::cuda::CUDAStreamGuard stream_guard(stream_);
                if (const char* arg_error = validate_bgr_split_to_planar_float_args(
                        frame.pitch_bytes,
                        split.width,
                        frame.region.height,
                        static_cast<uint32_t>(artifacts_.config.resolution),
                        static_cast<uint32_t>(artifacts_.config.resolution),
                        cuda_stream)) {
                    throw std::runtime_error(
                        "RF-DETR live backend preprocess arguments are invalid: " +
                        format_live_split_context(frame.frame_id,
                                                  static_cast<uint32_t>(split_index),
                                                  frame.pitch_bytes,
                                                  split.width,
                                                  frame.region.height,
                                                  static_cast<uint32_t>(artifacts_.config.resolution),
                                                  static_cast<uint32_t>(artifacts_.config.resolution)) +
                        " reason=" + arg_error);
                }
                ensure_cuda_ok(
                    launch_bgr_split_to_planar_float(split_ptr,
                                                     frame.pitch_bytes,
                                                     split.width,
                                                     frame.region.height,
                                                     input_gpu_.data_ptr<float>(),
                                                     static_cast<uint32_t>(artifacts_.config.resolution),
                                                     static_cast<uint32_t>(artifacts_.config.resolution),
                                                     cuda_stream),
                    ("launch_bgr_split_to_planar_float for live backend frame: " +
                     format_live_split_context(frame.frame_id,
                                               static_cast<uint32_t>(split_index),
                                               frame.pitch_bytes,
                                               split.width,
                                               frame.region.height,
                                               static_cast<uint32_t>(artifacts_.config.resolution),
                                               static_cast<uint32_t>(artifacts_.config.resolution)))
                        .c_str());
                input_gpu_.sub_(mean_).div_(std_);
                OutputTensors backend_outputs = backend_->run(input_gpu_);
                if (!include_masks_) {
                    backend_outputs.pred_masks.reset();
                }
                outputs = postprocess_outputs_fixed_size(
                    backend_outputs,
                    split_region.height,
                    split_region.width,
                    num_queries_);
                ensure_cuda_ok(cudaPeekAtLastError(), "live backend postprocess");
                if (include_status_detections_) {
                    const int image_id = live_image_id(frame.frame_id);
                    split_render.prediction.detections = filter_threshold(
                        result_to_predictions(
                            image_id,
                            outputs.front(),
                            category_count_,
                            max_dets_per_image),
                        threshold);
                }
                LiveOverlaySelection overlay = select_live_overlay_for_preview(
                    outputs.front(),
                    category_count_,
                    max_dets_per_image,
                    threshold,
                    static_cast<int>(category_count_),
                    split_region.width,
                    split_region.height,
                    cuda_stream);
                split_render.boxes_xyxy = std::move(overlay.boxes_xyxy);
                split_render.labels_zero_based = std::move(overlay.labels_zero_based);
                split_render.colors_rgb = std::move(overlay.colors_rgb);
                split_render.masks = std::move(overlay.masks);
            }
            out.splits.push_back(std::move(split_render));
        }
        ensure_cuda_ok(cudaEventRecord(ready_event_, cuda_stream),
                       "cudaEventRecord for live backend frame");
        out.ready_event = ready_event_;
        out.producer_stream = cuda_stream;
        return out;
    }

private:
    ResolvedModelArtifacts artifacts_;
    std::unique_ptr<InferenceBackend> backend_;
    c10::cuda::CUDAStream stream_;
    int device_id_ = 0;
    int64_t num_queries_ = 300;
    size_t category_count_ = 0;
    torch::Tensor mean_;
    torch::Tensor std_;
    torch::Tensor input_gpu_;
    bool include_masks_ = false;
    bool include_status_detections_ = false;
    cudaEvent_t ready_event_ = nullptr;
};

#if FASTLOADER_RFDETR_LIVE_CAPTURE

LiveCaptureStats make_capture_stats(const frameshow::CaptureStats& stats) {
    return LiveCaptureStats{
        stats.queued_v4l2_buffers,
        stats.dequeued_v4l2_buffers,
        stats.bytes_captured,
        stats.inference_frames_published,
        stats.frames_dropped,
        stats.empty_frames_dropped,
        stats.inference_backpressure_drops,
        stats.short_frames,
        stats.sequence_gaps,
        stats.requeue_failures,
        stats.running,
    };
}

frameshow::CaptureRegion to_frameshow_region(const LiveCaptureRegion& region) {
    return frameshow::CaptureRegion{
        region.x,
        region.y,
        region.width,
        region.height,
    };
}

LiveCaptureRegion from_frameshow_region(const frameshow::CaptureRegion& region) {
    return LiveCaptureRegion{
        region.x,
        region.y,
        region.width,
        region.height,
    };
}

frameshow::CaptureConfig make_capture_config(const LivePredictOptions& options) {
    frameshow::CaptureConfig config;
    config.device_path = options.source.device_path;
    config.cuda_device_index = options.device_id;
    config.width = options.source.width;
    config.height = options.source.height;
    config.fps = options.source.fps;
    config.v4l2_buffer_count = options.source.v4l2_buffer_count;
    config.preview_buffer_count = 0;
    config.initial_region = to_frameshow_region(options.source.initial_region);
    return config;
}

LiveFrameInputs make_frame_inputs(const frameshow::InferenceFrameView& frame) {
    LiveFrameInputs inputs;
    inputs.frame_id = frame.frame_id;
    inputs.capture_ns = frame.capture_ns;
    inputs.ready_ns = frame.ready_ns;
    inputs.ready_event = frame.buffer.ready_event;
    inputs.short_frame = frame.short_frame;
    inputs.data = reinterpret_cast<const std::uint8_t*>(frame.buffer.data);
    inputs.pitch_bytes = frame.buffer.pitch_bytes;
    inputs.region = LiveCaptureRegion{
        frame.buffer.x_px,
        frame.buffer.y_px,
        frame.buffer.width_px,
        frame.buffer.height_px,
    };
    return inputs;
}

#endif

} // namespace

struct LivePredictSession::Impl {
#if FASTLOADER_RFDETR_LIVE_CAPTURE
    enum class RenderedPreviewState : uint32_t {
        kFree = 0,
        kWriting = 1,
        kPublished = 2,
        kDisplaying = 3,
    };

    struct RenderedPreviewSlot {
        uint32_t slot_index = 0;
        CUdeviceptr device_ptr = 0;
        std::size_t pitch_bytes = 0;
        cudaStream_t stream = nullptr;
        cudaEvent_t ready_event = nullptr;
        cudaEvent_t source_copied_event = nullptr;
        RenderedPreviewState state = RenderedPreviewState::kFree;
        uint64_t frame_id = 0;
        uint64_t capture_ns = 0;
        uint64_t ready_ns = 0;
        bool short_frame = false;
        LiveCaptureRegion region{};
        std::vector<torch::Tensor> retained_tensors;
    };
#endif

    explicit Impl(LivePredictOptions options)
        : options_(std::move(options)),
          artifacts_(resolve_model_artifacts(options_)),
          backend_name_(predict_internal::choose_backend_name(options_.backend, artifacts_)) {
        if (options_.split_count == 0U) {
            throw std::runtime_error("RF-DETR live predict requires split_count >= 1");
        }
        if (options_.source.device_path.empty()) {
            throw std::runtime_error("RF-DETR live predict requires a device path");
        }
        if (options_.source.width == 0U || options_.source.height == 0U || options_.source.fps == 0U) {
            throw std::runtime_error("RF-DETR live predict requires positive capture width, height, and fps");
        }
        if (options_.source.v4l2_buffer_count == 0U) {
            throw std::runtime_error("RF-DETR live predict requires v4l2_buffer_count >= 1");
        }
        status_.active_split_count = options_.split_count;
        status_.active_input_width = static_cast<uint32_t>(artifacts_.config.resolution);
        status_.active_input_height = static_cast<uint32_t>(artifacts_.config.resolution);
        status_.backend_name = backend_name_;
        status_.active_model_path = artifacts_.input_path.string();
    }

    ~Impl() {
        try {
            stop();
        } catch (...) {
        }
    }

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_thread_.joinable()) {
            throw std::runtime_error("RF-DETR live predict session is already running");
        }
        stop_requested_ = false;
        status_.worker_running = true;
        status_.model_loading = true;
        status_.model_hot = false;
        status_.busy = false;
        status_.capture_running = false;
        status_.last_error.clear();
        worker_thread_ = std::thread([this]() { thread_main(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    void configure(uint32_t split_count) {
        if (split_count == 0U) {
            throw std::runtime_error("RF-DETR live predict requires split_count >= 1");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        options_.split_count = split_count;
        status_.active_split_count = split_count;
    }

    void set_capture_region(const LiveCaptureRegion& region) {
        std::lock_guard<std::mutex> lock(mutex_);
        options_.source.initial_region = region;
#if FASTLOADER_RFDETR_LIVE_CAPTURE
        if (capture_session_) {
            const frameshow::Status status = capture_session_->set_capture_region(to_frameshow_region(region));
            if (!status.ok()) {
                throw std::runtime_error("RF-DETR live predict capture region update failed: " + status.message);
            }
        }
#endif
    }

    void set_inference_region(const LiveCaptureRegion& region) {
        std::lock_guard<std::mutex> lock(mutex_);
        inference_region_ = region;
        has_inference_region_ = true;
    }

    void clear_inference_region() {
        std::lock_guard<std::mutex> lock(mutex_);
        has_inference_region_ = false;
        inference_region_ = {};
    }

    LiveCaptureRegion snapshot_capture_region() const {
        std::lock_guard<std::mutex> lock(mutex_);
#if FASTLOADER_RFDETR_LIVE_CAPTURE
        if (capture_session_) {
            return from_frameshow_region(capture_session_->snapshot_capture_region());
        }
#endif
        return options_.source.initial_region;
    }

    LivePredictStatus snapshot_status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        LivePredictStatus status = status_;
#if FASTLOADER_RFDETR_LIVE_CAPTURE
        if (capture_session_) {
            status.capture = make_capture_stats(capture_session_->snapshot_stats());
        }
#endif
        return status;
    }

    LivePreviewFormatInfo snapshot_preview_format() const {
#if !FASTLOADER_RFDETR_LIVE_CAPTURE
        return {};
#else
        std::lock_guard<std::mutex> lock(mutex_);
        return rendered_preview_format_;
#endif
    }

    void assign_last_error_locked(std::string message) {
        if (!message.empty() && status_.last_error != message) {
            log_live_predict_error_to_stderr(message);
        }
        status_.last_error = std::move(message);
    }

    bool try_acquire_latest_preview(LivePreviewFrame* out_frame, std::string* error_message) {
        if (out_frame == nullptr) {
            throw std::runtime_error("RF-DETR live preview requires a non-null output frame");
        }
        *out_frame = {};
#if !FASTLOADER_RFDETR_LIVE_CAPTURE
        if (error_message != nullptr) {
            *error_message = "RF-DETR live capture support was not built into this binary.";
        }
        return false;
#else
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_rendered_preview_index_ < 0 ||
            latest_rendered_preview_index_ >= static_cast<int>(rendered_preview_slots_.size())) {
            if (error_message != nullptr) {
                error_message->clear();
            }
            return false;
        }

        RenderedPreviewSlot& slot =
            *rendered_preview_slots_[static_cast<std::size_t>(latest_rendered_preview_index_)];
        if (slot.state != RenderedPreviewState::kPublished) {
            if (error_message != nullptr) {
                error_message->clear();
            }
            return false;
        }
        slot.state = RenderedPreviewState::kDisplaying;
        *out_frame = LivePreviewFrame{
            slot.slot_index,
            slot.frame_id,
            LivePreviewBuffer{
                static_cast<std::uintptr_t>(slot.device_ptr),
                slot.pitch_bytes,
                slot.region.x,
                slot.region.y,
                slot.region.width,
                slot.region.height,
                slot.ready_event,
            },
            slot.capture_ns,
            slot.ready_ns,
            slot.short_frame,
        };
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
#endif
    }

    bool release_preview(uint32_t buffer_index, std::string* error_message) {
#if !FASTLOADER_RFDETR_LIVE_CAPTURE
        (void)buffer_index;
        if (error_message != nullptr) {
            *error_message = "RF-DETR live capture support was not built into this binary.";
        }
        return false;
#else
        std::lock_guard<std::mutex> lock(mutex_);
        if (buffer_index >= rendered_preview_slots_.size()) {
            if (error_message != nullptr) {
                *error_message = "RF-DETR live preview buffer index out of range";
            }
            return false;
        }

        RenderedPreviewSlot& slot = *rendered_preview_slots_[buffer_index];
        if (slot.state != RenderedPreviewState::kDisplaying) {
            if (error_message != nullptr) {
                *error_message = "RF-DETR live preview buffer is not in use";
            }
            return false;
        }
        slot.state = RenderedPreviewState::kFree;
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
#endif
    }

#if FASTLOADER_RFDETR_LIVE_CAPTURE
    void initialize_rendered_preview_slots(uint32_t capture_width, uint32_t capture_height) {
        release_rendered_preview_slots();
        if (options_.source.preview_buffer_count == 0U || capture_width == 0U || capture_height == 0U) {
            return;
        }

        std::vector<std::unique_ptr<RenderedPreviewSlot>> slots;
        slots.reserve(options_.source.preview_buffer_count);
        uint32_t bytes_per_line = 0;
        try {
            for (uint32_t slot_index = 0; slot_index < options_.source.preview_buffer_count; ++slot_index) {
                auto slot = std::make_unique<RenderedPreviewSlot>();
                slot->slot_index = slot_index;
                ensure_cuda_ok(
                    cudaMallocPitch(reinterpret_cast<void**>(&slot->device_ptr),
                                    &slot->pitch_bytes,
                                    static_cast<std::size_t>(capture_width) * 3U,
                                    capture_height),
                    "cudaMallocPitch for live rendered preview");
                ensure_cuda_ok(fastloader::cuda_stream_create_with_highest_priority(&slot->stream, cudaStreamNonBlocking),
                               "cudaStreamCreateWithPriority for live rendered preview");
                ensure_cuda_ok(cudaEventCreateWithFlags(&slot->ready_event, cudaEventDisableTiming),
                               "cudaEventCreateWithFlags for live rendered preview");
                ensure_cuda_ok(cudaEventCreateWithFlags(&slot->source_copied_event, cudaEventDisableTiming),
                               "cudaEventCreateWithFlags for live rendered preview source copy");
                if (bytes_per_line == 0U) {
                    bytes_per_line = static_cast<uint32_t>(slot->pitch_bytes);
                }
                slots.push_back(std::move(slot));
            }
        } catch (...) {
            for (auto& slot : slots) {
                if (slot->source_copied_event != nullptr) {
                    cudaEventDestroy(slot->source_copied_event);
                }
                if (slot->ready_event != nullptr) {
                    cudaEventDestroy(slot->ready_event);
                }
                if (slot->stream != nullptr) {
                    cudaStreamDestroy(slot->stream);
                }
                if (slot->device_ptr != 0) {
                    cudaFree(reinterpret_cast<void*>(slot->device_ptr));
                }
            }
            throw;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        rendered_preview_slots_ = std::move(slots);
        rendered_preview_format_ = LivePreviewFormatInfo{capture_width, capture_height, bytes_per_line};
        latest_rendered_preview_index_ = -1;
        next_rendered_preview_slot_ = 0;
    }

    void release_rendered_preview_slots() {
        std::vector<std::unique_ptr<RenderedPreviewSlot>> slots;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_rendered_preview_index_ = -1;
            next_rendered_preview_slot_ = 0;
            rendered_preview_format_ = {};
            slots.swap(rendered_preview_slots_);
        }
        for (auto& slot : slots) {
            if (slot->source_copied_event != nullptr) {
                cudaEventDestroy(slot->source_copied_event);
            }
            if (slot->ready_event != nullptr) {
                cudaEventDestroy(slot->ready_event);
            }
            if (slot->stream != nullptr) {
                cudaStreamDestroy(slot->stream);
            }
            if (slot->device_ptr != 0) {
                cudaFree(reinterpret_cast<void*>(slot->device_ptr));
            }
        }
    }

    RenderedPreviewSlot* reserve_rendered_preview_slot_locked() {
        if (rendered_preview_slots_.empty()) {
            return nullptr;
        }
        for (std::size_t attempt = 0; attempt < rendered_preview_slots_.size(); ++attempt) {
            const std::size_t candidate =
                (next_rendered_preview_slot_ + attempt) % rendered_preview_slots_.size();
            RenderedPreviewSlot& slot = *rendered_preview_slots_[candidate];
            if (slot.state != RenderedPreviewState::kFree &&
                slot.state != RenderedPreviewState::kPublished) {
                continue;
            }
            if (slot.ready_event != nullptr) {
                const cudaError_t query_status = cudaEventQuery(slot.ready_event);
                if (query_status == cudaErrorNotReady) {
                    continue;
                }
                ensure_cuda_ok(query_status, "cudaEventQuery for live rendered preview reuse");
            }
            slot.retained_tensors.clear();
            if (slot.state == RenderedPreviewState::kPublished &&
                latest_rendered_preview_index_ == static_cast<int>(candidate)) {
                latest_rendered_preview_index_ = -1;
            }
            {
                slot.state = RenderedPreviewState::kWriting;
                next_rendered_preview_slot_ = (candidate + 1U) % rendered_preview_slots_.size();
                return &slot;
            }
        }
        return nullptr;
    }

    cudaEvent_t publish_rendered_preview(const LiveFrameInputs& frame_inputs,
                                         const LiveFrameRenderData& frame_render) {
        RenderedPreviewSlot* slot = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            slot = reserve_rendered_preview_slot_locked();
        }
        if (slot == nullptr) {
            return nullptr;
        }

        try {
            ensure_cuda_ok(cudaStreamWaitEvent(slot->stream, frame_inputs.ready_event, 0),
                           "cudaStreamWaitEvent for live rendered preview");
            const std::size_t copy_width_bytes =
                static_cast<std::size_t>(frame_inputs.region.width) * 3U;
            ensure_cuda_ok(
                cudaMemcpy2DAsync(reinterpret_cast<void*>(slot->device_ptr),
                                  slot->pitch_bytes,
                                  frame_inputs.data,
                                  frame_inputs.pitch_bytes,
                                  copy_width_bytes,
                                  static_cast<std::size_t>(frame_inputs.region.height),
                                  cudaMemcpyDeviceToDevice,
                                  slot->stream),
                "cudaMemcpy2DAsync for live rendered preview");
            ensure_cuda_ok(cudaEventRecord(slot->source_copied_event, slot->stream),
                           "cudaEventRecord for live rendered preview source copy");
            if (frame_render.ready_event != nullptr) {
                ensure_cuda_ok(cudaStreamWaitEvent(slot->stream, frame_render.ready_event, 0),
                               "cudaStreamWaitEvent for live rendered preview model outputs");
            }

            std::vector<torch::Tensor> retained_tensors;
            for (const auto& split_render : frame_render.splits) {
                if (!split_render.boxes_xyxy.defined() ||
                    !split_render.labels_zero_based.defined() ||
                    split_render.boxes_xyxy.numel() == 0 ||
                    split_render.labels_zero_based.numel() == 0) {
                    continue;
                }

                auto* split_image = reinterpret_cast<std::uint8_t*>(slot->device_ptr) +
                                    static_cast<std::size_t>(split_render.prediction.source_region.y - frame_inputs.region.y) *
                                        slot->pitch_bytes +
                                    static_cast<std::size_t>(split_render.prediction.source_region.x - frame_inputs.region.x) * 3U;
                const int split_width = static_cast<int>(split_render.prediction.source_region.width);
                const int split_height = static_cast<int>(split_render.prediction.source_region.height);
                const int instance_count = static_cast<int>(split_render.labels_zero_based.size(0));
                if (split_render.masks.defined() &&
                    split_render.masks.dim() == 3 &&
                    split_render.masks.size(0) == split_render.labels_zero_based.size(0) &&
                    split_render.masks.size(1) == static_cast<int64_t>(split_height) &&
                    split_render.masks.size(2) == static_cast<int64_t>(split_width)) {
                    launch_draw_masks_boxes_labels_bgr_pitched(
                        split_image,
                        slot->pitch_bytes,
                        split_width,
                        split_height,
                        split_render.masks.data_ptr<bool>(),
                        split_render.boxes_xyxy.data_ptr<float>(),
                        split_render.colors_rgb.data_ptr<std::uint8_t>(),
                        split_render.labels_zero_based.data_ptr<int>(),
                        instance_count,
                        0.45f,
                        2,
                        slot->stream);
                    ensure_cuda_ok(cudaPeekAtLastError(),
                                   "launch_draw_masks_boxes_labels_bgr_pitched for live rendered preview");
                } else {
                    launch_draw_boxes_labels_bgr_pitched(
                        split_image,
                        slot->pitch_bytes,
                        split_width,
                        split_height,
                        split_render.boxes_xyxy.data_ptr<float>(),
                        split_render.colors_rgb.data_ptr<std::uint8_t>(),
                        split_render.labels_zero_based.data_ptr<int>(),
                        instance_count,
                        2,
                        slot->stream);
                    ensure_cuda_ok(cudaPeekAtLastError(),
                                   "launch_draw_boxes_labels_bgr_pitched for live rendered preview");
                }

                retained_tensors.push_back(split_render.boxes_xyxy);
                retained_tensors.push_back(split_render.labels_zero_based);
                retained_tensors.push_back(split_render.colors_rgb);
                if (split_render.masks.defined()) {
                    retained_tensors.push_back(split_render.masks);
                }
            }

            ensure_cuda_ok(cudaEventRecord(slot->ready_event, slot->stream),
                           "cudaEventRecord for live rendered preview");

            std::lock_guard<std::mutex> lock(mutex_);
            slot->frame_id = frame_inputs.frame_id;
            slot->capture_ns = frame_inputs.capture_ns;
            slot->ready_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now().time_since_epoch())
                    .count());
            slot->short_frame = frame_inputs.short_frame;
            slot->region = frame_inputs.region;
            slot->retained_tensors = std::move(retained_tensors);
            slot->state = RenderedPreviewState::kPublished;
            latest_rendered_preview_index_ = static_cast<int>(slot->slot_index);
            return slot->source_copied_event;
        } catch (...) {
            (void)cudaStreamSynchronize(slot->stream);
            std::lock_guard<std::mutex> lock(mutex_);
            slot->retained_tensors.clear();
            slot->state = RenderedPreviewState::kFree;
            throw;
        }
    }
#endif

    void thread_main() {
#if !FASTLOADER_RFDETR_LIVE_CAPTURE
        std::lock_guard<std::mutex> lock(mutex_);
        status_.worker_running = false;
        status_.model_loading = false;
        status_.model_hot = false;
        status_.busy = false;
        status_.capture_running = false;
        assign_last_error_locked(
            "RF-DETR live capture support was not built into this binary.");
        return;
#else
        struct PendingInferenceRelease {
            uint32_t buffer_index = 0;
            cudaEvent_t ready_event = nullptr;
            bool own_event = false;
        };

        std::vector<PendingInferenceRelease> pending_releases;
        frameshow::CaptureSession* capture = nullptr;
        auto drain_pending_releases = [&](bool wait) {
            if (capture == nullptr) {
                return;
            }
            for (std::size_t index = 0; index < pending_releases.size();) {
                PendingInferenceRelease& pending = pending_releases[index];
                if (pending.ready_event != nullptr) {
                    if (wait) {
                        ensure_cuda_ok(cudaEventSynchronize(pending.ready_event),
                                       "cudaEventSynchronize for deferred live inference release");
                    } else {
                        const cudaError_t query_status = cudaEventQuery(pending.ready_event);
                        if (query_status == cudaErrorNotReady) {
                            ++index;
                            continue;
                        }
                        ensure_cuda_ok(query_status, "cudaEventQuery for deferred live inference release");
                    }
                }

                const frameshow::Status release_status = capture->release_inference_frame(pending.buffer_index);
                if (pending.own_event && pending.ready_event != nullptr) {
                    cudaEventDestroy(pending.ready_event);
                }
                if (!release_status.ok()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    assign_last_error_locked(release_status.message);
                }

                pending_releases[index] = std::move(pending_releases.back());
                pending_releases.pop_back();
            }
        };

        try {
            std::unique_ptr<LiveModelRunner> runner;
            {
                c10::cuda::CUDAGuard device_guard(checked_device_index(options_.device_id));
                torch::NoGradGuard no_grad;
                if (backend_name_ == "weights") {
                    runner = std::make_unique<LiveWeightsRunner>(artifacts_, options_);
                } else {
                    runner = std::make_unique<LiveBackendRunner>(artifacts_, options_, backend_name_);
                }
            }

            auto session = std::make_unique<frameshow::CaptureSession>(make_capture_config(options_));
            const frameshow::Status start_status = session->start();
            if (!start_status.ok()) {
                throw std::runtime_error("RF-DETR live capture start failed: " + start_status.message);
            }
            const frameshow::CaptureFormatInfo capture_format = session->snapshot_format();
            initialize_rendered_preview_slots(capture_format.width, capture_format.height);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                capture_session_ = std::move(session);
                status_.model_loading = false;
                status_.model_hot = true;
                status_.capture_running = true;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                capture = capture_session_.get();
            }

            while (true) {
                drain_pending_releases(false);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stop_requested_) {
                        break;
                    }
                }

                frameshow::InferenceFrameView frame{};
                const frameshow::Status acquire_status = capture->try_acquire_latest_inference_frame(&frame);
                if (acquire_status.code == frameshow::StatusCode::kNotReady) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (!acquire_status.ok()) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        assign_last_error_locked(acquire_status.message);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }

                const LiveFrameInputs frame_inputs = make_frame_inputs(frame);
                const auto started_at = Clock::now();
                uint32_t split_count = 1;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (status_.last_started_frame_id != 0U &&
                        frame_inputs.frame_id > status_.last_started_frame_id + 1U) {
                        status_.frames_skipped += frame_inputs.frame_id - status_.last_started_frame_id - 1U;
                    }
                    status_.busy = true;
                    status_.frames_started += 1U;
                    status_.last_started_frame_id = frame_inputs.frame_id;
                    split_count = options_.split_count;
                }

                LiveFrameInputs inference_inputs = frame_inputs;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (has_inference_region_) {
                        const LiveCaptureRegion& ir = inference_region_;
                        const uint32_t cap_x = frame_inputs.region.x;
                        const uint32_t cap_y = frame_inputs.region.y;
                        const uint32_t cap_w = frame_inputs.region.width;
                        const uint32_t cap_h = frame_inputs.region.height;
                        const uint32_t rel_x = ir.x >= cap_x ? ir.x - cap_x : 0U;
                        const uint32_t rel_y = ir.y >= cap_y ? ir.y - cap_y : 0U;
                        const uint32_t clamp_w = (rel_x + ir.width <= cap_w) ? ir.width : (cap_w > rel_x ? cap_w - rel_x : 1U);
                        const uint32_t clamp_h = (rel_y + ir.height <= cap_h) ? ir.height : (cap_h > rel_y ? cap_h - rel_y : 1U);
                        inference_inputs.data = frame_inputs.data +
                                                static_cast<std::size_t>(rel_y) * frame_inputs.pitch_bytes +
                                                static_cast<std::size_t>(rel_x) * 3U;
                        inference_inputs.region = LiveCaptureRegion{ir.x, ir.y, clamp_w, clamp_h};
                    }
                }

                LiveFrameRenderData frame_render;
                try {
                    frame_render =
                        runner->run_frame(inference_inputs, split_count, options_.max_dets_per_image, options_.threshold);
                    cudaEvent_t release_ready_event = publish_rendered_preview(frame_inputs, frame_render);
                    bool own_release_event = false;
                    if (release_ready_event == nullptr) {
                        if (frame_render.producer_stream == nullptr) {
                            throw std::runtime_error("RF-DETR live predict did not expose a producer stream");
                        }
                        ensure_cuda_ok(cudaEventCreateWithFlags(&release_ready_event, cudaEventDisableTiming),
                                       "cudaEventCreateWithFlags for deferred live inference release");
                        own_release_event = true;
                        ensure_cuda_ok(cudaEventRecord(release_ready_event, frame_render.producer_stream),
                                       "cudaEventRecord for deferred live inference release");
                    }
                    pending_releases.push_back(PendingInferenceRelease{
                        frame.buffer_index,
                        release_ready_event,
                        own_release_event,
                    });
                } catch (...) {
                    if (frame_render.producer_stream != nullptr) {
                        (void)cudaDeviceSynchronize();
                    }
                    (void)capture->release_inference_frame(frame.buffer_index);
                    throw;
                }
                const double latency_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                                              Clock::now() - started_at)
                                              .count();

                std::vector<LiveSplitPrediction> predictions;
                predictions.reserve(frame_render.splits.size());
                for (auto& split_render : frame_render.splits) {
                    predictions.push_back(std::move(split_render.prediction));
                }

                std::lock_guard<std::mutex> lock(mutex_);
                status_.busy = false;
                status_.frames_completed += 1U;
                status_.splits_started += predictions.size();
                status_.splits_completed += predictions.size();
                status_.last_completed_frame_id = frame_inputs.frame_id;
                status_.last_latency_ms = latency_ms;
                status_.active_split_count = static_cast<uint32_t>(predictions.size());
                status_.last_prediction.frame_id = frame_inputs.frame_id;
                status_.last_prediction.capture_ns = frame_inputs.capture_ns;
                status_.last_prediction.ready_ns = frame_inputs.ready_ns;
                status_.last_prediction.short_frame = frame_inputs.short_frame;
                status_.last_prediction.splits = std::move(predictions);
                assign_last_error_locked(std::string{});
            }
            drain_pending_releases(true);
        } catch (const std::exception& error) {
            std::string message = error.what();
            try {
                drain_pending_releases(true);
            } catch (const std::exception& cleanup_error) {
                message += " | cleanup: ";
                message += cleanup_error.what();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            assign_last_error_locked(std::move(message));
        }

        std::unique_ptr<frameshow::CaptureSession> capture_owner;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            capture_owner = std::move(capture_session_);
            capture = nullptr;
        }
        if (capture_owner) {
            (void)capture_owner->stop();
        }
        release_rendered_preview_slots();

        std::lock_guard<std::mutex> lock(mutex_);
        status_.worker_running = false;
        status_.capture_running = false;
        status_.model_loading = false;
        status_.model_hot = false;
        status_.busy = false;
#endif
    }

    mutable std::mutex mutex_;
    LivePredictOptions options_;
    ResolvedModelArtifacts artifacts_;
    std::string backend_name_;
    LivePredictStatus status_;
    bool stop_requested_ = false;
    bool has_inference_region_ = false;
    LiveCaptureRegion inference_region_{};
    std::thread worker_thread_;
#if FASTLOADER_RFDETR_LIVE_CAPTURE
    std::unique_ptr<frameshow::CaptureSession> capture_session_;
    std::vector<std::unique_ptr<RenderedPreviewSlot>> rendered_preview_slots_;
    int latest_rendered_preview_index_ = -1;
    std::size_t next_rendered_preview_slot_ = 0;
    LivePreviewFormatInfo rendered_preview_format_{};
#endif
};

bool live_capture_supported() {
#if FASTLOADER_RFDETR_LIVE_CAPTURE
    return true;
#else
    return false;
#endif
}

LivePredictSession::LivePredictSession(const LivePredictOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

LivePredictSession::~LivePredictSession() = default;

LivePredictSession::LivePredictSession(LivePredictSession&&) noexcept = default;

LivePredictSession& LivePredictSession::operator=(LivePredictSession&&) noexcept = default;

void LivePredictSession::start() {
    impl_->start();
}

void LivePredictSession::stop() {
    impl_->stop();
}

void LivePredictSession::configure(uint32_t split_count) {
    impl_->configure(split_count);
}

void LivePredictSession::set_capture_region(const LiveCaptureRegion& region) {
    impl_->set_capture_region(region);
}

void LivePredictSession::set_inference_region(const LiveCaptureRegion& region) {
    impl_->set_inference_region(region);
}

void LivePredictSession::clear_inference_region() {
    impl_->clear_inference_region();
}

LiveCaptureRegion LivePredictSession::snapshot_capture_region() const {
    return impl_->snapshot_capture_region();
}

LivePredictStatus LivePredictSession::snapshot_status() const {
    return impl_->snapshot_status();
}

LivePreviewFormatInfo LivePredictSession::snapshot_preview_format() const {
    return impl_->snapshot_preview_format();
}

bool LivePredictSession::try_acquire_latest_preview(LivePreviewFrame* out_frame, std::string* error_message) {
    return impl_->try_acquire_latest_preview(out_frame, error_message);
}

bool LivePredictSession::release_preview(uint32_t buffer_index, std::string* error_message) {
    return impl_->release_preview(buffer_index, error_message);
}

} // namespace fastloader::rfdetr
