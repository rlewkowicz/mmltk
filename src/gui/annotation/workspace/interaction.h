#pragma once

#include "gui/annotation/controller.h"
#include "gui/annotation/render/renderer.h"
#include "gui/annotation/workspace/canvas.h"
#include "gui/annotation/workspace/model.h"
#include "gui/source_selection.h"

#include <memory>
#include <optional>

namespace mmltk::gui {

struct AnnotationWorkspaceInteractionConfig {
    bool live_video = false;
    bool full_frame = false;
    bool overlay_hovered = false;
    bool overlay_left_clicked = false;
    bool overlay_left_down = false;
    bool overlay_left_double_clicked = false;
    bool color_sampled = false;
    int image_x = 0;
    int image_y = 0;
    int capture_x = 0;
    int capture_y = 0;
    int brush_radius = 1;
    int cuda_device_index = 0;
    int crop_handle_radius = 4;
    float crop_edge_hit_half_width = 0.0f;
    float crop_corner_hit_size = 0.0f;
};

struct AnnotationWorkspaceInteractionResult {
    bool preview_invalidated = false;
    RectDragKind cursor_kind = RectDragKind::None;
    std::optional<std::size_t> hovered_object_index;
    std::shared_ptr<const AnnotationProjectedScene> projected_scene;
    AnnotationInteractionOverlayRequest overlay_request;
};

AnnotationWorkspaceInteractionResult process_annotation_workspace_interaction(
    AnnotationDocument& document,
    AnnotationSession& session,
    AnnotationController& controller,
    AnnotationCategories& categories,
    const AnnotationFrame& frame,
    SourceSelectionState& source,
    const AnnotationWorkspaceViewModel& workspace_view,
    const AnnotationCanvasLayout& canvas_layout,
    const CanvasPointerState& canvas_pointer,
    const AnnotationWorkspaceInteractionConfig& config);

} // namespace mmltk::gui
