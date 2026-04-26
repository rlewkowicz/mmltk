#include "canvas_layers.h"

#include <algorithm>

namespace mmltk::gui {

namespace {

constexpr float kDefaultEdgeHitHalfWidth = 6.0f;
constexpr float kDefaultCornerHitSize = 18.0f;

bool boxes_equal(const AnnotationBox& lhs, const AnnotationBox& rhs) {
    return lhs.x1 == rhs.x1 && lhs.y1 == rhs.y1 && lhs.x2 == rhs.x2 && lhs.y2 == rhs.y2;
}

AnnotationBox clamp_box(AnnotationBox box, const int max_width, const int max_height) {
    const int clamped_width = std::max(0, max_width);
    const int clamped_height = std::max(0, max_height);
    box.x1 = std::clamp(box.x1, 0, clamped_width);
    box.y1 = std::clamp(box.y1, 0, clamped_height);
    box.x2 = std::clamp(box.x2, 0, clamped_width);
    box.y2 = std::clamp(box.y2, 0, clamped_height);
    if (box.x1 > box.x2) {
        std::swap(box.x1, box.x2);
    }
    if (box.y1 > box.y2) {
        std::swap(box.y1, box.y2);
    }
    return box;
}

CanvasScreenRect make_screen_rect(const CanvasViewport& viewport, const AnnotationBox& box) {
    if (viewport.image_width == 0U || viewport.image_height == 0U) {
        return {};
    }
    const float scale_x = viewport.screen_width / static_cast<float>(viewport.image_width);
    const float scale_y = viewport.screen_height / static_cast<float>(viewport.image_height);
    return CanvasScreenRect{
        viewport.screen_x + static_cast<float>(box.x1) * scale_x,
        viewport.screen_y + static_cast<float>(box.y1) * scale_y,
        viewport.screen_x + static_cast<float>(box.x2) * scale_x,
        viewport.screen_y + static_cast<float>(box.y2) * scale_y,
    };
}

AnnotationBox apply_rectangle_drag(const RectDragState& state, const CanvasPointerState& pointer,
                                   const CanvasViewport& viewport, const int max_width, const int max_height,
                                   const int min_size) {
    if (viewport.image_width == 0U || viewport.image_height == 0U) {
        return state.start_box;
    }

    const float scale_x = viewport.screen_width / static_cast<float>(viewport.image_width);
    const float scale_y = viewport.screen_height / static_cast<float>(viewport.image_height);
    if (scale_x <= 0.0f || scale_y <= 0.0f) {
        return state.start_box;
    }

    const int dx = static_cast<int>((pointer.screen_x - state.start_mouse_x) / scale_x);
    const int dy = static_cast<int>((pointer.screen_y - state.start_mouse_y) / scale_y);
    AnnotationBox updated = state.start_box;
    switch (state.kind) {
        case RectDragKind::Create:
            updated.x2 = std::clamp(state.start_box.x2 + dx, 1, max_width);
            updated.y2 = std::clamp(state.start_box.y2 + dy, 1, max_height);
            break;
        case RectDragKind::Move: {
            const int box_width = state.start_box.x2 - state.start_box.x1;
            const int box_height = state.start_box.y2 - state.start_box.y1;
            const int new_x1 = std::clamp(state.start_box.x1 + dx, 0, std::max(0, max_width - box_width));
            const int new_y1 = std::clamp(state.start_box.y1 + dy, 0, std::max(0, max_height - box_height));
            updated.x1 = new_x1;
            updated.y1 = new_y1;
            updated.x2 = new_x1 + box_width;
            updated.y2 = new_y1 + box_height;
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
    return clamp_box(updated, max_width, max_height);
}

const RectLayerSpec* find_layer_by_id(const RectLayerSpec* layers, const std::size_t layer_count, const int layer_id) {
    for (std::size_t index = 0; index < layer_count; ++index) {
        const RectLayerSpec& layer = layers[index];
        if (layer.enabled && layer.layer_id == layer_id) {
            return &layer;
        }
    }
    return nullptr;
}

}  // namespace

RectDragKind rectangle_hover_kind_with_options(const CanvasPointerState& pointer, const CanvasViewport& viewport,
                                               const AnnotationBox& box, bool edge_only_move, float edge_hit_half_width,
                                               float corner_hit_size);

CanvasViewport make_canvas_viewport(const float screen_x, const float screen_y, const float screen_width,
                                    const float screen_height, const std::uint32_t image_width,
                                    const std::uint32_t image_height) {
    return CanvasViewport{
        screen_x, screen_y, screen_width, screen_height, image_width, image_height,
    };
}

CanvasScreenRect canvas_rect_from_box(const CanvasViewport& viewport, const AnnotationBox& box) {
    return make_screen_rect(viewport, box);
}

RectDragKind rectangle_hover_kind(const CanvasPointerState& pointer, const CanvasViewport& viewport,
                                  const AnnotationBox& box) {
    return rectangle_hover_kind_with_options(pointer, viewport, box, false, kDefaultEdgeHitHalfWidth,
                                             kDefaultCornerHitSize);
}

RectDragKind rectangle_hover_kind_with_options(const CanvasPointerState& pointer, const CanvasViewport& viewport,
                                               const AnnotationBox& box, const bool edge_only_move,
                                               const float edge_hit_half_width, const float corner_hit_size) {
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
    const bool near_top_left = near_top && near_left && pointer.screen_x < screen_box.x1 + corner_size &&
                               pointer.screen_y < screen_box.y1 + corner_size;
    const bool near_top_right = near_top && near_right && pointer.screen_x > screen_box.x2 - corner_size &&
                                pointer.screen_y < screen_box.y1 + corner_size;
    const bool near_bottom_left = near_bottom && near_left && pointer.screen_x < screen_box.x1 + corner_size &&
                                  pointer.screen_y > screen_box.y2 - corner_size;
    const bool near_bottom_right = near_bottom && near_right && pointer.screen_x > screen_box.x2 - corner_size &&
                                   pointer.screen_y > screen_box.y2 - corner_size;
    const bool on_edge = !near_top_left && !near_top_right && !near_bottom_left && !near_bottom_right &&
                         ((near_left && in_y_range) || (near_right && in_y_range) || (near_top && in_x_range) ||
                          (near_bottom && in_x_range));
    if (near_top_left) {
        return RectDragKind::ResizeTopLeft;
    }
    if (near_top_right) {
        return RectDragKind::ResizeTopRight;
    }
    if (near_bottom_left) {
        return RectDragKind::ResizeBottomLeft;
    }
    if (near_bottom_right) {
        return RectDragKind::ResizeBottomRight;
    }
    if (on_edge || (!edge_only_move && inside_x && inside_y)) {
        return RectDragKind::Move;
    }
    return RectDragKind::None;
}

AnnotationBox apply_rect_drag(const RectDragState& state, const CanvasPointerState& pointer,
                              const CanvasViewport& viewport, const int max_width, const int max_height,
                              const int min_size) {
    return apply_rectangle_drag(state, pointer, viewport, max_width, max_height, min_size);
}

RectLayerFrameResult update_rect_layers(RectLayerState& state, const RectLayerSpec* layers,
                                        const std::size_t layer_count, const CanvasViewport& viewport,
                                        const CanvasPointerState& pointer) {
    RectLayerFrameResult result{};
    if ((layers == nullptr) || (layer_count == 0U)) {
        clear_rect_layer_state(state);
        return result;
    }

    const RectLayerSpec* hovered_layer = nullptr;
    if (pointer.canvas_hovered && state.active_layer_id == 0) {
        for (std::size_t index = 0; index < layer_count; ++index) {
            const RectLayerSpec& layer = layers[index];
            if (!layer.enabled) {
                continue;
            }
            const RectDragKind hover_kind =
                rectangle_hover_kind_with_options(pointer, viewport, layer.box, layer.edge_only_move);
            if (hover_kind == RectDragKind::None) {
                continue;
            }
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
        result.changed = !boxes_equal(result.box, active_layer->box);
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

void clear_rect_layer_state(RectLayerState& state) {
    state = {};
}

ResolvedVideoCrop resolve_video_crop(const SourceSelectionState& state) {
    ResolvedVideoCrop crop;
    crop.width = state.crop_width > 0 ? state.crop_width : std::max(0, state.capture_width);
    crop.height = state.crop_height > 0 ? state.crop_height : std::max(0, state.capture_height);
    crop.x = std::max(0, state.crop_x);
    crop.y = std::max(0, state.crop_y);

    if (crop.x == 0 && crop.y == 0 && state.capture_width > 0 && state.capture_height > 0 && crop.width > 0 &&
        crop.height > 0 && (crop.width < state.capture_width || crop.height < state.capture_height)) {
        crop.x = std::max(0, (state.capture_width - crop.width) / 2);
        crop.y = std::max(0, (state.capture_height - crop.height) / 2);
    }

    return crop;
}

AnnotationBox resolved_video_crop_box(const SourceSelectionState& state) {
    const ResolvedVideoCrop crop = resolve_video_crop(state);
    return clamp_box(
        AnnotationBox{
            crop.x,
            crop.y,
            crop.x + crop.width,
            crop.y + crop.height,
        },
        state.capture_width, state.capture_height);
}

bool assign_video_crop_box(SourceSelectionState& state, const AnnotationBox& box) {
    const AnnotationBox normalized = clamp_box(box, state.capture_width, state.capture_height);
    const int new_x = normalized.x1;
    const int new_y = normalized.y1;
    const int new_width = std::max(0, normalized.x2 - normalized.x1);
    const int new_height = std::max(0, normalized.y2 - normalized.y1);
    const bool changed = state.crop_x != new_x || state.crop_y != new_y || state.crop_width != new_width ||
                         state.crop_height != new_height;
    state.crop_x = new_x;
    state.crop_y = new_y;
    state.crop_width = new_width;
    state.crop_height = new_height;
    return changed;
}

}  // namespace mmltk::gui
