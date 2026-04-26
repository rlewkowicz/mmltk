#pragma once

#include "mmltk/live/live_frame_id.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::gui {

template <typename SampleFn>
void rasterize_line_samples(const int x0, const int y0, const int x1, const int y1, SampleFn&& sample_fn) {
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int steps = std::max(dx, dy);
    if (steps == 0) {
        sample_fn(x0, y0);
        return;
    }
    for (int step = 0; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const int sample_x = static_cast<int>(std::round(static_cast<float>(x0) + static_cast<float>(x1 - x0) * t));
        const int sample_y = static_cast<int>(std::round(static_cast<float>(y0) + static_cast<float>(y1 - y0) * t));
        sample_fn(sample_x, sample_y);
    }
}

struct AnnotationHsv {
    float hue_degrees = 180.0f;
    float saturation = 0.5f;
    float value = 0.5f;
};

[[nodiscard]] constexpr inline bool operator==(const AnnotationHsv& lhs, const AnnotationHsv& rhs) noexcept {
    return lhs.hue_degrees == rhs.hue_degrees && lhs.saturation == rhs.saturation && lhs.value == rhs.value;
}

struct AnnotationColorTolerance {
    float hue_minus_pct = 0.0f;
    float hue_plus_pct = 0.0f;
    float saturation_minus_pct = 0.0f;
    float saturation_plus_pct = 0.0f;
    float value_minus_pct = 0.0f;
    float value_plus_pct = 0.0f;
};

[[nodiscard]] constexpr inline bool operator==(const AnnotationColorTolerance& lhs,
                                               const AnnotationColorTolerance& rhs) noexcept {
    return lhs.hue_minus_pct == rhs.hue_minus_pct && lhs.hue_plus_pct == rhs.hue_plus_pct &&
           lhs.saturation_minus_pct == rhs.saturation_minus_pct && lhs.saturation_plus_pct == rhs.saturation_plus_pct &&
           lhs.value_minus_pct == rhs.value_minus_pct && lhs.value_plus_pct == rhs.value_plus_pct;
}

struct AnnotationColorRange {
    AnnotationHsv center{};
    AnnotationColorTolerance tolerance{};
    bool sampling = false;
};

[[nodiscard]] constexpr inline bool operator==(const AnnotationColorRange& lhs,
                                               const AnnotationColorRange& rhs) noexcept {
    return lhs.center == rhs.center && lhs.tolerance == rhs.tolerance && lhs.sampling == rhs.sampling;
}

struct AnnotationBox {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
};

struct AnnotationFrame {
    std::string source_name;
    std::filesystem::path source_path;
    std::uint64_t frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> live_frame_id;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t view_x = 0;
    std::uint32_t view_y = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    std::vector<std::uint8_t> pixels_bgr;
};

struct AnnotationMaskRegion {
    std::uint32_t capture_x = 0;
    std::uint32_t capture_y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

}  // namespace mmltk::gui
