export module mmltk.backend.imaging.annotation.preview_rect_drag;

export import mmltk.backend.imaging.annotation.core;
export import mmltk.backend.imaging.annotation.canvas_layers;

export namespace mmltk::backend::imaging::annotation {

inline constexpr int kCreateCommitMinExtent = 6;
inline constexpr int kCropCommitMinExtent = 32;

struct PreviewRectDragSession {
    bool active = false;
    RectDragState drag{};
    AnnotationBox original_box{};
    AnnotationBox draft_box{};
    int commit_min_extent = 1;
};

struct PreviewRectDragResult {
    bool active = false;
    bool changed = false;
    bool commit = false;
    bool cancel = false;
    bool delete_on_commit = false;
    bool clipped_on_commit = false;
    int move_dx = 0;
    int move_dy = 0;
    AnnotationBox box{};
};

bool preview_rect_box_meets_min_extent(const AnnotationBox& box, int min_extent);
void start_preview_rect_drag(PreviewRectDragSession& session, RectDragKind kind, float mouse_x, float mouse_y,
                             const AnnotationBox& start_box, int commit_min_extent);
PreviewRectDragResult update_preview_rect_drag(PreviewRectDragSession& session, bool left_down, float mouse_x, float mouse_y,
                                               const CanvasViewport& viewport, int max_width, int max_height, int min_size);
PreviewRectDragResult resolve_preview_rect_release(RectDragKind kind, const AnnotationBox& original_box, const AnnotationBox& draft_box,
                                                   int max_width, int max_height, int commit_min_extent);
PreviewRectDragResult apply_preview_rect_draft(PreviewRectDragSession& session, bool left_down, AnnotationBox draft, int max_width,
                                               int max_height);

}  // namespace mmltk::backend::imaging::annotation
