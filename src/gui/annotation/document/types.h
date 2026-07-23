#pragma once

#include "gui/annotation/common.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mmltk::gui {

enum class AnnotationShapeType : std::uint8_t {
    Box = 0,
    Mask = 1,
    Spline = 2,
    Point = 3,
    Skeleton = 4,
};

struct AnnotationPoint {
    float x = 0.0f;
    float y = 0.0f;
};

enum class AnnotationSplineHandleMode : std::uint8_t {
    Corner = 0,
    Smooth = 1,
    Mirrored = 2,
};

struct AnnotationSplineHandle {
    AnnotationPoint position{};
    bool enabled = false;
};

struct AnnotationSplineKnot {
    AnnotationPoint position{};
    AnnotationSplineHandle in_handle{};
    AnnotationSplineHandle out_handle{};
    AnnotationSplineHandleMode handle_mode = AnnotationSplineHandleMode::Corner;
};

struct AnnotationBoxShape {
    AnnotationBox box{};
};

struct AnnotationMaskShape {
    AnnotationBox box{};
    AnnotationMaskRegion region{};
    std::vector<std::uint8_t> mask;
    std::uint64_t seed_frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> seed_live_frame_id;
};

struct AnnotationSplineShape {
    bool closed = false;
    std::vector<AnnotationSplineKnot> knots;
};

struct AnnotationPointShape {
    AnnotationPoint point{};
};

struct AnnotationSkeletonNode {
    std::string key;
    AnnotationPoint point{};
    bool visible = true;
};

struct AnnotationSkeletonEdge {
    std::size_t source_index = 0;
    std::size_t target_index = 0;
};

struct AnnotationSkeletonShape {
    std::vector<AnnotationSkeletonNode> nodes;
    std::vector<AnnotationSkeletonEdge> edges;
};

using AnnotationShapeVariant = std::variant<AnnotationBoxShape, AnnotationMaskShape, AnnotationSplineShape,
                                            AnnotationPointShape, AnnotationSkeletonShape>;

struct AnnotationObject {
    std::string object_id;
    bool enabled = true;
    std::size_t category_index = 0;
    AnnotationColorRange sup{};
    AnnotationColorRange nosup{};
    AnnotationShapeVariant shape = AnnotationBoxShape{};
};

struct AnnotationResolvedObject {
    std::size_t object_index = 0;
    std::size_t category_index = 0;
    std::string class_name;
    AnnotationShapeType shape_type = AnnotationShapeType::Box;
    AnnotationBox bbox{};
    std::vector<std::uint8_t> mask;
    std::string mask_rle;
    std::vector<AnnotationPoint> points_xy;
    std::vector<std::uint8_t> crop_rgba;
    std::uint32_t crop_width = 0;
    std::uint32_t crop_height = 0;
};

const char* annotation_spline_handle_mode_name(AnnotationSplineHandleMode mode) noexcept;
AnnotationSplineHandleMode annotation_spline_handle_mode_from_name(std::string_view value);
AnnotationShapeType annotation_shape_type(const AnnotationShapeVariant& shape);
const char* annotation_shape_type_name(AnnotationShapeType shape_type);
const char* annotation_object_shape_label(const AnnotationObject& object);
std::optional<AnnotationBox> annotation_object_bbox(const AnnotationObject& object);
std::optional<AnnotationBox> annotation_object_display_box(const AnnotationObject& object);
std::vector<AnnotationPoint> annotation_object_points(const AnnotationObject& object);
AnnotationMaskRegion mask_region_from_box(const AnnotationBox& box);
AnnotationBox box_from_mask_region(const AnnotationMaskRegion& region);
const AnnotationMaskShape* annotation_object_mask_shape(const AnnotationObject& object);
AnnotationMaskShape* annotation_object_mask_shape(AnnotationObject* object);
bool annotation_object_supports_mask_editing(const AnnotationObject& object);
bool translate_annotation_object(AnnotationObject* object, int dx, int dy, std::uint32_t capture_width,
                                 std::uint32_t capture_height);
bool resize_annotation_object_to_box(AnnotationObject* object, const AnnotationBox& box, std::uint32_t capture_width,
                                     std::uint32_t capture_height);
std::optional<AnnotationPoint> annotation_spline_segment_point(const AnnotationSplineShape& spline,
                                                               std::size_t segment_index, float t);
std::vector<AnnotationPoint> sample_annotation_spline_points(const AnnotationSplineShape& spline,
                                                             int samples_per_segment = 32);

}  
