#pragma once

#include "gui/annotation/common.h"
#include "gui/canvas_layers.h"

#include <optional>

namespace mmltk::gui {

struct AnnotationCanvasState {
    float scale = 0.0f;
    float pan_x = 0.0f;
    float pan_y = 0.0f;
    bool auto_fit = true;
};

struct AnnotationCanvasLayoutInput {
    AnnotationCanvasState state{};
    float available_width = 0.0f;
    float available_height = 0.0f;
    float viewport_screen_x = 0.0f;
    float viewport_screen_y = 0.0f;
    float mouse_screen_x = 0.0f;
    float mouse_screen_y = 0.0f;
    bool window_hovered = false;
};

struct AnnotationCanvasLayout {
    AnnotationCanvasState state{};
    float fit_scale = 1.0f;
    float available_width = 0.0f;
    float available_height = 0.0f;
    float viewport_screen_x = 0.0f;
    float viewport_screen_y = 0.0f;
    float viewport_screen_max_x = 0.0f;
    float viewport_screen_max_y = 0.0f;
    float image_screen_x = 0.0f;
    float image_screen_y = 0.0f;
    float image_screen_max_x = 0.0f;
    float image_screen_max_y = 0.0f;
    float image_width = 0.0f;
    float image_height = 0.0f;
    bool viewport_hovered = false;
    bool overlay_hovered = false;
    int image_x = 0;
    int image_y = 0;
    int capture_x = 0;
    int capture_y = 0;
    CanvasViewport viewport{};
};

struct AnnotationCanvasViewportCommit {
    double center_x = 0.0;
    double center_y = 0.0;
    double zoom = 1.0;
    std::optional<AnnotationBox> focus_frame_box;
};

float clamp_annotation_canvas_pan_axis(float pan, float image_extent, float viewport_extent);

AnnotationCanvasLayout build_annotation_canvas_layout(const AnnotationFrame& frame,
                                                      const AnnotationCanvasLayoutInput& input);

AnnotationCanvasState annotation_canvas_fit_state(const AnnotationCanvasLayout& layout);
AnnotationCanvasState annotation_canvas_one_to_one_state() noexcept;
AnnotationCanvasState annotation_canvas_zoom_around_point(const AnnotationCanvasLayout& layout, float new_scale,
                                                          float anchor_screen_x, float anchor_screen_y);
AnnotationCanvasState annotation_canvas_pan_by_delta(const AnnotationCanvasLayout& layout, float delta_x,
                                                     float delta_y);
AnnotationCanvasState annotation_canvas_focus_box(const AnnotationCanvasLayout& layout, const AnnotationBox& frame_box);
AnnotationCanvasState annotation_canvas_apply_viewport_commit(const AnnotationCanvasLayout& layout,
                                                              const AnnotationCanvasViewportCommit& commit);

}  // namespace mmltk::gui
