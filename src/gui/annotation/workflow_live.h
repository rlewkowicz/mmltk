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
    PublishSnapshotFn&& publish_snapshot);

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
inline bool publish_annotation_workflow_live_manual_overlay(AnnotationWorkflowRuntime& runtime,
                                                            const AnnotationDocument& document,
                                                            const AnnotationSession& session,
                                                            const AnnotationFrame& frame,
                                                            PublishSnapshotFn&& publish_snapshot) {
    if (!annotation_workflow_live_overlay_needs_publish(
            runtime.live_overlay, make_annotation_workflow_live_overlay_request(&document, &session, &frame))) {
        return false;
    }

    const std::shared_ptr<const AnnotationProjectedScene> projected_scene =
        resolve_annotation_projected_scene(runtime, document, session, frame);
    return publish_annotation_workflow_live_manual_overlay(runtime, document, session, frame, projected_scene,
                                                           std::forward<PublishSnapshotFn>(publish_snapshot));
}

template <typename PublishSnapshotFn>
inline bool publish_annotation_workflow_live_manual_overlay(
    AnnotationWorkflowRuntime& runtime, const AnnotationDocument& document, const AnnotationSession& session,
    const AnnotationFrame& frame, const std::shared_ptr<const AnnotationProjectedScene>& projected_scene,
    PublishSnapshotFn&& publish_snapshot) {
    if (!annotation_workflow_live_overlay_needs_publish(
            runtime.live_overlay, make_annotation_workflow_live_overlay_request(&document, &session, &frame))) {
        return false;
    }
    if (projected_scene == nullptr) {
        return false;
    }

    note_annotation_workflow_live_overlay_published(runtime, document, session, frame);
    std::forward<PublishSnapshotFn>(publish_snapshot)(
        AnnotationRenderer::build_manual_overlay_snapshot(frame, document, session, *projected_scene));
    return true;
}

template <typename PublishSnapshotFn>
[[nodiscard]] inline AnnotationWorkflowLivePreviewRefreshResult refresh_annotation_workflow_live_preview(
    AnnotationWorkflowRuntime& runtime, const AnnotationDocument& document, const AnnotationSession& session,
    const AnnotationFrame& frame, const AnnotationCategories& categories, const bool has_frame_pixels,
    PublishSnapshotFn&& publish_snapshot) {
    AnnotationWorkflowLivePreviewRefreshResult result;
    result.projected_scene = resolve_annotation_projected_scene(runtime, document, session, frame);
    if (result.projected_scene != nullptr) {
        (void)publish_annotation_workflow_live_manual_overlay(runtime, document, session, frame, result.projected_scene,
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
            if (controller == nullptr || !frame_snapshot.live_frame_id.has_value()) {
                return false;
            }

            std::vector<std::uint8_t> pixels_bgr;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            mmltk::live::LiveCaptureRegion region{};
            if (!controller->readback_raw_base(*frame_snapshot.live_frame_id, &pixels_bgr, &width, &height, &region,
                                               readback_error)) {
                return false;
            }

            AnnotationFrame raw_frame = frame_snapshot;
            raw_frame.pixels_bgr = std::move(pixels_bgr);
            raw_frame.width = width;
            raw_frame.height = height;
            raw_frame.view_x = region.x;
            raw_frame.view_y = region.y;
            raw_frame.capture_width = static_cast<std::uint32_t>(std::max(1, source_snapshot.capture_width));
            raw_frame.capture_height = static_cast<std::uint32_t>(std::max(1, source_snapshot.capture_height));
            const mmltk::live::LiveCaptureRegion display_region =
                preview_region_for_source(region, source_snapshot, full_frame);
            if (display_region.x != region.x || display_region.y != region.y || display_region.width != region.width ||
                display_region.height != region.height) {
                frame_snapshot = extract_annotation_frame_region(raw_frame, box_from_live_region(display_region));
            } else {
                frame_snapshot = std::move(raw_frame);
            }
            return true;
        },
        error_message);
}

}  // namespace mmltk::gui
