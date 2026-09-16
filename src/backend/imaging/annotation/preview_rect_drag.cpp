module;
#include <algorithm>
#include <cstdint>
module mmltk.backend.imaging.annotation.preview_rect_drag;
namespace mmltk::backend::imaging::annotation {
bool preview_rect_box_meets_min_extent(const AnnotationBox& box, const int min_extent) {
    if (!annotation_box_has_area(box)) { return false; }
    const int required = std::max(1, min_extent);
    return (box.x2 - box.x1) >= required && (box.y2 - box.y1) >= required;
}
void start_preview_rect_drag(PreviewRectDragSession& session, const RectDragKind kind, const float mouse_x, const float mouse_y, const AnnotationBox& start_box,
                             const int commit_min_extent) {
    session.active = kind != RectDragKind::None;
    session.drag.kind = kind;
    session.drag.start_mouse_x = mouse_x;
    session.drag.start_mouse_y = mouse_y;
    session.drag.start_box = start_box;
    session.original_box = start_box;
    session.draft_box = start_box;
    session.commit_min_extent = std::max(1, commit_min_extent);
}
PreviewRectDragResult update_preview_rect_drag(PreviewRectDragSession& session, const bool left_down, const float mouse_x, const float mouse_y,
                                               const CanvasViewport& viewport, const int max_width, const int max_height, const int min_size) {
    PreviewRectDragResult result;
    if (!session.active) { return result; }
    if (left_down) {
        result.active = true;
        const CanvasPointerState pointer{
            mouse_x, mouse_y, true, false, true,
        };
        result.box = apply_rect_drag(session.drag, pointer, viewport, max_width, max_height, min_size);
        result.changed = !(result.box.x1 == session.original_box.x1 && result.box.y1 == session.original_box.y1 && result.box.x2 == session.original_box.x2 &&
                           result.box.y2 == session.original_box.y2);
        session.draft_box = result.box;
        if (session.drag.kind == RectDragKind::Create) { return result; }
        const float scale_x = viewport.image_width == 0U ? 0.0F : viewport.screen_width / static_cast<float>(viewport.image_width);
        const float scale_y = viewport.image_height == 0U ? 0.0F : viewport.screen_height / static_cast<float>(viewport.image_height);
        if (scale_x > 0.0F) {
            const int capture_dx = result.box.x1 - session.drag.start_box.x1;
            session.drag.start_mouse_x += static_cast<float>(capture_dx) * scale_x;
        }
        if (scale_y > 0.0F) {
            const int capture_dy = result.box.y1 - session.drag.start_box.y1;
            session.drag.start_mouse_y += static_cast<float>(capture_dy) * scale_y;
        }
        if (session.drag.kind == RectDragKind::ResizeTopRight || session.drag.kind == RectDragKind::ResizeBottomRight) {
            if (scale_x > 0.0F) {
                const int capture_dx = result.box.x2 - session.drag.start_box.x2;
                session.drag.start_mouse_x += static_cast<float>(capture_dx) * scale_x;
            }
        }
        if (session.drag.kind == RectDragKind::ResizeBottomLeft || session.drag.kind == RectDragKind::ResizeBottomRight) {
            if (scale_y > 0.0F) {
                const int capture_dy = result.box.y2 - session.drag.start_box.y2;
                session.drag.start_mouse_y += static_cast<float>(capture_dy) * scale_y;
            }
        }
        session.drag.start_box = result.box;
        return result;
    }
    result = resolve_preview_rect_release(session.drag.kind, session.original_box, session.draft_box, max_width, max_height, session.commit_min_extent);
    session = {};
    return result;
}
PreviewRectDragResult resolve_preview_rect_release(const RectDragKind kind, const AnnotationBox& original_box, const AnnotationBox& draft_box,
                                                   const int max_width, const int max_height, const int commit_min_extent) {
    PreviewRectDragResult result;
    result.active = false;
    result.box = draft_box;
    result.changed =
        !(draft_box.x1 == original_box.x1 && draft_box.y1 == original_box.y1 && draft_box.x2 == original_box.x2 && draft_box.y2 == original_box.y2);
    if (kind == RectDragKind::Create) {
        result.commit = preview_rect_box_meets_min_extent(draft_box, commit_min_extent);
        result.cancel = !result.commit;
    } else if (kind == RectDragKind::Move) {
        result.move_dx = draft_box.x1 - original_box.x1;
        result.move_dy = draft_box.y1 - original_box.y1;
        const bool fully_outside = draft_box.x2 <= 0 || draft_box.y2 <= 0 || draft_box.x1 >= max_width || draft_box.y1 >= max_height;
        result.delete_on_commit = result.changed && fully_outside;
        if (!fully_outside) {
            const AnnotationBox clipped{
                std::clamp(draft_box.x1, 0, max_width),
                std::clamp(draft_box.y1, 0, max_height),
                std::clamp(draft_box.x2, 0, max_width),
                std::clamp(draft_box.y2, 0, max_height),
            };
            result.clipped_on_commit = clipped.x1 != draft_box.x1 || clipped.y1 != draft_box.y1 || clipped.x2 != draft_box.x2 || clipped.y2 != draft_box.y2;
            result.box = clipped;
        }
        result.commit = result.changed && !result.delete_on_commit && preview_rect_box_meets_min_extent(result.box, commit_min_extent);
    } else {
        result.commit = result.changed && preview_rect_box_meets_min_extent(draft_box, commit_min_extent);
    }
    return result;
}
PreviewRectDragResult apply_preview_rect_draft(PreviewRectDragSession& session, const bool left_down, AnnotationBox draft, const int max_width,
                                               const int max_height) {
    PreviewRectDragResult result;
    if (!session.active) { return result; }
    if (draft.x1 > draft.x2) { std::swap(draft.x1, draft.x2); }
    if (draft.y1 > draft.y2) { std::swap(draft.y1, draft.y2); }
    if (session.drag.kind != RectDragKind::Move) {
        draft = normalize_annotation_box(draft, static_cast<std::uint32_t>(std::max(0, max_width)), static_cast<std::uint32_t>(std::max(0, max_height)));
    }
    session.draft_box = draft;
    if (left_down) {
        result.active = true;
        result.box = draft;
        result.changed = !(draft.x1 == session.original_box.x1 && draft.y1 == session.original_box.y1 && draft.x2 == session.original_box.x2 &&
                           draft.y2 == session.original_box.y2);
        return result;
    }
    result = resolve_preview_rect_release(session.drag.kind, session.original_box, draft, max_width, max_height, session.commit_min_extent);
    session = {};
    return result;
}
}  // namespace mmltk::backend::imaging::annotation
