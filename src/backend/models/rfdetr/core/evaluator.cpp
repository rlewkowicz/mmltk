#include "evaluator.h"
#include "src/backend/data/dataset_loader.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {

using CompactImageMatchRecord = EvaluationDatasetOwner::MatchRecord;
using ImageEvaluationMatches = EvaluationDatasetOwner::ImageMatches;

class CocoDataset {
   public:
    explicit inline CocoDataset(EvaluationMetricSet metric_set) : metric_set_(metric_set) {}
    CocoDataset(const CocoDataset& other);
    CocoDataset& operator=(const CocoDataset& other);
    CocoDataset(CocoDataset&&) noexcept = default;
    CocoDataset& operator=(CocoDataset&&) noexcept = default;

    static CocoDataset load_from_loader(const mmltk::backend::data::DatasetLoader& loader, EvaluationMetricSet metric_set);

    inline size_t num_images() const { return image_ids_.size(); }
    inline size_t num_categories() const { return catalog_->size(); }
    inline const std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>& class_catalog() const { return catalog_; }
    inline const std::vector<int>& image_ids() const { return image_ids_; }
    inline EvaluationMetricSet metric_set() const { return metric_set_; }
    inline size_t ground_truth_count() const { return ground_truth_boxes_.size(); }
    inline size_t prediction_count() const { return prediction_count_; }
    inline size_t mask_rle_pair_count() const { return ground_truth_mask_runs_ ? ground_truth_mask_runs_->size() : 0U; }

    void limit_images(size_t limit);
    bool has_image(int image_id) const;
    [[nodiscard]] ImageEvaluationMatches match_staged_predictions(std::int64_t dataset_index, const BBoxPredictionView& bbox,
                                                                  const std::optional<PackedMaskPredictionView>& mask,
                                                                  size_t max_dets_per_image, std::span<const Prediction> encoded_masks = {}) const;
    void merge_matches(ImageEvaluationMatches&& matches);
    void clear_predictions();
    EvalSummary evaluate(size_t max_dets_per_image, EvaluationDetailRetention retention, mmltk::common::concurrency::WorkerPool* worker_pool = nullptr) const;
    std::vector<EvaluationMetricDetail> take_details() { return std::exchange(details_, {}); }

   private:
    struct GroundTruthSpan {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };
    struct GroundTruthMask {
        std::uint32_t run_offset = 0;
        std::uint32_t run_count = 0;
        std::uint32_t area = 0;
    };
    // Compact per-area accumulation is shared by both retention modes. Only
    // explicitly requested detail rows own the much larger PR grid.
    struct AreaReduction {
        bool available = false;
        std::uint64_t ground_truth_count = 0U;
        decltype(EvaluationMetricDetail::average_precision) average_precision{};
        decltype(EvaluationMetricDetail::average_recall) average_recall{};
        EvaluationMetricDetail* detail = nullptr;
    };
    struct CategoryReductionScratch {
        std::array<AreaReduction, kEvaluationAreaCount> areas{};
        std::array<ConfidenceMetrics, kEvaluationConfidenceCount> confidence{};
        std::array<size_t, kEvaluationRecallCount> recall_indices{};
        std::vector<double> precisions;
    };
    struct MetricScratch {
        std::vector<CategoryReductionScratch> categories;

        inline void reset(size_t category_count) {
            categories.resize(category_count);
            for (CategoryReductionScratch& category : categories) {
                category.areas = {};
                category.confidence = {};
            }
        }
    };

    [[nodiscard]] inline size_t ground_truth_span_index(size_t image_index, size_t category_index) const {
        return image_index * catalog_->size() + category_index;
    }
    void rebuild_ground_truth_totals();
    void reset_match_storage();

    EvaluationMetricSet metric_set_;
    std::vector<int> image_ids_;
    std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog_;
    std::vector<GroundTruthSpan> ground_truth_spans_;
    std::vector<std::array<float, 4>> ground_truth_boxes_;
    std::vector<std::uint16_t> ground_truth_categories_;
    std::vector<std::uint32_t> ground_truth_ordinals_;
    std::vector<double> image_area_scale_;
    std::vector<double> ground_truth_areas_;
    std::vector<std::array<size_t, kEvaluationAreaCount>> area_ground_truth_totals_;
    std::vector<std::array<size_t, kEvaluationAreaCount>> mask_area_ground_truth_totals_;
    std::optional<std::vector<GroundTruthMask>> ground_truth_masks_;
    std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>> ground_truth_mask_runs_;
    std::uint32_t ground_truth_mask_height_ = 0;
    std::uint32_t ground_truth_mask_width_ = 0;
    std::vector<size_t> ground_truth_totals_;
    std::vector<size_t> ground_truth_nonempty_categories_;
    std::unordered_map<int, size_t> image_id_to_index_;
    mutable std::vector<std::vector<CompactImageMatchRecord>> bbox_matches_by_category_;
    mutable std::optional<std::vector<std::vector<CompactImageMatchRecord>>> mask_matches_by_category_;
    size_t prediction_count_ = 0;
    std::optional<size_t> matched_max_dets_per_image_;
    mutable MetricScratch bbox_metric_scratch_;
    mutable std::optional<MetricScratch> mask_metric_scratch_;
    mutable std::vector<EvaluationMetricDetail> details_;
};

}  // namespace mmltk::backend::models::rfdetr

namespace mmltk::backend::models::rfdetr {
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

namespace {

constexpr auto kIouThresholds = kEvaluationIouThresholds;


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

uint32_t intersection_area(const EncodedMask& lhs, const std::span<const std::pair<std::uint32_t, std::uint32_t>> rhs_runs) {
    mmltk::common::logging::ScopedNvtxRange nvtx_intersection_area_dense_ground_truth{"intersection_area_dense_ground_truth",
                                                                                      mmltk::common::logging::nvtx_color_yellow};
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_intersection_area_dense_ground_truth{
        "rfdetr.native.eval.intersection_area_dense_ground_truth"};
    return intersection_area_runs(std::span{lhs.runs}, rhs_runs);
}

constexpr std::uint16_t kAllThresholdBits = static_cast<std::uint16_t>((std::uint16_t{1} << std::size(kIouThresholds)) - 1U);

struct MatchCandidate {
    double iou = 0.0;
    std::uint32_t ground_truth_index = 0;
    std::uint32_t ground_truth_ordinal = 0;
    std::uint16_t eligible_threshold_bits = 0;
};

struct ImageMatchingScratch {
    std::vector<std::vector<std::uint32_t>> predictions_by_category;
    std::vector<std::array<std::uint16_t, kEvaluationAreaCount>> unmatched_threshold_bits;
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
    constexpr std::array<double, kEvaluationAreaCount> minimum{0.0, 0.0, 1024.0, 9216.0};
    constexpr std::array<double, kEvaluationAreaCount> maximum{1.0e10, 1024.0, 9216.0, 1.0e10};
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
        for (size_t area = 0; area < kEvaluationAreaCount; ++area) {
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
void reduce_category_matches(std::vector<CompactImageMatchRecord>& matches, const std::array<size_t, kEvaluationAreaCount>& ground_truth_count,
                             const std::array<std::uint32_t, 3>& caps, CategoryScratch& scratch) {
    std::ranges::sort(matches, [](const CompactImageMatchRecord& lhs, const CompactImageMatchRecord& rhs) {
        if (std::isnan(lhs.score) != std::isnan(rhs.score)) return !std::isnan(lhs.score);
        if (!std::isnan(lhs.score) && lhs.score != rhs.score) return lhs.score > rhs.score;
        if (lhs.image_ordinal != rhs.image_ordinal) return lhs.image_ordinal < rhs.image_ordinal;
        return lhs.prediction_ordinal < rhs.prediction_ordinal;
    });
    scratch.precisions.reserve(matches.size());
    for (size_t area = 0; area < kEvaluationAreaCount; ++area) {
        auto& detail = scratch.areas[area];
        detail.ground_truth_count = ground_truth_count[area];
        detail.available = ground_truth_count[area] != 0U;
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
                while (next_recall < kEvaluationRecallCount && recall >= kEvaluationAxes.recall[next_recall])
                    scratch.recall_indices[next_recall++] = scratch.precisions.size() - 1U;
            }
            for (size_t index = scratch.precisions.size(); index > 1U; --index)
                scratch.precisions[index - 2U] = std::max(scratch.precisions[index - 2U], scratch.precisions[index - 1U]);
            for (size_t recall = 0; recall < kEvaluationRecallCount; ++recall) {
                const size_t index = scratch.recall_indices[recall];
                const double precision = index < scratch.precisions.size() ? scratch.precisions[index] : 0.0;
                if (detail.detail) detail.detail->precision_curve[threshold][recall] = precision;
                detail.average_precision[threshold] += precision / static_cast<double>(kEvaluationRecallCount);
            }
            for (size_t cap = 0; cap < caps.size(); ++cap)
                detail.average_recall[cap][threshold] = static_cast<double>(true_positives[cap]) / static_cast<double>(ground_truth_count[area]);
        }
    }
    // One monotonic cursor, with scores widened before the inclusive comparison.
    // This is the upstream float64 sweep, without 101 detection rescans.
    size_t cursor = 0, tp = 0, count = 0;
    for (size_t reverse = kEvaluationConfidenceCount; reverse > 0U; --reverse) {
        const size_t threshold_index = reverse - 1U;
        const double threshold = kEvaluationAxes.confidence[threshold_index];
        while (cursor < matches.size() && static_cast<double>(matches[cursor].score) >= threshold) {
            const auto& match = matches[cursor++];
            if (match.category_rank >= caps.back() || (match.area_ignored_bits[0] & 1U) != 0U) continue;
            ++count;
            tp += (match.area_matched_bits[0] & 1U) != 0U ? 1U : 0U;
        }
        scratch.confidence[threshold_index] = confidence_metrics(tp, count, ground_truth_count[0]);
    }
}


} // namespace
CocoDataset::CocoDataset(const CocoDataset& other)
    : metric_set_(other.metric_set_),
      image_ids_(other.image_ids_),
      catalog_(other.catalog_),
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
    bbox_metric_scratch_.reset(catalog_->size());
    if (metric_set_ == EvaluationMetricSet::BBoxAndMask) {
        mask_metric_scratch_.emplace();
        mask_metric_scratch_->reset(catalog_->size());
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
    out.catalog_ = loader.class_catalog();

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
    ground_truth_totals_.assign(catalog_->size(), 0);
    area_ground_truth_totals_.assign(catalog_->size(), {});
    mask_area_ground_truth_totals_.assign(ground_truth_masks_ ? catalog_->size() : 0U, {});
    for (size_t image_index = 0; image_index < image_ids_.size(); ++image_index) {
        for (size_t category_index = 0; category_index < catalog_->size(); ++category_index) {
            const auto span = ground_truth_spans_[ground_truth_span_index(image_index, category_index)];
            ground_truth_totals_[category_index] += span.count;
            for (size_t gt = span.offset; gt < span.offset + span.count; ++gt)
                for (size_t area = 0; area < kEvaluationAreaCount; ++area) {
                    area_ground_truth_totals_[category_index][area] += evaluation_area_contains(area, ground_truth_areas_[gt]) ? 1U : 0U;
                    if (ground_truth_masks_) mask_area_ground_truth_totals_[category_index][area] +=
                        evaluation_area_contains(area, (*ground_truth_masks_)[gt].area * image_area_scale_[image_index]) ? 1U : 0U;
                }
        }
    }
    ground_truth_nonempty_categories_.clear();
    ground_truth_nonempty_categories_.reserve(catalog_->size());
    for (size_t category_index = 0; category_index < catalog_->size(); ++category_index) {
        if (ground_truth_totals_[category_index] != 0) { ground_truth_nonempty_categories_.push_back(category_index); }
    }
    bbox_metric_scratch_.reset(catalog_->size());
    if (metric_set_ == EvaluationMetricSet::BBoxAndMask) {
        if (!mask_metric_scratch_) { mask_metric_scratch_.emplace(); }
        mask_metric_scratch_->reset(catalog_->size());
    } else {
        mask_metric_scratch_.reset();
    }
}

void CocoDataset::reset_match_storage() {
    if (bbox_matches_by_category_.size() != catalog_->size()) { bbox_matches_by_category_.resize(catalog_->size()); }
    for (auto& category_matches : bbox_matches_by_category_) {
        category_matches.clear();
    }
    if (metric_set_ == EvaluationMetricSet::BBoxAndMask) {
        if (!mask_matches_by_category_) {
            mask_matches_by_category_.emplace(catalog_->size());
        } else if (mask_matches_by_category_->size() != catalog_->size()) {
            mask_matches_by_category_->resize(catalog_->size());
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
    const size_t retained_span_count = limit * catalog_->size();
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
    const size_t category_count = catalog_->size();
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

EvalSummary CocoDataset::evaluate(const size_t max_dets_per_image, EvaluationDetailRetention retention, mmltk::common::concurrency::WorkerPool* worker_pool) const {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_eval_total{"rfdetr.native.eval.total"};
    if (max_dets_per_image == 0 || (matched_max_dets_per_image_ && *matched_max_dets_per_image_ != max_dets_per_image)) {
        throw std::invalid_argument("evaluation detection limit does not match the image matcher");
    }
    const std::array<std::uint32_t, 3> caps{1U, static_cast<std::uint32_t>(std::min<size_t>(10U, max_dets_per_image)),
                                             static_cast<std::uint32_t>(max_dets_per_image)};
    details_.clear();
    const bool retain_details = retention == EvaluationDetailRetention::Detailed;
    if (retain_details) details_.resize((catalog_->size() + 1U) * kEvaluationAreaCount * (metric_set_ == EvaluationMetricSet::BBoxAndMask ? 2U : 1U));
    const auto reduce_metric = [this, worker_pool, &caps, retain_details](auto& matches_by_category, MetricScratch& scratch,
                                                         EvaluationMetricKind kind) {
        scratch.reset(catalog_->size());
        const auto row_width = catalog_->size() + 1U;
        const auto base = kind == EvaluationMetricKind::Box ? 0U : row_width * kEvaluationAreaCount;
        if (retain_details) for (size_t category = 0; category < catalog_->size(); ++category)
            for (size_t area = 0; area < kEvaluationAreaCount; ++area)
                scratch.categories[category].areas[area].detail = &details_[base + area * row_width + category + 1U];
        const auto reduce_range = [&](const size_t begin, const size_t end) {
            for (size_t category = begin; category < end; ++category) {
                reduce_category_matches(matches_by_category[category], (kind == EvaluationMetricKind::Mask ? mask_area_ground_truth_totals_ : area_ground_truth_totals_)[category], caps, scratch.categories[category]);
            }
        };
        if (worker_pool != nullptr && catalog_->size() > 1U) {
            const size_t workers = std::min(worker_pool->size(), catalog_->size());
            worker_pool->parallel_for<size_t>(0, catalog_->size(), static_cast<int>(workers), reduce_range);
        } else reduce_range(0, catalog_->size());

        std::array<ConfidenceMetrics, kEvaluationConfidenceCount> macro{};
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
        summary.confidence_threshold = kEvaluationAxes.confidence[best];
        for (size_t area = 0; area < kEvaluationAreaCount; ++area) {
            AreaReduction aggregate;
            EvaluationMetricDetail* aggregate_detail = retain_details ? &details_[base + area * row_width] : nullptr;
            if (aggregate_detail) {
                aggregate_detail->kind = kind;
                aggregate_detail->area = static_cast<EvaluationArea>(area);
                aggregate_detail->detection_limits = caps;
                aggregate_detail->confidence = macro[best];
                aggregate_detail->confidence_threshold = summary.confidence_threshold;
            }
            size_t available_categories = 0U;
            for (size_t category = 0; category < catalog_->size(); ++category) {
                auto& detail = scratch.categories[category].areas[area];
                if (detail.detail) {
                    auto& row = *detail.detail;
                    row.kind = kind;
                    row.area = static_cast<EvaluationArea>(area);
                    row.category = static_cast<std::uint32_t>(category);
                    row.available = detail.available;
                    row.ground_truth_count = detail.ground_truth_count;
                    row.detection_limits = caps;
                    row.average_precision = detail.average_precision;
                    row.average_recall = detail.average_recall;
                    row.confidence = scratch.categories[category].confidence[best];
                    row.confidence_threshold = summary.confidence_threshold;
                }
                aggregate.ground_truth_count += detail.ground_truth_count;
                if (!detail.available) continue;
                ++available_categories;
                for (size_t iou = 0; iou < kIouThresholds.size(); ++iou) {
                    aggregate.average_precision[iou] += detail.average_precision[iou];
                    for (size_t cap = 0; cap < caps.size(); ++cap)
                        aggregate.average_recall[cap][iou] += detail.average_recall[cap][iou];
                    if (aggregate_detail) for (size_t recall = 0; recall < kEvaluationRecallCount; ++recall)
                        aggregate_detail->precision_curve[iou][recall] += detail.detail->precision_curve[iou][recall];
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
                    if (aggregate_detail) for (double& precision : aggregate_detail->precision_curve[iou]) precision /= static_cast<double>(available_categories);
                }
            }
            if (aggregate_detail) {
                aggregate_detail->available = aggregate.available;
                aggregate_detail->ground_truth_count = aggregate.ground_truth_count;
                aggregate_detail->average_precision = aggregate.average_precision;
                aggregate_detail->average_recall = aggregate.average_recall;
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

} // namespace mmltk::backend::models::rfdetr
namespace mmltk::backend::models::rfdetr {

struct EvaluationDatasetOwner::Impl final {
    Impl(mmltk::backend::data::DatasetLoader& loader, const EvaluationMetricSet metric_set)
        : dataset(CocoDataset::load_from_loader(loader, metric_set)) {}

    Impl(const Impl&) = default;

    CocoDataset dataset;
};

EvaluationDatasetOwner::EvaluationDatasetOwner(mmltk::backend::data::DatasetLoader& loader, const EvaluationMetricSet metric_set)
    : impl_(std::make_unique<Impl>(loader, metric_set)) {}
EvaluationDatasetOwner::~EvaluationDatasetOwner() = default;
EvaluationDatasetOwner::EvaluationDatasetOwner(const EvaluationDatasetOwner& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
EvaluationDatasetOwner& EvaluationDatasetOwner::operator=(const EvaluationDatasetOwner& other) {
    if (this != &other) impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}
EvaluationDatasetOwner::EvaluationDatasetOwner(EvaluationDatasetOwner&&) noexcept = default;
EvaluationDatasetOwner& EvaluationDatasetOwner::operator=(EvaluationDatasetOwner&&) noexcept = default;

void EvaluationDatasetOwner::clear_predictions() { impl_->dataset.clear_predictions(); }
void EvaluationDatasetOwner::limit_images(const std::size_t limit) { impl_->dataset.limit_images(limit); }
void EvaluationDatasetOwner::merge_bbox_predictions(const std::int64_t dataset_index, const BBoxPredictionView predictions,
                                                    const std::size_t max_dets_per_image) {
    impl_->dataset.merge_matches(impl_->dataset.match_staged_predictions(dataset_index, predictions, std::nullopt, max_dets_per_image));
}
EvaluationDatasetOwner::ImageMatches EvaluationDatasetOwner::match_predictions(const std::int64_t dataset_index,
                                                                               const BBoxPredictionView predictions,
                                                                               const std::optional<PackedMaskPredictionView> masks,
                                                                               const std::size_t max_dets_per_image, const std::span<const Prediction> encoded_masks) const {
    return impl_->dataset.match_staged_predictions(dataset_index, predictions, masks, max_dets_per_image, encoded_masks);
}
void EvaluationDatasetOwner::merge_matches(ImageMatches matches) { impl_->dataset.merge_matches(std::move(matches)); }
EvalSummary EvaluationDatasetOwner::evaluate(const std::size_t max_dets_per_image, EvaluationDetailRetention retention) const {
    return impl_->dataset.evaluate(max_dets_per_image, retention);
}
std::vector<EvaluationMetricDetail> EvaluationDatasetOwner::take_details() { return impl_->dataset.take_details(); }
EvalSummary EvaluationDatasetOwner::evaluate(const std::size_t max_dets_per_image,
                                             mmltk::common::concurrency::WorkerPool& worker_pool, EvaluationDetailRetention retention) const {
    return impl_->dataset.evaluate(max_dets_per_image, retention, &worker_pool);
}
std::vector<int> EvaluationDatasetOwner::image_ids() const { return impl_->dataset.image_ids(); }
std::size_t EvaluationDatasetOwner::image_count() const noexcept { return impl_->dataset.num_images(); }
const std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>& EvaluationDatasetOwner::class_catalog() const noexcept { return impl_->dataset.class_catalog(); }
std::size_t EvaluationDatasetOwner::category_count() const noexcept { return impl_->dataset.num_categories(); }
EvaluationDatasetOwner::Facts EvaluationDatasetOwner::facts() const noexcept {
    return Facts{
        impl_->dataset.metric_set(),
        impl_->dataset.prediction_count(),
        impl_->dataset.ground_truth_count(),
        impl_->dataset.mask_rle_pair_count(),
    };
}
}  // namespace mmltk::backend::models::rfdetr
