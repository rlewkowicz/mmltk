namespace mmltk::backend::models::rfdetr {

#if defined(MMLTK_TRAINING_EVALUATION_RUNTIME)
namespace torch_api = mmltk::backend::ml::torch_api;
using evaluator_detail::checked_device_index;
using evaluator_detail::cudaError_t;
using evaluator_detail::cudaEvent_t;
using evaluator_detail::cudaEventCreate;
using evaluator_detail::cudaEventCreateWithFlags;
using evaluator_detail::cudaEventDestroy;
using evaluator_detail::cudaEventElapsedTime;
using evaluator_detail::cudaEventRecord;
using evaluator_detail::cudaEventSynchronize;
using evaluator_detail::cudaGetDevice;
using evaluator_detail::CudaGuard;
using evaluator_detail::cudaSetDevice;
using evaluator_detail::cudaStream_t;
using evaluator_detail::CudaStreamGuard;
using evaluator_detail::cudaStreamSynchronize;
using evaluator_detail::cudaSuccess;
using evaluator_detail::DeviceIndex;
using evaluator_detail::getStreamFromExternal;
using evaluator_detail::kCudaEventDisableTiming;

using mmltk::frameworks::gpu::ensure_cuda_ok;
#endif

#if defined(MMLTK_TRAINING_EVALUATION_RUNTIME)
void capture_sdp_backend_flags(EvaluationProfileRecord& profile) {
    const auto& context = at::globalContext();
    profile.sdp_flash_enabled = context.userEnabledFlashSDP();
    profile.sdp_mem_efficient_enabled = context.userEnabledMemEfficientSDP();
    profile.sdp_math_enabled = context.userEnabledMathSDP();
    profile.sdp_cudnn_enabled = context.userEnabledCuDNNSDP();
}

void accumulate_prediction_transfer_bytes(EvaluationProfileRecord& profile, const PostprocessedBatch& processed, const size_t image_count) {
    const size_t prediction_count = image_count * static_cast<size_t>(processed.scores.size(1));
    const size_t bbox_bytes =
        prediction_count * (static_cast<size_t>(processed.scores.element_size()) + static_cast<size_t>(processed.labels.element_size()) +
                            4U * static_cast<size_t>(processed.boxes.element_size()));
    size_t mask_bytes = 0;
    if (processed.masks.has_value()) {
        const size_t pixels_per_mask = static_cast<size_t>(processed.masks->size(2)) * static_cast<size_t>(processed.masks->size(3));
        mask_bytes = prediction_count * ((pixels_per_mask + 7U) / 8U);
    }
    profile.transferred_bytes += bbox_bytes + mask_bytes;
    profile.mask_transferred_bytes += mask_bytes;
}

const char* evaluation_precision_name(const at::ScalarType scalar_type) noexcept {
    switch (scalar_type) {
        case at::kFloat:
            return "fp32";
        case at::kHalf:
            return "fp16";
        case at::kBFloat16:
            return "bf16";
        default:
            return "other";
    }
}

EvaluationCudaBatchTiming::EvaluationCudaBatchTiming() {
    for (cudaEvent_t& event : events_) {
        const cudaError_t status = cudaEventCreate(&event);
        if (status != cudaSuccess) {
            destroy_events();
            ensure_cuda_ok(status, "cudaEventCreate for validation profile timing");
        }
    }
}

EvaluationCudaBatchTiming::~EvaluationCudaBatchTiming() { destroy_events(); }

void EvaluationCudaBatchTiming::record_start(const Phase phase, cudaStream_t stream) {
    ensure_cuda_ok(cudaEventRecord(events_[event_index(phase, false)], stream), "cudaEventRecord for validation profile phase start");
}

void EvaluationCudaBatchTiming::record_stop(const Phase phase, cudaStream_t stream) {
    ensure_cuda_ok(cudaEventRecord(events_[event_index(phase, true)], stream), "cudaEventRecord for validation profile phase stop");
}

void EvaluationCudaBatchTiming::accumulate(EvaluationProfileRecord& profile) const {
    ensure_cuda_ok(cudaEventSynchronize(events_[event_index(Phase::Postprocess, true)]),
                   "cudaEventSynchronize for validation profile timing");
    profile.preprocessing_seconds += elapsed_seconds(Phase::Preprocessing);
    profile.model_forward_seconds += elapsed_seconds(Phase::ModelForward);
    profile.postprocess_seconds += elapsed_seconds(Phase::Postprocess);
}

double EvaluationCudaBatchTiming::elapsed_seconds(const Phase phase) const {
    float elapsed_milliseconds = 0.0F;
    ensure_cuda_ok(cudaEventElapsedTime(&elapsed_milliseconds, events_[event_index(phase, false)], events_[event_index(phase, true)]),
                   "cudaEventElapsedTime for validation profile phase");
    return static_cast<double>(elapsed_milliseconds) / 1000.0;
}

void EvaluationCudaBatchTiming::destroy_events() noexcept {
    for (cudaEvent_t& event : events_) {
        if (event != nullptr) {
            cudaEventDestroy(event);
            event = nullptr;
        }
    }
}

EvaluationCudaTimingPool::EvaluationCudaTimingPool(const size_t slot_count) {
    if (slot_count == 0) { throw std::invalid_argument("validation CUDA timing pool requires at least one slot"); }
    slots_.reserve(slot_count);
    free_slots_.reserve(slot_count);
    for (size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
        slots_.push_back(std::make_unique<EvaluationCudaBatchTiming>());
        free_slots_.push_back(slot_count - slot_index - 1U);
    }
}

EvaluationCudaTimingLease EvaluationCudaTimingPool::acquire() {
    if (free_slots_.empty()) { throw std::logic_error("validation CUDA timing pool exhausted"); }
    const size_t slot_index = free_slots_.back();
    free_slots_.pop_back();
    return EvaluationCudaTimingLease{slots_[slot_index].get(), slot_index};
}

void EvaluationCudaTimingPool::release(EvaluationCudaTimingLease& lease) {
    if (!lease || lease.slot_index >= slots_.size() || slots_[lease.slot_index].get() != lease.timing) {
        throw std::logic_error("invalid validation CUDA timing lease release");
    }
    free_slots_.push_back(lease.slot_index);
    lease = {};
}
#endif

namespace {

constexpr auto kIouThresholds = kEvaluationIouThresholds;
constexpr double kRecallStep = 0.01;

double evaluation_box_area(const std::array<float, 4>& box) {
    return std::max(0.0, static_cast<double>(box[2]) - box[0]) * std::max(0.0, static_cast<double>(box[3]) - box[1]);
}

double bbox_iou(const std::array<float, 4>& lhs, const std::array<float, 4>& rhs) {
    const double left = std::max(lhs[0], rhs[0]), top = std::max(lhs[1], rhs[1]);
    const double right = std::min(lhs[2], rhs[2]), bottom = std::min(lhs[3], rhs[3]);
    const double intersect = std::max(0.0, right - left) * std::max(0.0, bottom - top);
    const double union_area = evaluation_box_area(lhs) + evaluation_box_area(rhs) - intersect;
    return union_area <= 0.0 ? 0.0 : intersect / union_area;
}

template <typename MaskValue>
EncodedMask encode_mask_from_linear_data(const MaskValue* data, uint32_t height, uint32_t width) {
    mmltk::common::logging::ScopedNvtxRange nvtx_encode_mask_from_linear_data{"encode_mask_from_linear_data",
                                                                              mmltk::common::logging::nvtx_color_blue};
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_encode_mask_linear{"rfdetr.native.eval.encode_mask_linear"};
    EncodedMask mask;
    encode_mask_values_into(height, width, mask, [data](const uint32_t index) { return static_cast<bool>(data[index]); });
    return mask;
}

void encode_mask_from_packed_data_into(const std::uint8_t* data, const uint32_t height, const uint32_t width, EncodedMask& mask) {
    mmltk::common::logging::ScopedNvtxRange nvtx_encode_mask_from_packed_data_into{"encode_mask_from_packed_data_into",
                                                                                   mmltk::common::logging::nvtx_color_blue};
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_encode_mask_packed_reuse{
        "rfdetr.native.eval.encode_mask_packed_reuse"};
    encode_mask_values_into(height, width, mask, [data](const uint32_t index) {
        return (data[index >> 3U] & static_cast<std::uint8_t>(1U << (index & 7U))) != 0;
    });
}

EncodedMask encode_mask_from_packed_data(const std::uint8_t* data, const uint32_t height, const uint32_t width) {
    EncodedMask mask;
    encode_mask_from_packed_data_into(data, height, width, mask);
    return mask;
}

uint32_t intersection_area_runs(const std::span<const std::pair<std::uint32_t, std::uint32_t>> lhs_runs,
                                const std::span<const std::pair<std::uint32_t, std::uint32_t>> rhs_runs) {
    size_t left_index = 0;
    size_t right_index = 0;
    uint32_t total = 0;
    while (left_index < lhs_runs.size() && right_index < rhs_runs.size()) {
        const auto [left_start, left_len] = lhs_runs[left_index];
        const auto [right_start, right_len] = rhs_runs[right_index];
        const uint32_t left_end = left_start + left_len;
        const uint32_t right_end = right_start + right_len;
        const uint32_t overlap_start = std::max(left_start, right_start);
        const uint32_t overlap_end = std::min(left_end, right_end);
        if (overlap_end > overlap_start) { total += overlap_end - overlap_start; }
        if (left_end <= right_end) {
            ++left_index;
        } else {
            ++right_index;
        }
    }
    return total;
}

uint32_t intersection_area(const EncodedMask& lhs, const EncodedMask& rhs) {
    mmltk::common::logging::ScopedNvtxRange nvtx_intersection_area{"intersection_area", mmltk::common::logging::nvtx_color_yellow};
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_intersection_area{"rfdetr.native.eval.intersection_area"};
    return intersection_area_runs(std::span{lhs.runs}, std::span{rhs.runs});
}

uint32_t intersection_area(const EncodedMask& lhs, const std::span<const std::pair<std::uint32_t, std::uint32_t>> rhs_runs) {
    mmltk::common::logging::ScopedNvtxRange nvtx_intersection_area_dense_ground_truth{"intersection_area_dense_ground_truth",
                                                                                      mmltk::common::logging::nvtx_color_yellow};
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_intersection_area_dense_ground_truth{
        "rfdetr.native.eval.intersection_area_dense_ground_truth"};
    return intersection_area_runs(std::span{lhs.runs}, rhs_runs);
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

#if defined(MMLTK_TRAINING_EVALUATION_RUNTIME)
struct SelectedPredictions {
    std::vector<Prediction> predictions;
    std::vector<int64_t> mask_source_indices;
};

SelectedPredictions select_predictions(int image_id, const torch_api::Tensor& scores, const torch_api::Tensor& labels,
                                       const torch_api::Tensor& boxes, size_t category_count, size_t max_dets_per_image) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_select_predictions{"rfdetr.native.eval.select_predictions"};
    const auto* score_ptr = scores.data_ptr<float>();
    const auto* label_ptr = labels.data_ptr<int64_t>();
    const auto* box_ptr = boxes.data_ptr<float>();
    const int64_t total = scores.size(0);

    SelectedPredictions out;
    out.predictions.reserve(std::min<size_t>(static_cast<size_t>(total), max_dets_per_image));
    out.mask_source_indices.reserve(out.predictions.capacity());
    for (int64_t index = 0; index < total; ++index) {
        const int64_t label = label_ptr[index];
        if (label < 0 || label >= static_cast<int64_t>(category_count)) { continue; }
        Prediction prediction;
        prediction.image_id = image_id;
        prediction.class_reference = static_cast<int>(label);
        prediction.score = score_ptr[index];
        prediction.bbox_xyxy = xyxy_clamped(box_ptr + index * 4);
        out.predictions.push_back(std::move(prediction));
        out.mask_source_indices.push_back(index);
        if (out.predictions.size() >= max_dets_per_image) { break; }
    }
    return out;
}
#endif

constexpr std::uint16_t kAllThresholdBits = static_cast<std::uint16_t>((std::uint16_t{1} << std::size(kIouThresholds)) - 1U);

struct MatchCandidate {
    double iou = 0.0;
    std::uint32_t ground_truth_index = 0;
    std::uint32_t ground_truth_ordinal = 0;
    std::uint16_t eligible_threshold_bits = 0;
};

struct ImageMatchingScratch {
    std::vector<std::vector<std::uint32_t>> predictions_by_category;
    std::vector<std::array<std::uint16_t, 4>> unmatched_threshold_bits;
    std::vector<MatchCandidate> candidates;
    std::vector<std::array<float, 4>> staged_boxes;
    std::vector<EncodedMask> staged_masks;
};

ImageMatchingScratch& image_matching_scratch(const size_t category_count) {
    thread_local ImageMatchingScratch scratch;
    scratch.predictions_by_category.resize(category_count);
    for (auto& predictions : scratch.predictions_by_category) {
        predictions.clear();
    }
    return scratch;
}

std::uint16_t eligible_threshold_bits(const double iou) {
    std::uint16_t bits = 0;
    for (size_t threshold_index = 0; threshold_index < std::size(kIouThresholds); ++threshold_index) {
        if (iou >= kIouThresholds[threshold_index]) {
            bits |= static_cast<std::uint16_t>(std::uint16_t{1} << threshold_index);
        }
    }
    return bits;
}

size_t group_staged_predictions_by_category(const BBoxPredictionView& predictions, const size_t category_count,
                                            const size_t max_dets_per_image, ImageMatchingScratch& scratch) {
    size_t selected_count = 0;
    for (size_t prediction_index = 0; prediction_index < predictions.count; ++prediction_index) {
        const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>(prediction_index);
        const std::int64_t label = predictions.labels_zero_based[offset * predictions.label_stride];
        if (label < 0 || label >= static_cast<std::int64_t>(category_count)) { continue; }
        auto& category_predictions = scratch.predictions_by_category[static_cast<size_t>(label)];
        category_predictions.push_back(static_cast<std::uint32_t>(prediction_index));
    }
    const auto score_order = [&predictions](const std::uint32_t lhs_index, const std::uint32_t rhs_index) {
        const float lhs_score = predictions.scores[static_cast<std::ptrdiff_t>(lhs_index) * predictions.score_stride];
        const float rhs_score = predictions.scores[static_cast<std::ptrdiff_t>(rhs_index) * predictions.score_stride];
        if (std::isnan(lhs_score) != std::isnan(rhs_score)) return !std::isnan(lhs_score);
        return lhs_score > rhs_score || ((lhs_score == rhs_score || std::isnan(lhs_score)) && lhs_index < rhs_index);
    };
    for (auto& category_predictions : scratch.predictions_by_category) {
        std::ranges::sort(category_predictions, score_order);
        if (category_predictions.size() > max_dets_per_image) category_predictions.resize(max_dets_per_image);
        selected_count += category_predictions.size();
    }
    return selected_count;
}

// COCO ranges overlap exactly at 32² and 96². Compiled annotations do not
// contain crowd/ignore flags; only the supported area exclusion is represented.
bool evaluation_area_contains(const size_t area, const double value) {
    constexpr std::array<double, 4> minimum{0.0, 0.0, 1024.0, 9216.0};
    constexpr std::array<double, 4> maximum{1.0e10, 1024.0, 9216.0, 1.0e10};
    return value >= minimum[area] && value <= maximum[area];
}

template <typename ScoreFn, typename IoUFn, typename GroundTruthAreaFn, typename PredictionAreaFn>
void match_category_predictions(const std::vector<std::uint32_t>& prediction_indices,
                                const std::span<const std::uint32_t> ground_truth_ordinals, const std::uint32_t image_ordinal,
                                const std::uint16_t category_index, ImageMatchingScratch& scratch,
                                std::vector<CompactImageMatchRecord>& output, size_t& iou_candidate_count, ScoreFn&& score_fn,
                                IoUFn&& iou_fn, GroundTruthAreaFn&& ground_truth_area, PredictionAreaFn&& prediction_area) {
    scratch.unmatched_threshold_bits.assign(ground_truth_ordinals.size(), {kAllThresholdBits, kAllThresholdBits, kAllThresholdBits, kAllThresholdBits});
    scratch.candidates.reserve(ground_truth_ordinals.size());
    std::uint32_t rank = 0;
    for (const std::uint32_t prediction_index : prediction_indices) {
        scratch.candidates.clear();
        for (size_t ground_truth_index = 0; ground_truth_index < ground_truth_ordinals.size(); ++ground_truth_index) {
            ++iou_candidate_count;
            const double iou = iou_fn(prediction_index, ground_truth_index);
            if (!(iou >= kIouThresholds.front())) continue;
            scratch.candidates.push_back({iou, static_cast<std::uint32_t>(ground_truth_index),
                                          ground_truth_ordinals[ground_truth_index], eligible_threshold_bits(iou)});
        }
        // evaluateImg replaces a previous match on equal IoU, so the last GT
        // in stable annotation order wins within each ignore group.
        std::ranges::sort(scratch.candidates, [](const MatchCandidate& lhs, const MatchCandidate& rhs) {
            return lhs.iou > rhs.iou || (lhs.iou == rhs.iou && lhs.ground_truth_ordinal > rhs.ground_truth_ordinal);
        });
        CompactImageMatchRecord record;
        record.score = score_fn(prediction_index);
        record.image_ordinal = image_ordinal;
        record.prediction_ordinal = prediction_index;
        record.category_index = category_index;
        record.category_rank = rank++;
        for (size_t area = 0; area < 4U; ++area) {
            std::uint16_t matched = 0;
            std::uint16_t ignored = 0;
            // Nonignored GT always takes precedence over ignored GT, even if
            // the latter has a larger IoU. Every area owns independent state.
            for (const bool ignore_group : {false, true}) {
                for (const auto& candidate : scratch.candidates) {
                    if ((!evaluation_area_contains(area, ground_truth_area(candidate.ground_truth_index))) != ignore_group) continue;
                    auto& unmatched = scratch.unmatched_threshold_bits[candidate.ground_truth_index][area];
                    const auto available = static_cast<std::uint16_t>(unmatched & candidate.eligible_threshold_bits & ~matched);
                    matched |= available;
                    unmatched &= static_cast<std::uint16_t>(~available);
                    if (ignore_group) ignored |= available;
                }
            }
            if (!evaluation_area_contains(area, prediction_area(prediction_index))) ignored |= static_cast<std::uint16_t>(kAllThresholdBits & ~matched);
            record.area_matched_bits[area] = matched;
            record.area_ignored_bits[area] = ignored;
        }
        output.push_back(record);
    }
}

ConfidenceMetrics confidence_metrics(const size_t tp, const size_t count, const size_t gt) {
    ConfidenceMetrics result;
    result.precision = count == 0U ? 0.0 : static_cast<double>(tp) / static_cast<double>(count);
    result.recall = gt == 0U ? 0.0 : static_cast<double>(tp) / static_cast<double>(gt);
    const double denominator = result.precision + result.recall;
    result.f1 = denominator == 0.0 ? 0.0 : 2.0 * result.precision * result.recall / denominator;
    return result;
}

template <typename CategoryScratch>
void reduce_category_matches(std::vector<CompactImageMatchRecord>& matches, const std::array<size_t, 4>& ground_truth_count,
                             const std::array<std::uint32_t, 3>& caps, CategoryScratch& scratch) {
    std::ranges::sort(matches, [](const CompactImageMatchRecord& lhs, const CompactImageMatchRecord& rhs) {
        if (std::isnan(lhs.score) != std::isnan(rhs.score)) return !std::isnan(lhs.score);
        if (!std::isnan(lhs.score) && lhs.score != rhs.score) return lhs.score > rhs.score;
        if (lhs.image_ordinal != rhs.image_ordinal) return lhs.image_ordinal < rhs.image_ordinal;
        return lhs.prediction_ordinal < rhs.prediction_ordinal;
    });
    scratch.precisions.reserve(matches.size());
    for (size_t area = 0; area < 4U; ++area) {
        auto& detail = *scratch.areas[area];
        detail.area = static_cast<EvaluationArea>(area);
        detail.ground_truth_count = ground_truth_count[area];
        detail.available = ground_truth_count[area] != 0U;
        detail.detection_limits = caps;
        if (!detail.available) continue;
        for (size_t threshold = 0; threshold < kIouThresholds.size(); ++threshold) {
            const auto bit = static_cast<std::uint16_t>(1U << threshold);
            scratch.precisions.clear();
            scratch.recall_indices.fill(matches.size());
            size_t next_recall = 0U;
            std::array<size_t, 3> true_positives{};
            for (const auto& match : matches) {
                if ((match.area_ignored_bits[area] & bit) != 0U) continue;
                const bool tp = (match.area_matched_bits[area] & bit) != 0U;
                for (size_t cap = 0; cap < caps.size(); ++cap)
                    if (match.category_rank < caps[cap]) true_positives[cap] += tp ? 1U : 0U;
                if (match.category_rank >= caps.back()) continue;
                scratch.precisions.push_back(static_cast<double>(true_positives.back()) / static_cast<double>(scratch.precisions.size() + 1U));
                const double recall = static_cast<double>(true_positives.back()) / static_cast<double>(ground_truth_count[area]);
                while (next_recall < 101U && recall >= static_cast<double>(next_recall) * kRecallStep)
                    scratch.recall_indices[next_recall++] = scratch.precisions.size() - 1U;
            }
            for (size_t index = scratch.precisions.size(); index > 1U; --index)
                scratch.precisions[index - 2U] = std::max(scratch.precisions[index - 2U], scratch.precisions[index - 1U]);
            for (size_t recall = 0; recall < 101U; ++recall) {
                const size_t index = scratch.recall_indices[recall];
                const double precision = index < scratch.precisions.size() ? scratch.precisions[index] : 0.0;
                detail.precision_curve[threshold][recall] = precision;
                detail.average_precision[threshold] += precision / 101.0;
            }
            for (size_t cap = 0; cap < caps.size(); ++cap)
                detail.average_recall[cap][threshold] = static_cast<double>(true_positives[cap]) / static_cast<double>(ground_truth_count[area]);
        }
    }
    // One monotonic cursor, with scores widened before the inclusive comparison.
    // This is the upstream float64 sweep, without 101 detection rescans.
    size_t cursor = 0, tp = 0, count = 0;
    for (size_t reverse = 101U; reverse > 0U; --reverse) {
        const size_t threshold_index = reverse - 1U;
        const double threshold = static_cast<double>(threshold_index) * 0.01;
        while (cursor < matches.size() && static_cast<double>(matches[cursor].score) >= threshold) {
            const auto& match = matches[cursor++];
            if (match.category_rank >= caps.back() || (match.area_ignored_bits[0] & 1U) != 0U) continue;
            ++count;
            tp += (match.area_matched_bits[0] & 1U) != 0U ? 1U : 0U;
        }
        scratch.confidence[threshold_index] = confidence_metrics(tp, count, ground_truth_count[0]);
    }
}

#if defined(MMLTK_TRAINING_EVALUATION_RUNTIME)
std::vector<Prediction> valid_predictions(const TensorMap& result, int image_id, size_t category_count, size_t max_dets_per_image,
                                          EvaluationProfileRecord* profile = nullptr) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_result_to_predictions{"rfdetr.native.eval.result_to_predictions"};
    torch_api::Tensor scores;
    torch_api::Tensor labels;
    torch_api::Tensor boxes;
    const auto bbox_transfer_started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_d2h_boxes_labels_scores{
            "rfdetr.native.eval.d2h_boxes_labels_scores"};
        scores = mmltk::backend::ml::cuda::numa_readback(result.at("scores")).contiguous();
        labels = mmltk::backend::ml::cuda::numa_readback(result.at("labels")).contiguous();
        boxes = mmltk::backend::ml::cuda::numa_readback(result.at("boxes")).contiguous();
    }
    if (profile != nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - bbox_transfer_started);
        profile->d2h_wait_nanoseconds.fetch_add(static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
    }
    const bool has_masks = result.contains("masks");
    const auto select_started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SelectedPredictions selected = select_predictions(image_id, scores, labels, boxes, category_count, max_dets_per_image);
    if (!has_masks || selected.predictions.empty()) {
        if (profile != nullptr) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - select_started);
            profile->cpu_encode_nanoseconds.fetch_add(static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
        }
        return selected.predictions;
    }
    if (profile != nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - select_started);
        profile->cpu_encode_nanoseconds.fetch_add(static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
    }

    torch_api::Tensor source_masks = result.at("masks");
    if (source_masks.dim() == 4 && source_masks.size(1) == 1) { source_masks = source_masks.squeeze(1); }
    if (source_masks.dim() != 3) { throw std::runtime_error("predicted masks must be [num_predictions,height,width]"); }

    torch_api::Tensor mask_index;
    const auto mask_transfer_started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_d2h_masks{"rfdetr.native.eval.d2h_masks"};
        mask_index = torch_api::tensor(selected.mask_source_indices,
                                       torch_api::TensorOptions().dtype(torch_api::kInt64).device(source_masks.device()));
        source_masks = mmltk::backend::ml::cuda::numa_readback(source_masks.index_select(0, mask_index));
    }
    if (profile != nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mask_transfer_started);
        profile->d2h_wait_nanoseconds.fetch_add(static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
    }

    const auto mask_encode_started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    torch_api::Tensor staged_masks;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_relayout_masks{"rfdetr.native.eval.relayout_masks"};
        staged_masks = source_masks.contiguous();
    }

    const auto height = static_cast<uint32_t>(source_masks.size(1));
    const auto width = static_cast<uint32_t>(source_masks.size(2));
    const uint32_t pixels_per_mask = width * height;
    const auto* staged_ptr = staged_masks.data_ptr<bool>();
    std::vector<EncodedMask> encoded_masks(selected.predictions.size());
    {
        mmltk::common::logging::ScopedNvtxRange nvtx_encode_masks_loop{"encode_masks_loop", mmltk::common::logging::nvtx_color_orange};
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_encode_masks{"rfdetr.native.eval.encode_masks"};
        for (size_t index = 0; index < selected.predictions.size(); ++index) {
            encoded_masks[index] = encode_mask_from_linear_data(staged_ptr + index * static_cast<size_t>(pixels_per_mask), height, width);
        }
    }

    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_accumulate_predictions{
            "rfdetr.native.eval.accumulate_predictions"};
        for (size_t index = 0; index < selected.predictions.size(); ++index) {
            selected.predictions[index].mask = std::move(encoded_masks[index]);
            selected.predictions[index].has_mask = true;
        }
    }
    if (profile != nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mask_encode_started);
        profile->cpu_encode_nanoseconds.fetch_add(static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
    }
    return selected.predictions;
}

Prediction* first_valid_prediction(std::vector<Prediction>& predictions) {
    if (predictions.empty()) { return nullptr; }
    return &predictions.front();
}
#endif

}  // namespace

#if defined(MMLTK_TRAINING_EVALUATION_RUNTIME)
PredictionBufferSlotPool::PredictionBufferSlotPool(const size_t slot_count, const PredictionBufferConfig& config) {
    if (slot_count == 0) { throw std::runtime_error("prediction buffer slot pool requires at least one slot"); }
    if (config.batch_capacity <= 0 || config.prediction_capacity <= 0 || config.device_id < 0) {
        throw std::invalid_argument("prediction buffer configuration requires positive batch and prediction capacity");
    }
    if (config.mask_shape && (config.mask_shape->first == 0 || config.mask_shape->second == 0)) {
        throw std::invalid_argument("prediction mask buffer configuration requires positive dimensions");
    }
    CudaGuard device_guard(checked_device_index(config.device_id));
    slots_.reserve(slot_count);
    const auto& mask_shape = config.mask_shape;
    for (size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
        auto slot = std::make_shared<PinnedPredictionBuffers>();
        slot->allow_mask_reconfiguration = config.allow_mask_reconfiguration;
        slot->bbox.ensure_capacity(config.batch_capacity, config.prediction_capacity);
        if (mask_shape) {
            PinnedMaskPredictionBuffers& mask_buffers = slot->mask.emplace();
            mask_buffers.ensure_capacity(config.batch_capacity, config.prediction_capacity, config.batch_capacity,
                                         config.prediction_capacity, mask_shape->first, mask_shape->second, config.device_id);
        }
        slot->ensure_ready_event(config.device_id);
        slots_.push_back(std::move(slot));
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
    lease.buffers->transition(PredictionSlotState::Free, PredictionSlotState::GpuFilling, "prediction slot acquisition");
    lease.release_guard = std::shared_ptr<void>(nullptr, [pool = shared_from_this(), slot_index](void*) { pool->release(slot_index); });
    return lease;
}

void PredictionBufferSlotPool::release(size_t slot_index) {
    const std::shared_ptr<PinnedPredictionBuffers>& slot = slots_.at(slot_index);
    if (slot->state.load(std::memory_order_acquire) == PredictionSlotState::D2HPending && slot->ready_event != nullptr) {
        CudaGuard device_guard(checked_device_index(slot->event_device_id));
        static_cast<void>(cudaEventSynchronize(slot->ready_event));
    }
    slot->state.store(PredictionSlotState::Free, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        free_slots_.push_back(slot_index);
    }
    cv_.notify_one();
}

PinnedPredictionBuffers::~PinnedPredictionBuffers() {
    if (ready_event != nullptr) {
        int previous_device_id = -1;
        const bool restore_device = event_device_id >= 0 && cudaGetDevice(&previous_device_id) == cudaSuccess &&
                                    previous_device_id != event_device_id && cudaSetDevice(event_device_id) == cudaSuccess;
        cudaEventDestroy(ready_event);
        if (restore_device) { cudaSetDevice(previous_device_id); }
        ready_event = nullptr;
    }
}

void PinnedBBoxPredictionBuffers::ensure_capacity(int64_t batch_count, int64_t prediction_count) {
    if (batch_capacity >= batch_count && prediction_capacity >= prediction_count && scores_cpu.defined() && labels_cpu.defined() &&
        boxes_cpu.defined()) {
        return;
    }
    batch_capacity = std::max<int64_t>(batch_capacity, batch_count);
    prediction_capacity = std::max<int64_t>(prediction_capacity, prediction_count);
    scores_cpu = mmltk::backend::ml::cuda::numa_empty({batch_capacity, prediction_capacity}, torch_api::kFloat32);
    labels_cpu = mmltk::backend::ml::cuda::numa_empty({batch_capacity, prediction_capacity}, torch_api::kInt64);
    boxes_cpu = mmltk::backend::ml::cuda::numa_empty({batch_capacity, prediction_capacity, 4}, torch_api::kFloat32);
}

void PinnedMaskPredictionBuffers::ensure_capacity(const int64_t batch_count, const int64_t prediction_count, const int64_t batch_capacity,
                                                  const int64_t prediction_capacity, const uint32_t height, const uint32_t width,
                                                  const int device_id) {
    const int64_t required_bytes = (mmltk::common::math::checked_cast<int64_t>(height, "prediction mask height overflow") *
                                        mmltk::common::math::checked_cast<int64_t>(width, "prediction mask width overflow") +
                                    7) /
                                   8;
    if (masks_cpu.defined() && masks_cpu.dim() == 3 && masks_cpu.size(0) >= batch_count && masks_cpu.size(1) >= prediction_count &&
        masks_gpu.defined() && masks_gpu.device().is_cuda() && masks_gpu.get_device() == device_id && mask_height == height &&
        mask_width == width && packed_mask_bytes == required_bytes) {
        return;
    }
    mask_height = height;
    mask_width = width;
    packed_mask_bytes = required_bytes;
    const std::vector<int64_t> packed_shape{batch_capacity, prediction_capacity, packed_mask_bytes};
    masks_cpu = mmltk::backend::ml::cuda::numa_empty(packed_shape, torch_api::kUInt8, device_id);
    masks_gpu = torch_api::empty(packed_shape, torch_api::TensorOptions().dtype(torch_api::kUInt8).device(torch_api::kCUDA, device_id));
}

void PinnedPredictionBuffers::ensure_ready_event(int device_id) {
    if (ready_event != nullptr && event_device_id == device_id) { return; }
    if (ready_event != nullptr) {
        CudaGuard previous_device_guard(checked_device_index(event_device_id));
        ensure_cuda_ok(cudaEventDestroy(ready_event), "cudaEventDestroy for prediction staging slot");
        ready_event = nullptr;
    }
    CudaGuard device_guard(checked_device_index(device_id));
    ensure_cuda_ok(cudaEventCreateWithFlags(&ready_event, kCudaEventDisableTiming), "cudaEventCreateWithFlags for prediction staging slot");
    event_device_id = device_id;
}

void PinnedPredictionBuffers::transition(const PredictionSlotState expected, const PredictionSlotState next, const char* operation) {
    PredictionSlotState observed = expected;
    if (!state.compare_exchange_strong(observed, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
        throw std::logic_error(std::string(operation) + " encountered an invalid prediction slot state");
    }
}

// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
StagedPredictionBatch stage_prediction_batch(std::vector<PredictionBatchMetadata> images, PostprocessedBatch batch, size_t category_count,
                                             size_t max_dets_per_image, PredictionBufferLease lease, int device_id, void* stream_handle) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_stage_prediction_batch{"rfdetr.native.eval.stage_prediction_batch"};
    if (!lease.buffers) { throw std::runtime_error("stage_prediction_batch requires a valid prediction buffer lease"); }
    if (!batch.scores.defined() || batch.scores.dim() != 2 || !batch.labels.defined() || batch.labels.dim() != 2 ||
        !batch.boxes.defined() || batch.boxes.dim() != 3 || batch.boxes.size(2) != 4) {
        throw std::runtime_error("staged prediction tensors must be scores[B,K], labels[B,K], and boxes[B,K,4]");
    }
    const int64_t batch_count = mmltk::common::math::checked_cast<int64_t>(images.size(), "prediction batch size overflow");
    const int64_t prediction_count = batch.scores.size(1);
    if (batch_count <= 0 || batch.size() < batch_count || batch.labels.size(0) < batch_count || batch.labels.size(1) != prediction_count ||
        batch.boxes.size(0) < batch_count || batch.boxes.size(1) != prediction_count) {
        throw std::runtime_error("prediction metadata and staged tensor shapes are not aligned");
    }
    PinnedPredictionBuffers& buffers = *lease.buffers;
    if (buffers.bbox.batch_capacity < batch_count || buffers.bbox.prediction_capacity < prediction_count ||
        !buffers.bbox.scores_cpu.defined() || !buffers.bbox.labels_cpu.defined() || !buffers.bbox.boxes_cpu.defined()) {
        throw std::runtime_error("prediction batch exceeds the configured bbox staging capacity");
    }
    if (buffers.ready_event == nullptr || buffers.event_device_id != device_id) {
        throw std::runtime_error("prediction batch device does not match the configured staging slot");
    }

    const auto score_view = buffers.bbox.scores_cpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
    const auto label_view = buffers.bbox.labels_cpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
    const auto box_view = buffers.bbox.boxes_cpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
    if (score_view.stride(1) != 1 || label_view.stride(1) != 1 || box_view.stride(1) != 4 || box_view.stride(2) != 1) {
        throw std::logic_error("pinned prediction bbox buffers have unexpected strides");
    }

    auto stream = reinterpret_cast<cudaStream_t>(stream_handle);
    if (stream == nullptr) { throw std::runtime_error("stage_prediction_batch requires a CUDA stream"); }

    std::optional<StagedMaskPredictionBatch> staged_mask;
    try {
        const DeviceIndex checked_index = checked_device_index(device_id);
        CudaGuard device_guard(checked_index);
        CudaStreamGuard stream_guard(getStreamFromExternal(stream, checked_index));
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_d2h_boxes_labels_scores{
                "rfdetr.native.eval.d2h_boxes_labels_scores"};
            score_view.copy_(batch.scores.narrow(0, 0, batch_count), true);
            label_view.copy_(batch.labels.narrow(0, 0, batch_count), true);
            box_view.copy_(batch.boxes.narrow(0, 0, batch_count), true);
        }
        if (batch.masks.has_value()) {
            const torch_api::Tensor& source_masks = *batch.masks;
            if (source_masks.dim() != 4 || source_masks.size(0) < batch_count || source_masks.size(1) != prediction_count) {
                throw std::runtime_error("predicted masks must be [batch,num_predictions,height,width]");
            }
            const uint32_t mask_height =
                mmltk::common::math::checked_cast<uint32_t>(source_masks.size(2), "prediction mask height overflow");
            const uint32_t mask_width = mmltk::common::math::checked_cast<uint32_t>(source_masks.size(3), "prediction mask width overflow");
            const bool mask_configuration_matches =
                buffers.mask && buffers.mask->masks_cpu.defined() && buffers.mask->masks_gpu.defined() &&
                buffers.mask->masks_gpu.device().is_cuda() && buffers.mask->masks_gpu.get_device() == device_id &&
                buffers.mask->mask_height == mask_height && buffers.mask->mask_width == mask_width &&
                buffers.mask->masks_cpu.size(0) >= batch_count && buffers.mask->masks_cpu.size(1) >= prediction_count;
            if (!mask_configuration_matches) {
                if (!buffers.allow_mask_reconfiguration) {
                    throw std::runtime_error("prediction masks do not match the configured staging capacity");
                }
                if (!buffers.mask) { buffers.mask.emplace(); }
                buffers.mask->ensure_capacity(batch_count, prediction_count, buffers.bbox.batch_capacity, buffers.bbox.prediction_capacity,
                                              mask_height, mask_width, device_id);
            }
            auto mask_view = buffers.mask->masks_cpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
            auto packed_gpu_view = buffers.mask->masks_gpu.narrow(0, 0, batch_count).narrow(1, 0, prediction_count);
            if (mask_view.stride(1) != buffers.mask->packed_mask_bytes || mask_view.stride(2) != 1 || !packed_gpu_view.is_contiguous()) {
                throw std::logic_error("pinned prediction mask buffer has unexpected strides");
            }
            mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_d2h_masks{"rfdetr.native.eval.d2h_masks"};
            pack_bool_masks_cuda_into(source_masks.narrow(0, 0, batch_count), packed_gpu_view);
            mask_view.copy_(packed_gpu_view, true);
            staged_mask = StagedMaskPredictionBatch{std::move(mask_view), mask_height, mask_width};
        } else if (buffers.mask) {
            throw std::runtime_error("prediction batch omitted masks required by the configured staging slot");
        }
        ensure_cuda_ok(cudaEventRecord(buffers.ready_event, stream), "cudaEventRecord for prediction staging slot");
        buffers.transition(PredictionSlotState::GpuFilling, PredictionSlotState::D2HPending, "prediction D2H submission");
    } catch (...) {
        static_cast<void>(cudaStreamSynchronize(stream));
        throw;
    }

    return StagedPredictionBatch{
        std::move(images),
        StagedBBoxPredictionBatch{score_view, label_view, box_view},
        std::move(staged_mask),
        std::move(batch),
        category_count,
        max_dets_per_image,
        static_cast<size_t>(batch_count),
        std::move(lease),
    };
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

namespace {

void synchronize_staged_prediction_batch(StagedPredictionBatch& staged, EvaluationProfileRecord* profile) {
    if (!staged.lease.buffers || staged.lease.buffers->ready_event == nullptr) {
        throw std::runtime_error("encoded prediction batch is missing its staging event");
    }
    PinnedPredictionBuffers& buffers = *staged.lease.buffers;
    std::lock_guard<std::mutex> consume_lock(buffers.consume_mutex);
    if (buffers.state.load(std::memory_order_acquire) == PredictionSlotState::CpuMatching) { return; }
    CudaGuard device_guard(checked_device_index(buffers.event_device_id));
    const auto started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ensure_cuda_ok(cudaEventSynchronize(buffers.ready_event), "cudaEventSynchronize for prediction staging slot");
    buffers.transition(PredictionSlotState::D2HPending, PredictionSlotState::CpuMatching, "prediction CPU consumption");
    if (profile != nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
        profile->d2h_wait_nanoseconds.fetch_add(static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
    }
    staged.pending_gpu = {};
}

PredictionBatchItem encode_staged_prediction_image(StagedPredictionBatch& staged, const size_t image_index,
                                                   EvaluationProfileRecord* profile, const EvaluationDatasetOwner* evaluation_dataset) {
    mmltk::common::logging::ScopedNvtxRange nvtx_encode_staged_prediction_image{"encode_staged_prediction_image",
                                                                                mmltk::common::logging::nvtx_color_orange};
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_consume_staged_image{"rfdetr.native.eval.consume_staged_image"};
    if (image_index >= staged.active_image_count || image_index >= staged.images.size()) {
        throw std::out_of_range("staged prediction image index exceeds the active image count");
    }
    synchronize_staged_prediction_batch(staged, profile);

    PredictionBatchItem result;
    result.dataset_index = staged.images[image_index].dataset_index;
    result.image_id = staged.images[image_index].image_id;
    result.source_name = std::move(staged.images[image_index].source_name);
    if (evaluation_dataset != nullptr) {
        const auto image_offset = static_cast<int64_t>(image_index);
        const BBoxPredictionView bbox_view{
            static_cast<int>(result.image_id),
            staged.bbox.scores_cpu.data_ptr<float>() + image_offset * staged.bbox.scores_cpu.stride(0),
            staged.bbox.labels_cpu.data_ptr<std::int64_t>() + image_offset * staged.bbox.labels_cpu.stride(0),
            staged.bbox.boxes_cpu.data_ptr<float>() + image_offset * staged.bbox.boxes_cpu.stride(0),
            static_cast<size_t>(staged.bbox.scores_cpu.size(1)),
            staged.bbox.scores_cpu.stride(1),
            staged.bbox.labels_cpu.stride(1),
            staged.bbox.boxes_cpu.stride(1),
        };
        std::optional<PackedMaskPredictionView> mask_view;
        if (staged.mask) {
            mask_view = PackedMaskPredictionView{
                staged.mask->masks_cpu.data_ptr<std::uint8_t>() + image_offset * staged.mask->masks_cpu.stride(0),
                staged.mask->masks_cpu.stride(1),
                staged.mask->height,
                staged.mask->width,
            };
        }
        const auto match_started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        result.evaluation_matches =
            evaluation_dataset->match_predictions(result.dataset_index, bbox_view, mask_view, staged.max_dets_per_image);
        if (profile != nullptr) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - match_started);
            profile->cpu_match_nanoseconds.fetch_add(static_cast<std::uint64_t>(elapsed.count()), std::memory_order_relaxed);
            profile->iou_candidate_count.fetch_add(
                result.evaluation_matches->bbox_iou_candidate_count + result.evaluation_matches->mask_iou_candidate_count,
                std::memory_order_relaxed);
            profile->mask_iou_candidate_count.fetch_add(result.evaluation_matches->mask_iou_candidate_count, std::memory_order_relaxed);
            if (mask_view) profile->mask_task_count.fetch_add(1U, std::memory_order_relaxed);
        }
        return result;
    }

    const bool has_masks = staged.mask.has_value();
    const std::uint8_t* mask_values = has_masks ? staged.mask->masks_cpu.data_ptr<std::uint8_t>() : nullptr;
    const int64_t mask_batch_stride = has_masks ? staged.mask->masks_cpu.stride(0) : 0;
    const int64_t mask_prediction_stride = has_masks ? staged.mask->masks_cpu.stride(1) : 0;

    const auto started = profile != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SelectedPredictions selected = select_predictions(
        static_cast<int>(staged.images[image_index].image_id), staged.bbox.scores_cpu.select(0, static_cast<int64_t>(image_index)),
        staged.bbox.labels_cpu.select(0, static_cast<int64_t>(image_index)),
        staged.bbox.boxes_cpu.select(0, static_cast<int64_t>(image_index)), staged.category_count, staged.max_dets_per_image);
    if (has_masks) {
        for (size_t prediction_index = 0; prediction_index < selected.predictions.size(); ++prediction_index) {
            const int64_t source_index = selected.mask_source_indices[prediction_index];
            const std::uint8_t* source =
                mask_values + static_cast<int64_t>(image_index) * mask_batch_stride + source_index * mask_prediction_stride;
            selected.predictions[prediction_index].mask = encode_mask_from_packed_data(source, staged.mask->height, staged.mask->width);
            selected.predictions[prediction_index].has_mask = true;
        }
    }

    result.predictions = std::move(selected.predictions);
    if (profile != nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
        profile->cpu_encode_nanoseconds.fetch_add(static_cast<uint64_t>(elapsed.count()), std::memory_order_relaxed);
    }
    return result;
}

}  // namespace

PendingPredictionBatchEncoding enqueue_prediction_batch_encoding(mmltk::common::concurrency::WorkerPool& cpu_pool,
                                                                 StagedPredictionBatch&& staged, EvaluationProfileRecord* profile,
                                                                 const EvaluationDatasetOwner* evaluation_dataset) {
    auto shared_staged = std::make_shared<StagedPredictionBatch>(std::move(staged));

    PendingPredictionBatchEncoding pending;
    pending.images.reserve(shared_staged->active_image_count);
    for (size_t image_index = 0; image_index < shared_staged->active_image_count; ++image_index) {
        pending.images.push_back(cpu_pool.enqueue([shared_staged, image_index, profile, evaluation_dataset] {
            return encode_staged_prediction_image(*shared_staged, image_index, profile, evaluation_dataset);
        }));
    }
    return pending;
}

std::vector<PredictionBatchItem> collect_prediction_batch_encoding(PendingPredictionBatchEncoding&& pending) {
    std::vector<PredictionBatchItem> results;
    results.reserve(pending.images.size());
    for (std::future<PredictionBatchItem>& image : pending.images) {
        results.push_back(image.get());
    }
    return results;
}
#endif

#if defined(MMLTK_CORE_EVALUATION_DATASET)
CocoDataset::CocoDataset(const CocoDataset& other)
    : metric_set_(other.metric_set_),
      image_ids_(other.image_ids_),
      category_names_(other.category_names_),
      ground_truth_spans_(other.ground_truth_spans_),
      ground_truth_boxes_(other.ground_truth_boxes_),
      ground_truth_categories_(other.ground_truth_categories_),
      ground_truth_ordinals_(other.ground_truth_ordinals_),
      image_area_scale_(other.image_area_scale_),
      ground_truth_areas_(other.ground_truth_areas_),
      area_ground_truth_totals_(other.area_ground_truth_totals_),
      mask_area_ground_truth_totals_(other.mask_area_ground_truth_totals_),
      ground_truth_masks_(other.ground_truth_masks_),
      ground_truth_mask_runs_(other.ground_truth_mask_runs_),
      ground_truth_mask_height_(other.ground_truth_mask_height_),
      ground_truth_mask_width_(other.ground_truth_mask_width_),
      ground_truth_totals_(other.ground_truth_totals_),
      ground_truth_nonempty_categories_(other.ground_truth_nonempty_categories_),
      image_id_to_index_(other.image_id_to_index_),
      bbox_matches_by_category_(other.bbox_matches_by_category_),
      mask_matches_by_category_(other.mask_matches_by_category_),
      prediction_count_(other.prediction_count_),
      matched_max_dets_per_image_(other.matched_max_dets_per_image_) {
    bbox_metric_scratch_.reset(category_names_.size());
    if (metric_set_ == EvaluationMetricSet::BBoxAndMask) {
        mask_metric_scratch_.emplace();
        mask_metric_scratch_->reset(category_names_.size());
    }
}

CocoDataset& CocoDataset::operator=(const CocoDataset& other) {
    if (this == &other) { return *this; }
    CocoDataset replacement(other);
    *this = std::move(replacement);
    return *this;
}

CocoDataset CocoDataset::load_from_loader(const mmltk::backend::data::DatasetLoader& loader, const EvaluationMetricSet metric_set) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_load_from_loader{"rfdetr.native.eval.load_from_loader"};
    CocoDataset out(metric_set);
    const size_t image_count = loader.num_images();
    const uint32_t num_classes = loader.num_classes();
    const size_t label_count = loader.num_label_instances();
    if (image_count > std::numeric_limits<std::uint32_t>::max() ||
        image_count > static_cast<size_t>(std::numeric_limits<int>::max()) || num_classes == 0 ||
        num_classes > std::numeric_limits<std::uint16_t>::max() ||
        label_count > std::numeric_limits<std::uint32_t>::max() ||
        image_count > std::numeric_limits<size_t>::max() / static_cast<size_t>(num_classes)) {
        throw std::runtime_error("compiled evaluation dataset exceeds the dense evaluator limits");
    }

    out.image_ids_.reserve(image_count);
    out.category_names_.reserve(num_classes);
    for (uint32_t i = 0; i < num_classes; ++i) {
        out.category_names_.emplace_back(loader.class_name(i));
    }

    const auto* label_index = loader.label_index();
    const auto* label_data = loader.label_data();
    if ((image_count != 0U && label_index == nullptr) || (label_count != 0U && label_data == nullptr)) {
        throw std::runtime_error("compiled evaluation dataset does not contain usable bbox ground truth");
    }
    const mmltk::backend::data::RLEPair* rle_data = nullptr;
    size_t rle_pair_count = 0;
    if (metric_set == EvaluationMetricSet::BBoxAndMask) {
        rle_data = loader.rle_data();
        rle_pair_count = loader.num_rle_pairs();
        if ((label_count != 0U && (rle_data == nullptr || rle_pair_count == 0)) || rle_pair_count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("bbox-and-mask evaluation requires a bounded mask RLE payload");
        }
        const uint64_t mask_pixels = static_cast<uint64_t>(loader.image_height()) * loader.image_width();
        if (mask_pixels == 0 || mask_pixels > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("evaluation masks exceed the encoded-mask area limit");
        }
        out.ground_truth_mask_height_ = loader.image_height();
        out.ground_truth_mask_width_ = loader.image_width();
    }

    out.ground_truth_spans_.resize(image_count * static_cast<size_t>(num_classes));
    size_t next_label_index = 0;
    for (size_t image_index = 0; image_index < image_count; ++image_index) {
        const int image_id = static_cast<int>(image_index + 1U);
        out.image_id_to_index_.emplace(image_id, image_index);
        out.image_ids_.push_back(image_id);

        const auto& entry = label_index[image_index];
        const size_t label_begin = entry.label_begin;
        const size_t label_end = label_begin + entry.num_instances;
        if (label_begin != next_label_index || label_end < label_begin || label_end > label_count) {
            throw std::runtime_error("compiled evaluation label index is not an exact contiguous partition");
        }
        next_label_index = label_end;
        for (uint16_t annotation_ordinal = 0; annotation_ordinal < entry.num_instances; ++annotation_ordinal) {
            const auto& packed = label_data[label_begin + annotation_ordinal];
            const std::array<float, 4> bbox = {
                static_cast<float>(packed.bbox_x1),
                static_cast<float>(packed.bbox_y1),
                static_cast<float>(packed.bbox_x2),
                static_cast<float>(packed.bbox_y2),
            };
            if (packed.class_id >= num_classes ||
                !std::ranges::all_of(bbox, [](const float coordinate) { return std::isfinite(coordinate); }) || bbox[2] <= bbox[0] ||
                bbox[3] <= bbox[1]) {
                throw std::runtime_error("compiled evaluation annotation contains an unusable bbox");
            }
            if (metric_set == EvaluationMetricSet::BBoxAndMask) {
                if (packed.mask_rle_pairs == 0 || packed.mask_rle_offset % sizeof(mmltk::backend::data::RLEPair) != 0) {
                    throw std::runtime_error("bbox-and-mask evaluation requires a mask for every annotation");
                }
                const size_t rle_start_index = packed.mask_rle_offset / sizeof(mmltk::backend::data::RLEPair);
                const size_t rle_end_index = rle_start_index + packed.mask_rle_pairs;
                if (rle_end_index < rle_start_index || rle_end_index > rle_pair_count) {
                    throw std::runtime_error("compiled evaluation mask index exceeds the RLE payload");
                }
            }
            GroundTruthSpan& span = out.ground_truth_spans_[out.ground_truth_span_index(image_index, static_cast<size_t>(packed.class_id))];
            ++span.count;
        }
    }
    if (next_label_index != label_count) {
        throw std::runtime_error("compiled evaluation annotation cardinality does not match the label index");
    }

    std::uint64_t annotation_offset = 0;
    for (GroundTruthSpan& span : out.ground_truth_spans_) {
        span.offset = static_cast<std::uint32_t>(annotation_offset);
        annotation_offset += span.count;
    }
    if (annotation_offset != label_count) { throw std::runtime_error("compiled evaluation dense-span cardinality mismatch"); }

    out.image_area_scale_.resize(image_count);
    for (size_t image = 0; image < image_count; ++image) {
        const auto& entry = loader.image_entry(static_cast<std::uint32_t>(image));
        const auto geometry = loader.letterbox(static_cast<std::uint32_t>(image));
        out.image_area_scale_[image] = (static_cast<double>(entry.original_width) / geometry.resized_width) *
                                       (static_cast<double>(entry.original_height) / geometry.resized_height);
    }
    out.ground_truth_areas_.resize(label_count);
    out.ground_truth_boxes_.resize(label_count);
    out.ground_truth_categories_.resize(label_count);
    out.ground_truth_ordinals_.resize(label_count);
    std::vector<GroundTruthMask>* ground_truth_masks_storage = nullptr;
    std::vector<std::pair<std::uint32_t, std::uint32_t>>* ground_truth_mask_runs_storage = nullptr;
    if (metric_set == EvaluationMetricSet::BBoxAndMask) {
        ground_truth_masks_storage = &out.ground_truth_masks_.emplace(label_count);
        ground_truth_mask_runs_storage = &out.ground_truth_mask_runs_.emplace();
        ground_truth_mask_runs_storage->reserve(rle_pair_count);
    }
    std::vector<std::uint32_t> span_cursors;
    span_cursors.reserve(out.ground_truth_spans_.size());
    for (const GroundTruthSpan& span : out.ground_truth_spans_) {
        span_cursors.push_back(span.offset);
    }

    const uint64_t mask_pixels = static_cast<uint64_t>(out.ground_truth_mask_height_) * out.ground_truth_mask_width_;
    for (size_t image_index = 0; image_index < image_count; ++image_index) {
        const auto& entry = label_index[image_index];
        for (uint16_t annotation_ordinal = 0; annotation_ordinal < entry.num_instances; ++annotation_ordinal) {
            const auto& packed = label_data[static_cast<size_t>(entry.label_begin) + annotation_ordinal];
            const size_t span_index = out.ground_truth_span_index(image_index, packed.class_id);
            const std::uint32_t dense_index = span_cursors[span_index]++;
            out.ground_truth_boxes_[dense_index] = {
                static_cast<float>(packed.bbox_x1),
                static_cast<float>(packed.bbox_y1),
                static_cast<float>(packed.bbox_x2),
                static_cast<float>(packed.bbox_y2),
            };
            out.ground_truth_areas_[dense_index] = evaluation_box_area(out.ground_truth_boxes_[dense_index]) * out.image_area_scale_[image_index];
            out.ground_truth_categories_[dense_index] = packed.class_id;
            out.ground_truth_ordinals_[dense_index] = annotation_ordinal;

            if (metric_set == EvaluationMetricSet::BBoxAndMask) {
                if (ground_truth_masks_storage == nullptr || ground_truth_mask_runs_storage == nullptr) {
                    throw std::logic_error("mask evaluation storage was not initialized");
                }
                std::vector<GroundTruthMask>& ground_truth_masks = *ground_truth_masks_storage;
                auto& ground_truth_mask_runs = *ground_truth_mask_runs_storage;
                const size_t rle_start_index = packed.mask_rle_offset / sizeof(mmltk::backend::data::RLEPair);
                GroundTruthMask& mask = ground_truth_masks[dense_index];
                if (ground_truth_mask_runs.size() > std::numeric_limits<std::uint32_t>::max() - packed.mask_rle_pairs) {
                    throw std::runtime_error("dense evaluation mask payload exceeds the RLE offset limit");
                }
                mask.run_offset = static_cast<std::uint32_t>(ground_truth_mask_runs.size());
                mask.run_count = packed.mask_rle_pairs;
                uint64_t previous_run_end = 0;
                uint64_t mask_area = 0;
                for (uint16_t run_index = 0; run_index < packed.mask_rle_pairs; ++run_index) {
                    const mmltk::backend::data::RLEPair& rle = rle_data[rle_start_index + run_index];
                    const uint64_t run_end = static_cast<uint64_t>(rle.start) + rle.length;
                    if (rle.length == 0 || static_cast<uint64_t>(rle.start) < previous_run_end || run_end > mask_pixels) {
                        throw std::runtime_error("compiled evaluation mask contains an invalid RLE run");
                    }
                    previous_run_end = run_end;
                    mask_area += rle.length;
                    ground_truth_mask_runs.emplace_back(rle.start, rle.length);
                }
                mask.area = static_cast<std::uint32_t>(mask_area);
            }
        }
    }
    for (size_t span_index = 0; span_index < out.ground_truth_spans_.size(); ++span_index) {
        const GroundTruthSpan& span = out.ground_truth_spans_[span_index];
        if (span_cursors[span_index] != span.offset + span.count) {
            throw std::runtime_error("compiled evaluation dense-span fill mismatch");
        }
    }
    out.rebuild_ground_truth_totals();
    out.reset_match_storage();
    return out;
}

void CocoDataset::rebuild_ground_truth_totals() {
    ground_truth_totals_.assign(category_names_.size(), 0);
    area_ground_truth_totals_.assign(category_names_.size(), {});
    mask_area_ground_truth_totals_.assign(ground_truth_masks_ ? category_names_.size() : 0U, {});
    for (size_t image_index = 0; image_index < image_ids_.size(); ++image_index) {
        for (size_t category_index = 0; category_index < category_names_.size(); ++category_index) {
            const auto span = ground_truth_spans_[ground_truth_span_index(image_index, category_index)];
            ground_truth_totals_[category_index] += span.count;
            for (size_t gt = span.offset; gt < span.offset + span.count; ++gt)
                for (size_t area = 0; area < 4U; ++area) {
                    area_ground_truth_totals_[category_index][area] += evaluation_area_contains(area, ground_truth_areas_[gt]) ? 1U : 0U;
                    if (ground_truth_masks_) mask_area_ground_truth_totals_[category_index][area] +=
                        evaluation_area_contains(area, (*ground_truth_masks_)[gt].area * image_area_scale_[image_index]) ? 1U : 0U;
                }
        }
    }
    ground_truth_nonempty_categories_.clear();
    ground_truth_nonempty_categories_.reserve(category_names_.size());
    for (size_t category_index = 0; category_index < category_names_.size(); ++category_index) {
        if (ground_truth_totals_[category_index] != 0) { ground_truth_nonempty_categories_.push_back(category_index); }
    }
    bbox_metric_scratch_.reset(category_names_.size());
    if (metric_set_ == EvaluationMetricSet::BBoxAndMask) {
        if (!mask_metric_scratch_) { mask_metric_scratch_.emplace(); }
        mask_metric_scratch_->reset(category_names_.size());
    } else {
        mask_metric_scratch_.reset();
    }
}

void CocoDataset::reset_match_storage() {
    if (bbox_matches_by_category_.size() != category_names_.size()) { bbox_matches_by_category_.resize(category_names_.size()); }
    for (auto& category_matches : bbox_matches_by_category_) {
        category_matches.clear();
    }
    if (metric_set_ == EvaluationMetricSet::BBoxAndMask) {
        if (!mask_matches_by_category_) {
            mask_matches_by_category_.emplace(category_names_.size());
        } else if (mask_matches_by_category_->size() != category_names_.size()) {
            mask_matches_by_category_->resize(category_names_.size());
        }
        for (auto& category_matches : *mask_matches_by_category_) {
            category_matches.clear();
        }
    } else {
        mask_matches_by_category_.reset();
    }
    prediction_count_ = 0;
    details_.clear();
    matched_max_dets_per_image_.reset();
}

void CocoDataset::limit_images(const size_t limit) {
    if (limit >= image_ids_.size()) { return; }
    const size_t retained_span_count = limit * category_names_.size();
    size_t retained_annotation_count = 0;
    if (retained_span_count > 0) {
        const GroundTruthSpan& last_span = ground_truth_spans_[retained_span_count - 1U];
        retained_annotation_count = static_cast<size_t>(last_span.offset) + last_span.count;
    }
    ground_truth_spans_.resize(retained_span_count);
    image_area_scale_.resize(limit);
    ground_truth_areas_.resize(retained_annotation_count);
    ground_truth_boxes_.resize(retained_annotation_count);
    ground_truth_categories_.resize(retained_annotation_count);
    ground_truth_ordinals_.resize(retained_annotation_count);
    if (ground_truth_masks_) {
        if (!ground_truth_mask_runs_) { throw std::logic_error("mask ground truth is missing its run storage"); }
        auto& masks = *ground_truth_masks_;
        auto& mask_runs = *ground_truth_mask_runs_;
        masks.resize(retained_annotation_count);
        size_t retained_run_count = 0;
        for (const GroundTruthMask& mask : masks) {
            retained_run_count = std::max(retained_run_count, static_cast<size_t>(mask.run_offset) + mask.run_count);
        }
        mask_runs.resize(retained_run_count);
    }
    image_ids_.resize(limit);
    image_id_to_index_.clear();
    for (size_t index = 0; index < image_ids_.size(); ++index) {
        image_id_to_index_.emplace(image_ids_[index], index);
    }
    const auto remove_outside_limit = [limit](const CompactImageMatchRecord& record) { return record.image_ordinal >= limit; };
    prediction_count_ = 0;
    for (auto& category_matches : bbox_matches_by_category_) {
        std::erase_if(category_matches, remove_outside_limit);
        prediction_count_ += category_matches.size();
    }
    if (mask_matches_by_category_) {
        for (auto& category_matches : *mask_matches_by_category_) {
            std::erase_if(category_matches, remove_outside_limit);
        }
    }
    if (prediction_count_ == 0) { matched_max_dets_per_image_.reset(); }
    rebuild_ground_truth_totals();
}

bool CocoDataset::has_image(int image_id) const { return image_id_to_index_.contains(image_id); }

ImageEvaluationMatches CocoDataset::match_staged_predictions(const std::int64_t dataset_index, const BBoxPredictionView& bbox,
                                                             const std::optional<PackedMaskPredictionView>& mask,
                                                             const size_t max_dets_per_image, const std::span<const Prediction> encoded_masks) const {
    if (dataset_index < 0 || static_cast<size_t>(dataset_index) >= image_ids_.size()) {
        throw std::out_of_range("staged prediction image index is outside the evaluation dataset");
    }
    if (max_dets_per_image == 0 || bbox.count > std::numeric_limits<std::uint32_t>::max() ||
        (bbox.count != 0U && (bbox.scores == nullptr || bbox.labels_zero_based == nullptr || bbox.boxes_xyxy == nullptr)) || bbox.score_stride <= 0 || bbox.label_stride <= 0 ||
        bbox.box_stride < 4) {
        throw std::invalid_argument("staged bbox prediction view is invalid");
    }
    const size_t image_index = static_cast<size_t>(dataset_index);
    if (bbox.image_id != image_ids_[image_index]) {
        throw std::invalid_argument("staged prediction image ID does not match the image matcher index");
    }
    const bool mask_mode = metric_set_ == EvaluationMetricSet::BBoxAndMask;
    if ((!mask_mode && (mask || !encoded_masks.empty())) || (mask_mode && bbox.count != 0U && !mask && encoded_masks.size() != bbox.count)) {
        throw std::invalid_argument("staged prediction mask view does not match the dataset metric mode");
    }
    if (mask_mode && (!ground_truth_masks_ || !ground_truth_mask_runs_)) {
        throw std::logic_error("mask evaluation ground truth storage is incomplete");
    }
    const std::uint64_t packed_mask_bytes = mask ? (static_cast<std::uint64_t>(mask->height) * mask->width + 7U) / 8U : 0U;
    if (mask &&
        ((bbox.count != 0U && mask->data == nullptr) || mask->prediction_stride <= 0 || static_cast<std::uint64_t>(mask->prediction_stride) < packed_mask_bytes ||
         mask->height != ground_truth_mask_height_ || mask->width != ground_truth_mask_width_)) {
        throw std::invalid_argument("staged mask prediction view is invalid");
    }

    const auto image_ordinal = static_cast<std::uint32_t>(image_index);
    const size_t category_count = category_names_.size();
    ImageMatchingScratch& scratch = image_matching_scratch(category_count);
    ImageEvaluationMatches result;
    result.max_dets_per_image = max_dets_per_image;
    size_t bbox_iou_candidate_count = 0;
    size_t mask_iou_candidate_count = 0;

    const size_t bbox_prediction_count = group_staged_predictions_by_category(bbox, category_count, max_dets_per_image, scratch);
    result.bbox.reserve(bbox_prediction_count);
    scratch.staged_boxes.resize(bbox.count);
    for (const auto& category_predictions : scratch.predictions_by_category) {
        for (const std::uint32_t prediction_index : category_predictions) {
            scratch.staged_boxes[prediction_index] =
                xyxy_clamped(bbox.boxes_xyxy + static_cast<std::ptrdiff_t>(prediction_index) * bbox.box_stride);
        }
    }
    const auto category_ground_truth_span = [&](const size_t category_index) {
        return ground_truth_spans_[ground_truth_span_index(image_index, category_index)];
    };
    const auto span_ordinals = [this](const GroundTruthSpan span) {
        return std::span<const std::uint32_t>(ground_truth_ordinals_).subspan(span.offset, span.count);
    };
    const auto bbox_prediction_score = [&bbox](const std::uint32_t prediction_index) {
        return bbox.scores[static_cast<std::ptrdiff_t>(prediction_index) * bbox.score_stride];
    };

    for (size_t category_index = 0; category_index < category_count; ++category_index) {
        const GroundTruthSpan span = category_ground_truth_span(category_index);
        match_category_predictions(
            scratch.predictions_by_category[category_index], span_ordinals(span), image_ordinal, static_cast<std::uint16_t>(category_index),
            scratch, result.bbox, bbox_iou_candidate_count, bbox_prediction_score,
            [this, span, &scratch](const std::uint32_t prediction_index, const size_t ground_truth_index) {
                return bbox_iou(scratch.staged_boxes[prediction_index], ground_truth_boxes_[span.offset + ground_truth_index]);
            }, [this, span](size_t index) { return ground_truth_areas_[span.offset + index]; },
            [this, image_index, &scratch](size_t index) { return evaluation_box_area(scratch.staged_boxes[index]) * image_area_scale_[image_index]; });
    }
    result.prediction_count = result.bbox.size();

    if (mask_mode) {
        auto& mask_matches = result.mask.emplace();
        mask_matches.reserve(bbox_prediction_count);
        if (mask) {
            scratch.staged_masks.resize(bbox.count);
            for (const auto& category_predictions : scratch.predictions_by_category)
                for (const std::uint32_t index : category_predictions)
                    encode_mask_from_packed_data_into(mask->data + static_cast<std::ptrdiff_t>(index) * mask->prediction_stride,
                                                     mask->height, mask->width, scratch.staged_masks[index]);
        } else {
            for (const auto& prediction : encoded_masks)
                if (!prediction.has_mask || prediction.mask.height != ground_truth_mask_height_ || prediction.mask.width != ground_truth_mask_width_)
                    throw std::invalid_argument("encoded evaluation masks do not match compiled geometry");
        }
        const auto prediction_mask_at = [&](size_t index) -> const EncodedMask& {
            return mask ? scratch.staged_masks[index] : encoded_masks[index].mask;
        };
        const auto& masks = *ground_truth_masks_;
        const auto& runs = *ground_truth_mask_runs_;
        for (size_t category_index = 0; category_index < category_count; ++category_index) {
            const GroundTruthSpan span = category_ground_truth_span(category_index);
            match_category_predictions(
                scratch.predictions_by_category[category_index], span_ordinals(span), image_ordinal,
                static_cast<std::uint16_t>(category_index), scratch, mask_matches, mask_iou_candidate_count, bbox_prediction_score,
                [span, &masks, &runs, &prediction_mask_at](const std::uint32_t prediction_index, const size_t ground_truth_index) {
                    const EncodedMask& prediction_mask = prediction_mask_at(prediction_index);
                    const GroundTruthMask& ground_truth_mask = masks[span.offset + ground_truth_index];
                    const auto ground_truth_runs = std::span<const std::pair<std::uint32_t, std::uint32_t>>(runs).subspan(
                        ground_truth_mask.run_offset, ground_truth_mask.run_count);
                    const std::uint32_t intersect = intersection_area(prediction_mask, ground_truth_runs);
                    const std::uint64_t union_area = static_cast<std::uint64_t>(prediction_mask.area) + ground_truth_mask.area - intersect;
                    return union_area == 0 ? 0.0 : static_cast<double>(intersect) / static_cast<double>(union_area);
                }, [this, span, image_index](size_t index) { return (*ground_truth_masks_)[span.offset + index].area * image_area_scale_[image_index]; },
                [this, image_index, &prediction_mask_at](size_t index) { return prediction_mask_at(index).area * image_area_scale_[image_index]; });
        }
    }
    result.bbox_iou_candidate_count = bbox_iou_candidate_count;
    result.mask_iou_candidate_count = mask_iou_candidate_count;
    return result;
}

void CocoDataset::merge_matches(ImageEvaluationMatches&& matches) {
    if (matches.max_dets_per_image == 0) { throw std::invalid_argument("cannot merge evaluation matches without a detection limit"); }
    if (matches.prediction_count != matches.bbox.size()) {
        throw std::invalid_argument("compact evaluation batch has an inconsistent prediction count");
    }
    if (matched_max_dets_per_image_ && *matched_max_dets_per_image_ != matches.max_dets_per_image) {
        throw std::invalid_argument("evaluation match batches use inconsistent detection limits");
    }
    auto* mask_records = matches.mask ? &*matches.mask : nullptr;
    auto* mask_matches_by_category = mask_matches_by_category_ ? &*mask_matches_by_category_ : nullptr;
    if ((metric_set_ == EvaluationMetricSet::BBoxAndMask) != (mask_records != nullptr)) {
        throw std::invalid_argument("compact evaluation batch does not match the dataset metric mode");
    }
    if (mask_records != nullptr && mask_matches_by_category == nullptr) {
        throw std::logic_error("mask evaluation match storage is unavailable");
    }
    for (const CompactImageMatchRecord& record : matches.bbox) {
        if (record.category_index >= bbox_matches_by_category_.size()) {
            throw std::out_of_range("bbox match category is outside the evaluation dataset");
        }
    }
    if (mask_records != nullptr) {
        for (const CompactImageMatchRecord& record : *mask_records) {
            if (record.category_index >= mask_matches_by_category->size()) {
                throw std::out_of_range("mask match category is outside the evaluation dataset");
            }
        }
    }
    matched_max_dets_per_image_ = matches.max_dets_per_image;
    for (CompactImageMatchRecord& record : matches.bbox) {
        bbox_matches_by_category_[record.category_index].push_back(record);
    }
    if (mask_records != nullptr) {
        for (const CompactImageMatchRecord& record : *mask_records) {
            (*mask_matches_by_category)[record.category_index].push_back(record);
        }
    }
    prediction_count_ += matches.prediction_count;
}

void CocoDataset::clear_predictions() { reset_match_storage(); }

EvalSummary CocoDataset::evaluate(const size_t max_dets_per_image, mmltk::common::concurrency::WorkerPool* worker_pool) const {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_total{"rfdetr.native.eval.total"};
    if (max_dets_per_image == 0 || (matched_max_dets_per_image_ && *matched_max_dets_per_image_ != max_dets_per_image)) {
        throw std::invalid_argument("evaluation detection limit does not match the image matcher");
    }
    const std::array<std::uint32_t, 3> caps{1U, static_cast<std::uint32_t>(std::min<size_t>(10U, max_dets_per_image)),
                                             static_cast<std::uint32_t>(max_dets_per_image)};
    details_.clear();
    details_.resize((category_names_.size() + 1U) * 4U * (metric_set_ == EvaluationMetricSet::BBoxAndMask ? 2U : 1U));
    const auto reduce_metric = [this, worker_pool, &caps](auto& matches_by_category, MetricScratch& scratch,
                                                         EvaluationMetricKind kind) {
        scratch.reset(category_names_.size());
        const auto row_width = category_names_.size() + 1U;
        const auto base = kind == EvaluationMetricKind::Box ? 0U : row_width * 4U;
        for (size_t category = 0; category < category_names_.size(); ++category)
            for (size_t area = 0; area < 4U; ++area)
                scratch.categories[category].areas[area] = &details_[base + area * row_width + category + 1U];
        const auto reduce_range = [&](const size_t begin, const size_t end) {
            for (size_t category = begin; category < end; ++category) {
                reduce_category_matches(matches_by_category[category], (kind == EvaluationMetricKind::Mask ? mask_area_ground_truth_totals_ : area_ground_truth_totals_)[category], caps, scratch.categories[category]);
            }
        };
        if (worker_pool != nullptr && category_names_.size() > 1U) {
            const size_t workers = std::min(worker_pool->size(), category_names_.size());
            worker_pool->parallel_for<size_t>(0, category_names_.size(), static_cast<int>(workers), reduce_range);
        } else reduce_range(0, category_names_.size());

        std::array<ConfidenceMetrics, 101> macro{};
        for (const auto category : ground_truth_nonempty_categories_) {
            const auto& values = scratch.categories[category].confidence;
            for (size_t threshold = 0; threshold < macro.size(); ++threshold) {
                macro[threshold].precision += values[threshold].precision;
                macro[threshold].recall += values[threshold].recall;
                macro[threshold].f1 += values[threshold].f1;
            }
        }
        size_t best = 0U;
        for (size_t threshold = 0; threshold < macro.size(); ++threshold) {
            if (!ground_truth_nonempty_categories_.empty()) {
                const double divisor = static_cast<double>(ground_truth_nonempty_categories_.size());
                macro[threshold].precision /= divisor;
                macro[threshold].recall /= divisor;
                macro[threshold].f1 /= divisor;
            }
            if (macro[threshold].f1 > macro[best].f1) best = threshold;
        }
        MetricSummary summary;
        summary.detection_limits = caps;
        summary.confidence = macro[best];
        summary.confidence_threshold = static_cast<double>(best) * 0.01;
        for (size_t area = 0; area < 4U; ++area) {
            auto& aggregate = details_[base + area * row_width];
            aggregate.kind = kind;
            aggregate.area = static_cast<EvaluationArea>(area);
            aggregate.detection_limits = caps;
            aggregate.confidence = macro[best];
            aggregate.confidence_threshold = summary.confidence_threshold;
            size_t available_categories = 0U;
            for (size_t category = 0; category < category_names_.size(); ++category) {
                auto& detail = *scratch.categories[category].areas[area];
                detail.kind = kind;
                detail.category = static_cast<std::uint32_t>(category);
                detail.confidence = scratch.categories[category].confidence[best];
                detail.confidence_threshold = summary.confidence_threshold;
                aggregate.ground_truth_count += detail.ground_truth_count;
                if (!detail.available) continue;
                ++available_categories;
                for (size_t iou = 0; iou < kIouThresholds.size(); ++iou) {
                    aggregate.average_precision[iou] += detail.average_precision[iou];
                    for (size_t cap = 0; cap < caps.size(); ++cap)
                        aggregate.average_recall[cap][iou] += detail.average_recall[cap][iou];
                    for (size_t recall = 0; recall < 101U; ++recall)
                        aggregate.precision_curve[iou][recall] += detail.precision_curve[iou][recall];
                }
            }
            aggregate.available = available_categories != 0U;
            double ap = 0.0;
            std::array<double, 3> ar{};
            if (aggregate.available) {
                for (size_t iou = 0; iou < kIouThresholds.size(); ++iou) {
                    aggregate.average_precision[iou] /= static_cast<double>(available_categories);
                    ap += aggregate.average_precision[iou] / static_cast<double>(kIouThresholds.size());
                    for (size_t cap = 0; cap < caps.size(); ++cap) {
                        aggregate.average_recall[cap][iou] /= static_cast<double>(available_categories);
                        ar[cap] += aggregate.average_recall[cap][iou] / static_cast<double>(kIouThresholds.size());
                    }
                    for (double& precision : aggregate.precision_curve[iou]) precision /= static_cast<double>(available_categories);
                }
            }
            if (area == 0U) {
                summary.available = aggregate.available;
                summary.ap = ap;
                summary.ap50 = aggregate.average_precision[0];
                summary.ap75 = aggregate.average_precision[5];
                if (aggregate.available) for (size_t cap = 0; cap < caps.size(); ++cap) summary.average_recall[cap] = ar[cap];
            } else if (aggregate.available) {
                summary.area_ap[area - 1U] = ap;
                summary.area_ar[area - 1U] = ar.back();
            }

        }
        return summary;
    };
    EvalSummary summary;
    summary.model_detection_budget = static_cast<std::uint32_t>(max_dets_per_image);
    summary.bbox = reduce_metric(bbox_matches_by_category_, bbox_metric_scratch_, EvaluationMetricKind::Box);
    if (metric_set_ == EvaluationMetricSet::BBoxAndMask) {
        if (!mask_matches_by_category_ || !mask_metric_scratch_) throw std::logic_error("mask evaluation reduction storage is unavailable");
        summary.mask = reduce_metric(*mask_matches_by_category_, *mask_metric_scratch_, EvaluationMetricKind::Mask);
    }
    return summary;
}
#endif

#if defined(MMLTK_TRAINING_EVALUATION_RUNTIME)
std::vector<Prediction> result_to_predictions(int image_id, const TensorMap& result, size_t category_count, size_t max_dets_per_image,
                                              EvaluationProfileRecord* profile) {
    return valid_predictions(result, image_id, category_count, max_dets_per_image, profile);
}

AlignmentStats compare_top1(const TensorMap& lhs, const TensorMap& rhs, size_t category_count) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_align_top1{"rfdetr.native.eval.align_top1"};
    auto lhs_predictions = valid_predictions(lhs, 0, category_count, 1);
    auto rhs_predictions = valid_predictions(rhs, 0, category_count, 1);
    AlignmentStats stats;
    Prediction* lhs_top1 = first_valid_prediction(lhs_predictions);
    Prediction* rhs_top1 = first_valid_prediction(rhs_predictions);
    if (lhs_top1 == nullptr || rhs_top1 == nullptr) { return stats; }

    stats.images_compared = 1;
    const double score_diff = std::abs(static_cast<double>(lhs_top1->score) - static_cast<double>(rhs_top1->score));
    stats.top1_score_abs_diff_mean = score_diff;
    stats.top1_score_abs_diff_max = score_diff;

    double box_diff = 0.0;
    for (size_t index = 0; index < lhs_top1->bbox_xyxy.size(); ++index) {
        box_diff =
            std::max(box_diff, std::abs(static_cast<double>(lhs_top1->bbox_xyxy[index]) - static_cast<double>(rhs_top1->bbox_xyxy[index])));
    }
    stats.top1_box_abs_diff_px_mean = box_diff;
    stats.top1_box_abs_diff_px_max = box_diff;

    if (lhs_top1->has_mask && rhs_top1->has_mask) {
        const uint32_t intersect = intersection_area(lhs_top1->mask, rhs_top1->mask);
        const auto xor_pixels = static_cast<double>(lhs_top1->mask.area + rhs_top1->mask.area - 2 * intersect);
        stats.top1_mask_xor_pixels_mean = xor_pixels;
        stats.top1_mask_xor_pixels_max = xor_pixels;
    }
    return stats;
}
#endif

}  // namespace mmltk::backend::models::rfdetr
