#include "annotation_core.h"

#include <nlohmann/json.hpp>

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace fastloader::gui {

namespace {

using json = nlohmann::json;

constexpr float kHueRange = 360.0f;
constexpr float kMaskBlend = 0.38f;

std::string fallback_source_name(const std::filesystem::path& path) {
    return path.filename().empty() ? path.string() : path.filename().string();
}

float clamp_unit(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float wrap_hue(float hue_degrees) {
    float wrapped = std::fmod(hue_degrees, kHueRange);
    if (wrapped < 0.0f) {
        wrapped += kHueRange;
    }
    return wrapped;
}

AnnotationHsv bgr_to_hsv(std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    const float bf = static_cast<float>(b) / 255.0f;
    const float gf = static_cast<float>(g) / 255.0f;
    const float rf = static_cast<float>(r) / 255.0f;
    const float maximum = std::max({rf, gf, bf});
    const float minimum = std::min({rf, gf, bf});
    const float delta = maximum - minimum;

    AnnotationHsv hsv;
    hsv.value = maximum;
    hsv.saturation = maximum <= 0.0f ? 0.0f : delta / maximum;
    if (delta <= 0.0f) {
        hsv.hue_degrees = 0.0f;
        return hsv;
    }

    if (maximum == rf) {
        hsv.hue_degrees = 60.0f * std::fmod(((gf - bf) / delta), 6.0f);
    } else if (maximum == gf) {
        hsv.hue_degrees = 60.0f * (((bf - rf) / delta) + 2.0f);
    } else {
        hsv.hue_degrees = 60.0f * (((rf - gf) / delta) + 4.0f);
    }
    hsv.hue_degrees = wrap_hue(hsv.hue_degrees);
    return hsv;
}

bool hue_in_window(float hue, float minimum, float maximum) {
    const float wrapped_hue = wrap_hue(hue);
    const float wrapped_min = wrap_hue(minimum);
    const float wrapped_max = wrap_hue(maximum);
    if (wrapped_min <= wrapped_max) {
        return wrapped_hue >= wrapped_min && wrapped_hue <= wrapped_max;
    }
    return wrapped_hue >= wrapped_min || wrapped_hue <= wrapped_max;
}

bool pixel_matches_range(const AnnotationColorRange& range, const AnnotationHsv& hsv) {
    if (!annotation_range_active(range)) {
        return false;
    }

    const float hue_minus = range.tolerance.hue_minus_pct * (kHueRange / 100.0f);
    const float hue_plus = range.tolerance.hue_plus_pct * (kHueRange / 100.0f);
    const float sat_min = clamp_unit(range.center.saturation - range.tolerance.saturation_minus_pct / 100.0f);
    const float sat_max = clamp_unit(range.center.saturation + range.tolerance.saturation_plus_pct / 100.0f);
    const float value_min = clamp_unit(range.center.value - range.tolerance.value_minus_pct / 100.0f);
    const float value_max = clamp_unit(range.center.value + range.tolerance.value_plus_pct / 100.0f);

    return hue_in_window(hsv.hue_degrees, range.center.hue_degrees - hue_minus, range.center.hue_degrees + hue_plus) &&
           hsv.saturation >= sat_min && hsv.saturation <= sat_max &&
           hsv.value >= value_min && hsv.value <= value_max;
}

std::array<std::uint8_t, 3> category_color(std::size_t index) {
    static constexpr std::array<std::array<std::uint8_t, 3>, 8> palette{{
        {{240, 196, 68}},
        {{88, 188, 255}},
        {{255, 128, 88}},
        {{96, 214, 146}},
        {{214, 112, 255}},
        {{255, 96, 152}},
        {{180, 214, 92}},
        {{255, 168, 64}},
    }};
    return palette[index % palette.size()];
}

void draw_rect_bgr(std::vector<std::uint8_t>& pixels,
                   std::uint32_t width,
                   std::uint32_t height,
                   const AnnotationBox& box,
                   const std::array<std::uint8_t, 3>& color,
                   int thickness) {
    if (width == 0 || height == 0 || thickness <= 0) {
        return;
    }
    const AnnotationBox clamped = normalize_annotation_box(box, width, height);
    if (clamped.x2 <= clamped.x1 || clamped.y2 <= clamped.y1) {
        return;
    }

    const auto paint = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
            return;
        }
        const std::size_t offset =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3U;
        pixels[offset + 0] = color[2];
        pixels[offset + 1] = color[1];
        pixels[offset + 2] = color[0];
    };

    for (int t = 0; t < thickness; ++t) {
        const int x1 = clamped.x1 - t;
        const int x2 = clamped.x2 - 1 + t;
        const int y1 = clamped.y1 - t;
        const int y2 = clamped.y2 - 1 + t;
        for (int x = x1; x <= x2; ++x) {
            paint(x, y1);
            paint(x, y2);
        }
        for (int y = y1; y <= y2; ++y) {
            paint(x1, y);
            paint(x2, y);
        }
    }
}

std::vector<std::uint8_t> seed_mask_from_box(const AnnotationBox& box,
                                             std::uint32_t width,
                                             std::uint32_t height) {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    const AnnotationBox clamped = normalize_annotation_box(box, width, height);
    for (int y = clamped.y1; y < clamped.y2; ++y) {
        const std::size_t row_offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = clamped.x1; x < clamped.x2; ++x) {
            mask[row_offset + static_cast<std::size_t>(x)] = 1U;
        }
    }
    return mask;
}

AnnotationBox intersect_boxes(const AnnotationBox& lhs, const AnnotationBox& rhs) {
    const AnnotationBox overlap{
        std::max(lhs.x1, rhs.x1),
        std::max(lhs.y1, rhs.y1),
        std::min(lhs.x2, rhs.x2),
        std::min(lhs.y2, rhs.y2),
    };
    if (!annotation_box_has_area(overlap)) {
        return {};
    }
    return overlap;
}

AnnotationBox mask_region_box(const AnnotationMaskRegion& region) {
    return AnnotationBox{
        static_cast<int>(region.capture_x),
        static_cast<int>(region.capture_y),
        static_cast<int>(region.capture_x + region.width),
        static_cast<int>(region.capture_y + region.height),
    };
}

bool mask_region_valid(const AnnotationInstance& instance) {
    return instance.seed_mask_region.width > 0 &&
           instance.seed_mask_region.height > 0 &&
           instance.seed_mask.size() ==
               static_cast<std::size_t>(instance.seed_mask_region.width) *
                   static_cast<std::size_t>(instance.seed_mask_region.height);
}

std::vector<std::uint8_t> project_mask_region_to_frame(const AnnotationFrame& frame,
                                                       const AnnotationMaskRegion& region,
                                                       const std::vector<std::uint8_t>& mask) {
    std::vector<std::uint8_t> projected(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height), 0U);
    if (projected.empty() || mask.empty() || region.width == 0 || region.height == 0) {
        return projected;
    }

    const AnnotationBox region_box = mask_region_box(region);
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    const AnnotationBox overlap = intersect_boxes(region_box, view_box);
    if (!annotation_box_has_area(overlap)) {
        return projected;
    }

    for (int capture_y = overlap.y1; capture_y < overlap.y2; ++capture_y) {
        const std::size_t src_row =
            static_cast<std::size_t>(capture_y - region_box.y1) * static_cast<std::size_t>(region.width);
        const std::size_t dst_row =
            static_cast<std::size_t>(capture_y - view_box.y1) * static_cast<std::size_t>(frame.width);
        for (int capture_x = overlap.x1; capture_x < overlap.x2; ++capture_x) {
            const std::size_t src_index = src_row + static_cast<std::size_t>(capture_x - region_box.x1);
            const std::size_t dst_index = dst_row + static_cast<std::size_t>(capture_x - view_box.x1);
            projected[dst_index] = mask[src_index];
        }
    }
    return projected;
}

std::vector<std::uint8_t> effective_seed_mask(const AnnotationFrame& frame,
                                              const AnnotationInstance& instance,
                                              bool live_mode,
                                              AnnotationBox* out_box) {
    AnnotationBox box = annotation_box_to_frame(frame, instance.box);
    if (instance.seed_kind == AnnotationSeedKind::ModelMask &&
        mask_region_valid(instance) &&
        (!live_mode || instance.seed_frame_id == 0U || instance.seed_frame_id == frame.frame_id)) {
        std::vector<std::uint8_t> projected_mask =
            project_mask_region_to_frame(frame, instance.seed_mask_region, instance.seed_mask);
        if (const std::optional<AnnotationBox> mask_box =
                annotation_bbox_from_mask(projected_mask, frame.width, frame.height);
            mask_box.has_value()) {
            box = *mask_box;
        }
        if (out_box != nullptr) {
            *out_box = box;
        }
        return projected_mask;
    }
    if (out_box != nullptr) {
        *out_box = box;
    }
    return seed_mask_from_box(box, frame.width, frame.height);
}

AnnotationResolvedInstance resolve_instance(const AnnotationFrame& frame,
                                            const AnnotationCategories& categories,
                                            const AnnotationInstance& instance,
                                            std::size_t instance_index,
                                            bool live_mode) {
    if (instance.category_index >= categories.items.size()) {
        throw std::runtime_error("annotation instance category index is out of range");
    }

    AnnotationBox effective_box{};
    std::vector<std::uint8_t> seed_mask = effective_seed_mask(frame, instance, live_mode, &effective_box);
    if (seed_mask.size() != static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height)) {
        throw std::runtime_error("annotation seed mask size does not match frame dimensions");
    }

    AnnotationResolvedInstance resolved;
    resolved.instance_index = instance_index;
    resolved.category_index = instance.category_index;
    resolved.class_name = categories.items[instance.category_index].name;
    resolved.mask.assign(seed_mask.size(), 0U);

    const AnnotationBox box = normalize_annotation_box(effective_box, frame.width, frame.height);
    for (int y = box.y1; y < box.y2; ++y) {
        const std::size_t row_offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width);
        for (int x = box.x1; x < box.x2; ++x) {
            const std::size_t pixel_index = row_offset + static_cast<std::size_t>(x);
            if (seed_mask[pixel_index] == 0U) {
                continue;
            }
            const std::size_t byte_offset = pixel_index * 3U;
            const AnnotationHsv hsv =
                bgr_to_hsv(frame.pixels_bgr[byte_offset + 0],
                           frame.pixels_bgr[byte_offset + 1],
                           frame.pixels_bgr[byte_offset + 2]);
            const bool sup_match = pixel_matches_range(instance.sup, hsv);
            const bool nosup_match = pixel_matches_range(instance.nosup, hsv);
            if (!sup_match || nosup_match) {
                resolved.mask[pixel_index] = 1U;
            }
        }
    }

    const std::optional<AnnotationBox> bbox = annotation_bbox_from_mask(resolved.mask, frame.width, frame.height);
    if (!bbox.has_value()) {
        resolved.mask.clear();
        return resolved;
    }
    resolved.bbox = *bbox;
    resolved.mask_rle = encode_annotation_mask_rle(resolved.mask);
    resolved.crop_width = static_cast<std::uint32_t>(resolved.bbox.x2 - resolved.bbox.x1);
    resolved.crop_height = static_cast<std::uint32_t>(resolved.bbox.y2 - resolved.bbox.y1);
    resolved.crop_rgba.assign(static_cast<std::size_t>(resolved.crop_width) *
                                  static_cast<std::size_t>(resolved.crop_height) * 4U,
                              0U);
    for (int y = resolved.bbox.y1; y < resolved.bbox.y2; ++y) {
        for (int x = resolved.bbox.x1; x < resolved.bbox.x2; ++x) {
            const std::size_t source_index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x);
            const std::size_t crop_index =
                (static_cast<std::size_t>(y - resolved.bbox.y1) * static_cast<std::size_t>(resolved.crop_width) +
                 static_cast<std::size_t>(x - resolved.bbox.x1)) *
                4U;
            const std::size_t source_byte = source_index * 3U;
            resolved.crop_rgba[crop_index + 0] = frame.pixels_bgr[source_byte + 2];
            resolved.crop_rgba[crop_index + 1] = frame.pixels_bgr[source_byte + 1];
            resolved.crop_rgba[crop_index + 2] = frame.pixels_bgr[source_byte + 0];
            resolved.crop_rgba[crop_index + 3] = resolved.mask[source_index] == 0U ? 0U : 255U;
        }
    }
    return resolved;
}

std::vector<std::uint8_t> bgr_to_rgb_copy(const std::vector<std::uint8_t>& pixels_bgr) {
    std::vector<std::uint8_t> pixels_rgb = pixels_bgr;
    for (std::size_t offset = 0; offset + 2U < pixels_rgb.size(); offset += 3U) {
        std::swap(pixels_rgb[offset + 0], pixels_rgb[offset + 2]);
    }
    return pixels_rgb;
}

void write_png_checked(const std::filesystem::path& path,
                       int width,
                       int height,
                       int channels,
                       const void* pixels,
                       int stride_bytes) {
    if (stbi_write_png(path.c_str(), width, height, channels, pixels, stride_bytes) == 0) {
        throw std::runtime_error("failed to write PNG: " + path.string());
    }
}

bool all_digits(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::uint32_t next_scene_index(const std::filesystem::path& split_dir) {
    std::uint32_t maximum = 0;
    if (!std::filesystem::exists(split_dir)) {
        return 1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(split_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".png") {
            continue;
        }
        const std::string stem = entry.path().stem().string();
        if (!all_digits(stem)) {
            continue;
        }
        maximum = std::max(maximum, static_cast<std::uint32_t>(std::stoul(stem)));
    }
    return maximum + 1U;
}

std::string format_scene_name(std::uint32_t scene_index) {
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%06u", scene_index);
    if (written <= 0) {
        throw std::runtime_error("failed to format annotation scene index");
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

json category_json(const AnnotationCategory& category) {
    return json{
        {"id", category.id},
        {"name", category.name},
    };
}

json category_split_stats(const std::filesystem::path& split_dir) {
    std::uint64_t total = 0;
    if (std::filesystem::exists(split_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(split_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".png") {
                ++total;
            }
        }
    }
    return json{
        {"total", total},
        {"background", 0},
        {"annotated", total},
    };
}

void append_jsonl(const std::filesystem::path& path, const json& entry) {
    std::ofstream stream(path, std::ios::app);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to open JSONL manifest: " + path.string());
    }
    stream << entry.dump() << '\n';
}

} // namespace

AnnotationFrame load_annotation_frame(const fastloader::rfdetr::PredictImageInput& input) {
    int raw_width = 0;
    int raw_height = 0;
    int raw_channels = 0;
    stbi_uc* raw_pixels = stbi_load(input.image_path.c_str(), &raw_width, &raw_height, &raw_channels, 3);
    if (raw_pixels == nullptr) {
        throw std::runtime_error("failed to load annotation image: " + input.image_path.string());
    }
    if (raw_width <= 0 || raw_height <= 0) {
        stbi_image_free(raw_pixels);
        throw std::runtime_error("annotation image has invalid dimensions: " + input.image_path.string());
    }

    AnnotationFrame frame;
    frame.source_name = input.source_name.empty() ? fallback_source_name(input.image_path) : input.source_name;
    frame.source_path = input.image_path;
    frame.frame_id = input.image_id > 0 ? static_cast<std::uint64_t>(input.image_id) : 1U;
    frame.width = static_cast<std::uint32_t>(raw_width);
    frame.height = static_cast<std::uint32_t>(raw_height);
    frame.view_x = 0;
    frame.view_y = 0;
    frame.capture_width = frame.width;
    frame.capture_height = frame.height;
    const std::size_t byte_count =
        static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3U;
    frame.pixels_bgr.assign(raw_pixels, raw_pixels + byte_count);
    stbi_image_free(raw_pixels);
    for (std::size_t offset = 0; offset + 2U < frame.pixels_bgr.size(); offset += 3U) {
        std::swap(frame.pixels_bgr[offset + 0], frame.pixels_bgr[offset + 2]);
    }
    return frame;
}

void write_annotation_frame_png(const std::filesystem::path& path, const AnnotationFrame& frame) {
    const std::vector<std::uint8_t> pixels_rgb = bgr_to_rgb_copy(frame.pixels_bgr);
    write_png_checked(path,
                      static_cast<int>(frame.width),
                      static_cast<int>(frame.height),
                      3,
                      pixels_rgb.data(),
                      static_cast<int>(frame.width * 3U));
}

AnnotationHsv sample_annotation_hsv(const AnnotationFrame& frame, int x, int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(frame.width) || y >= static_cast<int>(frame.height)) {
        throw std::runtime_error("annotation eyedropper sample is outside the current frame");
    }
    const std::size_t pixel_index =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x);
    const std::size_t byte_offset = pixel_index * 3U;
    return bgr_to_hsv(frame.pixels_bgr[byte_offset + 0],
                      frame.pixels_bgr[byte_offset + 1],
                      frame.pixels_bgr[byte_offset + 2]);
}

void recenter_annotation_range(AnnotationColorRange& range, const AnnotationHsv& center) {
    range.center.hue_degrees = wrap_hue(center.hue_degrees);
    range.center.saturation = clamp_unit(center.saturation);
    range.center.value = clamp_unit(center.value);
    range.tolerance = {};
    range.sampling = false;
}

bool annotation_range_active(const AnnotationColorRange& range) {
    return range.tolerance.hue_minus_pct > 0.0f ||
           range.tolerance.hue_plus_pct > 0.0f ||
           range.tolerance.saturation_minus_pct > 0.0f ||
           range.tolerance.saturation_plus_pct > 0.0f ||
           range.tolerance.value_minus_pct > 0.0f ||
           range.tolerance.value_plus_pct > 0.0f;
}

bool annotation_box_has_area(const AnnotationBox& box) {
    return box.x2 > box.x1 && box.y2 > box.y1;
}

AnnotationBox normalize_annotation_box(AnnotationBox box, std::uint32_t width, std::uint32_t height) {
    const int max_width = static_cast<int>(width);
    const int max_height = static_cast<int>(height);
    box.x1 = std::clamp(box.x1, 0, max_width);
    box.x2 = std::clamp(box.x2, 0, max_width);
    box.y1 = std::clamp(box.y1, 0, max_height);
    box.y2 = std::clamp(box.y2, 0, max_height);
    if (box.x2 < box.x1) {
        std::swap(box.x1, box.x2);
    }
    if (box.y2 < box.y1) {
        std::swap(box.y1, box.y2);
    }
    return box;
}

std::uint32_t annotation_frame_capture_width(const AnnotationFrame& frame) {
    return frame.capture_width > 0 ? frame.capture_width : frame.width;
}

std::uint32_t annotation_frame_capture_height(const AnnotationFrame& frame) {
    return frame.capture_height > 0 ? frame.capture_height : frame.height;
}

AnnotationBox annotation_frame_view_box(const AnnotationFrame& frame) {
    const std::uint32_t capture_width = annotation_frame_capture_width(frame);
    const std::uint32_t capture_height = annotation_frame_capture_height(frame);
    return normalize_annotation_box(
        AnnotationBox{
            static_cast<int>(frame.view_x),
            static_cast<int>(frame.view_y),
            static_cast<int>(frame.view_x + frame.width),
            static_cast<int>(frame.view_y + frame.height),
        },
        capture_width,
        capture_height);
}

AnnotationBox annotation_box_to_frame(const AnnotationFrame& frame, const AnnotationBox& capture_box) {
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    const AnnotationBox normalized =
        normalize_annotation_box(capture_box, annotation_frame_capture_width(frame), annotation_frame_capture_height(frame));
    const AnnotationBox overlap = intersect_boxes(normalized, view_box);
    if (!annotation_box_has_area(overlap)) {
        return {};
    }
    return AnnotationBox{
        overlap.x1 - view_box.x1,
        overlap.y1 - view_box.y1,
        overlap.x2 - view_box.x1,
        overlap.y2 - view_box.y1,
    };
}

AnnotationBox annotation_box_from_frame(const AnnotationFrame& frame, const AnnotationBox& frame_box) {
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    const AnnotationBox normalized = normalize_annotation_box(frame_box, frame.width, frame.height);
    return normalize_annotation_box(
        AnnotationBox{
            normalized.x1 + view_box.x1,
            normalized.y1 + view_box.y1,
            normalized.x2 + view_box.x1,
            normalized.y2 + view_box.y1,
        },
        annotation_frame_capture_width(frame),
        annotation_frame_capture_height(frame));
}

AnnotationMaskRegion annotation_mask_region_from_frame(const AnnotationFrame& frame) {
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    return AnnotationMaskRegion{
        static_cast<std::uint32_t>(view_box.x1),
        static_cast<std::uint32_t>(view_box.y1),
        frame.width,
        frame.height,
    };
}

AnnotationFrame extract_annotation_frame_region(const AnnotationFrame& frame, const AnnotationBox& capture_box) {
    const AnnotationBox requested =
        normalize_annotation_box(capture_box, annotation_frame_capture_width(frame), annotation_frame_capture_height(frame));
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    const AnnotationBox overlap = intersect_boxes(requested, view_box);
    if (!annotation_box_has_area(requested) ||
        overlap.x1 != requested.x1 ||
        overlap.y1 != requested.y1 ||
        overlap.x2 != requested.x2 ||
        overlap.y2 != requested.y2) {
        throw std::runtime_error("requested annotation crop is outside the current frame view");
    }

    AnnotationFrame extracted = frame;
    extracted.view_x = static_cast<std::uint32_t>(requested.x1);
    extracted.view_y = static_cast<std::uint32_t>(requested.y1);
    extracted.width = static_cast<std::uint32_t>(requested.x2 - requested.x1);
    extracted.height = static_cast<std::uint32_t>(requested.y2 - requested.y1);
    extracted.capture_width = annotation_frame_capture_width(frame);
    extracted.capture_height = annotation_frame_capture_height(frame);
    extracted.pixels_bgr.assign(static_cast<std::size_t>(extracted.width) * static_cast<std::size_t>(extracted.height) * 3U,
                                0U);

    const AnnotationBox local = annotation_box_to_frame(frame, requested);
    for (std::uint32_t row = 0; row < extracted.height; ++row) {
        const std::size_t src_offset =
            (static_cast<std::size_t>(local.y1) + static_cast<std::size_t>(row)) * static_cast<std::size_t>(frame.width) * 3U +
            static_cast<std::size_t>(local.x1) * 3U;
        const std::size_t dst_offset = static_cast<std::size_t>(row) * static_cast<std::size_t>(extracted.width) * 3U;
        std::copy_n(frame.pixels_bgr.begin() + static_cast<std::ptrdiff_t>(src_offset),
                    static_cast<std::ptrdiff_t>(extracted.width * 3U),
                    extracted.pixels_bgr.begin() + static_cast<std::ptrdiff_t>(dst_offset));
    }
    return extracted;
}

std::optional<AnnotationBox> annotation_bbox_from_mask(const std::vector<std::uint8_t>& mask,
                                                       std::uint32_t width,
                                                       std::uint32_t height) {
    if (mask.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) ||
        width == 0U || height == 0U) {
        return std::nullopt;
    }

    int min_x = static_cast<int>(width);
    int min_y = static_cast<int>(height);
    int max_x = -1;
    int max_y = -1;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t row_offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (std::uint32_t x = 0; x < width; ++x) {
            if (mask[row_offset + static_cast<std::size_t>(x)] == 0U) {
                continue;
            }
            min_x = std::min(min_x, static_cast<int>(x));
            min_y = std::min(min_y, static_cast<int>(y));
            max_x = std::max(max_x, static_cast<int>(x));
            max_y = std::max(max_y, static_cast<int>(y));
        }
    }
    if (max_x < min_x || max_y < min_y) {
        return std::nullopt;
    }
    return AnnotationBox{
        min_x,
        min_y,
        max_x + 1,
        max_y + 1,
    };
}

std::vector<std::uint8_t> decode_annotation_prediction_mask(const fastloader::rfdetr::EncodedMask& mask,
                                                            std::uint32_t width,
                                                            std::uint32_t height) {
    std::vector<std::uint8_t> dense(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    if (mask.width != width || mask.height != height) {
        return dense;
    }
    const std::size_t pixel_count = dense.size();
    for (const auto& run : mask.runs) {
        const std::size_t start = std::min<std::size_t>(run.first, pixel_count);
        const std::size_t end = std::min<std::size_t>(start + run.second, pixel_count);
        std::fill(dense.begin() + static_cast<std::ptrdiff_t>(start),
                  dense.begin() + static_cast<std::ptrdiff_t>(end),
                  1U);
    }
    return dense;
}

std::string encode_annotation_mask_rle(const std::vector<std::uint8_t>& mask) {
    std::string encoded;
    std::size_t cursor = 0;
    while (cursor < mask.size()) {
        while (cursor < mask.size() && mask[cursor] == 0U) {
            ++cursor;
        }
        if (cursor >= mask.size()) {
            break;
        }
        const std::size_t start = cursor;
        while (cursor < mask.size() && mask[cursor] != 0U) {
            ++cursor;
        }
        if (!encoded.empty()) {
            encoded.push_back(' ');
        }
        encoded += std::to_string(start);
        encoded.push_back(':');
        encoded += std::to_string(cursor - start);
    }
    return encoded;
}

AnnotationPreviewResult build_annotation_preview(const AnnotationFrame& frame,
                                                 const AnnotationCategories& categories,
                                                 const std::vector<AnnotationInstance>& instances,
                                                 bool live_mode) {
    AnnotationPreviewResult preview;
    preview.preview_bgr = frame.pixels_bgr;
    preview.resolved_instances.reserve(instances.size());

    for (std::size_t index = 0; index < instances.size(); ++index) {
        const AnnotationInstance& instance = instances[index];
        if (!instance.enabled) {
            continue;
        }
        AnnotationResolvedInstance resolved;
        try {
            resolved = resolve_instance(frame, categories, instance, index, live_mode);
        } catch (const std::exception&) {
            continue;
        }
        if (resolved.mask.empty() || resolved.mask_rle.empty()) {
            continue;
        }
        if (resolved.mask.size() * 3U > preview.preview_bgr.size()) {
            continue;
        }
        const std::array<std::uint8_t, 3> color = category_color(resolved.category_index);
        for (std::size_t pixel_index = 0; pixel_index < resolved.mask.size(); ++pixel_index) {
            if (resolved.mask[pixel_index] == 0U) {
                continue;
            }
            const std::size_t byte_offset = pixel_index * 3U;
            const float b = static_cast<float>(preview.preview_bgr[byte_offset + 0]);
            const float g = static_cast<float>(preview.preview_bgr[byte_offset + 1]);
            const float r = static_cast<float>(preview.preview_bgr[byte_offset + 2]);
            preview.preview_bgr[byte_offset + 0] =
                static_cast<std::uint8_t>(std::clamp((1.0f - kMaskBlend) * b + kMaskBlend * color[2], 0.0f, 255.0f));
            preview.preview_bgr[byte_offset + 1] =
                static_cast<std::uint8_t>(std::clamp((1.0f - kMaskBlend) * g + kMaskBlend * color[1], 0.0f, 255.0f));
            preview.preview_bgr[byte_offset + 2] =
                static_cast<std::uint8_t>(std::clamp((1.0f - kMaskBlend) * r + kMaskBlend * color[0], 0.0f, 255.0f));
        }
        draw_rect_bgr(preview.preview_bgr, frame.width, frame.height, resolved.bbox, color, 2);
        preview.resolved_instances.push_back(std::move(resolved));
    }

    return preview;
}

AnnotationCategories load_annotation_categories(const std::filesystem::path& output_root) {
    AnnotationCategories categories;
    if (!output_root.empty() && output_root.filename() != ".") {
        categories.dataset_name = output_root.filename().string();
    }
    const std::filesystem::path categories_path = output_root / "categories.json";
    if (!std::filesystem::exists(categories_path)) {
        return categories;
    }
    std::ifstream stream(categories_path);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to open annotation categories: " + categories_path.string());
    }
    const json parsed = json::parse(stream);
    if (const auto meta = parsed.find("meta"); meta != parsed.end() && meta->is_object()) {
        if (const auto dataset_name = meta->find("dataset_name");
            dataset_name != meta->end() && dataset_name->is_string()) {
            categories.dataset_name = dataset_name->get<std::string>();
        }
    }
    if (const auto classes = parsed.find("classes"); classes != parsed.end() && classes->is_array()) {
        categories.items.clear();
        for (const auto& entry : *classes) {
            categories.items.push_back(AnnotationCategory{
                entry.value("id", static_cast<int>(categories.items.size()) + 1),
                entry.value("name", std::string{}),
            });
        }
    }
    return categories;
}

std::size_t ensure_annotation_category(AnnotationCategories& categories, const std::string& class_name) {
    const auto found = std::find_if(categories.items.begin(), categories.items.end(), [&](const AnnotationCategory& item) {
        return item.name == class_name;
    });
    if (found != categories.items.end()) {
        return static_cast<std::size_t>(std::distance(categories.items.begin(), found));
    }
    const int next_id = categories.items.empty() ? 1 : categories.items.back().id + 1;
    categories.items.push_back(AnnotationCategory{next_id, class_name});
    return categories.items.size() - 1U;
}

void write_annotation_categories(const std::filesystem::path& output_root,
                                 const AnnotationCategories& categories) {
    std::filesystem::create_directories(output_root);
    json payload;
    payload["meta"] = {
        {"dataset_name", categories.dataset_name.empty() ? output_root.filename().string() : categories.dataset_name},
        {"version", "1.0"},
        {"image_format", "png"},
        {"bbox_format", "xyxy_absolute_pixels"},
        {"mask_format", "rle_row_major_start_length"},
        {"background_annotation_policy", "empty_jsonl_file"},
    };
    payload["classes"] = json::array();
    for (const AnnotationCategory& category : categories.items) {
        payload["classes"].push_back(category_json(category));
    }

    json splits = json::object();
    for (const char* split_name : {"train", "val", "test"}) {
        const std::filesystem::path split_dir = output_root / split_name;
        if (!std::filesystem::exists(split_dir)) {
            continue;
        }
        splits[split_name] = category_split_stats(split_dir);
    }
    if (!splits.empty()) {
        payload["splits"] = std::move(splits);
    }

    const std::filesystem::path categories_path = output_root / "categories.json";
    std::ofstream stream(categories_path, std::ios::trunc);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to write annotation categories: " + categories_path.string());
    }
    stream << payload.dump(2) << '\n';
}

AnnotationSaveResult save_annotation_scene(const AnnotationSaveConfig& config,
                                           const AnnotationFrame& frame,
                                           AnnotationCategories& categories,
                                           const std::vector<AnnotationResolvedInstance>& instances) {
    if (config.output_root.empty()) {
        throw std::runtime_error("annotation output root must not be empty");
    }
    std::filesystem::create_directories(config.output_root);
    const std::filesystem::path split_dir = config.output_root / config.split;
    const std::filesystem::path entity_dir = config.output_root / "entities";
    const std::filesystem::path manifest_dir = config.output_root / "manifests";
    std::filesystem::create_directories(split_dir);
    std::filesystem::create_directories(entity_dir);
    std::filesystem::create_directories(manifest_dir);

    const std::uint32_t scene_index = next_scene_index(split_dir);
    const std::string scene_stem = format_scene_name(scene_index);
    const std::filesystem::path scene_image_path = split_dir / (scene_stem + ".png");
    const std::filesystem::path scene_jsonl_path = split_dir / (scene_stem + ".jsonl");

    const std::vector<std::uint8_t> scene_rgb = bgr_to_rgb_copy(frame.pixels_bgr);
    write_png_checked(scene_image_path,
                      static_cast<int>(frame.width),
                      static_cast<int>(frame.height),
                      3,
                      scene_rgb.data(),
                      static_cast<int>(frame.width * 3U));

    std::ofstream scene_stream(scene_jsonl_path, std::ios::trunc);
    if (!scene_stream.is_open()) {
        throw std::runtime_error("failed to write annotation JSONL: " + scene_jsonl_path.string());
    }

    AnnotationSaveResult result;
    result.scene_image_path = scene_image_path;
    result.scene_jsonl_path = scene_jsonl_path;
    result.scene_index = scene_index;
    result.entity_paths.reserve(instances.size());

    for (std::size_t index = 0; index < instances.size(); ++index) {
        const AnnotationResolvedInstance& instance = instances[index];
        if (instance.category_index >= categories.items.size()) {
            throw std::runtime_error("resolved annotation instance category index is out of range");
        }
        const AnnotationCategory& category = categories.items[instance.category_index];
        scene_stream << json{
            {"class", category.name},
            {"bbox_xyxy", {instance.bbox.x1, instance.bbox.y1, instance.bbox.x2, instance.bbox.y2}},
            {"mask_rle_encoding", "row_major_start_length"},
            {"mask_rle", instance.mask_rle},
            {"image_size_wh", {frame.width, frame.height}},
        }.dump() << '\n';

        const std::filesystem::path class_dir = entity_dir / category.name;
        std::filesystem::create_directories(class_dir);
        const std::array<char, 64> suffix = [] (std::uint32_t scene_id, std::size_t instance_id) {
            std::array<char, 64> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%06u_%03zu.png", scene_id, instance_id + 1U);
            return buffer;
        }(scene_index, index);
        const std::filesystem::path entity_path = class_dir / (config.split + "_" + std::string(suffix.data()));
        write_png_checked(entity_path,
                          static_cast<int>(instance.crop_width),
                          static_cast<int>(instance.crop_height),
                          4,
                          instance.crop_rgba.data(),
                          static_cast<int>(instance.crop_width * 4U));
        result.entity_paths.push_back(entity_path);

        append_jsonl(manifest_dir / "entities.jsonl",
                     json{
                         {"split", config.split},
                         {"scene_index", scene_index},
                         {"source_name", frame.source_name},
                         {"source_path", frame.source_path.string()},
                         {"frame_id", frame.frame_id},
                         {"class", category.name},
                         {"bbox_xyxy", {instance.bbox.x1, instance.bbox.y1, instance.bbox.x2, instance.bbox.y2}},
                         {"entity_png", std::filesystem::relative(entity_path, config.output_root).string()},
                         {"scene_png", std::filesystem::relative(scene_image_path, config.output_root).string()},
                     });
    }
    scene_stream.close();

    append_jsonl(manifest_dir / "scenes.jsonl",
                 json{
                     {"split", config.split},
                     {"scene_index", scene_index},
                     {"source_name", frame.source_name},
                     {"source_path", frame.source_path.string()},
                     {"frame_id", frame.frame_id},
                     {"scene_png", std::filesystem::relative(scene_image_path, config.output_root).string()},
                     {"scene_jsonl", std::filesystem::relative(scene_jsonl_path, config.output_root).string()},
                     {"instance_count", instances.size()},
                 });

    write_annotation_categories(config.output_root, categories);
    return result;
}

} // namespace fastloader::gui
