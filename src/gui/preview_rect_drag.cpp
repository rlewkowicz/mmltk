#include "preview_rect_drag.h"

#include <algorithm>

namespace mmltk::gui {

bool preview_rect_box_meets_min_extent(const AnnotationBox& box, const int min_extent) {
    if (!annotation_box_has_area(box)) {
        return false;
    }
    const int required = std::max(1, min_extent);
    return (box.x2 - box.x1) >= required && (box.y2 - box.y1) >= required;
}

void start_preview_rect_drag(PreviewRectDragSession& session, const RectDragKind kind, const float mouse_x,
                             const float mouse_y, const AnnotationBox& start_box, const int commit_min_extent) {
    session.active = kind != RectDragKind::None;
    session.drag.kind = kind;
    session.drag.start_mouse_x = mouse_x;
    session.drag.start_mouse_y = mouse_y;
    session.drag.start_box = start_box;
    session.draft_box = start_box;
    session.commit_min_extent = std::max(1, commit_min_extent);
}

PreviewRectDragResult update_preview_rect_drag(PreviewRectDragSession& session, const bool left_down,
                                               const float mouse_x, const float mouse_y, const CanvasViewport& viewport,
                                               const int max_width, const int max_height, const int min_size) {
    PreviewRectDragResult result;
    if (!session.active) {
        return result;
    }

    if (left_down) {
        result.active = true;
        const CanvasPointerState pointer{
            mouse_x, mouse_y, true, false, true,
        };
        result.box = apply_rect_drag(session.drag, pointer, viewport, max_width, max_height, min_size);
        result.changed = !(result.box.x1 == session.drag.start_box.x1 && result.box.y1 == session.drag.start_box.y1 &&
                           result.box.x2 == session.drag.start_box.x2 && result.box.y2 == session.drag.start_box.y2);
        session.draft_box = result.box;
        return result;
    }

    result.active = false;
    result.box = session.draft_box;
    result.changed =
        !(session.draft_box.x1 == session.drag.start_box.x1 && session.draft_box.y1 == session.drag.start_box.y1 &&
          session.draft_box.x2 == session.drag.start_box.x2 && session.draft_box.y2 == session.drag.start_box.y2);
    if (session.drag.kind == RectDragKind::Create) {
        result.commit = preview_rect_box_meets_min_extent(session.draft_box, session.commit_min_extent);
        result.cancel = !result.commit;
    } else {
        result.commit =
            result.changed && preview_rect_box_meets_min_extent(session.draft_box, session.commit_min_extent);
    }
    session = {};
    return result;
}

}  // namespace mmltk::gui
