#pragma once

#include "gui/annotation/common.h"
#include "gui/annotation/document/types.h"
#include "gui/annotation/session.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace mmltk::gui {

struct AnnotationCategory;

enum class AnnotationMaskCleanupOp : std::uint8_t {
    LargestComponent = 0,
    FillHoles = 1,
    Dilate = 2,
    Erode = 3,
    Open = 4,
    Close = 5,
};

const char* annotation_mask_cleanup_label(AnnotationMaskCleanupOp op) noexcept;

std::string next_annotation_object_id(std::size_t object_count);
AnnotationPoint annotation_frame_capture_center(const AnnotationFrame& frame);

AnnotationObject make_box_annotation_object(std::size_t object_count, std::size_t category_index,
                                            const AnnotationBox& box);
AnnotationObject make_point_annotation_object(std::size_t object_count, std::size_t category_index,
                                              const AnnotationPoint& point);
AnnotationObject make_spline_annotation_object(std::size_t object_count, std::size_t category_index);
AnnotationObject make_skeleton_annotation_object(std::size_t object_count, std::size_t category_index,
                                                 const AnnotationCategory* category);
bool remap_skeleton_annotation_object_to_category(AnnotationObject* object, const AnnotationCategory* category);

bool set_point_annotation_position(AnnotationObject* object, const AnnotationPoint& point, std::uint32_t capture_width,
                                   std::uint32_t capture_height);
bool set_annotation_object_handle_position(AnnotationObject* object, const AnnotationHandleId& handle,
                                           const AnnotationPoint& point, std::uint32_t capture_width,
                                           std::uint32_t capture_height);
bool append_spline_knot(AnnotationObject* object, const AnnotationPoint& point);
bool insert_spline_knot(AnnotationObject* object, std::size_t segment_index, const AnnotationPoint& point);
bool remove_spline_knot(AnnotationObject* object, std::size_t knot_index);
bool close_spline_shape(AnnotationObject* object);
bool reopen_spline_shape(AnnotationObject* object);
bool cycle_spline_knot_handle_mode(AnnotationObject* object, std::size_t knot_index);
bool set_spline_knot_handle_mode(AnnotationObject* object, std::size_t knot_index, AnnotationSplineHandleMode mode);
bool place_skeleton_node(AnnotationObject* object, const AnnotationPoint& point);
bool place_skeleton_node_at(AnnotationObject* object, std::size_t node_index, const AnnotationPoint& point);
bool set_skeleton_node_visibility(AnnotationObject* object, std::size_t node_index, bool visible);
bool reset_skeleton_node(AnnotationObject* object, std::size_t node_index);
std::optional<std::size_t> first_hidden_skeleton_node_index(const AnnotationSkeletonShape* shape);
std::optional<std::size_t> next_skeleton_node_index(const AnnotationSkeletonShape* shape,
                                                    std::optional<std::size_t> after_index);
std::size_t visible_skeleton_node_count(const AnnotationSkeletonShape& shape);

bool set_annotation_object_box(AnnotationObject* object, const AnnotationBox& box, std::uint32_t capture_width,
                               std::uint32_t capture_height);
bool paint_annotation_object_mask(AnnotationObject* object, int capture_x, int capture_y, int radius, bool erase,
                                  std::uint32_t capture_width, std::uint32_t capture_height);
bool fill_annotation_object_mask(AnnotationObject* object, int capture_x, int capture_y, std::uint32_t capture_width,
                                 std::uint32_t capture_height);
bool cleanup_annotation_object_mask(AnnotationObject* object, AnnotationMaskCleanupOp op, int radius,
                                    std::uint32_t capture_width, std::uint32_t capture_height);

}  // namespace mmltk::gui
