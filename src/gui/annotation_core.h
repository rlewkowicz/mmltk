#pragma once

#include "fastloader/rfdetr/predict.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fastloader::gui {

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

enum class AnnotationSeedKind : int {
    Box = 0,
    ModelMask = 1,
};

struct AnnotationCategory {
    int id = 1;
    std::string name;
};

struct AnnotationCategories {
    std::string dataset_name = "annotation-dataset";
    std::vector<AnnotationCategory> items;
};

struct AnnotationFrame {
    std::string source_name;
    std::filesystem::path source_path;
    std::uint64_t frame_id = 0;
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

struct AnnotationInstance {
    std::string instance_id;
    bool enabled = true;
    AnnotationSeedKind seed_kind = AnnotationSeedKind::Box;
    AnnotationBox box{};
    std::vector<std::uint8_t> seed_mask;
    AnnotationMaskRegion seed_mask_region{};
    std::uint64_t seed_frame_id = 0;
    std::size_t category_index = 0;
    AnnotationColorRange sup{};
    AnnotationColorRange nosup{};
};

struct AnnotationResolvedInstance {
    std::size_t instance_index = 0;
    std::size_t category_index = 0;
    std::string class_name;
    AnnotationBox bbox{};
    std::vector<std::uint8_t> mask;
    std::string mask_rle;
    std::vector<std::uint8_t> crop_rgba;
    std::uint32_t crop_width = 0;
    std::uint32_t crop_height = 0;
};

struct AnnotationPreviewResult {
    std::vector<std::uint8_t> preview_bgr;
    std::vector<AnnotationResolvedInstance> resolved_instances;
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

AnnotationFrame load_annotation_frame(const fastloader::rfdetr::PredictImageInput& input);
void write_annotation_frame_png(const std::filesystem::path& path, const AnnotationFrame& frame);
AnnotationHsv sample_annotation_hsv(const AnnotationFrame& frame, int x, int y);
void recenter_annotation_range(AnnotationColorRange& range, const AnnotationHsv& center);
bool annotation_range_active(const AnnotationColorRange& range);
bool annotation_box_has_area(const AnnotationBox& box);
AnnotationBox normalize_annotation_box(AnnotationBox box, std::uint32_t width, std::uint32_t height);
std::uint32_t annotation_frame_capture_width(const AnnotationFrame& frame);
std::uint32_t annotation_frame_capture_height(const AnnotationFrame& frame);
AnnotationBox annotation_frame_view_box(const AnnotationFrame& frame);
AnnotationBox annotation_box_to_frame(const AnnotationFrame& frame, const AnnotationBox& capture_box);
AnnotationBox annotation_box_from_frame(const AnnotationFrame& frame, const AnnotationBox& frame_box);
AnnotationMaskRegion annotation_mask_region_from_frame(const AnnotationFrame& frame);
AnnotationFrame extract_annotation_frame_region(const AnnotationFrame& frame, const AnnotationBox& capture_box);
std::optional<AnnotationBox> annotation_bbox_from_mask(const std::vector<std::uint8_t>& mask,
                                                       std::uint32_t width,
                                                       std::uint32_t height);
std::vector<std::uint8_t> decode_annotation_prediction_mask(const fastloader::rfdetr::EncodedMask& mask,
                                                            std::uint32_t width,
                                                            std::uint32_t height);
std::string encode_annotation_mask_rle(const std::vector<std::uint8_t>& mask);
AnnotationPreviewResult build_annotation_preview(const AnnotationFrame& frame,
                                                 const AnnotationCategories& categories,
                                                 const std::vector<AnnotationInstance>& instances,
                                                 bool live_mode);
AnnotationCategories load_annotation_categories(const std::filesystem::path& output_root);
std::size_t ensure_annotation_category(AnnotationCategories& categories, const std::string& class_name);
void write_annotation_categories(const std::filesystem::path& output_root,
                                 const AnnotationCategories& categories);
AnnotationSaveResult save_annotation_scene(const AnnotationSaveConfig& config,
                                           const AnnotationFrame& frame,
                                           AnnotationCategories& categories,
                                           const std::vector<AnnotationResolvedInstance>& instances);

} // namespace fastloader::gui
