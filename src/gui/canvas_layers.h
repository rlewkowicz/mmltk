#pragma once

#include "annotation_core.h"
#include "source_selection.h"

#include <cstddef>
#include <cstdint>

namespace mmltk::gui {

enum class RectDragKind : std::uint8_t {
    None = 0,
    Create = 1,
    Move = 2,
    ResizeTopLeft = 3,
    ResizeTopRight = 4,
    ResizeBottomLeft = 5,
    ResizeBottomRight = 6,
};

struct RectDragState {
    RectDragKind kind = RectDragKind::None;
    float start_mouse_x = 0.0f;
    float start_mouse_y = 0.0f;
    AnnotationBox start_box{};
};

struct CanvasViewport {
    float screen_x = 0.0f;
    float screen_y = 0.0f;
    float screen_width = 0.0f;
    float screen_height = 0.0f;
    std::uint32_t image_width = 0;
    std::uint32_t image_height = 0;
};

struct CanvasScreenRect {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};

struct CanvasPointerState {
    float screen_x = 0.0f;
    float screen_y = 0.0f;
    bool canvas_hovered = false;
    bool left_clicked = false;
    bool left_down = false;
};

struct RectLayerSpec {
    int layer_id = 0;
    AnnotationBox box{};
    std::uint32_t bounds_width = 0;
    std::uint32_t bounds_height = 0;
    int min_size = 1;
    int priority = 0;
    bool enabled = true;
    bool edge_only_move = false;
};

struct RectLayerState {
    int active_layer_id = 0;
    RectDragState drag{};
};

struct RectLayerFrameResult {
    int hovered_layer_id = 0;
    RectDragKind hovered_kind = RectDragKind::None;
    int active_layer_id = 0;
    bool dragging = false;
    bool changed = false;
    bool commit = false;
    AnnotationBox box{};
};

CanvasViewport make_canvas_viewport(float screen_x, float screen_y, float screen_width, float screen_height,
                                    std::uint32_t image_width, std::uint32_t image_height);
CanvasScreenRect canvas_rect_from_box(const CanvasViewport& viewport, const AnnotationBox& box);
RectDragKind rectangle_hover_kind(const CanvasPointerState& pointer, const CanvasViewport& viewport,
                                  const AnnotationBox& box);
RectDragKind rectangle_hover_kind_with_options(const CanvasPointerState& pointer, const CanvasViewport& viewport,
                                               const AnnotationBox& box, bool edge_only_move,
                                               float edge_hit_half_width = 6.0f, float corner_hit_size = 18.0f);
AnnotationBox apply_rect_drag(const RectDragState& state, const CanvasPointerState& pointer,
                              const CanvasViewport& viewport, int max_width, int max_height, int min_size);
RectLayerFrameResult update_rect_layers(RectLayerState& state, const RectLayerSpec* layers, std::size_t layer_count,
                                        const CanvasViewport& viewport, const CanvasPointerState& pointer);
void clear_rect_layer_state(RectLayerState& state);
ResolvedVideoCrop resolve_video_crop(const SourceSelectionState& state);
AnnotationBox resolved_video_crop_box(const SourceSelectionState& state);
bool assign_video_crop_box(SourceSelectionState& state, const AnnotationBox& box);

}  
