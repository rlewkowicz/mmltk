#pragma once

#include "gui/annotation/projected_scene_cache.h"
#include "gui/annotation/save_tab.h"
#include "gui/annotation_core.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mmltk::gui {

struct AnnotationWorkflowPreviewState {
    bool dirty = true;
    bool running = false;
    int error_count = 0;
    std::uint64_t generation = 0;
    std::uint64_t frame_id = 0;
};

struct AnnotationWorkflowLiveOverlayState {
    bool published = false;
    std::uint64_t document_generation = 0;
    std::uint64_t session_revision = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationWorkflowRuntime {
    AnnotationProjectedSceneCache projected_scene_cache;
    AnnotationSaveControllerState save_controller{};
    AnnotationWorkflowPreviewState preview{};
    AnnotationWorkflowLiveOverlayState live_overlay{};
};

struct AnnotationWorkflowProjectedSceneRequest {
    const AnnotationDocument* document = nullptr;
    const AnnotationSession* session = nullptr;
    const AnnotationFrame* frame = nullptr;
};

[[nodiscard]] inline const AnnotationFrame* annotation_frame_ptr(const std::optional<AnnotationFrame>& frame) noexcept {
    return frame.has_value() ? std::addressof(*frame) : nullptr;
}

[[nodiscard]] inline AnnotationWorkflowProjectedSceneRequest make_annotation_workflow_projected_scene_request(
    const AnnotationDocument* document, const AnnotationSession* session, const AnnotationFrame* frame) noexcept {
    return AnnotationWorkflowProjectedSceneRequest{
        document,
        session,
        frame,
    };
}

[[nodiscard]] inline AnnotationWorkflowProjectedSceneRequest make_annotation_current_projected_scene_request(
    const AnnotationDocument& document, const AnnotationSession& session,
    const std::optional<AnnotationFrame>& frame) noexcept {
    return make_annotation_workflow_projected_scene_request(&document, &session, annotation_frame_ptr(frame));
}

[[nodiscard]] inline std::shared_ptr<const AnnotationProjectedScene> resolve_annotation_projected_scene(
    AnnotationWorkflowRuntime& runtime, const AnnotationWorkflowProjectedSceneRequest& request) {
    return runtime.projected_scene_cache.resolve(AnnotationProjectedSceneCacheInputs{
        request.document,
        request.session,
        request.frame,
    });
}

[[nodiscard]] inline std::shared_ptr<const AnnotationProjectedScene> resolve_annotation_projected_scene(
    AnnotationWorkflowRuntime& runtime, const AnnotationDocument& document, const AnnotationSession& session,
    const std::optional<AnnotationFrame>& frame) {
    return resolve_annotation_projected_scene(
        runtime, make_annotation_current_projected_scene_request(document, session, frame));
}

[[nodiscard]] inline bool projected_scene_matches_document_generation(
    const AnnotationProjectedScene* projected_scene, const std::uint64_t document_generation) noexcept {
    return projected_scene != nullptr && projected_scene->document_generation == document_generation;
}

[[nodiscard]] inline std::shared_ptr<const AnnotationProjectedScene> retain_projected_scene_for_document_generation(
    std::shared_ptr<const AnnotationProjectedScene> projected_scene, const std::uint64_t document_generation) noexcept {
    if (!projected_scene_matches_document_generation(projected_scene.get(), document_generation)) {
        return {};
    }
    return projected_scene;
}

[[nodiscard]] inline std::shared_ptr<const AnnotationProjectedScene>
resolve_annotation_projected_scene_for_document_generation(AnnotationWorkflowRuntime& runtime,
                                                           const AnnotationDocument& document,
                                                           const AnnotationSession& session,
                                                           const std::optional<AnnotationFrame>& frame,
                                                           const std::uint64_t document_generation) {
    return retain_projected_scene_for_document_generation(
        resolve_annotation_projected_scene(runtime, document, session, frame), document_generation);
}

struct AnnotationWorkflowResolvedObjectsResult {
    bool ok = false;
    std::vector<AnnotationResolvedObject> resolved_objects;
    std::shared_ptr<const AnnotationProjectedScene> projected_scene;
    std::string error_message;
};

struct AnnotationWorkflowPreparedPreview {
    AnnotationPreviewResult preview;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frame_id = 0;
};

[[nodiscard]] inline AnnotationWorkflowResolvedObjectsResult resolve_annotation_workflow_objects(
    AnnotationWorkflowRuntime& runtime, const AnnotationDocument& document, const AnnotationSession& session,
    const std::optional<AnnotationFrame>& frame, const AnnotationCategories& categories, const bool live_mode) {
    AnnotationWorkflowResolvedObjectsResult result;
    if (!frame.has_value()) {
        return result;
    }

    try {
        result.projected_scene = resolve_annotation_projected_scene(runtime, document, session, frame);
        result.resolved_objects =
            resolve_annotation_objects(*frame, categories, document.objects(), live_mode, result.projected_scene.get());
        result.ok = true;
    } catch (const std::exception& error) {
        result.error_message = error.what();
    }
    return result;
}

[[nodiscard]] inline AnnotationWorkflowResolvedObjectsResult resolve_annotation_workflow_objects(
    const AnnotationFrame& frame, const AnnotationDocument& document, const AnnotationCategories& categories,
    const std::shared_ptr<const AnnotationProjectedScene>& projected_scene, const bool live_mode) {
    AnnotationWorkflowResolvedObjectsResult result;
    if (projected_scene == nullptr) {
        return result;
    }

    try {
        result.projected_scene = projected_scene;
        result.resolved_objects =
            resolve_annotation_objects(frame, categories, document.objects(), live_mode, result.projected_scene.get());
        result.ok = true;
    } catch (const std::exception& error) {
        result.error_message = error.what();
    }
    return result;
}

[[nodiscard]] inline AnnotationWorkflowPreparedPreview prepare_annotation_workflow_preview(
    const AnnotationFrame& frame, const AnnotationCategories& categories,
    const AnnotationDocumentSnapshot& document_snapshot, const AnnotationProjectedScene* projected_scene) {
    AnnotationWorkflowPreparedPreview prepared;
    prepared.preview = build_annotation_preview(frame, categories, document_snapshot.objects, false, projected_scene);
    prepared.width = frame.width;
    prepared.height = frame.height;
    prepared.frame_id = frame.frame_id;
    return prepared;
}

inline void invalidate_annotation_projected_scene_cache(AnnotationWorkflowRuntime& runtime) noexcept {
    runtime.projected_scene_cache.invalidate();
}

inline void store_annotation_projected_scene(AnnotationWorkflowRuntime& runtime,
                                             std::shared_ptr<const AnnotationProjectedScene> scene,
                                             const AnnotationWorkflowProjectedSceneRequest& request) noexcept {
    runtime.projected_scene_cache.update(std::move(scene), AnnotationProjectedSceneCacheInputs{
                                                               request.document,
                                                               request.session,
                                                               request.frame,
                                                           });
}

inline void store_annotation_projected_scene(AnnotationWorkflowRuntime& runtime,
                                             std::shared_ptr<const AnnotationProjectedScene> scene,
                                             const AnnotationDocument& document, const AnnotationSession& session,
                                             const std::optional<AnnotationFrame>& frame) noexcept {
    store_annotation_projected_scene(runtime, std::move(scene),
                                     make_annotation_current_projected_scene_request(document, session, frame));
}

[[nodiscard]] inline bool annotation_workflow_should_reset_canvas_view(const AnnotationFrame* current_frame,
                                                                       const AnnotationFrame& next_frame) noexcept {
    return current_frame == nullptr || current_frame->width != next_frame.width ||
           current_frame->height != next_frame.height || current_frame->view_x != next_frame.view_x ||
           current_frame->view_y != next_frame.view_y;
}

inline void reset_annotation_workflow_runtime(AnnotationWorkflowRuntime& runtime) noexcept {
    runtime.projected_scene_cache.invalidate();
    runtime.save_controller = {};
    runtime.preview.running = false;
    runtime.preview.error_count = 0;
    runtime.preview.frame_id = 0;
    runtime.preview.dirty = true;
    ++runtime.preview.generation;
    runtime.live_overlay = {};
}

[[nodiscard]] inline bool annotation_workflow_preview_running(const AnnotationWorkflowRuntime& runtime) noexcept {
    return runtime.preview.running;
}

[[nodiscard]] inline bool annotation_workflow_preview_pending(const AnnotationWorkflowRuntime& runtime,
                                                              const AnnotationFrame* frame) noexcept {
    return frame != nullptr && (runtime.preview.dirty || runtime.preview.frame_id != frame->frame_id);
}

inline void reset_annotation_workflow_preview(AnnotationWorkflowRuntime& runtime, const bool dirty) noexcept {
    runtime.preview.running = false;
    runtime.preview.error_count = 0;
    if (dirty) {
        runtime.preview.frame_id = 0;
    }
    runtime.preview.dirty = dirty;
    ++runtime.preview.generation;
}

inline void invalidate_annotation_workflow_preview(AnnotationWorkflowRuntime& runtime) noexcept {
    runtime.preview.dirty = true;
    runtime.preview.error_count = 0;
    ++runtime.preview.generation;
}

[[nodiscard]] inline std::uint64_t begin_annotation_workflow_preview(AnnotationWorkflowRuntime& runtime) noexcept {
    runtime.preview.running = true;
    runtime.preview.dirty = false;
    return runtime.preview.generation;
}

inline void end_annotation_workflow_preview(AnnotationWorkflowRuntime& runtime) noexcept {
    runtime.preview.running = false;
    runtime.preview.error_count = 0;
}

inline void note_annotation_workflow_preview_ready(AnnotationWorkflowRuntime& runtime,
                                                   const AnnotationFrame& frame) noexcept {
    runtime.preview.running = false;
    runtime.preview.error_count = 0;
    runtime.preview.dirty = false;
    runtime.preview.frame_id = frame.frame_id;
}

inline void note_annotation_workflow_preview_error(AnnotationWorkflowRuntime& runtime) noexcept {
    runtime.preview.running = false;
    ++runtime.preview.error_count;
}

[[nodiscard]] inline bool annotation_workflow_preview_generation_matches(const AnnotationWorkflowRuntime& runtime,
                                                                         const std::uint64_t generation) noexcept {
    return runtime.preview.generation == generation;
}

[[nodiscard]] inline bool annotation_workflow_preview_can_retry(const AnnotationWorkflowRuntime& runtime,
                                                                const int max_retries) noexcept {
    return runtime.preview.error_count < max_retries;
}

}  // namespace mmltk::gui
