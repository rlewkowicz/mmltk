#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

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

struct MetricSummary {
    double ap = 0.0;
    double ap50 = 0.0;
    double ap75 = 0.0;
};

struct EvalSummary {
    MetricSummary bbox;
    MetricSummary mask;
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

}  // namespace mmltk::rfdetr
