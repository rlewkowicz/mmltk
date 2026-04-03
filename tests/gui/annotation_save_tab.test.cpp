#include "gui/annotation/save_tab.h"
#include "gui/annotation/workflow_runtime.h"
#include "gui/annotation/workflow_ui.h"

#include "support/catch2_compat.hpp"

namespace {

using namespace mmltk::gui;

AnnotationQueuedSave make_queued_save(std::uint64_t frame_id) {
    AnnotationFrame frame;
    frame.frame_id = frame_id;

    AnnotationObject object;
    object.object_id = "object-" + std::to_string(frame_id);

    return AnnotationQueuedSave{
        frame,
        {std::move(object)},
    };
}

AnnotationSaveTabPlan make_live_save_plan(bool save_running, bool hold_save_active = true) {
    return plan_annotation_save_tab(
        AnnotationSaveTabState{
            SourceKind::VideoStream,
            true,
            save_running,
            hold_save_active,
            false,
            true,
            true,
            0U,
            1U,
        });
}

void test_save_tab_ui_actions_stop_hold_save_and_clear_queue_for_non_live_sources() {
    AnnotationSaveControllerState controller_state;
    controller_state.hold_save = true;
    controller_state.hold_save_blocked = true;
    controller_state.queued_save = make_queued_save(17U);

    const AnnotationSaveTabUiCommitResult result =
        apply_annotation_save_tab_ui_actions(&controller_state,
                                             false,
                                             AnnotationSaveTabUiActions{});

    assert(result.clear_queue);
    assert(!controller_state.hold_save);
    assert(!controller_state.hold_save_blocked);
    assert(!controller_state.queued_save.has_value());
}

void test_hold_save_planner_enqueues_newest_frame_while_writer_is_busy() {
    AnnotationSaveControllerState controller_state;
    controller_state.hold_save = true;

    const AnnotationHoldSaveDispatchPlan plan =
        plan_annotation_hold_save_dispatch(
            controller_state,
            AnnotationHoldSaveDispatchState{
                make_live_save_plan(true),
                true,
                true,
                false,
                false,
            });

    assert(!plan.clear_queue);
    assert(plan.refresh_live_frame);
    assert(plan.enqueue_current_frame);
    assert(!plan.dispatch_current_frame_save);
    assert(!plan.block_hold_save);
}

void test_hold_save_planner_skips_refresh_when_current_frame_already_matches_queue() {
    AnnotationSaveControllerState controller_state;
    controller_state.hold_save = true;
    controller_state.queued_save = make_queued_save(23U);

    const AnnotationHoldSaveDispatchPlan plan =
        plan_annotation_hold_save_dispatch(
            controller_state,
            AnnotationHoldSaveDispatchState{
                make_live_save_plan(true),
                true,
                true,
                false,
                true,
            });

    assert(!plan.clear_queue);
    assert(!plan.refresh_live_frame);
    assert(!plan.enqueue_current_frame);
    assert(!plan.dispatch_current_frame_save);
    assert(!plan.block_hold_save);
}

void test_hold_save_planner_blocks_when_writer_queue_would_overflow() {
    AnnotationSaveControllerState controller_state;
    controller_state.hold_save = true;
    controller_state.queued_save = make_queued_save(23U);

    const AnnotationHoldSaveDispatchPlan plan =
        plan_annotation_hold_save_dispatch(
            controller_state,
            AnnotationHoldSaveDispatchState{
                make_live_save_plan(true),
                true,
                true,
                false,
                false,
            });

    assert(plan.clear_queue);
    assert(!plan.refresh_live_frame);
    assert(!plan.enqueue_current_frame);
    assert(!plan.dispatch_current_frame_save);
    assert(plan.block_hold_save);
}

void test_queued_save_dispatch_respects_hold_save_state() {
    AnnotationSaveControllerState dispatch_state;
    dispatch_state.hold_save = true;
    dispatch_state.queued_save = make_queued_save(31U);

    AnnotationQueuedSaveDispatchPlan plan =
        plan_annotation_queued_save_dispatch(dispatch_state, false);
    assert(!plan.clear_queue);
    assert(plan.dispatch_queued_save);

    dispatch_state.hold_save = false;
    plan = plan_annotation_queued_save_dispatch(dispatch_state, false);
    assert(plan.clear_queue);
    assert(!plan.dispatch_queued_save);
}

void test_annotation_workflow_setup_dispatch_plans_navigation_and_live_actions() {
    AnnotationSetupTabActions actions;
    actions.browse_request = AnnotationSetupBrowseRequest::Onnx;
    actions.reset_canvas_interactions = true;
    actions.preview_invalidated = true;
    actions.request_start_live = true;
    actions.request_reload_frame = true;
    actions.request_next_frame = true;

    const AnnotationWorkflowSetupDispatchPlan plan =
        plan_annotation_workflow_setup_dispatch(actions, 1U, 3U);
    assert(plan.browse_request == AnnotationSetupBrowseRequest::Onnx);
    assert(plan.prepare_source);
    assert(plan.reset_canvas_interactions);
    assert(plan.invalidate_preview);
    assert(plan.start_live);
    assert(!plan.stop_live);
    assert(plan.reload_frame);
    assert(plan.frame_step == 1);

    actions = AnnotationSetupTabActions{};
    actions.request_prev_frame = true;
    const AnnotationWorkflowSetupDispatchPlan first_frame_plan =
        plan_annotation_workflow_setup_dispatch(actions, 0U, 3U);
    assert(first_frame_plan.frame_step == 0);
}

void test_annotation_workflow_preview_runtime_tracks_dirty_ready_and_retry_state() {
    AnnotationWorkflowRuntime runtime;
    AnnotationFrame frame;
    frame.frame_id = 41U;

    assert(annotation_workflow_preview_pending(runtime, &frame));
    const std::uint64_t generation =
        begin_annotation_workflow_preview(runtime);
    assert(annotation_workflow_preview_running(runtime));
    assert(annotation_workflow_preview_generation_matches(runtime, generation));

    note_annotation_workflow_preview_ready(runtime, frame);
    assert(!annotation_workflow_preview_running(runtime));
    assert(!annotation_workflow_preview_pending(runtime, &frame));

    invalidate_annotation_workflow_preview(runtime);
    assert(annotation_workflow_preview_pending(runtime, &frame));

    note_annotation_workflow_preview_error(runtime);
    assert(annotation_workflow_preview_can_retry(runtime, 2));
    note_annotation_workflow_preview_error(runtime);
    assert(!annotation_workflow_preview_can_retry(runtime, 2));

    reset_annotation_workflow_preview(runtime, false);
    assert(!annotation_workflow_preview_pending(runtime, &frame));
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[gui][annotation_save_tab]",
                         test_save_tab_ui_actions_stop_hold_save_and_clear_queue_for_non_live_sources);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_save_tab]",
                         test_hold_save_planner_enqueues_newest_frame_while_writer_is_busy);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_save_tab]",
                         test_hold_save_planner_skips_refresh_when_current_frame_already_matches_queue);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_save_tab]",
                         test_hold_save_planner_blocks_when_writer_queue_would_overflow);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_save_tab]",
                         test_queued_save_dispatch_respects_hold_save_state);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_save_tab]",
                         test_annotation_workflow_setup_dispatch_plans_navigation_and_live_actions);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_save_tab]",
                         test_annotation_workflow_preview_runtime_tracks_dirty_ready_and_retry_state);
