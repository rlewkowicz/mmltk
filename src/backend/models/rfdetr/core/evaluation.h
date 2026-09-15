#pragma once
#include "src/backend/models/rfdetr/contract/prediction_limits.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace mmltk::backend::models::rfdetr {

struct PhaseTiming {
    double seconds = 0.0;
    double img_per_s = 0.0;
    std::size_t images = 0;
};

enum class EvaluationMetricSet : std::uint8_t {
    BBox,
    BBoxAndMask,
};

[[nodiscard]] constexpr const char* evaluation_metric_set_name(const EvaluationMetricSet metric_set) noexcept {
    switch (metric_set) {
        case EvaluationMetricSet::BBox:
            return "bbox";
        case EvaluationMetricSet::BBoxAndMask:
            return "bbox_and_mask";
    }
    return "unknown";
}

struct EncodedMask {
    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t area = 0;
    std::vector<std::pair<uint32_t, uint32_t>> runs;
};

template <typename ValueAt>
void encode_mask_values_into(const uint32_t height, const uint32_t width, EncodedMask& mask, ValueAt&& value_at,
                             std::size_t maximum_runs = kMaximumPredictionMaskRuns) {
    mask.height = height;
    mask.width = width;
    mask.area = 0;
    mask.runs.clear();
    if (width == 0U || height == 0U) return;
    const auto pixel_count = static_cast<uint32_t>(checked_prediction_extent(width, height, kMaximumEncodedMaskPixels));
    maximum_runs = std::min(maximum_runs, kMaximumPredictionMaskRuns);
    // Allocate only emitted runs. Geometric spare storage stays below twice
    // their physical size and is independent of the serialized run allowance.
    const auto append_run = [&](uint32_t start, uint32_t length) {
        if (mask.runs.size() == maximum_runs) throw std::invalid_argument("RF-DETR encoded masks exceed aggregate storage capacity");
        if (mask.runs.size() == mask.runs.capacity())
            mask.runs.reserve(std::min(maximum_runs, std::max(std::size_t{1U}, mask.runs.capacity() * 2U)));
        mask.runs.emplace_back(start, length);
    };
    bool in_run = false;
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    for (uint32_t index = 0; index < pixel_count; ++index) {
        const bool value = value_at(index);
        mask.area += value ? 1U : 0U;
        if (value) {
            if (!in_run) {
                in_run = true;
                run_start = index;
                run_length = 1;
            } else {
                ++run_length;
            }
        } else if (in_run) {
            append_run(run_start, run_length);
            in_run = false;
            run_length = 0;
        }
    }
    if (in_run) { append_run(run_start, run_length); }
}

struct Prediction {
    int image_id = 0;
    int category_id = 0;
    float score = 0.0f;
    std::array<float, 4> bbox_xyxy{};
    EncodedMask mask;
    bool has_mask = false;
};

struct BBoxPredictionView {
    int image_id = 0;
    const float* scores = nullptr;
    const std::int64_t* labels_zero_based = nullptr;
    const float* boxes_xyxy = nullptr;
    size_t count = 0;
    std::ptrdiff_t score_stride = 1;
    std::ptrdiff_t label_stride = 1;
    std::ptrdiff_t box_stride = 4;
};

struct PackedMaskPredictionView {
    const std::uint8_t* data = nullptr;
    std::ptrdiff_t prediction_stride = 0;
    std::uint32_t height = 0;
    std::uint32_t width = 0;
};

struct MetricSummary {
    double ap = 0.0;
    double ap50 = 0.0;
    double ap75 = 0.0;
};

struct EvalSummary {
    MetricSummary bbox;
    std::optional<MetricSummary> mask;
};

struct AlignmentStats {
    size_t images_compared = 0;
    double top1_score_abs_diff_mean = 0.0;
    double top1_score_abs_diff_max = 0.0;
    double top1_box_abs_diff_px_mean = 0.0;
    double top1_box_abs_diff_px_max = 0.0;
    double top1_mask_xor_pixels_mean = 0.0;
    double top1_mask_xor_pixels_max = 0.0;
};

}  // namespace mmltk::backend::models::rfdetr
