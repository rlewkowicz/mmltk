#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

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

}  
