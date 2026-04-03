#pragma once

#include "mmltk/live/live_frame_id.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::gui {

struct AnnotationHsv {
    float hue_degrees = 180.0f;
    float saturation = 0.5f;
    float value = 0.5f;
};

struct AnnotationColorTolerance {
    float hue_minus_pct = 0.0f;
    float hue_plus_pct = 0.0f;
    float saturation_minus_pct = 0.0f;
    float saturation_plus_pct = 0.0f;
    float value_minus_pct = 0.0f;
    float value_plus_pct = 0.0f;
};

struct AnnotationColorRange {
    AnnotationHsv center{};
    AnnotationColorTolerance tolerance{};
    bool sampling = false;
};

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

} // namespace mmltk::gui
