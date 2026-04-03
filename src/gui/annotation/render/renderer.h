#pragma once

#include "gui/canvas_layers.h"
#include "gui/annotation/document/document.h"
#include "gui/annotation/session.h"
#include "gui/preview_interaction_overlay.h"
#include "mmltk/live/manual_overlay_document.h"

#include <optional>
#include <utility>
#include <vector>

namespace mmltk::gui {

struct AnnotationVisibleGeometry {
    std::vector<AnnotationPoint> capture_points;
    std::vector<AnnotationPoint> frame_points;
    std::vector<PreviewInteractionOverlayPoint> preview_points;
    std::vector<PreviewInteractionOverlayEdge> preview_edges;
    std::vector<PreviewInteractionOverlayPoint> control_preview_points;
    std::vector<PreviewInteractionOverlayPoint> handle_preview_points;
    std::vector<PreviewInteractionOverlayPoint> latent_handle_preview_points;
    std::vector<std::pair<PreviewInteractionOverlayPoint, PreviewInteractionOverlayPoint>>
        handle_preview_segments;
    std::vector<std::pair<PreviewInteractionOverlayPoint, PreviewInteractionOverlayPoint>>
        latent_handle_preview_segments;
    std::vector<mmltk::live::ManualOverlayPoint> manual_points;
    std::vector<mmltk::live::ManualOverlayEdge> manual_edges;
    std::vector<AnnotationSkeletonEdge> edges;
    bool closed = false;
};

struct AnnotationVisibleObject {
    std::size_t index = 0;
    std::size_t category_index = 0;
    AnnotationShapeType shape_type = AnnotationShapeType::Box;
    AnnotationBox capture_box{};
    AnnotationBox frame_box{};
    bool fully_visible = false;
    AnnotationVisibleGeometry geometry{};
};

struct AnnotationProjectedBox {
    std::optional<AnnotationBox> frame_box;
    bool fully_visible = false;
};

struct AnnotationVisibleObjectHit {
    AnnotationVisibleObject object{};
    RectDragKind drag_kind = RectDragKind::None;
};

struct AnnotationEditableHandle {
    AnnotationHandleId id{};
    std::size_t category_index = 0;
    AnnotationPoint capture_point{};
    AnnotationPoint frame_point{};
    AnnotationPoint tether_frame_point{};
    bool has_tether = false;
    bool materialized = true;
};

struct AnnotationInteractionOverlayObject {
    std::size_t object_index = 0;
    std::size_t category_index = 0;
    AnnotationShapeType shape_type = AnnotationShapeType::Box;
    AnnotationBox frame_box{};
    bool selected = false;
    bool hovered = false;
};

struct AnnotationInteractionOverlayRequest {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int cuda_device_index = 0;
    std::optional<AnnotationBox> crop_box;
    int crop_handle_radius = 4;
    std::optional<AnnotationBox> drag_box;
    std::optional<AnnotationBox> create_box;
    std::vector<AnnotationInteractionOverlayObject> objects;
    std::vector<PreviewInteractionOverlayPolyline> polylines;
    std::vector<PreviewInteractionOverlayMarkerSet> marker_sets;
    std::vector<PreviewInteractionOverlaySkeleton> skeletons;
};

struct AnnotationProjectedScene {
    std::uint64_t document_generation = 0;
    std::optional<std::size_t> selected_object_index;
    std::vector<AnnotationVisibleObject> visible_objects;
    std::vector<AnnotationEditableHandle> editable_handles;
};

class AnnotationRenderer {
public:
    static AnnotationProjectedScene build_projected_scene(
        const AnnotationFrame& frame,
        const AnnotationDocument& document,
        std::optional<std::size_t> selected_object_index);
    static std::vector<AnnotationEditableHandle> build_editable_handles(
        const AnnotationFrame& frame,
        const AnnotationDocument& document,
        std::optional<std::size_t> selected_object_index);
    static AnnotationProjectedScene refresh_projected_scene_selection(
        const AnnotationFrame& frame,
        const AnnotationDocument& document,
        AnnotationProjectedScene scene,
        std::optional<std::size_t> selected_object_index);

    static AnnotationProjectedBox project_capture_box(const AnnotationFrame& frame,
                                                      const AnnotationBox& capture_box);

    static std::optional<AnnotationVisibleObjectHit> hit_test_visible_objects(
        const std::vector<AnnotationVisibleObject>& objects,
        const CanvasViewport& viewport,
        const CanvasPointerState& pointer,
        int image_x,
        int image_y,
        bool direct_drag_mode);

    static AnnotationInteractionOverlayRequest build_interaction_overlay_request(
        const AnnotationFrame& frame,
        int cuda_device_index,
        const AnnotationProjectedScene& scene,
        std::optional<std::size_t> hovered_object_index,
        std::optional<std::size_t> replaced_object_index,
        std::optional<AnnotationBox> crop_box,
        int crop_handle_radius,
        std::optional<AnnotationBox> drag_box,
        std::optional<AnnotationBox> create_box,
        std::optional<AnnotationHandleId> hovered_handle,
        std::optional<AnnotationHandleId> active_handle);

    static PreviewInteractionOverlaySnapshot build_interaction_overlay_snapshot(
        const AnnotationInteractionOverlayRequest& request);

    static mmltk::live::ManualOverlayDocumentSnapshot build_manual_overlay_snapshot(
        const AnnotationFrame& frame,
        const AnnotationDocument& document,
        const AnnotationSession& session);

    static mmltk::live::ManualOverlayDocumentSnapshot build_manual_overlay_snapshot(
        const AnnotationFrame& frame,
        const AnnotationDocument& document,
        const AnnotationSession& session,
        const AnnotationProjectedScene& scene);
};

} // namespace mmltk::gui
