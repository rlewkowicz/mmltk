#pragma once

#include "mmltk/live/manual_overlay_document.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace mmltk::detail {

template <typename T, typename EqualFn>
inline bool sequence_equal(const std::vector<T>& lhs, const std::vector<T>& rhs, EqualFn&& equal) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                      [&equal](const T& left, const T& right) { return std::forward<EqualFn>(equal)(left, right); });
}

}  

namespace mmltk::overlay_compare {

inline bool manual_box_equals(const live::ManualOverlayBox& lhs, const live::ManualOverlayBox& rhs) {
    return lhs.x1 == rhs.x1 && lhs.y1 == rhs.y1 && lhs.x2 == rhs.x2 && lhs.y2 == rhs.y2;
}

inline bool manual_mask_region_equals(const live::ManualOverlayMaskRegion& lhs,
                                      const live::ManualOverlayMaskRegion& rhs) {
    return lhs.capture_x == rhs.capture_x && lhs.capture_y == rhs.capture_y && lhs.width == rhs.width &&
           lhs.height == rhs.height;
}

inline bool manual_mask_run_equals(const live::ManualOverlayMaskRun& lhs,
                                   const live::ManualOverlayMaskRun& rhs) noexcept {
    return lhs.offset == rhs.offset && lhs.length == rhs.length;
}

inline bool manual_point_equals(const live::ManualOverlayPoint& lhs, const live::ManualOverlayPoint& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

inline bool manual_edge_equals(const live::ManualOverlayEdge& lhs, const live::ManualOverlayEdge& rhs) {
    return lhs.source_index == rhs.source_index && lhs.target_index == rhs.target_index;
}

inline bool manual_selected_instance_equals(const std::optional<std::size_t>& lhs,
                                            const std::optional<std::size_t>& rhs) {
    return lhs == rhs;
}

inline bool manual_brush_preview_equals(const std::optional<live::ManualOverlayBrushPreview>& lhs,
                                        const std::optional<live::ManualOverlayBrushPreview>& rhs) {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    if (!lhs.has_value()) {
        return true;
    }
    return lhs->capture_x == rhs->capture_x && lhs->capture_y == rhs->capture_y && lhs->radius == rhs->radius &&
           lhs->erase == rhs->erase;
}

inline bool manual_style_equals(const std::optional<live::ManualOverlayStyle>& lhs,
                                const std::optional<live::ManualOverlayStyle>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs.has_value() || (lhs->r == rhs->r && lhs->g == rhs->g && lhs->b == rhs->b && lhs->alpha == rhs->alpha &&
                                lhs->line_thickness == rhs->line_thickness && lhs->point_radius == rhs->point_radius &&
                                lhs->draw_handles == rhs->draw_handles && lhs->handle_radius == rhs->handle_radius);
}

inline bool manual_instance_equals(const live::ManualOverlayInstance& lhs, const live::ManualOverlayInstance& rhs) {
    return lhs.instance_id == rhs.instance_id && lhs.enabled == rhs.enabled && manual_box_equals(lhs.box, rhs.box) &&
           manual_mask_region_equals(lhs.mask_region, rhs.mask_region) && lhs.mask == rhs.mask &&
           detail::sequence_equal(lhs.mask_runs, rhs.mask_runs, manual_mask_run_equals) &&
           detail::sequence_equal(lhs.polyline_points, rhs.polyline_points, manual_point_equals) &&
           lhs.polyline_closed == rhs.polyline_closed &&
           detail::sequence_equal(lhs.points, rhs.points, manual_point_equals) &&
           detail::sequence_equal(lhs.skeleton_edges, rhs.skeleton_edges, manual_edge_equals) &&
           lhs.category_index == rhs.category_index && manual_style_equals(lhs.style, rhs.style);
}

inline bool manual_snapshot_equals(const live::ManualOverlayDocumentSnapshot& lhs,
                                   const live::ManualOverlayDocumentSnapshot& rhs) {
    return lhs.document_generation == rhs.document_generation && lhs.session_revision == rhs.session_revision &&
           lhs.capture_width == rhs.capture_width && lhs.capture_height == rhs.capture_height &&
           manual_selected_instance_equals(lhs.selected_instance, rhs.selected_instance) &&
           manual_brush_preview_equals(lhs.brush_preview, rhs.brush_preview) &&
           detail::sequence_equal(lhs.instances, rhs.instances, manual_instance_equals) &&
           detail::sequence_equal(lhs.interaction_instances, rhs.interaction_instances, manual_instance_equals);
}

}  
