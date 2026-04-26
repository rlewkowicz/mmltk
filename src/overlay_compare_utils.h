#pragma once

#include "manual_overlay_compare_utils.h"
#include "gui/preview_interaction_overlay_types.h"

namespace mmltk::overlay_compare {

inline bool preview_box_equals(const gui::PreviewInteractionOverlayBox& lhs,
                               const gui::PreviewInteractionOverlayBox& rhs) {
    return lhs.box.x1 == rhs.box.x1 && lhs.box.y1 == rhs.box.y1 && lhs.box.x2 == rhs.box.x2 &&
           lhs.box.y2 == rhs.box.y2 && lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b &&
           lhs.thickness == rhs.thickness && lhs.draw_handles == rhs.draw_handles &&
           lhs.handle_radius == rhs.handle_radius;
}

inline bool preview_point_equals(const gui::PreviewInteractionOverlayPoint& lhs,
                                 const gui::PreviewInteractionOverlayPoint& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

inline bool preview_edge_equals(const gui::PreviewInteractionOverlayEdge& lhs,
                                const gui::PreviewInteractionOverlayEdge& rhs) {
    return lhs.source_index == rhs.source_index && lhs.target_index == rhs.target_index;
}

inline bool preview_polyline_equals(const gui::PreviewInteractionOverlayPolyline& lhs,
                                    const gui::PreviewInteractionOverlayPolyline& rhs) {
    return lhs.closed == rhs.closed && lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b &&
           lhs.thickness == rhs.thickness && detail::sequence_equal(lhs.points, rhs.points, preview_point_equals);
}

inline bool preview_marker_set_equals(const gui::PreviewInteractionOverlayMarkerSet& lhs,
                                      const gui::PreviewInteractionOverlayMarkerSet& rhs) {
    return lhs.radius == rhs.radius && lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.alpha == rhs.alpha &&
           detail::sequence_equal(lhs.points, rhs.points, preview_point_equals);
}

inline bool preview_skeleton_equals(const gui::PreviewInteractionOverlaySkeleton& lhs,
                                    const gui::PreviewInteractionOverlaySkeleton& rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.thickness == rhs.thickness &&
           detail::sequence_equal(lhs.points, rhs.points, preview_point_equals) &&
           detail::sequence_equal(lhs.edges, rhs.edges, preview_edge_equals);
}

inline bool preview_snapshot_equals(const gui::PreviewInteractionOverlaySnapshot& lhs,
                                    const gui::PreviewInteractionOverlaySnapshot& rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height && lhs.cuda_device_index == rhs.cuda_device_index &&
           detail::sequence_equal(lhs.boxes, rhs.boxes, preview_box_equals) &&
           detail::sequence_equal(lhs.polylines, rhs.polylines, preview_polyline_equals) &&
           detail::sequence_equal(lhs.marker_sets, rhs.marker_sets, preview_marker_set_equals) &&
           detail::sequence_equal(lhs.skeletons, rhs.skeletons, preview_skeleton_equals);
}

}  // namespace mmltk::overlay_compare
