#include <array>

#include "catch2_compat.hpp"

import mmltk.backend.imaging.annotation.canvas_layers;

#define ANNOTATION_TEST_CASE(fn) MMLTK_TEST_CASE("[backend][imaging][annotation]", fn)

namespace {

using namespace mmltk::backend::imaging::annotation;

constexpr int kCropLayerId = 1;
constexpr int kBoxLayerId = 2;

CanvasViewport make_viewport() { return make_canvas_viewport(0.0f, 0.0f, 100.0f, 100.0f, 100U, 100U); }

CanvasPointerState make_pointer(float x, float y, bool hovered, bool clicked, bool down) {
    return CanvasPointerState{
        x, y, hovered, clicked, down,
    };
}

ANNOTATION_TEST_CASE(test_hover_prefers_higher_priority_layer) {
    const CanvasViewport viewport = make_viewport();
    const std::array<RectLayerSpec, 2> layers = {{
        RectLayerSpec{kBoxLayerId, AnnotationBox{20, 20, 80, 80}, 100U, 100U, 1, 1, true},
        RectLayerSpec{kCropLayerId, AnnotationBox{10, 10, 90, 90}, 100U, 100U, 1, 2, true},
    }};

    RectLayerState state{};
    const RectLayerFrameResult frame =
        update_rect_layers(state, layers.data(), layers.size(), viewport, make_pointer(12.0f, 12.0f, true, false, false));
    REQUIRE(frame.hovered_layer_id == kCropLayerId);
    REQUIRE(frame.hovered_kind == RectDragKind::ResizeTopLeft);
    REQUIRE(state.active_layer_id == 0);
}

ANNOTATION_TEST_CASE(test_active_layer_keeps_pointer_capture_until_release) {
    const CanvasViewport viewport = make_viewport();
    const std::array<RectLayerSpec, 2> layers = {{
        RectLayerSpec{kCropLayerId, AnnotationBox{10, 10, 40, 40}, 100U, 100U, 10, 2, true},
        RectLayerSpec{kBoxLayerId, AnnotationBox{50, 50, 90, 90}, 100U, 100U, 10, 1, true},
    }};

    RectLayerState state{};
    RectLayerFrameResult frame =
        update_rect_layers(state, layers.data(), layers.size(), viewport, make_pointer(25.0f, 25.0f, true, true, true));
    REQUIRE(state.active_layer_id == kCropLayerId);
    REQUIRE(frame.active_layer_id == kCropLayerId);
    REQUIRE(!frame.commit);

    frame = update_rect_layers(state, layers.data(), layers.size(), viewport, make_pointer(80.0f, 80.0f, false, false, true));
    REQUIRE(frame.active_layer_id == kCropLayerId);
    REQUIRE(frame.dragging);
    REQUIRE(frame.changed);
    REQUIRE(!frame.commit);
    REQUIRE(state.active_layer_id == kCropLayerId);

    frame = update_rect_layers(state, layers.data(), layers.size(), viewport, make_pointer(80.0f, 80.0f, false, false, false));
    REQUIRE(frame.active_layer_id == kCropLayerId);
    REQUIRE(frame.commit);
    REQUIRE(frame.changed);
    REQUIRE(state.active_layer_id == 0);
}

ANNOTATION_TEST_CASE(test_release_commits_the_exact_canvas_box) {
    const CanvasViewport viewport = make_viewport();
    const RectLayerSpec layer{
        kCropLayerId, AnnotationBox{34, 24, 66, 56}, 100U, 100U, 10, 1, true,
    };

    RectLayerState state{};
    RectLayerFrameResult frame = update_rect_layers(state, &layer, 1U, viewport, make_pointer(50.0f, 40.0f, true, true, true));
    REQUIRE(frame.active_layer_id == kCropLayerId);

    frame = update_rect_layers(state, &layer, 1U, viewport, make_pointer(60.0f, 48.0f, true, false, true));
    REQUIRE(frame.changed);
    REQUIRE(!frame.commit);

    frame = update_rect_layers(state, &layer, 1U, viewport, make_pointer(60.0f, 48.0f, true, false, false));
    REQUIRE(frame.commit);
    REQUIRE((frame.box == AnnotationBox{44, 32, 76, 64}));
}

ANNOTATION_TEST_CASE(test_edge_only_move_requires_hitting_crop_outline) {
    const CanvasViewport viewport = make_viewport();
    const RectLayerSpec crop_layer{
        kCropLayerId, AnnotationBox{10, 10, 40, 40}, 100U, 100U, 10, 1, true, true,
    };

    RectLayerState state{};
    RectLayerFrameResult frame = update_rect_layers(state, &crop_layer, 1U, viewport, make_pointer(25.0f, 25.0f, true, false, false));
    REQUIRE(frame.hovered_kind == RectDragKind::None);
    REQUIRE(frame.hovered_layer_id == 0);

    frame = update_rect_layers(state, &crop_layer, 1U, viewport, make_pointer(25.0f, 10.0f, true, false, false));
    REQUIRE(frame.hovered_layer_id == kCropLayerId);
    REQUIRE(frame.hovered_kind == RectDragKind::Move);
}

ANNOTATION_TEST_CASE(test_custom_white_crop_hit_metrics_support_larger_edges_and_corners) {
    const CanvasViewport viewport = make_viewport();
    const AnnotationBox box{20, 20, 80, 80};
    const float edge_hit_half_width = 8.0f;
    const float corner_hit_size = 20.0f;

    RectDragKind hover = rectangle_hover_kind_with_options(make_pointer(50.0f, 27.0f, true, false, false), viewport, box, true,
                                                           edge_hit_half_width, corner_hit_size);
    REQUIRE(hover == RectDragKind::Move);

    hover = rectangle_hover_kind_with_options(make_pointer(50.0f, 29.0f, true, false, false), viewport, box, true, edge_hit_half_width,
                                              corner_hit_size);
    REQUIRE(hover == RectDragKind::None);

    hover = rectangle_hover_kind_with_options(make_pointer(27.0f, 27.0f, true, false, false), viewport, box, true, edge_hit_half_width,
                                              corner_hit_size);
    REQUIRE(hover == RectDragKind::ResizeTopLeft);
}

}  // namespace
