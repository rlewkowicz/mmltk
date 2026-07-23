#pragma once

#include "gui/annotation/workflow_runtime.h"

namespace mmltk::gui {

inline void clear_annotation_workflow_live_overlay_state(AnnotationWorkflowRuntime& runtime) noexcept {
    runtime.live_overlay = {};
}

struct AnnotationWorkflowLiveOverlayRequest {
    const AnnotationDocument* document = nullptr;
    const AnnotationSession* session = nullptr;
    const AnnotationFrame* frame = nullptr;
};

[[nodiscard]] inline AnnotationWorkflowLiveOverlayState make_annotation_workflow_live_overlay_state(
    const bool published, const std::uint64_t document_generation, const std::uint64_t session_revision,
    const std::uint32_t capture_width, const std::uint32_t capture_height) noexcept {
    return AnnotationWorkflowLiveOverlayState{
        published, document_generation, session_revision, capture_width, capture_height,
    };
}

[[nodiscard]] inline AnnotationWorkflowLiveOverlayRequest make_annotation_workflow_live_overlay_request(
    const AnnotationDocument* document, const AnnotationSession* session, const AnnotationFrame* frame) noexcept {
    return AnnotationWorkflowLiveOverlayRequest{
        document,
        session,
        frame,
    };
}

[[nodiscard]] inline bool annotation_workflow_live_overlay_needs_publish(
    const AnnotationWorkflowLiveOverlayState& state, const AnnotationWorkflowLiveOverlayRequest& request) noexcept {
    if (request.document == nullptr || request.session == nullptr || request.frame == nullptr) {
        return false;
    }
    if (!state.published) {
        return true;
    }

    return state.document_generation != request.document->generation() ||
           state.session_revision != request.session->overlay_revision() ||
           state.capture_width != annotation_frame_capture_width(*request.frame) ||
           state.capture_height != annotation_frame_capture_height(*request.frame);
}

inline void note_annotation_workflow_live_overlay_published(AnnotationWorkflowRuntime& runtime,
                                                            const AnnotationDocument& document,
                                                            const AnnotationSession& session,
                                                            const AnnotationFrame& frame) noexcept {
    runtime.live_overlay = make_annotation_workflow_live_overlay_state(
        true, document.generation(), session.overlay_revision(), annotation_frame_capture_width(frame),
        annotation_frame_capture_height(frame));
}

}  
