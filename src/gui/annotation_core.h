#pragma once

#include "gui/annotation/common.h"
#include "gui/annotation/document/types.h"

#include "mmltk/rfdetr/evaluation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

struct PredictImageInput;

}

namespace mmltk::gui {

struct AnnotationProjectedScene;
struct AnnotationVisibleGeometry;
struct AnnotationVisibleObject;

struct AnnotationProjectedSceneLookup {
    const AnnotationProjectedScene* projected_scene = nullptr;
    std::size_t object_count = 0;
    mutable std::size_t visible_object_cursor = 0;
    mutable std::size_t visible_geometry_cursor = 0;

    [[nodiscard]] const AnnotationVisibleObject* visible_object(std::size_t index) const noexcept;
    [[nodiscard]] const AnnotationVisibleGeometry* visible_geometry(std::size_t index) const noexcept;
};

struct AnnotationCategorySkeletonEdge {
    std::size_t source_index = 0;
    std::size_t target_index = 0;
};

struct AnnotationCategory {
    AnnotationCategory() = default;
    AnnotationCategory(int id_in, std::string name_in) : id(id_in), name(std::move(name_in)) {}

    int id = 1;
    std::string name;
    std::vector<std::string> keypoints;
    std::vector<AnnotationCategorySkeletonEdge> skeleton_edges;
};

struct AnnotationCategories {
    std::string dataset_name = "annotation-dataset";
    std::vector<AnnotationCategory> items;
};

struct AnnotationSaveConfig {
    std::filesystem::path output_root;
    std::string split = "train";
};

struct AnnotationSaveResult {
    std::filesystem::path scene_image_path;
    std::filesystem::path scene_jsonl_path;
    std::vector<std::filesystem::path> entity_paths;
    std::uint32_t scene_index = 0;
};

[[nodiscard]] inline float annotation_clamp_unit(const float value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] inline float annotation_wrap_hue(const float hue_degrees) noexcept {
    constexpr float kHueRange = 360.0f;
    float wrapped = std::fmod(hue_degrees, kHueRange);
    if (wrapped < 0.0f) {
        wrapped += kHueRange;
    }
    return wrapped;
}

[[nodiscard]] inline AnnotationHsv annotation_bgr_to_hsv(const std::uint8_t b, const std::uint8_t g,
                                                         const std::uint8_t r) noexcept {
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
    hsv.hue_degrees = annotation_wrap_hue(hsv.hue_degrees);
    return hsv;
}

AnnotationFrame load_annotation_frame(const mmltk::rfdetr::PredictImageInput& input);
void write_annotation_frame_png(const std::filesystem::path& path, const AnnotationFrame& frame);
AnnotationHsv sample_annotation_hsv(const AnnotationFrame& frame, int x, int y);
void recenter_annotation_range(AnnotationColorRange& range, const AnnotationHsv& center);
bool annotation_range_active(const AnnotationColorRange& range);
bool annotation_box_has_area(const AnnotationBox& box);
[[nodiscard]] inline AnnotationBox annotation_intersect_boxes(const AnnotationBox& lhs,
                                                              const AnnotationBox& rhs) noexcept {
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

AnnotationBox normalize_annotation_box(AnnotationBox box, std::uint32_t width, std::uint32_t height);
std::uint32_t annotation_frame_capture_width(const AnnotationFrame& frame);
std::uint32_t annotation_frame_capture_height(const AnnotationFrame& frame);
AnnotationBox annotation_frame_view_box(const AnnotationFrame& frame);
AnnotationBox annotation_box_to_frame(const AnnotationFrame& frame, const AnnotationBox& capture_box);
AnnotationBox annotation_box_from_frame(const AnnotationFrame& frame, const AnnotationBox& frame_box);
[[nodiscard]] inline AnnotationPoint annotation_capture_point_to_frame_unclipped(
    const AnnotationFrame& frame, const AnnotationPoint& point) noexcept {
    return AnnotationPoint{
        point.x - static_cast<float>(frame.view_x),
        point.y - static_cast<float>(frame.view_y),
    };
}

[[nodiscard]] inline AnnotationPoint annotation_frame_point_to_capture_unclipped(const AnnotationPoint& point,
                                                                                 const std::uint32_t view_x,
                                                                                 const std::uint32_t view_y) noexcept {
    return AnnotationPoint{
        point.x + static_cast<float>(view_x),
        point.y + static_cast<float>(view_y),
    };
}

AnnotationMaskRegion annotation_mask_region_from_frame(const AnnotationFrame& frame);
AnnotationFrame extract_annotation_frame_region(const AnnotationFrame& frame, const AnnotationBox& capture_box);
std::optional<AnnotationBox> annotation_bbox_from_mask(const std::vector<std::uint8_t>& mask, std::uint32_t width,
                                                       std::uint32_t height);
std::vector<std::uint8_t> decode_annotation_prediction_mask(const mmltk::rfdetr::EncodedMask& mask, std::uint32_t width,
                                                            std::uint32_t height);
std::vector<std::uint8_t> decode_annotation_mask_rle(std::string_view encoded_mask, std::uint32_t width,
                                                     std::uint32_t height);
std::string encode_annotation_mask_rle(const std::vector<std::uint8_t>& mask);
[[nodiscard]] AnnotationProjectedSceneLookup make_annotation_projected_scene_lookup(
    const AnnotationProjectedScene* projected_scene, std::size_t object_count);
std::vector<AnnotationResolvedObject> resolve_annotation_objects(
    const AnnotationFrame& frame, const AnnotationCategories& categories, const std::vector<AnnotationObject>& objects,
    bool live_mode, const AnnotationProjectedScene* projected_scene = nullptr);
AnnotationCategories load_annotation_categories(const std::filesystem::path& output_root);
std::size_t ensure_annotation_category(AnnotationCategories& categories, const std::string& class_name);
void write_annotation_categories(const std::filesystem::path& output_root, const AnnotationCategories& categories);
std::vector<AnnotationObject> load_annotation_scene_objects(const std::filesystem::path& scene_jsonl_path,
                                                            AnnotationCategories* categories);
std::optional<std::vector<AnnotationObject>> load_saved_annotation_scene_for_frame(
    const std::filesystem::path& output_root, const AnnotationFrame& frame, AnnotationCategories* categories);
void write_annotation_png(const std::filesystem::path& path, int width, int height, int channels, const void* pixels,
                          int stride_bytes);
AnnotationSaveResult save_annotation_scene(const AnnotationSaveConfig& config, const AnnotationFrame& frame,
                                           AnnotationCategories& categories,
                                           const std::vector<AnnotationObject>& objects, bool live_mode,
                                           const AnnotationProjectedScene* projected_scene = nullptr);

}  
