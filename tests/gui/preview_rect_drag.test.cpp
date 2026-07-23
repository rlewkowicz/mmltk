#include "gui/preview_rect_drag.h"

#include "support/catch2_compat.hpp"

namespace {

using namespace mmltk::gui;

CanvasViewport make_viewport() {
    return make_canvas_viewport(0.0f, 0.0f, 100.0f, 100.0f, 100U, 100U);
}

void start_test_create_drag(PreviewRectDragSession& session) {
    start_preview_rect_drag(session, RectDragKind::Create, 10.0f, 10.0f, AnnotationBox{10, 10, 11, 11}, 6);
}

void test_create_drag_defers_commit_until_release() {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_test_create_drag(session);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, 18.0f, 19.0f, viewport, 100, 100, 1);
    assert(drag.active);
    assert(drag.changed);
    assert(!drag.commit);
    assert(!drag.cancel);
    assert(drag.box.x1 == 10);
    assert(drag.box.y1 == 10);
    assert(drag.box.x2 == 19);
    assert(drag.box.y2 == 20);
    assert(session.active);

    const PreviewRectDragResult release = update_preview_rect_drag(session, false, 18.0f, 19.0f, viewport, 100, 100, 1);
    assert(!release.active);
    assert(release.changed);
    assert(release.commit);
    assert(!release.cancel);
    assert(release.box.x1 == 10);
    assert(release.box.y1 == 10);
    assert(release.box.x2 == 19);
    assert(release.box.y2 == 20);
    assert(!session.active);
}

void test_create_drag_cancels_when_box_is_too_small() {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_test_create_drag(session);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, 13.0f, 13.0f, viewport, 100, 100, 1);
    assert(drag.changed);
    assert(!drag.commit);

    const PreviewRectDragResult release = update_preview_rect_drag(session, false, 13.0f, 13.0f, viewport, 100, 100, 1);
    assert(!release.active);
    assert(release.changed);
    assert(!release.commit);
    assert(release.cancel);
    assert(release.box.x1 == 10);
    assert(release.box.y1 == 10);
    assert(release.box.x2 == 14);
    assert(release.box.y2 == 14);
    assert(!session.active);
}

void test_move_drag_commits_only_on_release() {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::Move, 25.0f, 25.0f, AnnotationBox{20, 20, 40, 40}, 1);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, 35.0f, 30.0f, viewport, 100, 100, 1);
    assert(drag.active);
    assert(drag.changed);
    assert(!drag.commit);
    assert(drag.box.x1 == 30);
    assert(drag.box.y1 == 25);
    assert(drag.box.x2 == 50);
    assert(drag.box.y2 == 45);

    const PreviewRectDragResult release = update_preview_rect_drag(session, false, 35.0f, 30.0f, viewport, 100, 100, 1);
    assert(!release.active);
    assert(release.changed);
    assert(release.commit);
    assert(!release.cancel);
    assert(release.box.x1 == 30);
    assert(release.box.y1 == 25);
    assert(release.box.x2 == 50);
    assert(release.box.y2 == 45);
}

void test_resize_drag_clamps_to_bounds_before_commit() {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::ResizeBottomRight, 40.0f, 40.0f, AnnotationBox{20, 20, 40, 40}, 1);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, 120.0f, 130.0f, viewport, 100, 100, 1);
    assert(drag.active);
    assert(drag.changed);
    assert(!drag.commit);
    assert(drag.box.x1 == 20);
    assert(drag.box.y1 == 20);
    assert(drag.box.x2 == 100);
    assert(drag.box.y2 == 100);

    const PreviewRectDragResult release =
        update_preview_rect_drag(session, false, 120.0f, 130.0f, viewport, 100, 100, 1);
    assert(!release.active);
    assert(release.changed);
    assert(release.commit);
    assert(!release.cancel);
    assert(release.box.x1 == 20);
    assert(release.box.y1 == 20);
    assert(release.box.x2 == 100);
    assert(release.box.y2 == 100);
}

}  

MMLTK_REGISTER_TEST_CASE("[gui][preview_rect_drag]", test_create_drag_defers_commit_until_release);
MMLTK_REGISTER_TEST_CASE("[gui][preview_rect_drag]", test_create_drag_cancels_when_box_is_too_small);
MMLTK_REGISTER_TEST_CASE("[gui][preview_rect_drag]", test_move_drag_commits_only_on_release);
MMLTK_REGISTER_TEST_CASE("[gui][preview_rect_drag]", test_resize_drag_clamps_to_bounds_before_commit);
