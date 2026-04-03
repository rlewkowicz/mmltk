#pragma once

#include "gui/annotation/workflow_runtime.h"
#include "gui/live_preview_texture.h"
#include "gui/live_session_utils.h"
#include "gui/source_selection.h"
#include "mmltk/live/live_session_controller.h"

#include <optional>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace mmltk::gui {

inline void stop_annotation_workflow_hold_save(
    AnnotationWorkflowRuntime &runtime) noexcept;

inline void clear_annotation_workflow_live_overlay_state(
    AnnotationWorkflowRuntime &runtime) noexcept {
  runtime.live_overlay = {};
}

struct AnnotationWorkflowLiveOverlayRequest {
  const AnnotationDocument *document = nullptr;
  const AnnotationSession *session = nullptr;
  const AnnotationFrame *frame = nullptr;
};

struct AnnotationWorkflowLivePreviewRefreshResult {
  bool ok = true;
  std::vector<AnnotationResolvedObject> resolved_objects;
  std::shared_ptr<const AnnotationProjectedScene> projected_scene;
  std::string error_message;
};

template <typename PublishSnapshotFn>
inline bool publish_annotation_workflow_live_manual_overlay(
    AnnotationWorkflowRuntime &runtime, const AnnotationDocument &document,
    const AnnotationSession &session, const AnnotationFrame &frame,
    const std::shared_ptr<const AnnotationProjectedScene> &projected_scene,
    PublishSnapshotFn &&publish_snapshot);

[[nodiscard]] inline AnnotationWorkflowLiveOverlayState
make_annotation_workflow_live_overlay_state(
    const bool published, const std::uint64_t document_generation,
    const std::uint64_t session_revision, const std::uint32_t capture_width,
    const std::uint32_t capture_height) noexcept {
  return AnnotationWorkflowLiveOverlayState{
      published,     document_generation, session_revision,
      capture_width, capture_height,
  };
}

[[nodiscard]] inline AnnotationWorkflowLiveOverlayRequest
make_annotation_workflow_live_overlay_request(
    const AnnotationDocument *document, const AnnotationSession *session,
    const AnnotationFrame *frame) noexcept {
  return AnnotationWorkflowLiveOverlayRequest{
      document,
      session,
      frame,
  };
}

[[nodiscard]] inline bool annotation_workflow_live_overlay_needs_publish(
    const AnnotationWorkflowLiveOverlayState &state,
    const AnnotationWorkflowLiveOverlayRequest &request) noexcept {
  if (request.document == nullptr || request.session == nullptr ||
      request.frame == nullptr) {
    return false;
  }
  if (!state.published) {
    return true;
  }

  return state.document_generation != request.document->generation() ||
         state.session_revision != request.session->overlay_revision() ||
         state.capture_width !=
             annotation_frame_capture_width(*request.frame) ||
         state.capture_height !=
             annotation_frame_capture_height(*request.frame);
}

inline void note_annotation_workflow_live_overlay_published(
    AnnotationWorkflowRuntime &runtime, const AnnotationDocument &document,
    const AnnotationSession &session, const AnnotationFrame &frame) noexcept {
  runtime.live_overlay = make_annotation_workflow_live_overlay_state(
      true, document.generation(), session.overlay_revision(),
      annotation_frame_capture_width(frame),
      annotation_frame_capture_height(frame));
}

inline void clear_annotation_workflow_live_manual_overlay(
    AnnotationWorkflowRuntime &runtime,
    mmltk::live::LiveSessionController *controller,
    const SourceSelectionState &source) noexcept {
  clear_annotation_workflow_live_overlay_state(runtime);
  if (controller == nullptr) {
    return;
  }
  controller->manual_overlay_document().clear(
      static_cast<std::uint32_t>(std::max(1, source.capture_width)),
      static_cast<std::uint32_t>(std::max(1, source.capture_height)));
}

template <typename PublishSnapshotFn>
inline bool publish_annotation_workflow_live_manual_overlay(
    AnnotationWorkflowRuntime &runtime, const AnnotationDocument &document,
    const AnnotationSession &session, const AnnotationFrame &frame,
    PublishSnapshotFn &&publish_snapshot) {
  if (!annotation_workflow_live_overlay_needs_publish(
          runtime.live_overlay, make_annotation_workflow_live_overlay_request(
                                    &document, &session, &frame))) {
    return false;
  }

  const std::shared_ptr<const AnnotationProjectedScene> projected_scene =
      resolve_annotation_projected_scene(runtime, document, session, frame);
  return publish_annotation_workflow_live_manual_overlay(
      runtime, document, session, frame, projected_scene,
      std::forward<PublishSnapshotFn>(publish_snapshot));
}

template <typename PublishSnapshotFn>
inline bool publish_annotation_workflow_live_manual_overlay(
    AnnotationWorkflowRuntime &runtime, const AnnotationDocument &document,
    const AnnotationSession &session, const AnnotationFrame &frame,
    const std::shared_ptr<const AnnotationProjectedScene> &projected_scene,
    PublishSnapshotFn &&publish_snapshot) {
  if (!annotation_workflow_live_overlay_needs_publish(
          runtime.live_overlay,
          make_annotation_workflow_live_overlay_request(&document, &session,
                                                        &frame))) {
    return false;
  }
  if (projected_scene == nullptr) {
    return false;
  }

  note_annotation_workflow_live_overlay_published(runtime, document, session,
                                                  frame);
  std::forward<PublishSnapshotFn>(publish_snapshot)(
      AnnotationRenderer::build_manual_overlay_snapshot(frame, document,
                                                        session,
                                                        *projected_scene));
  return true;
}

template <typename PublishSnapshotFn>
[[nodiscard]] inline AnnotationWorkflowLivePreviewRefreshResult
refresh_annotation_workflow_live_preview(
    AnnotationWorkflowRuntime &runtime, const AnnotationDocument &document,
    const AnnotationSession &session, const AnnotationFrame &frame,
    const AnnotationCategories &categories, const bool has_frame_pixels,
    PublishSnapshotFn &&publish_snapshot) {
  AnnotationWorkflowLivePreviewRefreshResult result;
  result.projected_scene =
      resolve_annotation_projected_scene(runtime, document, session, frame);
  if (result.projected_scene != nullptr) {
    (void)publish_annotation_workflow_live_manual_overlay(
        runtime, document, session, frame, result.projected_scene,
        std::forward<PublishSnapshotFn>(publish_snapshot));
  }
  if (has_frame_pixels) {
    AnnotationWorkflowResolvedObjectsResult resolved =
        result.projected_scene != nullptr
            ? resolve_annotation_workflow_objects(frame, document, categories,
                                                  result.projected_scene, true)
            : resolve_annotation_workflow_objects(
                  runtime, document, session,
                  std::optional<AnnotationFrame>{frame}, categories, true);
    result.ok = resolved.ok;
    result.resolved_objects = std::move(resolved.resolved_objects);
    result.error_message = std::move(resolved.error_message);
  }

  note_annotation_workflow_preview_ready(runtime, frame);
  return result;
}

inline void reset_annotation_workflow_live_session_state(
    AnnotationWorkflowRuntime &runtime,
    std::optional<AnnotationFrame> *frame,
    std::vector<AnnotationResolvedObject> *resolved_instances,
    LivePreviewTexture *preview_texture) noexcept {
  if (preview_texture != nullptr) {
    preview_texture->clear_frame();
  }
  if (frame != nullptr) {
    frame->reset();
  }
  if (resolved_instances != nullptr) {
    resolved_instances->clear();
  }
  reset_annotation_workflow_preview(runtime, false);
}

struct AnnotationWorkflowLiveSessionStartOutcome {
  std::unique_ptr<mmltk::live::LiveSessionController> controller;
  std::unique_ptr<mmltk::live::LiveSessionStatus> status;
};

struct AnnotationWorkflowLiveSessionShellStartResult {
  bool started = false;
  std::string error_message;
};

#if MMLTK_RFDETR_LIVE_CAPTURE
[[nodiscard]] inline AnnotationWorkflowLiveSessionStartOutcome
start_annotation_workflow_live_session(
    const SourceSelectionState &source, const int cuda_device_index,
    LivePreviewTexture *preview_texture) {
  if (source.kind != SourceKind::VideoStream) {
    throw std::runtime_error(
        "annotation live capture requires a video source");
  }

  mmltk::live::LiveSessionConfig session_config;
  session_config.capture.device_path =
      "/dev/video" + std::to_string(std::max(0, source.device_index));
  session_config.capture.cuda_device_index = cuda_device_index;
  session_config.capture.width =
      static_cast<std::uint32_t>(std::max(1, source.capture_width));
  session_config.capture.height =
      static_cast<std::uint32_t>(std::max(1, source.capture_height));
  session_config.capture.fps =
      static_cast<std::uint32_t>(std::max(1, source.capture_fps));
  session_config.capture.v4l2_buffer_count =
      static_cast<std::uint32_t>(std::max(1, source.v4l2_buffer_count));
  session_config.capture.preview_buffer_count = 1U;
  const mmltk::live::LiveCaptureRegion initial_region =
      full_capture_region_for_source(source);
  session_config.capture.initial_region = frameshow::CaptureRegion{
      initial_region.x,
      initial_region.y,
      initial_region.width,
      initial_region.height,
  };
  session_config.detect_slot_count = 2U;
  session_config.output_slot_count = 4U;
  session_config.cuda_device_index = cuda_device_index;

  AnnotationWorkflowLiveSessionStartOutcome outcome;
  auto controller = std::make_unique<mmltk::live::LiveSessionController>(
      std::move(session_config));
  try {
    controller->start();
    controller->manual_overlay_document().clear(
        static_cast<std::uint32_t>(std::max(1, source.capture_width)),
        static_cast<std::uint32_t>(std::max(1, source.capture_height)));
    if (preview_texture != nullptr) {
      preview_texture->clear_frame();
      preview_texture->begin_live_stream(*controller, cuda_device_index);
    }
  } catch (...) {
    try {
      controller->stop();
    } catch (...) {
      (void)0;
    }
    throw;
  }

  outcome.status = std::make_unique<mmltk::live::LiveSessionStatus>(
      controller->snapshot_status());
  outcome.controller = std::move(controller);
  return outcome;
}
#else
[[nodiscard]] inline AnnotationWorkflowLiveSessionStartOutcome
start_annotation_workflow_live_session(const SourceSelectionState &source,
                                       const int cuda_device_index,
                                       LivePreviewTexture *preview_texture) {
  (void)source;
  (void)cuda_device_index;
  (void)preview_texture;
  throw std::runtime_error(
      "annotation live capture is unavailable because live capture support was "
      "not built into this binary.");
}
#endif

inline void stop_annotation_workflow_live_session(
    AnnotationWorkflowRuntime &runtime,
    std::unique_ptr<mmltk::live::LiveSessionController> *controller,
    std::unique_ptr<mmltk::live::LiveSessionStatus> *status,
    LivePreviewTexture *preview_texture) {
  clear_annotation_workflow_live_overlay_state(runtime);
  if (preview_texture != nullptr) {
    preview_texture->end_live_stream();
  }
  if (controller != nullptr && controller->get() != nullptr) {
    controller->get()->stop();
    controller->reset();
  }
  if (status != nullptr) {
    status->reset();
  }
  if (preview_texture != nullptr) {
    preview_texture->clear_frame();
  }
}

template <typename CancelInteractionsFn>
inline void reset_annotation_workflow_live_session_shell(
    AnnotationWorkflowRuntime &runtime,
    std::unique_ptr<mmltk::live::LiveSessionController> *controller,
    std::unique_ptr<mmltk::live::LiveSessionStatus> *status,
    std::optional<AnnotationFrame> *frame,
    std::vector<AnnotationResolvedObject> *resolved_instances,
    LivePreviewTexture *preview_texture,
    CancelInteractionsFn &&cancel_interactions) {
  stop_annotation_workflow_hold_save(runtime);
  std::forward<CancelInteractionsFn>(cancel_interactions)();
  stop_annotation_workflow_live_session(runtime, controller, status,
                                        preview_texture);
  reset_annotation_workflow_live_session_state(runtime, frame,
                                               resolved_instances,
                                               preview_texture);
}

template <typename CancelInteractionsFn>
[[nodiscard]] inline AnnotationWorkflowLiveSessionShellStartResult
restart_annotation_workflow_live_session_shell(
    AnnotationWorkflowRuntime &runtime,
    std::unique_ptr<mmltk::live::LiveSessionController> *controller,
    std::unique_ptr<mmltk::live::LiveSessionStatus> *status,
    std::optional<AnnotationFrame> *frame,
    std::vector<AnnotationResolvedObject> *resolved_instances,
    LivePreviewTexture *preview_texture,
    const SourceSelectionState &source,
    const int cuda_device_index,
    CancelInteractionsFn &&cancel_interactions) {
  reset_annotation_workflow_live_session_shell(
      runtime, controller, status, frame, resolved_instances, preview_texture,
      std::forward<CancelInteractionsFn>(cancel_interactions));

  AnnotationWorkflowLiveSessionShellStartResult result;
  try {
    AnnotationWorkflowLiveSessionStartOutcome start_outcome =
        start_annotation_workflow_live_session(source, cuda_device_index,
                                               preview_texture);
    if (controller != nullptr) {
      *controller = std::move(start_outcome.controller);
    }
    if (status != nullptr) {
      *status = std::move(start_outcome.status);
    }
    result.started = true;
  } catch (const std::exception &error) {
    result.error_message = error.what();
  }
  return result;
}

[[nodiscard]] inline std::optional<AnnotationFrame>
make_annotation_workflow_live_annotation_frame_from_preview(
    const SourceSelectionState &source, const bool full_frame,
    const LivePreviewTexture &preview_texture,
    const std::optional<AnnotationFrame> &current_frame) {
  const LivePreviewTextureState preview_state = preview_texture.snapshot();
  if (!preview_state.has_frame || !preview_state.live_frame_id.has_value()) {
    return std::nullopt;
  }

  const mmltk::live::LiveCaptureRegion region = preview_region_for_source(
      preview_state.displayed_region, source, full_frame);
  if (current_frame.has_value() &&
      current_frame->live_frame_id == preview_state.live_frame_id &&
      current_frame->view_x == region.x && current_frame->view_y == region.y &&
      current_frame->width == region.width &&
      current_frame->height == region.height) {
    return std::nullopt;
  }

  AnnotationFrame frame;
  frame.source_name = "live";
  frame.source_path = "/dev/video" + std::to_string(std::max(0, source.device_index));
  frame.frame_id = preview_state.last_frame_id;
  frame.live_frame_id = preview_state.live_frame_id;
  frame.width = region.width;
  frame.height = region.height;
  frame.view_x = region.x;
  frame.view_y = region.y;
  frame.capture_width =
      static_cast<std::uint32_t>(std::max(1, source.capture_width));
  frame.capture_height =
      static_cast<std::uint32_t>(std::max(1, source.capture_height));
  return frame;
}

template <typename ReadbackFn>
inline bool ensure_annotation_workflow_live_annotation_frame_pixels(
    const SourceSelectionState &source, std::optional<AnnotationFrame> &frame,
    const bool annotation_live_running, ReadbackFn &&readback_frame,
    std::string *error_message) {
  if (!frame.has_value()) {
    if (error_message != nullptr) {
      *error_message = "no live annotation frame is available";
    }
    return false;
  }
  if (!annotation_live_running || !frame->live_frame_id.has_value()) {
    if (frame->pixels_bgr.empty()) {
      if (error_message != nullptr) {
        *error_message = "annotation frame pixels are unavailable";
      }
      return false;
    }
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }
  if (!frame->pixels_bgr.empty()) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }

  if (!std::forward<ReadbackFn>(readback_frame)(*frame, source, error_message)) {
    return false;
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

inline bool ensure_annotation_workflow_live_annotation_frame_pixels(
    const SourceSelectionState &source, std::optional<AnnotationFrame> &frame,
    const bool annotation_live_running,
    mmltk::live::LiveSessionController *controller, const bool full_frame,
    std::string *error_message) {
  return ensure_annotation_workflow_live_annotation_frame_pixels(
      source, frame, annotation_live_running,
      [controller, full_frame](AnnotationFrame &frame_snapshot,
                               const SourceSelectionState &source_snapshot,
                               std::string *readback_error) {
        if (controller == nullptr ||
            !frame_snapshot.live_frame_id.has_value()) {
          return false;
        }

        std::vector<std::uint8_t> pixels_bgr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        mmltk::live::LiveCaptureRegion region{};
        if (!controller->readback_raw_base(*frame_snapshot.live_frame_id,
                                           &pixels_bgr, &width, &height,
                                           &region, readback_error)) {
          return false;
        }

        AnnotationFrame raw_frame = frame_snapshot;
        raw_frame.pixels_bgr = std::move(pixels_bgr);
        raw_frame.width = width;
        raw_frame.height = height;
        raw_frame.view_x = region.x;
        raw_frame.view_y = region.y;
        raw_frame.capture_width =
            static_cast<std::uint32_t>(std::max(1, source_snapshot.capture_width));
        raw_frame.capture_height =
            static_cast<std::uint32_t>(std::max(1, source_snapshot.capture_height));
        const mmltk::live::LiveCaptureRegion display_region =
            preview_region_for_source(region, source_snapshot, full_frame);
        if (display_region.x != region.x || display_region.y != region.y ||
            display_region.width != region.width ||
            display_region.height != region.height) {
          frame_snapshot = extract_annotation_frame_region(
              raw_frame, box_from_live_region(display_region));
        } else {
          frame_snapshot = std::move(raw_frame);
        }
        return true;
      },
      error_message);
}

} // namespace mmltk::gui
