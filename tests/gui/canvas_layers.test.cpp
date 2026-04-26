#include "gui/canvas_layers.h"

#include "support/catch2_compat.hpp"

#include <array>

namespace {

using namespace mmltk::gui;

constexpr int kCropLayerId = 1;
constexpr int kBoxLayerId = 2;

CanvasViewport make_viewport() {
    return make_canvas_viewport(0.0f, 0.0f, 100.0f, 100.0f, 100U, 100U);
}

CanvasPointerState make_pointer(float x, float y, bool hovered, bool clicked, bool down) {
    return CanvasPointerState{
        x, y, hovered, clicked, down,
    };
}

void test_hover_prefers_higher_priority_layer() {
    const CanvasViewport viewport = make_viewport();
    const std::array<RectLayerSpec, 2> layers = {{
        RectLayerSpec{kBoxLayerId, AnnotationBox{20, 20, 80, 80}, 100U, 100U, 1, 1, true},
        RectLayerSpec{kCropLayerId, AnnotationBox{10, 10, 90, 90}, 100U, 100U, 1, 2, true},
    }};

    RectLayerState state{};
    const RectLayerFrameResult frame = update_rect_layers(state, layers.data(), layers.size(), viewport,
                                                          make_pointer(12.0f, 12.0f, true, false, false));
    assert(frame.hovered_layer_id == kCropLayerId);
    assert(frame.hovered_kind == RectDragKind::ResizeTopLeft);
    assert(state.active_layer_id == 0);
}

void test_active_layer_keeps_pointer_capture_until_release() {
    const CanvasViewport viewport = make_viewport();
    const std::array<RectLayerSpec, 2> layers = {{
        RectLayerSpec{kCropLayerId, AnnotationBox{10, 10, 40, 40}, 100U, 100U, 10, 2, true},
        RectLayerSpec{kBoxLayerId, AnnotationBox{50, 50, 90, 90}, 100U, 100U, 10, 1, true},
    }};

    RectLayerState state{};
    RectLayerFrameResult frame =
        update_rect_layers(state, layers.data(), layers.size(), viewport, make_pointer(25.0f, 25.0f, true, true, true));
    assert(state.active_layer_id == kCropLayerId);
    assert(frame.active_layer_id == kCropLayerId);
    assert(!frame.commit);

    frame = update_rect_layers(state, layers.data(), layers.size(), viewport,
                               make_pointer(80.0f, 80.0f, false, false, true));
    assert(frame.active_layer_id == kCropLayerId);
    assert(frame.dragging);
    assert(frame.changed);
    assert(!frame.commit);
    assert(state.active_layer_id == kCropLayerId);

    frame = update_rect_layers(state, layers.data(), layers.size(), viewport,
                               make_pointer(80.0f, 80.0f, false, false, false));
    assert(frame.active_layer_id == kCropLayerId);
    assert(frame.commit);
    assert(frame.changed);
    assert(state.active_layer_id == 0);
}

void test_commit_on_release_uses_explicit_crop_box() {
    const CanvasViewport viewport = make_viewport();
    const RectLayerSpec layer{
        kCropLayerId, AnnotationBox{34, 24, 66, 56}, 100U, 100U, 10, 1, true,
    };

    RectLayerState state{};
    SourceSelectionState source;
    source.kind = SourceKind::VideoStream;
    source.capture_width = 100;
    source.capture_height = 100;
    source.crop_x = 0;
    source.crop_y = 0;
    source.crop_width = 32;
    source.crop_height = 32;

    RectLayerFrameResult frame =
        update_rect_layers(state, &layer, 1U, viewport, make_pointer(50.0f, 40.0f, true, true, true));
    assert(frame.active_layer_id == kCropLayerId);
    assert(source.crop_x == 0);
    assert(source.crop_y == 0);

    frame = update_rect_layers(state, &layer, 1U, viewport, make_pointer(60.0f, 48.0f, true, false, true));
    assert(frame.changed);
    assert(!frame.commit);
    assert(source.crop_x == 0);
    assert(source.crop_y == 0);

    frame = update_rect_layers(state, &layer, 1U, viewport, make_pointer(60.0f, 48.0f, true, false, false));
    assert(frame.commit);
    assert(assign_video_crop_box(source, frame.box));
    assert(source.crop_x == 44);
    assert(source.crop_y == 32);
    assert(source.crop_width == 32);
    assert(source.crop_height == 32);
}

void test_edge_only_move_requires_hitting_crop_outline() {
    const CanvasViewport viewport = make_viewport();
    const RectLayerSpec crop_layer{
        kCropLayerId, AnnotationBox{10, 10, 40, 40}, 100U, 100U, 10, 1, true, true,
    };

    RectLayerState state{};
    RectLayerFrameResult frame =
        update_rect_layers(state, &crop_layer, 1U, viewport, make_pointer(25.0f, 25.0f, true, false, false));
    assert(frame.hovered_kind == RectDragKind::None);
    assert(frame.hovered_layer_id == 0);

    frame = update_rect_layers(state, &crop_layer, 1U, viewport, make_pointer(25.0f, 10.0f, true, false, false));
    assert(frame.hovered_layer_id == kCropLayerId);
    assert(frame.hovered_kind == RectDragKind::Move);
}

void test_custom_white_crop_hit_metrics_support_larger_edges_and_corners() {
    const CanvasViewport viewport = make_viewport();
    const AnnotationBox box{20, 20, 80, 80};
    const float edge_hit_half_width = 8.0f;
    const float corner_hit_size = 20.0f;

    RectDragKind hover = rectangle_hover_kind_with_options(make_pointer(50.0f, 27.0f, true, false, false), viewport,
                                                           box, true, edge_hit_half_width, corner_hit_size);
    assert(hover == RectDragKind::Move);

    hover = rectangle_hover_kind_with_options(make_pointer(50.0f, 29.0f, true, false, false), viewport, box, true,
                                              edge_hit_half_width, corner_hit_size);
    assert(hover == RectDragKind::None);

    hover = rectangle_hover_kind_with_options(make_pointer(27.0f, 27.0f, true, false, false), viewport, box, true,
                                              edge_hit_half_width, corner_hit_size);
    assert(hover == RectDragKind::ResizeTopLeft);
}

void test_resolved_centered_crop_commits_to_explicit_coordinates() {
    SourceSelectionState source;
    source.kind = SourceKind::VideoStream;
    source.capture_width = 1920;
    source.capture_height = 1080;
    source.crop_x = 0;
    source.crop_y = 0;
    source.crop_width = 432;
    source.crop_height = 432;

    const AnnotationBox box = resolved_video_crop_box(source);
    assert(box.x1 == 744);
    assert(box.y1 == 324);
    assert(box.x2 == 1176);
    assert(box.y2 == 756);

    assert(assign_video_crop_box(source, box));
    assert(source.crop_x == 744);
    assert(source.crop_y == 324);
    assert(source.crop_width == 432);
    assert(source.crop_height == 432);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][canvas_layers]", test_hover_prefers_higher_priority_layer);
MMLTK_REGISTER_TEST_CASE("[gui][canvas_layers]", test_active_layer_keeps_pointer_capture_until_release);
MMLTK_REGISTER_TEST_CASE("[gui][canvas_layers]", test_commit_on_release_uses_explicit_crop_box);
MMLTK_REGISTER_TEST_CASE("[gui][canvas_layers]", test_edge_only_move_requires_hitting_crop_outline);
MMLTK_REGISTER_TEST_CASE("[gui][canvas_layers]", test_custom_white_crop_hit_metrics_support_larger_edges_and_corners);
MMLTK_REGISTER_TEST_CASE("[gui][canvas_layers]", test_resolved_centered_crop_commits_to_explicit_coordinates);
