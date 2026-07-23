#pragma once

#include "gui/annotation/workflow_live_overlay.h"
#include "gui/annotation/workflow_runtime.h"
#include "gui/live_session_utils.h"
#include "gui/source_selection.h"
#include "mmltk/live/live_session_controller.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace mmltk::gui {

inline void stop_annotation_workflow_hold_save(AnnotationWorkflowRuntime& runtime) noexcept;

struct AnnotationWorkflowLivePreviewRefreshResult {
    bool ok = true;
    std::vector<AnnotationResolvedObject> resolved_objects;
    std::shared_ptr<const AnnotationProjectedScene> projected_scene;
    std::string error_message;
};

template <typename PublishSnapshotFn>
inline bool publish_annotation_workflow_live_manual_overlay(
    AnnotationWorkflowRuntime& runtime, const AnnotationDocument& document, const AnnotationSession& session,
    const AnnotationFrame& frame, const std::shared_ptr<const AnnotationProjectedScene>& projected_scene,
    std::optional<AnnotationBox> crop_capture_box, PublishSnapshotFn&& publish_snapshot);

inline void clear_annotation_workflow_live_manual_overlay(AnnotationWorkflowRuntime& runtime,
                                                          mmltk::live::LiveSessionController* controller,
                                                          const SourceSelectionState& source) noexcept {
    clear_annotation_workflow_live_overlay_state(runtime);
    if (controller == nullptr) {
        return;
    }
    controller->manual_overlay_document().clear(static_cast<std::uint32_t>(std::max(1, source.capture_width)),
                                                static_cast<std::uint32_t>(std::max(1, source.capture_height)));
}

template <typename PublishSnapshotFn>
inline bool publish_annotation_workflow_live_manual_overlay(
    AnnotationWorkflowRuntime& runtime, const AnnotationDocument& document, const AnnotationSession& session,
    const AnnotationFrame& frame, std::optional<AnnotationBox> crop_capture_box, PublishSnapshotFn&& publish_snapshot) {
    if (!annotation_workflow_live_overlay_needs_publish(
            runtime.live_overlay, make_annotation_workflow_live_overlay_request(&document, &session, &frame))) {
        return false;
    }

    const std::shared_ptr<const AnnotationProjectedScene> projected_scene =
        resolve_annotation_projected_scene(runtime, document, session, frame);
    return publish_annotation_workflow_live_manual_overlay(runtime, document, session, frame, projected_scene,
                                                           crop_capture_box,
                                                           std::forward<PublishSnapshotFn>(publish_snapshot));
}

template <typename PublishSnapshotFn>
inline bool publish_annotation_workflow_live_manual_overlay(
    AnnotationWorkflowRuntime& runtime, const AnnotationDocument& document, const AnnotationSession& session,
    const AnnotationFrame& frame, const std::shared_ptr<const AnnotationProjectedScene>& projected_scene,
    std::optional<AnnotationBox> crop_capture_box, PublishSnapshotFn&& publish_snapshot) {
    if (!annotation_workflow_live_overlay_needs_publish(
            runtime.live_overlay, make_annotation_workflow_live_overlay_request(&document, &session, &frame))) {
        return false;
    }
    if (projected_scene == nullptr) {
        return false;
    }

    note_annotation_workflow_live_overlay_published(runtime, document, session, frame);
    std::forward<PublishSnapshotFn>(publish_snapshot)(AnnotationRenderer::build_manual_overlay_snapshot(
        frame, document, session, *projected_scene, crop_capture_box));
    return true;
}

template <typename PublishSnapshotFn>
[[nodiscard]] inline AnnotationWorkflowLivePreviewRefreshResult refresh_annotation_workflow_live_preview(
    AnnotationWorkflowRuntime& runtime, const AnnotationDocument& document, const AnnotationSession& session,
    const AnnotationFrame& frame, const AnnotationCategories& categories, const bool has_frame_pixels,
    std::optional<AnnotationBox> crop_capture_box, PublishSnapshotFn&& publish_snapshot) {
    AnnotationWorkflowLivePreviewRefreshResult result;
    result.projected_scene = resolve_annotation_projected_scene(runtime, document, session, frame);
    if (result.projected_scene != nullptr) {
        (void)publish_annotation_workflow_live_manual_overlay(runtime, document, session, frame, result.projected_scene,
                                                              crop_capture_box,
                                                              std::forward<PublishSnapshotFn>(publish_snapshot));
    }
    if (has_frame_pixels) {
        AnnotationWorkflowResolvedObjectsResult resolved =
            result.projected_scene != nullptr
                ? resolve_annotation_workflow_objects(frame, document, categories, result.projected_scene, true)
                : resolve_annotation_workflow_objects(runtime, document, session, std::optional<AnnotationFrame>{frame},
                                                      categories, true);
        result.ok = resolved.ok;
        result.resolved_objects = std::move(resolved.resolved_objects);
        result.error_message = std::move(resolved.error_message);
    }

    note_annotation_workflow_preview_ready(runtime, frame);
    return result;
}

template <typename ReadbackFn>
inline bool ensure_annotation_workflow_live_annotation_frame_pixels(const SourceSelectionState& source,
                                                                    std::optional<AnnotationFrame>& frame,
                                                                    const bool annotation_live_running,
                                                                    ReadbackFn&& readback_frame,
                                                                    std::string* error_message) {
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

inline bool ensure_annotation_workflow_live_annotation_frame_pixels(const SourceSelectionState& source,
                                                                    std::optional<AnnotationFrame>& frame,
                                                                    const bool annotation_live_running,
                                                                    mmltk::live::LiveSessionController* controller,
                                                                    const bool full_frame, std::string* error_message) {
    return ensure_annotation_workflow_live_annotation_frame_pixels(
        source, frame, annotation_live_running,
        [controller, full_frame](AnnotationFrame& frame_snapshot, const SourceSelectionState& source_snapshot,
                                 std::string* readback_error) {
            (void)controller;
            (void)full_frame;
            (void)frame_snapshot;
            (void)source_snapshot;
            if (readback_error != nullptr) {
                *readback_error = "live annotation frame pixels are unavailable on the native-presented live path";
            }
            return false;
        },
        error_message);
}

}  
