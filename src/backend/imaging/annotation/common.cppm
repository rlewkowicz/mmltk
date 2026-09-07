module;
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

export module mmltk.backend.imaging.annotation.common;

export import mmltk.backend.imaging.annotation.semantic_scene;

export namespace mmltk::backend::imaging::annotation {

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

using AnnotationHsv = Hsv;
using AnnotationColorTolerance = ColorTolerance;
using AnnotationColorRange = ColorRange;
using AnnotationBox = Box;

struct AnnotationFrame {
    std::string source_name;
    std::filesystem::path source_path;
    std::uint64_t frame_id = 0;
    std::optional<ContentIdentity> live_frame_id;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t view_x = 0;
    std::uint32_t view_y = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> pixels_bgr;
};

[[nodiscard]] inline const std::vector<std::uint8_t>& annotation_frame_pixels(const AnnotationFrame& frame) noexcept {
    static const std::vector<std::uint8_t> kEmptyAnnotationPixels;
    return frame.pixels_bgr != nullptr ? *frame.pixels_bgr : kEmptyAnnotationPixels;
}

inline void set_annotation_frame_pixels(AnnotationFrame& frame, std::vector<std::uint8_t> pixels) {
    frame.pixels_bgr = std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
}

using AnnotationMaskRegion = MaskRegion;

}  // namespace mmltk::backend::imaging::annotation
