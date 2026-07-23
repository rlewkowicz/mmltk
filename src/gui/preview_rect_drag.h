#pragma once

#include "annotation_core.h"
#include "canvas_layers.h"

namespace mmltk::gui {

struct PreviewRectDragSession {
    bool active = false;
    RectDragState drag{};
    AnnotationBox draft_box{};
    int commit_min_extent = 1;
};

struct PreviewRectDragResult {
    bool active = false;
    bool changed = false;
    bool commit = false;
    bool cancel = false;
    AnnotationBox box{};
};

bool preview_rect_box_meets_min_extent(const AnnotationBox& box, int min_extent);
void start_preview_rect_drag(PreviewRectDragSession& session, RectDragKind kind, float mouse_x, float mouse_y,
                             const AnnotationBox& start_box, int commit_min_extent);
PreviewRectDragResult update_preview_rect_drag(PreviewRectDragSession& session, bool left_down, float mouse_x,
                                               float mouse_y, const CanvasViewport& viewport, int max_width,
                                               int max_height, int min_size);

}  
