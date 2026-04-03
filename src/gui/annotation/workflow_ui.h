#pragma once

#include "gui/annotation/setup_tab.h"
#include "gui/annotation/sidebar_edits.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

struct ImFont;

namespace mmltk::gui {

struct AnnotationWorkflowSetupRequest {
  AnnotateViewState *annotate = nullptr;
  bool live_video = false;
  bool live_running = false;
  bool block_actions = false;
  bool can_use_video = false;
  bool prepare_running = false;
  bool single_image_browse_busy = false;
  bool weights_browse_busy = false;
  bool onnx_browse_busy = false;
  bool tensorrt_browse_busy = false;
  std::size_t current_input_index = 0;
  std::size_t input_count = 0;
  ImFont *compact_font = nullptr;
};

[[nodiscard]] inline AnnotationWorkflowSetupRequest
make_annotation_workflow_setup_request(
    AnnotateViewState *annotate, const bool live_video, const bool live_running,
    const bool block_actions, const bool can_use_video,
    const bool prepare_running, const bool single_image_browse_busy,
    const bool weights_browse_busy, const bool onnx_browse_busy,
    const bool tensorrt_browse_busy, const std::size_t current_input_index,
    const std::size_t input_count, ImFont *compact_font) noexcept {
  return AnnotationWorkflowSetupRequest{
      annotate,
      live_video,
      live_running,
      block_actions,
      can_use_video,
      prepare_running,
      single_image_browse_busy,
      weights_browse_busy,
      onnx_browse_busy,
      tensorrt_browse_busy,
      current_input_index,
      input_count,
      compact_font,
  };
}

[[nodiscard]] inline AnnotationSetupTabState make_annotation_setup_tab_state(
    const AnnotationWorkflowSetupRequest &inputs) noexcept {
  return AnnotationSetupTabState{
      inputs.annotate,
      inputs.live_video,
      inputs.live_running,
      inputs.block_actions,
      inputs.can_use_video,
      inputs.prepare_running,
      inputs.single_image_browse_busy,
      inputs.weights_browse_busy,
      inputs.onnx_browse_busy,
      inputs.tensorrt_browse_busy,
      inputs.current_input_index,
      inputs.input_count,
      inputs.compact_font,
  };
}

struct AnnotationWorkflowSetupDispatchPlan {
  AnnotationSetupBrowseRequest browse_request =
      AnnotationSetupBrowseRequest::None;
  bool prepare_source = true;
  bool reset_canvas_interactions = false;
  bool invalidate_preview = false;
  bool start_live = false;
  bool stop_live = false;
  bool reload_frame = false;
  int frame_step = 0;
};

[[nodiscard]] inline AnnotationWorkflowSetupDispatchPlan
plan_annotation_workflow_setup_dispatch(
    const AnnotationSetupTabActions &actions,
    const std::size_t current_input_index,
    const std::size_t input_count) noexcept {
  AnnotationWorkflowSetupDispatchPlan plan;
  plan.browse_request = actions.browse_request;
  plan.reset_canvas_interactions = actions.reset_canvas_interactions;
  plan.invalidate_preview = actions.preview_invalidated;
  plan.start_live = actions.request_start_live;
  plan.stop_live = actions.request_stop_live;
  plan.reload_frame = actions.request_reload_frame;
  if (actions.request_prev_frame && current_input_index > 0U) {
    plan.frame_step = -1;
  } else if (actions.request_next_frame &&
             current_input_index + 1U < input_count) {
    plan.frame_step = 1;
  }
  return plan;
}

template <typename BrowseFn, typename PrepareSourceFn, typename ResetCanvasFn,
          typename InvalidatePreviewFn, typename StartLiveFn,
          typename StopLiveFn, typename ReloadFrameFn, typename StepFrameFn>
inline void dispatch_annotation_workflow_setup(
    const AnnotationWorkflowSetupDispatchPlan &plan, BrowseFn &&browse,
    PrepareSourceFn &&prepare_source,
    ResetCanvasFn &&reset_canvas_interactions,
    InvalidatePreviewFn &&invalidate_preview, StartLiveFn &&start_live,
    StopLiveFn &&stop_live, ReloadFrameFn &&reload_frame,
    StepFrameFn &&step_frame) {
  std::forward<BrowseFn>(browse)(plan.browse_request);
  if (plan.prepare_source) {
    std::forward<PrepareSourceFn>(prepare_source)();
  }
  if (plan.reset_canvas_interactions) {
    std::forward<ResetCanvasFn>(reset_canvas_interactions)();
  }
  if (plan.invalidate_preview) {
    std::forward<InvalidatePreviewFn>(invalidate_preview)();
  }
  if (plan.start_live) {
    std::forward<StartLiveFn>(start_live)();
  }
  if (plan.stop_live) {
    std::forward<StopLiveFn>(stop_live)();
  }
  if (plan.reload_frame) {
    std::forward<ReloadFrameFn>(reload_frame)();
  }
  if (plan.frame_step != 0) {
    std::forward<StepFrameFn>(step_frame)(plan.frame_step);
  }
}

struct AnnotationWorkflowObjectsTabRequest {
  AnnotationController &controller;
  AnnotationDocument &document;
  AnnotationSession &session;
  AnnotationCategories &categories;
  const AnnotationFrame *frame = nullptr;
  std::string *pending_class_name = nullptr;
  bool assist_available = false;
};

[[nodiscard]] inline AnnotationObjectsTabApplyContext
make_annotation_objects_tab_apply_context(
    AnnotationController &controller, AnnotationDocument &document,
    AnnotationSession &session, AnnotationCategories &categories,
    const AnnotationFrame *frame, std::string *pending_class_name,
    const bool assist_available) noexcept {
  return AnnotationObjectsTabApplyContext{
      controller, document,           session,          categories,
      frame,      pending_class_name, assist_available,
  };
}

[[nodiscard]] inline AnnotationObjectsTabApplyContext
make_annotation_workflow_objects_tab_apply_context(
    const AnnotationWorkflowObjectsTabRequest& request) noexcept {
  return make_annotation_objects_tab_apply_context(
      request.controller, request.document, request.session, request.categories,
      request.frame, request.pending_class_name, request.assist_available);
}

struct AnnotationWorkflowMaskTabRequest {
  AnnotationDocument &document;
  AnnotationSession &session;
  const AnnotationFrame *frame = nullptr;
  int *cleanup_radius = nullptr;
};

[[nodiscard]] inline AnnotationMaskTabApplyContext
make_annotation_mask_tab_apply_context(AnnotationDocument &document,
                                       AnnotationSession &session,
                                       const AnnotationFrame *frame,
                                       int *cleanup_radius) noexcept {
  return AnnotationMaskTabApplyContext{
      document,
      session,
      frame,
      cleanup_radius,
  };
}

[[nodiscard]] inline AnnotationMaskTabApplyContext
make_annotation_workflow_mask_tab_apply_context(
    const AnnotationWorkflowMaskTabRequest& request) noexcept {
  return make_annotation_mask_tab_apply_context(
      request.document, request.session, request.frame, request.cleanup_radius);
}

[[nodiscard]] inline bool annotation_workflow_should_launch_assist(
    const AnnotationSidebarMutationResult &result,
    const bool allow_assist) noexcept {
  return allow_assist && result.request_assist;
}

[[nodiscard]] inline bool annotation_workflow_should_reset_canvas_interactions(
    const AnnotationSidebarMutationResult &result) noexcept {
  return result.reset_canvas_interactions;
}

[[nodiscard]] inline bool annotation_workflow_should_set_next_tool(
    const AnnotationSidebarMutationResult &result) noexcept {
  return result.next_tool.has_value();
}

[[nodiscard]] inline bool annotation_workflow_should_invalidate_preview(
    const AnnotationSidebarMutationResult &result) noexcept {
  return result.preview_invalidated;
}

template <typename LaunchAssistFn, typename ResetCanvasFn, typename SetToolFn,
          typename InvalidatePreviewFn>
inline void apply_annotation_sidebar_shell_effects(
    const AnnotationSidebarMutationResult &result, const bool allow_assist,
    LaunchAssistFn &&launch_assist, ResetCanvasFn &&reset_canvas_interactions,
    SetToolFn &&set_active_tool, InvalidatePreviewFn &&invalidate_preview) {
  if (annotation_workflow_should_launch_assist(result, allow_assist)) {
    std::forward<LaunchAssistFn>(launch_assist)();
  }
  if (annotation_workflow_should_reset_canvas_interactions(result)) {
    std::forward<ResetCanvasFn>(reset_canvas_interactions)();
  }
  if (annotation_workflow_should_set_next_tool(result)) {
    const AnnotationToolKind next_tool =
        result.next_tool.value_or(AnnotationToolKind::Direct);
    std::forward<SetToolFn>(set_active_tool)(next_tool);
  }
  if (annotation_workflow_should_invalidate_preview(result)) {
    std::forward<InvalidatePreviewFn>(invalidate_preview)();
  }
}

} // namespace mmltk::gui
