#include "gui/annotation/workspace/canvas.h"

#include "support/catch2_compat.hpp"

namespace {

using namespace mmltk::gui;

AnnotationFrame make_canvas_test_frame() {
    AnnotationFrame frame;
    frame.width = 100U;
    frame.height = 50U;
    frame.view_x = 10U;
    frame.view_y = 20U;
    frame.capture_width = 200U;
    frame.capture_height = 120U;
    return frame;
}

AnnotationCanvasLayout build_default_canvas_layout(const AnnotationFrame& frame,
                                                   const AnnotationCanvasState& state = {}) {
    return build_annotation_canvas_layout(frame, AnnotationCanvasLayoutInput{
                                                     state,
                                                     400.0f,
                                                     200.0f,
                                                     0.0f,
                                                     0.0f,
                                                     200.0f,
                                                     100.0f,
                                                     true,
                                                 });
}

void test_annotation_canvas_layout_maps_view_and_capture_coordinates() {
    const AnnotationFrame frame = make_canvas_test_frame();
    const AnnotationCanvasLayout layout = build_annotation_canvas_layout(frame, AnnotationCanvasLayoutInput{
                                                                                    AnnotationCanvasState{
                                                                                        1.0f,
                                                                                        0.0f,
                                                                                        0.0f,
                                                                                        false,
                                                                                    },
                                                                                    200.0f,
                                                                                    100.0f,
                                                                                    30.0f,
                                                                                    40.0f,
                                                                                    80.0f,
                                                                                    90.0f,
                                                                                    true,
                                                                                });

    assert(!layout.state.auto_fit);
    assert(layout.state.scale == 1.0f);
    assert(layout.viewport_hovered);
    assert(layout.overlay_hovered);
    assert(layout.image_screen_x == 80.0f);
    assert(layout.image_screen_y == 65.0f);
    assert(layout.image_x == 0);
    assert(layout.image_y == 25);
    assert(layout.capture_x == 10);
    assert(layout.capture_y == 45);
}

void test_annotation_canvas_zoom_preserves_anchor_position() {
    const AnnotationFrame frame = make_canvas_test_frame();
    const AnnotationCanvasLayout base_layout = build_default_canvas_layout(frame);
    assert(base_layout.state.auto_fit);
    assert(base_layout.state.scale == 4.0f);

    const AnnotationCanvasState zoomed_state = annotation_canvas_zoom_around_point(base_layout, 8.0f, 200.0f, 100.0f);
    const AnnotationCanvasLayout zoomed_layout = build_default_canvas_layout(frame, zoomed_state);

    assert(!zoomed_state.auto_fit);
    assert(zoomed_layout.state.scale == 8.0f);
    assert(zoomed_layout.image_x == 50);
    assert(zoomed_layout.image_y == 25);
    assert(zoomed_layout.capture_x == 60);
    assert(zoomed_layout.capture_y == 45);
}

void test_annotation_canvas_viewport_commit_centers_requested_point() {
    const AnnotationFrame frame = make_canvas_test_frame();
    const AnnotationCanvasLayout base_layout = build_default_canvas_layout(frame);

    const AnnotationCanvasState committed_state =
        annotation_canvas_apply_viewport_commit(base_layout, AnnotationCanvasViewportCommit{
                                                                 75.0,
                                                                 25.0,
                                                                 8.0,
                                                                 std::nullopt,
                                                             });
    const AnnotationCanvasLayout committed_layout = build_default_canvas_layout(frame, committed_state);

    assert(!committed_state.auto_fit);
    assert(committed_layout.state.scale == 8.0f);
    assert(committed_layout.image_x == 75);
    assert(committed_layout.image_y == 25);
}

}  

MMLTK_REGISTER_TEST_CASE("[gui][annotation_workspace_canvas]",
                         test_annotation_canvas_layout_maps_view_and_capture_coordinates);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_workspace_canvas]", test_annotation_canvas_zoom_preserves_anchor_position);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_workspace_canvas]",
                         test_annotation_canvas_viewport_commit_centers_requested_point);
