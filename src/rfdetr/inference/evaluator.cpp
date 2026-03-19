#include "rfdetr/evaluator.h"

#include "common_utils.h"
#include "dataset_loader.h"
#include "profile_utils.h"
#include "rfdetr/cuda_utils.h"

#include <c10/cuda/CUDAGuard.h>
#include <torch/torch.h>

namespace fastloader::rfdetr {

namespace {

constexpr double kIouThresholds[] = {0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85, 0.90, 0.95};
constexpr double kRecallStep = 0.01;

float bbox_iou(const std::array<float, 4>& lhs, const std::array<float, 4>& rhs) {
    const float left = std::max(lhs[0], rhs[0]);
    const float top = std::max(lhs[1], rhs[1]);
    const float right = std::min(lhs[2], rhs[2]);
    const float bottom = std::min(lhs[3], rhs[3]);
    const float intersect_w = std::max(0.0f, right - left);
    const float intersect_h = std::max(0.0f, bottom - top);
    const float intersect = intersect_w * intersect_h;
    const float lhs_area = std::max(0.0f, lhs[2] - lhs[0]) * std::max(0.0f, lhs[3] - lhs[1]);
    const float rhs_area = std::max(0.0f, rhs[2] - rhs[0]) * std::max(0.0f, rhs[3] - rhs[1]);
    const float union_area = lhs_area + rhs_area - intersect;
    if (union_area <= 0.0f) {
        return 0.0f;
    }
    return intersect / union_area;
}

template <typename MaskValue>
EncodedMask encode_mask_from_linear_data(const MaskValue* data, uint32_t height, uint32_t width) {
    FASTLOADER_NVTX_RANGE("encode_mask_from_linear_data", NVTX_COLOR_BLUE);
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.encode_mask_linear");
    EncodedMask mask;
    mask.height = height;
    mask.width = width;
    const uint32_t pixel_count = width * height;

    uint32_t run_count = 0;
    bool in_run = false;
    for (uint32_t index = 0; index < pixel_count; ++index) {
        const bool value = static_cast<bool>(data[index]);
        mask.area += value ? 1U : 0U;
        if (value && !in_run) {
            in_run = true;
            ++run_count;
        } else if (!value) {
            in_run = false;
        }
    }

    mask.runs.resize(run_count);
    in_run = false;
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    uint32_t run_index = 0;
    for (uint32_t index = 0; index < pixel_count; ++index) {
        const bool value = static_cast<bool>(data[index]);
        if (value) {
            if (!in_run) {
                in_run = true;
                run_start = index;
                run_length = 1;
            } else {
                ++run_length;
            }
        } else if (in_run) {
            mask.runs[run_index++] = {run_start, run_length};
            in_run = false;
            run_length = 0;
        }
    }
    if (in_run) {
        mask.runs[run_index++] = {run_start, run_length};
    }
    return mask;
}

uint32_t intersection_area(const EncodedMask& lhs, const EncodedMask& rhs) {
    FASTLOADER_NVTX_RANGE("intersection_area", NVTX_COLOR_YELLOW);
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.intersection_area");
    size_t left_index = 0;
    size_t right_index = 0;
    uint32_t total = 0;
    while (left_index < lhs.runs.size() && right_index < rhs.runs.size()) {
        const auto [left_start, left_len] = lhs.runs[left_index];
        const auto [right_start, right_len] = rhs.runs[right_index];
        const uint32_t left_end = left_start + left_len;
        const uint32_t right_end = right_start + right_len;
        const uint32_t overlap_start = std::max(left_start, right_start);
        const uint32_t overlap_end = std::min(left_end, right_end);
        if (overlap_end > overlap_start) {
            total += overlap_end - overlap_start;
        }
        if (left_end <= right_end) {
            ++left_index;
        } else {
            ++right_index;
        }
    }
    return total;
}

float mask_iou(const EncodedMask& lhs, const EncodedMask& rhs) {
    if (lhs.height != rhs.height || lhs.width != rhs.width) {
        return 0.0f;
    }
    const uint32_t intersect = intersection_area(lhs, rhs);
    const uint32_t union_area = lhs.area + rhs.area - intersect;
    if (union_area == 0) {
        return 0.0f;
    }
    return static_cast<float>(intersect) / static_cast<float>(union_area);
}

std::array<float, 4> xyxy_clamped(const float* box_values) {
    const float x1 = box_values[0];
    const float y1 = box_values[1];
    const float x2 = box_values[2];
    const float y2 = box_values[3];
    return {
        std::min(x1, x2),
        std::min(y1, y2),
        std::max(x1, x2),
        std::max(y1, y2),
    };
}

struct CategoryImageGroundTruth {
    const GroundTruthAnnotation* annotation = nullptr;
    bool matched = false;
};

struct SelectedPredictions {
    std::vector<Prediction> predictions;
    std::vector<int64_t> mask_source_indices;
};

struct CategoryMetricWork {
    std::unordered_map<int, std::vector<const GroundTruthAnnotation*>> gt_by_image;
    std::vector<const Prediction*> predictions;
};

SelectedPredictions select_predictions(int image_id,
                                       const torch::Tensor& scores,
                                       const torch::Tensor& labels,
                                       const torch::Tensor& boxes,
                                       size_t category_count,
                                       size_t max_dets_per_image) {
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.select_predictions");
    const auto* score_ptr = scores.data_ptr<float>();
    const auto* label_ptr = labels.data_ptr<int64_t>();
    const auto* box_ptr = boxes.data_ptr<float>();
    const int64_t total = scores.size(0);

    SelectedPredictions out;
    out.predictions.reserve(std::min<size_t>(static_cast<size_t>(total), max_dets_per_image));
    out.mask_source_indices.reserve(out.predictions.capacity());
    for (int64_t index = 0; index < total; ++index) {
        const int64_t label = label_ptr[index];
        if (label < 0 || label >= static_cast<int64_t>(category_count)) {
            continue;
        }
        Prediction prediction;
        prediction.image_id = image_id;
        prediction.category_id = static_cast<int>(label) + 1;
        prediction.score = score_ptr[index];
        prediction.bbox_xyxy = xyxy_clamped(box_ptr + index * 4);
        out.predictions.push_back(std::move(prediction));
        out.mask_source_indices.push_back(index);
        if (out.predictions.size() >= max_dets_per_image) {
            break;
        }
    }
    return out;
}

template <typename IoUFn>
double compute_average_precision(const std::vector<const Prediction*>& predictions,
                                 const std::unordered_map<int, std::vector<const GroundTruthAnnotation*>>& gt_by_image,
                                 double threshold,
                                 IoUFn&& iou_fn) {
    FASTLOADER_NVTX_RANGE("compute_average_precision", NVTX_COLOR_MAGENTA);
    size_t gt_count = 0;
    for (const auto& item : gt_by_image) {
        gt_count += item.second.size();
    }
    if (gt_count == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    std::unordered_map<int, std::vector<CategoryImageGroundTruth>> image_matches;
    image_matches.reserve(gt_by_image.size());
    for (const auto& item : gt_by_image) {
        auto& entries = image_matches[item.first];
        entries.reserve(item.second.size());
        for (const GroundTruthAnnotation* annotation : item.second) {
            entries.push_back(CategoryImageGroundTruth{annotation, false});
        }
    }

    std::vector<double> true_positives;
    std::vector<double> false_positives;
    true_positives.reserve(predictions.size());
    false_positives.reserve(predictions.size());

    for (const Prediction* prediction : predictions) {
        auto found = image_matches.find(prediction->image_id);
        if (found == image_matches.end()) {
            true_positives.push_back(0.0);
            false_positives.push_back(1.0);
            continue;
        }

        auto& gt_entries = found->second;
        float best_iou = -1.0f;
        size_t best_index = gt_entries.size();
        for (size_t gt_index = 0; gt_index < gt_entries.size(); ++gt_index) {
            if (gt_entries[gt_index].matched) {
                continue;
            }
            const float iou = iou_fn(*prediction, *gt_entries[gt_index].annotation);
            if (iou >= static_cast<float>(threshold) && iou > best_iou) {
                best_iou = iou;
                best_index = gt_index;
            }
        }

        if (best_index < gt_entries.size()) {
            gt_entries[best_index].matched = true;
            true_positives.push_back(1.0);
            false_positives.push_back(0.0);
        } else {
            true_positives.push_back(0.0);
            false_positives.push_back(1.0);
        }
    }

    std::partial_sum(true_positives.begin(), true_positives.end(), true_positives.begin());
    std::partial_sum(false_positives.begin(), false_positives.end(), false_positives.begin());

    std::vector<double> recalls(true_positives.size(), 0.0);
    std::vector<double> precisions(true_positives.size(), 0.0);
    for (size_t index = 0; index < true_positives.size(); ++index) {
        recalls[index] = true_positives[index] / static_cast<double>(gt_count);
        const double denominator = true_positives[index] + false_positives[index];
        precisions[index] = denominator > 0.0 ? true_positives[index] / denominator : 0.0;
    }

    for (size_t index = precisions.size(); index > 1; --index) {
        precisions[index - 2] = std::max(precisions[index - 2], precisions[index - 1]);
    }

    double ap = 0.0;
    for (int recall_index = 0; recall_index <= 100; ++recall_index) {
        const double recall_threshold = static_cast<double>(recall_index) * kRecallStep;
        double precision = 0.0;
        for (size_t sample = 0; sample < recalls.size(); ++sample) {
            if (recalls[sample] >= recall_threshold) {
                precision = precisions[sample];
                break;
            }
        }
        ap += precision;
    }
    return ap / 101.0;
}

template <typename IoUFn>
MetricSummary compute_metric_summary(const std::vector<GroundTruthAnnotation>& ground_truths,
                                     const std::vector<Prediction>& predictions,
                                     size_t category_count,
                                     size_t max_dets_per_image,
                                     IoUFn&& iou_fn) {
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.metric");
    std::vector<CategoryMetricWork> work_by_category(category_count);
    for (const GroundTruthAnnotation& gt : ground_truths) {
        if (gt.category_id <= 0 || gt.category_id > static_cast<int>(category_count)) {
            continue;
        }
        work_by_category[static_cast<size_t>(gt.category_id - 1)].gt_by_image[gt.image_id].push_back(&gt);
    }
    for (const Prediction& prediction : predictions) {
        if (prediction.category_id <= 0 || prediction.category_id > static_cast<int>(category_count)) {
            continue;
        }
        work_by_category[static_cast<size_t>(prediction.category_id - 1)].predictions.push_back(&prediction);
    }

    std::vector<std::array<double, std::size(kIouThresholds)>> category_ap_sums(category_count);
    std::vector<std::array<size_t, std::size(kIouThresholds)>> category_ap_counts(category_count);
    for (auto& category_sum : category_ap_sums) {
        category_sum.fill(0.0);
    }
    for (auto& category_count_values : category_ap_counts) {
        category_count_values.fill(0);
    }

    const unsigned hardware_threads = std::thread::hardware_concurrency();
    const int worker_count = static_cast<int>(std::max(1u, hardware_threads == 0 ? 1u : hardware_threads));
    parallel_for_range<size_t>(0, category_count, worker_count, [&](size_t begin, size_t end) {
        FASTLOADER_NVTX_RANGE("compute_metric_summary_worker", NVTX_COLOR_CYAN);
        for (size_t category_index = begin; category_index < end; ++category_index) {
            auto& work = work_by_category[category_index];
            if (work.gt_by_image.empty()) {
                continue;
            }

            std::unordered_map<int, size_t> seen_per_image;
            seen_per_image.reserve(work.gt_by_image.size());
            std::vector<const Prediction*> preds;
            preds.reserve(work.predictions.size());
            for (const Prediction* prediction : work.predictions) {
                size_t& seen = seen_per_image[prediction->image_id];
                if (seen >= max_dets_per_image) {
                    continue;
                }
                ++seen;
                preds.push_back(prediction);
            }
            std::sort(preds.begin(),
                      preds.end(),
                      [](const Prediction* lhs, const Prediction* rhs) { return lhs->score > rhs->score; });

            for (size_t threshold_index = 0; threshold_index < std::size(kIouThresholds); ++threshold_index) {
                const double ap = compute_average_precision(
                    preds, work.gt_by_image, kIouThresholds[threshold_index], iou_fn);
                if (!std::isnan(ap)) {
                    category_ap_sums[category_index][threshold_index] = ap;
                    category_ap_counts[category_index][threshold_index] = 1;
                }
            }
        }
    });

    std::array<double, std::size(kIouThresholds)> ap_means{};
    std::array<size_t, std::size(kIouThresholds)> ap_counts{};
    for (size_t category_index = 0; category_index < category_count; ++category_index) {
        for (size_t threshold_index = 0; threshold_index < std::size(kIouThresholds); ++threshold_index) {
            ap_means[threshold_index] += category_ap_sums[category_index][threshold_index];
            ap_counts[threshold_index] += category_ap_counts[category_index][threshold_index];
        }
    }

    MetricSummary summary;
    double overall_ap = 0.0;
    size_t overall_count = 0;
    for (size_t threshold_index = 0; threshold_index < std::size(kIouThresholds); ++threshold_index) {
        if (ap_counts[threshold_index] == 0) {
            continue;
        }
        const double mean_ap = ap_means[threshold_index] / static_cast<double>(ap_counts[threshold_index]);
        overall_ap += mean_ap;
        ++overall_count;
        if (std::abs(kIouThresholds[threshold_index] - 0.50) < 1e-9) {
            summary.ap50 = mean_ap;
        }
        if (std::abs(kIouThresholds[threshold_index] - 0.75) < 1e-9) {
            summary.ap75 = mean_ap;
        }
    }
    summary.ap = overall_count > 0 ? overall_ap / static_cast<double>(overall_count) : 0.0;
    return summary;
}

std::vector<Prediction> valid_predictions(const TensorMap& result,
                                          int image_id,
                                          size_t category_count,
                                          size_t max_dets_per_image) {
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.result_to_predictions");
    torch::Tensor scores;
    torch::Tensor labels;
    torch::Tensor boxes;
    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.d2h_boxes_labels_scores");
        scores = result.at("scores").to(torch::kCPU).contiguous();
        labels = result.at("labels").to(torch::kCPU).contiguous();
        boxes = result.at("boxes").to(torch::kCPU).contiguous();
    }
    const bool has_masks = result.find("masks") != result.end();
    SelectedPredictions selected =
        select_predictions(image_id, scores, labels, boxes, category_count, max_dets_per_image);
    if (!has_masks || selected.predictions.empty()) {
        return selected.predictions;
    }

    torch::Tensor source_masks = result.at("masks");
    if (source_masks.dim() == 4 && source_masks.size(1) == 1) {
        source_masks = source_masks.squeeze(1);
    }
    if (source_masks.dim() != 3) {
        throw std::runtime_error("predicted masks must be [num_predictions,height,width]");
    }

    torch::Tensor mask_index;
    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.d2h_masks");
        mask_index = torch::tensor(
            selected.mask_source_indices,
            torch::TensorOptions().dtype(torch::kInt64).device(source_masks.device()));
        source_masks = source_masks.index_select(0, mask_index).to(torch::kCPU);
    }

    torch::Tensor staged_masks;
    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.relayout_masks");
        staged_masks = source_masks.contiguous();
    }

    const uint32_t height = static_cast<uint32_t>(source_masks.size(1));
    const uint32_t width = static_cast<uint32_t>(source_masks.size(2));
    const uint32_t pixels_per_mask = width * height;
    const auto* staged_ptr = staged_masks.data_ptr<bool>();
    std::vector<EncodedMask> encoded_masks(selected.predictions.size());
    {
        FASTLOADER_NVTX_RANGE("encode_masks_loop", NVTX_COLOR_ORANGE);
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.encode_masks");
        for (size_t index = 0; index < selected.predictions.size(); ++index) {
            encoded_masks[index] = encode_mask_from_linear_data(
                staged_ptr + index * static_cast<size_t>(pixels_per_mask),
                height,
                width);
        }
    }

    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.accumulate_predictions");
        for (size_t index = 0; index < selected.predictions.size(); ++index) {
            selected.predictions[index].mask = std::move(encoded_masks[index]);
            selected.predictions[index].has_mask = true;
        }
    }
    return selected.predictions;
}

Prediction* first_valid_prediction(std::vector<Prediction>& predictions) {
    if (predictions.empty()) {
        return nullptr;
    }
    return &predictions.front();
}

} // namespace

PredictionBufferSlotPool::PredictionBufferSlotPool(size_t slot_count) {
    if (slot_count == 0) {
        throw std::runtime_error("prediction buffer slot pool requires at least one slot");
    }
    slots_.reserve(slot_count);
    for (size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
        slots_.push_back(std::make_shared<PinnedPredictionBuffers>());
        free_slots_.push_back(slot_index);
    }
}

PredictionBufferLease PredictionBufferSlotPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !free_slots_.empty(); });
    const size_t slot_index = free_slots_.front();
    free_slots_.pop_front();
    PredictionBufferLease lease;
    lease.buffers = slots_.at(slot_index);
    lease.release_guard = std::shared_ptr<void>(
        nullptr,
        [pool = shared_from_this(), slot_index](void*) { pool->release(slot_index); });
    return lease;
}

void PredictionBufferSlotPool::release(size_t slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        free_slots_.push_back(slot_index);
    }
    cv_.notify_one();
}

void PinnedPredictionBuffers::ensure_prediction_capacity(int64_t count) {
    if (prediction_capacity >= count &&
        scores_cpu.defined() &&
        labels_cpu.defined() &&
        boxes_cpu.defined()) {
        return;
    }
    prediction_capacity = std::max<int64_t>(prediction_capacity, count);
    const auto float_options = torch::TensorOptions()
                                   .dtype(torch::kFloat32)
                                   .device(torch::kCPU)
                                   .pinned_memory(true);
    const auto int_options = torch::TensorOptions()
                                 .dtype(torch::kInt64)
                                 .device(torch::kCPU)
                                 .pinned_memory(true);
    scores_cpu = torch::empty({prediction_capacity}, float_options);
    labels_cpu = torch::empty({prediction_capacity}, int_options);
    boxes_cpu = torch::empty({prediction_capacity, 4}, float_options);
}

void PinnedPredictionBuffers::ensure_mask_capacity(int64_t count, uint32_t height, uint32_t width) {
    if (masks_cpu.defined() &&
        masks_cpu.dim() == 3 &&
        masks_cpu.size(0) >= count &&
        mask_height == height &&
        mask_width == width) {
        return;
    }
    ensure_prediction_capacity(count);
    mask_height = height;
    mask_width = width;
    masks_cpu = torch::empty(
        {prediction_capacity, static_cast<int64_t>(height), static_cast<int64_t>(width)},
        torch::TensorOptions().dtype(torch::kBool).device(torch::kCPU).pinned_memory(true));
}

// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
StagedPredictionBatch stage_result_to_predictions(int image_id,
                                                  const TensorMap& result,
                                                  size_t category_count,
                                                  size_t max_dets_per_image,
                                                  PredictionBufferLease lease,
                                                  int device_id,
                                                  void* stream_handle) {
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.result_to_predictions");
    if (!lease.buffers) {
        throw std::runtime_error("stage_result_to_predictions requires a valid prediction buffer lease");
    }
    PinnedPredictionBuffers& buffers = *lease.buffers;
    const auto& scores_gpu = result.at("scores");
    const auto& labels_gpu = result.at("labels");
    const auto& boxes_gpu = result.at("boxes");
    const int64_t prediction_count = scores_gpu.size(0);
    buffers.ensure_prediction_capacity(prediction_count);

    const auto score_view = buffers.scores_cpu.narrow(0, 0, prediction_count);
    const auto label_view = buffers.labels_cpu.narrow(0, 0, prediction_count);
    const auto box_view = buffers.boxes_cpu.narrow(0, 0, prediction_count);

    cudaStream_t stream = reinterpret_cast<cudaStream_t>(stream_handle);
    if (stream == nullptr) {
        throw std::runtime_error("stage_result_to_predictions requires a CUDA stream");
    }

    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.d2h_boxes_labels_scores");
        const c10::DeviceIndex checked_index = checked_device_index(device_id);
        c10::cuda::CUDAGuard device_guard(checked_index);
        c10::cuda::CUDAStreamGuard stream_guard(c10::cuda::getStreamFromExternal(stream, checked_index));
        score_view.copy_(scores_gpu, true);
        label_view.copy_(labels_gpu, true);
        box_view.copy_(boxes_gpu, true);
    }
    cudaError_t sync_status = cudaStreamSynchronize(stream);
    if (sync_status != cudaSuccess) {
        throw std::runtime_error(std::string("cudaStreamSynchronize failed during prediction staging: ") +
                                 cudaGetErrorString(sync_status));
    }

    SelectedPredictions selected =
        select_predictions(image_id, score_view, label_view, box_view, category_count, max_dets_per_image);
    if (selected.predictions.empty() || result.find("masks") == result.end()) {
        return StagedPredictionBatch{
            std::move(selected.predictions),
            torch::Tensor(),
            torch::Tensor(),
            0,
            0,
            nullptr,
            std::move(lease),
        };
    }

    torch::Tensor source_masks = result.at("masks");
    if (source_masks.dim() == 4 && source_masks.size(1) == 1) {
        source_masks = source_masks.squeeze(1);
    }
    if (source_masks.dim() != 3) {
        throw std::runtime_error("predicted masks must be [num_predictions,height,width]");
    }

    const uint32_t mask_height = static_cast<uint32_t>(source_masks.size(1));
    const uint32_t mask_width = static_cast<uint32_t>(source_masks.size(2));
    buffers.ensure_mask_capacity(
        checked_cast<int64_t>(selected.predictions.size(), "selected prediction count overflow"),
        mask_height,
        mask_width);

    const auto mask_view = buffers.masks_cpu.narrow(
        0,
        0,
        checked_cast<int64_t>(selected.predictions.size(), "selected prediction count overflow"));
    torch::Tensor staged_masks;
    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.d2h_masks");
        const c10::DeviceIndex checked_index = checked_device_index(device_id);
        c10::cuda::CUDAGuard device_guard(checked_index);
        c10::cuda::CUDAStreamGuard stream_guard(c10::cuda::getStreamFromExternal(stream, checked_index));
        const auto mask_index = torch::tensor(
            selected.mask_source_indices,
            torch::TensorOptions().dtype(torch::kInt64).device(source_masks.device()));
        {
            FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.relayout_masks");
            staged_masks = source_masks.index_select(0, mask_index).contiguous();
            mask_view.copy_(staged_masks, true);
        }
    }
    cudaEvent_t ready_event = nullptr;
    sync_status = cudaEventCreateWithFlags(&ready_event, cudaEventDisableTiming);
    if (sync_status != cudaSuccess) {
        throw std::runtime_error(std::string("cudaEventCreateWithFlags failed during mask staging: ") +
                                 cudaGetErrorString(sync_status));
    }
    sync_status = cudaEventRecord(ready_event, stream);
    if (sync_status != cudaSuccess) {
        cudaEventDestroy(ready_event);
        throw std::runtime_error(std::string("cudaEventRecord failed during mask staging: ") +
                                 cudaGetErrorString(sync_status));
    }

    return StagedPredictionBatch{
        std::move(selected.predictions),
        mask_view,
        std::move(staged_masks),
        mask_height,
        mask_width,
        std::make_unique<CudaEventHandle>(ready_event),
        std::move(lease),
    };
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

std::vector<Prediction> encode_staged_predictions(StagedPredictionBatch&& staged) {
    if (!staged.linear_masks_cpu.defined() || staged.predictions.empty()) {
        return std::move(staged.predictions);
    }

    if (staged.ready_event) {
        const auto sync_status = cudaEventSynchronize(staged.ready_event->get());
        if (sync_status != cudaSuccess) {
            throw std::runtime_error(std::string("cudaEventSynchronize failed during mask encode: ") +
                                     cudaGetErrorString(sync_status));
        }
        staged.pending_masks_gpu = torch::Tensor();
        staged.ready_event.reset();
    }

    FASTLOADER_NVTX_RANGE("encode_staged_predictions", NVTX_COLOR_ORANGE);
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.encode_masks");
    const uint32_t pixels_per_mask = staged.mask_height * staged.mask_width;
    const auto* staged_ptr = staged.linear_masks_cpu.data_ptr<bool>();
    for (size_t index = 0; index < staged.predictions.size(); ++index) {
        staged.predictions[index].mask = encode_mask_from_linear_data(
            staged_ptr + index * static_cast<size_t>(pixels_per_mask),
            staged.mask_height,
            staged.mask_width);
        staged.predictions[index].has_mask = true;
    }
    return std::move(staged.predictions);
}

CocoDataset CocoDataset::load_from_binary(const std::filesystem::path& path) {
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.load_from_binary");
    DatasetLoader::Config config;
    config.compiled_path = path.string();
    config.batch_size = 1;
    config.shuffle = false;
    DatasetLoader loader(config);

    CocoDataset out;
    const uint32_t num_images = static_cast<uint32_t>(loader.num_images());
    const uint32_t num_classes = loader.num_classes();

    out.image_ids_.reserve(num_images);
    out.category_names_.reserve(num_classes);
    for (uint32_t i = 0; i < num_classes; ++i) {
        out.category_names_.push_back(loader.class_name(i));
    }

    const auto* label_index = loader.label_index();
    const auto* label_data = loader.label_data();
    const auto* rle_data = loader.rle_data();

    for (uint32_t i = 0; i < num_images; ++i) {
        const int image_id = static_cast<int>(i + 1);
        out.image_id_to_index_[image_id] = out.image_ids_.size();
        out.image_ids_.push_back(image_id);

        const auto& entry = label_index[i];
        for (uint16_t j = 0; j < entry.num_instances; ++j) {
            const auto& packed = label_data[entry.label_begin + j];
            GroundTruthAnnotation gt;
            gt.image_id = image_id;
            gt.category_id = static_cast<int>(packed.class_id) + 1;
            gt.bbox_xyxy = {
                static_cast<float>(packed.bbox_x1),
                static_cast<float>(packed.bbox_y1),
                static_cast<float>(packed.bbox_x2),
                static_cast<float>(packed.bbox_y2)
            };
            if (packed.mask_rle_pairs > 0) {
                gt.has_mask = true;
                gt.mask.height = loader.image_height();
                gt.mask.width = loader.image_width();
                gt.mask.runs.reserve(packed.mask_rle_pairs);
                const size_t rle_start_index = packed.mask_rle_offset / sizeof(RLEPair);
                for (uint16_t k = 0; k < packed.mask_rle_pairs; ++k) {
                    const auto& rle = rle_data[rle_start_index + k];
                    gt.mask.runs.emplace_back(rle.start, rle.length);
                    gt.mask.area += rle.length;
                }
            }
            out.ground_truths_.push_back(std::move(gt));
        }
    }
    return out;
}

void CocoDataset::limit_images(size_t limit) {
    if (limit >= image_ids_.size()) {
        return;
    }
    const std::unordered_set<int> keep(
        image_ids_.begin(), image_ids_.begin() + static_cast<std::ptrdiff_t>(limit));
    image_ids_.resize(limit);
    image_id_to_index_.clear();
    for (size_t index = 0; index < image_ids_.size(); ++index) {
        image_id_to_index_[image_ids_[index]] = index;
    }
    ground_truths_.erase(
        std::remove_if(
            ground_truths_.begin(),
            ground_truths_.end(),
            [&keep](const GroundTruthAnnotation& gt) { return keep.find(gt.image_id) == keep.end(); }),
        ground_truths_.end());
    predictions_.erase(
        std::remove_if(
            predictions_.begin(),
            predictions_.end(),
            [&keep](const Prediction& prediction) { return keep.find(prediction.image_id) == keep.end(); }),
        predictions_.end());
}

bool CocoDataset::has_image(int image_id) const {
    return image_id_to_index_.find(image_id) != image_id_to_index_.end();
}

void CocoDataset::add_predictions(const std::vector<Prediction>& predictions) {
    predictions_.insert(predictions_.end(), predictions.begin(), predictions.end());
}

void CocoDataset::add_predictions(std::vector<Prediction>&& predictions) {
    predictions_.reserve(predictions_.size() + predictions.size());
    predictions_.insert(predictions_.end(),
                        std::make_move_iterator(predictions.begin()),
                        std::make_move_iterator(predictions.end()));
}

EvalSummary CocoDataset::evaluate(size_t max_dets_per_image) const {
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.total");
    EvalSummary summary;
    summary.bbox = compute_metric_summary(
        ground_truths_,
        predictions_,
        category_names_.size(),
        max_dets_per_image,
        [](const Prediction& prediction, const GroundTruthAnnotation& gt) {
            return bbox_iou(prediction.bbox_xyxy, gt.bbox_xyxy);
        });

    std::vector<GroundTruthAnnotation> mask_ground_truths;
    std::vector<Prediction> mask_predictions;
    mask_ground_truths.reserve(ground_truths_.size());
    mask_predictions.reserve(predictions_.size());
    for (const GroundTruthAnnotation& gt : ground_truths_) {
        if (gt.has_mask) {
            mask_ground_truths.push_back(gt);
        }
    }
    for (const Prediction& prediction : predictions_) {
        if (prediction.has_mask) {
            mask_predictions.push_back(prediction);
        }
    }
    summary.mask = compute_metric_summary(
        mask_ground_truths,
        mask_predictions,
        category_names_.size(),
        max_dets_per_image,
        [](const Prediction& prediction, const GroundTruthAnnotation& gt) {
            return mask_iou(prediction.mask, gt.mask);
        });
    return summary;
}

std::vector<Prediction> result_to_predictions(int image_id,
                                              const TensorMap& result,
                                              size_t category_count,
                                              size_t max_dets_per_image) {
    return valid_predictions(result, image_id, category_count, max_dets_per_image);
}

AlignmentStats compare_top1(const TensorMap& lhs, const TensorMap& rhs, size_t category_count) {
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.eval.align_top1");
    auto lhs_predictions = valid_predictions(lhs, 0, category_count, 1);
    auto rhs_predictions = valid_predictions(rhs, 0, category_count, 1);
    AlignmentStats stats;
    Prediction* lhs_top1 = first_valid_prediction(lhs_predictions);
    Prediction* rhs_top1 = first_valid_prediction(rhs_predictions);
    if (lhs_top1 == nullptr || rhs_top1 == nullptr) {
        return stats;
    }

    stats.images_compared = 1;
    const double score_diff = std::abs(static_cast<double>(lhs_top1->score) - static_cast<double>(rhs_top1->score));
    stats.top1_score_abs_diff_mean = score_diff;
    stats.top1_score_abs_diff_max = score_diff;

    double box_diff = 0.0;
    for (size_t index = 0; index < lhs_top1->bbox_xyxy.size(); ++index) {
        box_diff = std::max(
            box_diff,
            std::abs(static_cast<double>(lhs_top1->bbox_xyxy[index]) -
                     static_cast<double>(rhs_top1->bbox_xyxy[index])));
    }
    stats.top1_box_abs_diff_px_mean = box_diff;
    stats.top1_box_abs_diff_px_max = box_diff;

    if (lhs_top1->has_mask && rhs_top1->has_mask) {
        const uint32_t intersect = intersection_area(lhs_top1->mask, rhs_top1->mask);
        const double xor_pixels =
            static_cast<double>(lhs_top1->mask.area + rhs_top1->mask.area - 2 * intersect);
        stats.top1_mask_xor_pixels_mean = xor_pixels;
        stats.top1_mask_xor_pixels_max = xor_pixels;
    }
    return stats;
}

} // namespace fastloader::rfdetr
