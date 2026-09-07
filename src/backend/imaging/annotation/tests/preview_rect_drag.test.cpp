#include "catch2_compat.hpp"

import mmltk.backend.imaging.annotation.preview_rect_drag;

#define ANNOTATION_TEST_CASE(fn) MMLTK_TEST_CASE("[backend][imaging][annotation]", fn)

namespace {

using namespace mmltk::backend::imaging::annotation;

CanvasViewport make_viewport() { return make_canvas_viewport(0.0f, 0.0f, 100.0f, 100.0f, 100U, 100U); }

void start_test_create_drag(PreviewRectDragSession& session) {
    start_preview_rect_drag(session, RectDragKind::Create, 10.0f, 10.0f, AnnotationBox{10, 10, 11, 11}, 6);
}

ANNOTATION_TEST_CASE(test_create_drag_defers_commit_until_release) {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_test_create_drag(session);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, 18.0f, 19.0f, viewport, 100, 100, 1);
    REQUIRE(drag.active);
    REQUIRE(drag.changed);
    REQUIRE(!drag.commit);
    REQUIRE(!drag.cancel);
    REQUIRE(drag.box.x1 == 10);
    REQUIRE(drag.box.y1 == 10);
    REQUIRE(drag.box.x2 == 19);
    REQUIRE(drag.box.y2 == 20);
    REQUIRE(session.active);

    const PreviewRectDragResult release = update_preview_rect_drag(session, false, 18.0f, 19.0f, viewport, 100, 100, 1);
    REQUIRE(!release.active);
    REQUIRE(release.changed);
    REQUIRE(release.commit);
    REQUIRE(!release.cancel);
    REQUIRE(release.box.x1 == 10);
    REQUIRE(release.box.y1 == 10);
    REQUIRE(release.box.x2 == 19);
    REQUIRE(release.box.y2 == 20);
    REQUIRE(!session.active);
}

ANNOTATION_TEST_CASE(test_create_drag_cancels_when_box_is_too_small) {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_test_create_drag(session);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, 13.0f, 13.0f, viewport, 100, 100, 1);
    REQUIRE(drag.changed);
    REQUIRE(!drag.commit);

    const PreviewRectDragResult release = update_preview_rect_drag(session, false, 13.0f, 13.0f, viewport, 100, 100, 1);
    REQUIRE(!release.active);
    REQUIRE(release.changed);
    REQUIRE(!release.commit);
    REQUIRE(release.cancel);
    REQUIRE(release.box.x1 == 10);
    REQUIRE(release.box.y1 == 10);
    REQUIRE(release.box.x2 == 14);
    REQUIRE(release.box.y2 == 14);
    REQUIRE(!session.active);
}

ANNOTATION_TEST_CASE(test_move_drag_commits_only_on_release) {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::Move, 25.0f, 25.0f, AnnotationBox{20, 20, 40, 40}, 1);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, 35.0f, 30.0f, viewport, 100, 100, 1);
    REQUIRE(drag.active);
    REQUIRE(drag.changed);
    REQUIRE(!drag.commit);
    REQUIRE(drag.box.x1 == 30);
    REQUIRE(drag.box.y1 == 25);
    REQUIRE(drag.box.x2 == 50);
    REQUIRE(drag.box.y2 == 45);

    const PreviewRectDragResult release = update_preview_rect_drag(session, false, 35.0f, 30.0f, viewport, 100, 100, 1);
    REQUIRE(!release.active);
    REQUIRE(release.changed);
    REQUIRE(release.commit);
    REQUIRE(!release.cancel);
    REQUIRE(release.box.x1 == 30);
    REQUIRE(release.box.y1 == 25);
    REQUIRE(release.box.x2 == 50);
    REQUIRE(release.box.y2 == 45);
}

ANNOTATION_TEST_CASE(test_move_drag_reverses_immediately_after_edge_overshoot) {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::Move, 25.0f, 25.0f, AnnotationBox{20, 20, 40, 40}, 1);

    const PreviewRectDragResult overshoot = update_preview_rect_drag(session, true, 130.0f, 25.0f, viewport, 100, 100, 1);
    REQUIRE(overshoot.box.x1 == 125);
    REQUIRE(overshoot.box.x2 == 145);

    const PreviewRectDragResult reverse = update_preview_rect_drag(session, true, 125.0f, 25.0f, viewport, 100, 100, 1);
    REQUIRE(reverse.box.x1 == 120);
    REQUIRE(reverse.box.x2 == 140);
}

ANNOTATION_TEST_CASE(test_move_drag_clips_partial_overlap_on_release) {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::Move, 25.0f, 25.0f, AnnotationBox{20, 20, 40, 40}, 1);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, -5.0f, 25.0f, viewport, 100, 100, 1);
    REQUIRE(drag.box.x1 == -10);
    REQUIRE(drag.box.x2 == 10);

    const PreviewRectDragResult release = update_preview_rect_drag(session, false, -5.0f, 25.0f, viewport, 100, 100, 1);
    REQUIRE(release.commit);
    REQUIRE(release.clipped_on_commit);
    REQUIRE(!release.delete_on_commit);
    REQUIRE(release.box.x1 == 0);
    REQUIRE(release.box.x2 == 10);
}

ANNOTATION_TEST_CASE(test_move_drag_deletes_fully_off_canvas_on_release) {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::Move, 25.0f, 25.0f, AnnotationBox{20, 20, 40, 40}, 1);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, -30.0f, 25.0f, viewport, 100, 100, 1);
    REQUIRE(drag.box.x2 < 0);

    const PreviewRectDragResult release = update_preview_rect_drag(session, false, -30.0f, 25.0f, viewport, 100, 100, 1);
    REQUIRE(release.changed);
    REQUIRE(release.delete_on_commit);
    REQUIRE(!release.commit);
    REQUIRE(!release.cancel);
}

ANNOTATION_TEST_CASE(test_create_drag_clamps_both_corners_to_canvas) {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::Create, 110.0f, 110.0f, AnnotationBox{100, 100, 100, 100}, 1);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, -20.0f, -30.0f, viewport, 100, 100, 1);
    REQUIRE(drag.box.x1 == 1);
    REQUIRE(drag.box.y1 == 1);
    REQUIRE(drag.box.x2 == 100);
    REQUIRE(drag.box.y2 == 100);
}

// CLEANUP-IGNORE -- the inlined literals are the independently readable scenario oracle.
ANNOTATION_TEST_CASE(test_resize_drag_clamps_to_bounds_before_commit) {
    const CanvasViewport viewport = make_viewport();
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::ResizeBottomRight, 40.0f, 40.0f, AnnotationBox{20, 20, 40, 40}, 1);

    const PreviewRectDragResult drag = update_preview_rect_drag(session, true, 120.0f, 130.0f, viewport, 100, 100, 1);
    REQUIRE(drag.active);
    REQUIRE(drag.changed);
    REQUIRE(!drag.commit);
    REQUIRE(drag.box.x1 == 20);
    REQUIRE(drag.box.y1 == 20);
    REQUIRE(drag.box.x2 == 100);
    REQUIRE(drag.box.y2 == 100);

    const PreviewRectDragResult release = update_preview_rect_drag(session, false, 120.0f, 130.0f, viewport, 100, 100, 1);
    REQUIRE(!release.active);
    REQUIRE(release.changed);
    REQUIRE(release.commit);
    REQUIRE(!release.cancel);
    REQUIRE(release.box.x1 == 20);
    REQUIRE(release.box.y1 == 20);
    REQUIRE(release.box.x2 == 100);
    REQUIRE(release.box.y2 == 100);
}

ANNOTATION_TEST_CASE(test_release_rules_match_between_pointer_and_draft_paths) {
    const AnnotationBox original{10, 10, 30, 30};

    const PreviewRectDragResult create_commit =
        resolve_preview_rect_release(RectDragKind::Create, original, AnnotationBox{10, 10, 19, 20}, 100, 100, 6);
    REQUIRE(create_commit.commit);
    REQUIRE(!create_commit.cancel);

    const PreviewRectDragResult create_cancel =
        resolve_preview_rect_release(RectDragKind::Create, original, AnnotationBox{10, 10, 13, 13}, 100, 100, 6);
    REQUIRE(!create_cancel.commit);
    REQUIRE(create_cancel.cancel);

    const PreviewRectDragResult move_clip =
        resolve_preview_rect_release(RectDragKind::Move, original, AnnotationBox{-5, 10, 15, 30}, 100, 100, 1);
    REQUIRE(move_clip.commit);
    REQUIRE(move_clip.clipped_on_commit);
    REQUIRE(move_clip.move_dx == -15);
    REQUIRE(move_clip.box.x1 == 0);
    REQUIRE(move_clip.box.x2 == 15);

    const PreviewRectDragResult move_delete =
        resolve_preview_rect_release(RectDragKind::Move, original, AnnotationBox{-40, 10, -20, 30}, 100, 100, 1);
    REQUIRE(move_delete.delete_on_commit);
    REQUIRE(!move_delete.commit);
}

ANNOTATION_TEST_CASE(test_browser_draft_updates_drive_the_session) {
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::ResizeBottomRight, 0.0f, 0.0f, AnnotationBox{10, 10, 30, 30}, 1);

    const PreviewRectDragResult drag = apply_preview_rect_draft(session, true, AnnotationBox{10, 10, 140, 50}, 100, 100);
    REQUIRE(drag.active);
    REQUIRE(drag.changed);
    REQUIRE(drag.box.x2 == 100);
    REQUIRE(drag.box.y2 == 50);
    REQUIRE(session.active);
    REQUIRE(session.draft_box.x2 == 100);

    const PreviewRectDragResult release = apply_preview_rect_draft(session, false, AnnotationBox{10, 10, 140, 50}, 100, 100);
    REQUIRE(!release.active);
    REQUIRE(release.commit);
    REQUIRE(release.box.x1 == 10);
    REQUIRE(release.box.y1 == 10);
    REQUIRE(release.box.x2 == 100);
    REQUIRE(release.box.y2 == 50);
    REQUIRE(!session.active);
}

ANNOTATION_TEST_CASE(test_browser_move_draft_stays_unclamped_until_release) {
    PreviewRectDragSession session{};
    start_preview_rect_drag(session, RectDragKind::Move, 0.0f, 0.0f, AnnotationBox{10, 10, 30, 30}, 1);

    const PreviewRectDragResult drag = apply_preview_rect_draft(session, true, AnnotationBox{-8, 10, 12, 30}, 100, 100);
    REQUIRE(drag.active);
    REQUIRE(drag.box.x1 == -8);
    REQUIRE(session.draft_box.x1 == -8);

    const PreviewRectDragResult release = apply_preview_rect_draft(session, false, AnnotationBox{-8, 10, 12, 30}, 100, 100);
    REQUIRE(release.commit);
    REQUIRE(release.clipped_on_commit);
    REQUIRE(release.move_dx == -18);
    REQUIRE(release.box.x1 == 0);
    REQUIRE(release.box.x2 == 12);
}

}  // namespace
