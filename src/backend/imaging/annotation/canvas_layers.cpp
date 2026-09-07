module;
#include <algorithm>
#include <cstdint>

module mmltk.backend.imaging.annotation.canvas_layers;

namespace mmltk::backend::imaging::annotation {

namespace {

constexpr float kDefaultEdgeHitHalfWidth = 6.0f;
constexpr float kDefaultCornerHitSize = 18.0f;

AnnotationBox clamp_box(AnnotationBox box, const int max_width, const int max_height) {
    return normalize_annotation_box(box, static_cast<std::uint32_t>(std::max(0, max_width)),
                                    static_cast<std::uint32_t>(std::max(0, max_height)));
}

CanvasScreenRect make_screen_rect(const CanvasViewport& viewport, const AnnotationBox& box) {
    if (viewport.image_width == 0U || viewport.image_height == 0U) { return {}; }
    const float scale_x = viewport.screen_width / static_cast<float>(viewport.image_width);
    const float scale_y = viewport.screen_height / static_cast<float>(viewport.image_height);
    return CanvasScreenRect{
        viewport.screen_x + static_cast<float>(box.x1) * scale_x,
        viewport.screen_y + static_cast<float>(box.y1) * scale_y,
        viewport.screen_x + static_cast<float>(box.x2) * scale_x,
        viewport.screen_y + static_cast<float>(box.y2) * scale_y,
    };
}

AnnotationBox apply_rectangle_drag(const RectDragState& state, const CanvasPointerState& pointer, const CanvasViewport& viewport,
                                   const int max_width, const int max_height, const int min_size) {
    if (viewport.image_width == 0U || viewport.image_height == 0U) { return state.start_box; }

    const float scale_x = viewport.screen_width / static_cast<float>(viewport.image_width);
    const float scale_y = viewport.screen_height / static_cast<float>(viewport.image_height);
    if (scale_x <= 0.0f || scale_y <= 0.0f) { return state.start_box; }

    const int dx = static_cast<int>((pointer.screen_x - state.start_mouse_x) / scale_x);
    const int dy = static_cast<int>((pointer.screen_y - state.start_mouse_y) / scale_y);
    AnnotationBox updated = state.start_box;
    switch (state.kind) {
        case RectDragKind::Create:
            updated.x2 = std::clamp(state.start_box.x2 + dx, 1, max_width);
            updated.y2 = std::clamp(state.start_box.y2 + dy, 1, max_height);
            break;
        case RectDragKind::Move: {
            updated.x1 += dx;
            updated.y1 += dy;
            updated.x2 += dx;
            updated.y2 += dy;
            break;
        }
        case RectDragKind::ResizeTopLeft:
            updated.x1 = std::clamp(state.start_box.x1 + dx, 0, state.start_box.x2 - min_size);
            updated.y1 = std::clamp(state.start_box.y1 + dy, 0, state.start_box.y2 - min_size);
            break;
        case RectDragKind::ResizeTopRight:
            updated.x2 = std::clamp(state.start_box.x2 + dx, state.start_box.x1 + min_size, max_width);
            updated.y1 = std::clamp(state.start_box.y1 + dy, 0, state.start_box.y2 - min_size);
            break;
        case RectDragKind::ResizeBottomLeft:
            updated.x1 = std::clamp(state.start_box.x1 + dx, 0, state.start_box.x2 - min_size);
            updated.y2 = std::clamp(state.start_box.y2 + dy, state.start_box.y1 + min_size, max_height);
            break;
        case RectDragKind::ResizeBottomRight:
            updated.x2 = std::clamp(state.start_box.x2 + dx, state.start_box.x1 + min_size, max_width);
            updated.y2 = std::clamp(state.start_box.y2 + dy, state.start_box.y1 + min_size, max_height);
            break;
        case RectDragKind::None:
            break;
    }
    return state.kind == RectDragKind::Move ? updated : clamp_box(updated, max_width, max_height);
}

const RectLayerSpec* find_layer_by_id(const RectLayerSpec* layers, const std::size_t layer_count, const int layer_id) {
    for (std::size_t index = 0; index < layer_count; ++index) {
        const RectLayerSpec& layer = layers[index];
        if (layer.enabled && layer.layer_id == layer_id) { return &layer; }
    }
    return nullptr;
}

}  // namespace

RectDragKind rectangle_hover_kind_with_options(const CanvasPointerState& pointer, const CanvasViewport& viewport, const AnnotationBox& box,
                                               bool edge_only_move, float edge_hit_half_width, float corner_hit_size);

CanvasViewport make_canvas_viewport(const float screen_x, const float screen_y, const float screen_width, const float screen_height,
                                    const std::uint32_t image_width, const std::uint32_t image_height) {
    return CanvasViewport{
        screen_x, screen_y, screen_width, screen_height, image_width, image_height,
    };
}

CanvasScreenRect canvas_rect_from_box(const CanvasViewport& viewport, const AnnotationBox& box) { return make_screen_rect(viewport, box); }

RectDragKind rectangle_hover_kind(const CanvasPointerState& pointer, const CanvasViewport& viewport, const AnnotationBox& box) {
    return rectangle_hover_kind_with_options(pointer, viewport, box, false, kDefaultEdgeHitHalfWidth, kDefaultCornerHitSize);
}

RectDragKind rectangle_hover_kind_with_options(const CanvasPointerState& pointer, const CanvasViewport& viewport, const AnnotationBox& box,
                                               const bool edge_only_move, const float edge_hit_half_width, const float corner_hit_size) {
    const CanvasScreenRect screen_box = make_screen_rect(viewport, box);
    const float hit_pad = std::max(0.0f, edge_hit_half_width);
    const float corner_size = std::max(0.0f, corner_hit_size);
    const bool near_left = pointer.screen_x >= screen_box.x1 - hit_pad && pointer.screen_x <= screen_box.x1 + hit_pad;
    const bool near_right = pointer.screen_x >= screen_box.x2 - hit_pad && pointer.screen_x <= screen_box.x2 + hit_pad;
    const bool near_top = pointer.screen_y >= screen_box.y1 - hit_pad && pointer.screen_y <= screen_box.y1 + hit_pad;
    const bool near_bottom = pointer.screen_y >= screen_box.y2 - hit_pad && pointer.screen_y <= screen_box.y2 + hit_pad;
    const bool in_y_range = pointer.screen_y >= screen_box.y1 - hit_pad && pointer.screen_y <= screen_box.y2 + hit_pad;
    const bool in_x_range = pointer.screen_x >= screen_box.x1 - hit_pad && pointer.screen_x <= screen_box.x2 + hit_pad;
    const bool inside_x = pointer.screen_x >= screen_box.x1 && pointer.screen_x <= screen_box.x2;
    const bool inside_y = pointer.screen_y >= screen_box.y1 && pointer.screen_y <= screen_box.y2;
    const bool near_top_left =
        near_top && near_left && pointer.screen_x < screen_box.x1 + corner_size && pointer.screen_y < screen_box.y1 + corner_size;
    const bool near_top_right =
        near_top && near_right && pointer.screen_x > screen_box.x2 - corner_size && pointer.screen_y < screen_box.y1 + corner_size;
    const bool near_bottom_left =
        near_bottom && near_left && pointer.screen_x < screen_box.x1 + corner_size && pointer.screen_y > screen_box.y2 - corner_size;
    const bool near_bottom_right =
        near_bottom && near_right && pointer.screen_x > screen_box.x2 - corner_size && pointer.screen_y > screen_box.y2 - corner_size;
    const bool on_edge =
        !near_top_left && !near_top_right && !near_bottom_left && !near_bottom_right &&
        ((near_left && in_y_range) || (near_right && in_y_range) || (near_top && in_x_range) || (near_bottom && in_x_range));
    if (near_top_left) { return RectDragKind::ResizeTopLeft; }
    if (near_top_right) { return RectDragKind::ResizeTopRight; }
    if (near_bottom_left) { return RectDragKind::ResizeBottomLeft; }
    if (near_bottom_right) { return RectDragKind::ResizeBottomRight; }
    if (on_edge || (!edge_only_move && inside_x && inside_y)) { return RectDragKind::Move; }
    return RectDragKind::None;
}

AnnotationBox apply_rect_drag(const RectDragState& state, const CanvasPointerState& pointer, const CanvasViewport& viewport,
                              const int max_width, const int max_height, const int min_size) {
    return apply_rectangle_drag(state, pointer, viewport, max_width, max_height, min_size);
}

RectLayerFrameResult update_rect_layers(RectLayerState& state, const RectLayerSpec* layers, const std::size_t layer_count,
                                        const CanvasViewport& viewport, const CanvasPointerState& pointer) {
    RectLayerFrameResult result{};
    if ((layers == nullptr) || (layer_count == 0U)) {
        clear_rect_layer_state(state);
        return result;
    }

    const RectLayerSpec* hovered_layer = nullptr;
    if (pointer.canvas_hovered && state.active_layer_id == 0) {
        for (std::size_t index = 0; index < layer_count; ++index) {
            const RectLayerSpec& layer = layers[index];
            if (!layer.enabled) { continue; }
            const RectDragKind hover_kind = rectangle_hover_kind_with_options(pointer, viewport, layer.box, layer.edge_only_move);
            if (hover_kind == RectDragKind::None) { continue; }
            if ((hovered_layer == nullptr) || (layer.priority > hovered_layer->priority)) {
                hovered_layer = &layer;
                result.hovered_layer_id = layer.layer_id;
                result.hovered_kind = hover_kind;
            }
        }
    }

    if (state.active_layer_id != 0) {
        const RectLayerSpec* active_layer = find_layer_by_id(layers, layer_count, state.active_layer_id);
        if (active_layer == nullptr) {
            clear_rect_layer_state(state);
            return result;
        }
        result.active_layer_id = active_layer->layer_id;
        result.dragging = true;
        result.box = apply_rectangle_drag(state.drag, pointer, viewport, static_cast<int>(active_layer->bounds_width),
                                          static_cast<int>(active_layer->bounds_height), active_layer->min_size);
        result.changed = !(result.box == active_layer->box);
        if (!pointer.left_down) {
            result.commit = true;
            clear_rect_layer_state(state);
        }
        return result;
    }

    if (pointer.left_clicked && hovered_layer != nullptr && result.hovered_kind != RectDragKind::None) {
        state.active_layer_id = hovered_layer->layer_id;
        state.drag.kind = result.hovered_kind;
        state.drag.start_mouse_x = pointer.screen_x;
        state.drag.start_mouse_y = pointer.screen_y;
        state.drag.start_box = hovered_layer->box;
        result.active_layer_id = hovered_layer->layer_id;
        result.dragging = pointer.left_down;
        result.box = hovered_layer->box;
    }
    return result;
}

void clear_rect_layer_state(RectLayerState& state) { state = {}; }

}  // namespace mmltk::backend::imaging::annotation
