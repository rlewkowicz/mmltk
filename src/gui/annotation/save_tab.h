#pragma once

#include "gui/annotation/common.h"
#include "gui/annotation/document/types.h"
#include "gui/annotation/render/renderer.h"
#include "gui/live_session_utils.h"
#include "gui/source_selection.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ImFont;

namespace mmltk::gui {

struct AnnotationProjectedScene;

struct AnnotationSaveNowPlan {
    bool enabled = false;
    bool requires_live_frame_refresh = false;
    bool should_advance_image_folder = false;
};

struct AnnotationHoldSavePlan {
    bool enabled = false;
    bool blocked = false;
    bool should_clear_queue = false;
};

struct AnnotationLiveSavePlan {
    bool enabled = false;
    bool requires_live_frame_pixels = false;
};

struct AnnotationSaveTabPlan {
    AnnotationSaveNowPlan save_now;
    AnnotationHoldSavePlan hold_save;
    AnnotationLiveSavePlan live_save;
};

struct AnnotationSaveTabUiState {
    std::string* output_root = nullptr;
    std::string* split = nullptr;
    std::size_t loaded_class_count = 0;
    bool live_video = false;
    bool hold_save_active = false;
    bool hold_save_blocked = false;
    ImFont* compact_font = nullptr;
};

struct AnnotationSaveTabUiActions {
    bool request_save_now = false;
    bool hold_save_active = false;
    bool hold_save_blocked = false;
};

struct AnnotationQueuedSave {
    AnnotationFrame frame;
    std::vector<AnnotationObject> objects;
    std::shared_ptr<const AnnotationProjectedScene> projected_scene;
};

struct AnnotationSaveControllerState {
    bool hold_save = false;
    bool hold_save_blocked = false;
    std::uint64_t last_saved_frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> last_saved_live_frame_id;
    std::optional<AnnotationQueuedSave> queued_save;
};

struct AnnotationSaveSnapshot {
    AnnotationFrame frame;
    std::vector<AnnotationObject> objects;
    std::shared_ptr<const AnnotationProjectedScene> projected_scene;
};

struct AnnotationSaveTabState {
    SourceKind source_kind = SourceKind::CompiledDataset;
    bool annotation_live_running = false;
    bool save_running = false;
    bool hold_save = false;
    bool hold_save_blocked = false;
    bool has_annotation_frame = false;
    bool has_resolved_instances = false;
    std::size_t current_input_index = 0;
    std::size_t input_count = 0;
};

[[nodiscard]] inline AnnotationSaveTabPlan plan_annotation_save_tab(
    const AnnotationSaveTabState& state) noexcept {
    const bool live_video = state.source_kind == SourceKind::VideoStream;
    const bool live_save_active = live_video && state.annotation_live_running;

    AnnotationSaveTabPlan plan;
    plan.save_now.enabled =
        state.has_annotation_frame && state.has_resolved_instances && !state.save_running;
    plan.save_now.requires_live_frame_refresh = live_save_active;
    plan.save_now.should_advance_image_folder =
        plan.save_now.enabled &&
        state.source_kind == SourceKind::ImageFolder &&
        state.input_count > 0U &&
        state.current_input_index + 1U < state.input_count;

    plan.hold_save.enabled = live_save_active;
    plan.hold_save.blocked = state.hold_save_blocked;
    plan.hold_save.should_clear_queue = !live_video || !state.hold_save;

    plan.live_save.enabled = live_save_active && state.has_annotation_frame;
    plan.live_save.requires_live_frame_pixels = live_save_active;
    return plan;
}

[[nodiscard]] inline bool has_queued_annotation_save(
    const AnnotationSaveControllerState& state) noexcept {
    return state.queued_save.has_value();
}

inline void clear_annotation_save_queue(AnnotationSaveControllerState* state) noexcept {
    if (state == nullptr) {
        return;
    }
    state->queued_save.reset();
}

inline void reset_annotation_save_progress(AnnotationSaveControllerState* state) noexcept {
    if (state == nullptr) {
        return;
    }
    state->hold_save_blocked = false;
    state->last_saved_frame_id = 0;
    state->last_saved_live_frame_id.reset();
    clear_annotation_save_queue(state);
}

inline void stop_annotation_hold_save(AnnotationSaveControllerState* state) noexcept {
    if (state == nullptr) {
        return;
    }
    state->hold_save = false;
    state->hold_save_blocked = false;
    clear_annotation_save_queue(state);
}

inline void note_annotation_save_started(AnnotationSaveControllerState* state,
                                         const AnnotationFrame& frame) noexcept {
    if (state == nullptr) {
        return;
    }
    state->last_saved_frame_id = frame.frame_id;
    state->last_saved_live_frame_id = frame.live_frame_id;
}

inline void queue_annotation_save(AnnotationSaveControllerState* state,
                                  AnnotationFrame frame,
                                  std::vector<AnnotationObject> objects,
                                  std::shared_ptr<const AnnotationProjectedScene> projected_scene = {}) {
    if (state == nullptr) {
        return;
    }
    state->queued_save = AnnotationQueuedSave{
        std::move(frame),
        std::move(objects),
        std::move(projected_scene),
    };
}

[[nodiscard]] inline std::optional<AnnotationQueuedSave> take_queued_annotation_save(
    AnnotationSaveControllerState* state) {
    if (state == nullptr || !state->queued_save.has_value()) {
        return std::nullopt;
    }
    std::optional<AnnotationQueuedSave> queued_save = std::move(state->queued_save);
    state->queued_save.reset();
    return queued_save;
}

struct AnnotationSaveTabUiCommitResult {
    bool clear_queue = false;
};

[[nodiscard]] inline AnnotationSaveTabUiCommitResult apply_annotation_save_tab_ui_actions(
    AnnotationSaveControllerState* controller_state,
    const bool live_video,
    const AnnotationSaveTabUiActions& actions) noexcept {
    AnnotationSaveTabUiCommitResult result;
    if (controller_state == nullptr) {
        return result;
    }

    if (!live_video) {
        result.clear_queue = has_queued_annotation_save(*controller_state);
        stop_annotation_hold_save(controller_state);
        return result;
    }

    controller_state->hold_save = actions.hold_save_active;
    controller_state->hold_save_blocked = actions.hold_save_blocked;
    return result;
}

struct AnnotationQueuedSaveDispatchPlan {
    bool clear_queue = false;
    bool dispatch_queued_save = false;
};

struct AnnotationSavePipelineState {
    AnnotationSaveTabPlan tab_plan;
    bool save_running = false;
    bool has_current_frame = false;
    bool current_frame_matches_last_saved = false;
    bool current_frame_matches_queued = false;
};

[[nodiscard]] inline AnnotationSavePipelineState make_annotation_save_pipeline_state(
    const AnnotationSaveTabPlan& tab_plan,
    const AnnotationSaveControllerState& controller_state,
    const AnnotationFrame* current_frame,
    const bool save_running) noexcept {
    const bool has_current_frame = current_frame != nullptr;
    const bool current_frame_matches_last_saved =
        has_current_frame &&
        annotation_frame_matches_saved_identity(*current_frame,
                                                controller_state.last_saved_frame_id,
                                                controller_state.last_saved_live_frame_id);
    const bool current_frame_matches_queued =
        has_current_frame &&
        controller_state.queued_save.has_value() &&
        annotation_frames_match_for_save(controller_state.queued_save->frame, *current_frame);
    return AnnotationSavePipelineState{
        tab_plan,
        save_running,
        has_current_frame,
        current_frame_matches_last_saved,
        current_frame_matches_queued,
    };
}

[[nodiscard]] inline AnnotationQueuedSaveDispatchPlan plan_annotation_queued_save_dispatch(
    const AnnotationSaveControllerState& controller_state,
    const bool save_running) noexcept {
    AnnotationQueuedSaveDispatchPlan plan;
    if (save_running || !controller_state.queued_save.has_value()) {
        return plan;
    }

    plan.dispatch_queued_save = controller_state.hold_save;
    plan.clear_queue = !controller_state.hold_save;
    return plan;
}

struct AnnotationHoldSaveDispatchState {
    AnnotationSaveTabPlan tab_plan;
    bool save_running = false;
    bool has_current_frame = false;
    bool current_frame_matches_last_saved = false;
    bool current_frame_matches_queued = false;
};

struct AnnotationHoldSaveDispatchPlan {
    bool clear_queue = false;
    bool refresh_live_frame = false;
    bool enqueue_current_frame = false;
    bool dispatch_current_frame_save = false;
    bool block_hold_save = false;
};

struct AnnotationSavePipelinePlan {
    AnnotationQueuedSaveDispatchPlan queued_save;
    AnnotationHoldSaveDispatchPlan hold_save;
};

struct AnnotationSavePipelineRequest {
    AnnotationSaveTabPlan tab_plan;
    const AnnotationSaveControllerState* controller_state = nullptr;
    const AnnotationFrame* current_frame = nullptr;
    bool save_running = false;
};

struct AnnotationSaveSnapshotRequest {
    bool refresh_live_frame = false;
    const AnnotationFrame* frame = nullptr;
    std::uint64_t document_generation = 0;
    const std::vector<AnnotationObject>* document_objects = nullptr;
    bool has_resolved_instances = false;
    std::string* error_message = nullptr;
    const char* resolve_error_context = "annotate save resolve error";
};

[[nodiscard]] inline AnnotationHoldSaveDispatchPlan plan_annotation_hold_save_dispatch(
    const AnnotationSaveControllerState& controller_state,
    const AnnotationHoldSaveDispatchState& state) noexcept {
    AnnotationHoldSaveDispatchPlan plan;

    if (state.tab_plan.hold_save.should_clear_queue || !state.tab_plan.hold_save.enabled) {
        plan.clear_queue = true;
        return plan;
    }

    if (!controller_state.hold_save ||
        !state.tab_plan.live_save.enabled ||
        !state.has_current_frame ||
        state.current_frame_matches_last_saved) {
        return plan;
    }

    if (state.save_running) {
        if (!controller_state.queued_save.has_value()) {
            plan.refresh_live_frame = true;
            plan.enqueue_current_frame = true;
        } else if (!state.current_frame_matches_queued) {
            plan.clear_queue = true;
            plan.block_hold_save = true;
        }
        return plan;
    }

    plan.refresh_live_frame = true;
    plan.dispatch_current_frame_save = true;
    return plan;
}

[[nodiscard]] inline AnnotationSavePipelinePlan plan_annotation_save_pipeline(
    const AnnotationSaveControllerState& controller_state,
    const AnnotationSavePipelineState& state) noexcept {
    return AnnotationSavePipelinePlan{
        plan_annotation_queued_save_dispatch(controller_state, state.save_running),
        plan_annotation_hold_save_dispatch(
            controller_state,
            AnnotationHoldSaveDispatchState{
                state.tab_plan,
                state.save_running,
                state.has_current_frame,
                state.current_frame_matches_last_saved,
                state.current_frame_matches_queued,
        }),
    };
}

[[nodiscard]] inline AnnotationSavePipelineState make_annotation_save_pipeline_state(
    const AnnotationSavePipelineRequest& request) noexcept {
    if (request.controller_state == nullptr) {
        return AnnotationSavePipelineState{
            request.tab_plan,
            request.save_running,
            request.current_frame != nullptr,
            false,
            false,
        };
    }
    return make_annotation_save_pipeline_state(request.tab_plan,
                                               *request.controller_state,
                                               request.current_frame,
                                               request.save_running);
}

[[nodiscard]] inline AnnotationSavePipelinePlan plan_annotation_save_pipeline(
    const AnnotationSavePipelineRequest& request) noexcept {
    if (request.controller_state == nullptr) {
        return {};
    }
    return plan_annotation_save_pipeline(*request.controller_state,
                                         make_annotation_save_pipeline_state(request));
}

template <typename EnsureLiveFramePixelsFn,
          typename ResolveCurrentObjectsFn,
          typename CurrentProjectedSceneFn>
[[nodiscard]] inline bool prepare_annotation_save_snapshot(
    const AnnotationSaveSnapshotRequest& request,
    EnsureLiveFramePixelsFn&& ensure_live_frame_pixels,
    ResolveCurrentObjectsFn&& resolve_current_annotation_objects,
    CurrentProjectedSceneFn&& current_projected_scene,
    AnnotationSaveSnapshot* snapshot) {
    if (snapshot == nullptr || request.frame == nullptr || !request.has_resolved_instances) {
        return false;
    }

    std::shared_ptr<const AnnotationProjectedScene> projected_scene;
    if (request.refresh_live_frame) {
        if (!std::forward<EnsureLiveFramePixelsFn>(ensure_live_frame_pixels)(request.error_message)) {
            return false;
        }
        if (!std::forward<ResolveCurrentObjectsFn>(resolve_current_annotation_objects)(
                true,
                request.resolve_error_context,
                false,
                &projected_scene)) {
            return false;
        }
    } else {
        try {
            projected_scene = std::forward<CurrentProjectedSceneFn>(current_projected_scene)();
        } catch (const std::exception&) {
            projected_scene.reset();
        }
    }

    if (projected_scene != nullptr &&
        projected_scene->document_generation != request.document_generation) {
        projected_scene.reset();
    }

    snapshot->frame = *request.frame;
    snapshot->objects = request.document_objects != nullptr ? *request.document_objects
                                                           : std::vector<AnnotationObject>{};
    snapshot->projected_scene = std::move(projected_scene);
    if (request.error_message != nullptr) {
        request.error_message->clear();
    }
    return true;
}

template <typename EnsureLiveFramePixelsFn,
          typename ResolveCurrentObjectsFn,
          typename CurrentProjectedSceneFn>
[[nodiscard]] inline bool prepare_annotation_save_snapshot(
    const bool refresh_live_frame,
    EnsureLiveFramePixelsFn&& ensure_live_frame_pixels,
    ResolveCurrentObjectsFn&& resolve_current_annotation_objects,
    CurrentProjectedSceneFn&& current_projected_scene,
    const AnnotationFrame* frame,
    const std::uint64_t document_generation,
    const std::vector<AnnotationObject>& document_objects,
    const bool has_resolved_instances,
    AnnotationSaveSnapshot* snapshot,
    std::string* error_message,
    const char* resolve_error_context = "annotate save resolve error") {
    return prepare_annotation_save_snapshot(
        AnnotationSaveSnapshotRequest{
            refresh_live_frame,
            frame,
            document_generation,
            &document_objects,
            has_resolved_instances,
            error_message,
            resolve_error_context,
        },
        std::forward<EnsureLiveFramePixelsFn>(ensure_live_frame_pixels),
        std::forward<ResolveCurrentObjectsFn>(resolve_current_annotation_objects),
        std::forward<CurrentProjectedSceneFn>(current_projected_scene),
        snapshot);
}

template <typename EnsureLiveFramePixelsFn,
          typename ResolveCurrentObjectsFn,
          typename CurrentProjectedSceneFn>
[[nodiscard]] inline std::optional<AnnotationSaveSnapshot> make_annotation_save_snapshot(
    const AnnotationSaveSnapshotRequest& request,
    EnsureLiveFramePixelsFn&& ensure_live_frame_pixels,
    ResolveCurrentObjectsFn&& resolve_current_annotation_objects,
    CurrentProjectedSceneFn&& current_projected_scene) {
    AnnotationSaveSnapshot snapshot;
    if (!prepare_annotation_save_snapshot(request,
                                          std::forward<EnsureLiveFramePixelsFn>(ensure_live_frame_pixels),
                                          std::forward<ResolveCurrentObjectsFn>(resolve_current_annotation_objects),
                                          std::forward<CurrentProjectedSceneFn>(current_projected_scene),
                                          &snapshot)) {
        return std::nullopt;
    }
    return std::make_optional<AnnotationSaveSnapshot>(std::move(snapshot));
}

template <typename EnsureLiveFramePixelsFn,
          typename ResolveCurrentObjectsFn,
          typename CurrentProjectedSceneFn>
[[nodiscard]] inline std::optional<AnnotationSaveSnapshot> make_annotation_save_snapshot(
    const bool refresh_live_frame,
    EnsureLiveFramePixelsFn&& ensure_live_frame_pixels,
    ResolveCurrentObjectsFn&& resolve_current_annotation_objects,
    CurrentProjectedSceneFn&& current_projected_scene,
    const AnnotationFrame* frame,
    const std::uint64_t document_generation,
    const std::vector<AnnotationObject>& document_objects,
    const bool has_resolved_instances,
    std::string* error_message,
    const char* resolve_error_context = "annotate save resolve error") {
    return make_annotation_save_snapshot(
        AnnotationSaveSnapshotRequest{
            refresh_live_frame,
            frame,
            document_generation,
            &document_objects,
            has_resolved_instances,
            error_message,
            resolve_error_context,
        },
        std::forward<EnsureLiveFramePixelsFn>(ensure_live_frame_pixels),
        std::forward<ResolveCurrentObjectsFn>(resolve_current_annotation_objects),
        std::forward<CurrentProjectedSceneFn>(current_projected_scene));
}

template <typename PrepareSnapshotFn,
          typename QueueSnapshotFn,
          typename LaunchSnapshotFn,
          typename ClearQueueFn,
          typename TakeQueuedSaveFn,
          typename LaunchQueuedSaveFn,
          typename BlockHoldSaveFn>
inline void drive_annotation_save_pipeline(
    const AnnotationSavePipelinePlan& pipeline_plan,
    PrepareSnapshotFn&& prepare_snapshot,
    QueueSnapshotFn&& queue_snapshot,
    LaunchSnapshotFn&& launch_snapshot,
    ClearQueueFn&& clear_queue,
    TakeQueuedSaveFn&& take_queued_save,
    LaunchQueuedSaveFn&& launch_queued_save,
    BlockHoldSaveFn&& block_hold_save) {
    if (pipeline_plan.queued_save.clear_queue) {
        std::forward<ClearQueueFn>(clear_queue)();
    }

    if (pipeline_plan.queued_save.dispatch_queued_save) {
        std::optional<AnnotationQueuedSave> queued_save =
            std::forward<TakeQueuedSaveFn>(take_queued_save)();
        if (queued_save.has_value() && !queued_save->objects.empty()) {
            std::forward<LaunchQueuedSaveFn>(launch_queued_save)(std::move(*queued_save));
        }
    }

    if (pipeline_plan.hold_save.block_hold_save) {
        std::forward<BlockHoldSaveFn>(block_hold_save)();
        return;
    }

    if (!pipeline_plan.hold_save.refresh_live_frame) {
        return;
    }

    std::optional<AnnotationSaveSnapshot> save_snapshot =
        std::forward<PrepareSnapshotFn>(prepare_snapshot)();
    if (!save_snapshot.has_value()) {
        return;
    }

    if (pipeline_plan.hold_save.enqueue_current_frame) {
        std::forward<QueueSnapshotFn>(queue_snapshot)(std::move(*save_snapshot));
        return;
    }

    if (pipeline_plan.hold_save.dispatch_current_frame_save) {
        std::forward<LaunchSnapshotFn>(launch_snapshot)(std::move(*save_snapshot));
    }
}

[[nodiscard]] inline bool annotation_save_now_ready(const AnnotationSaveTabPlan& plan) noexcept {
    return plan.save_now.enabled;
}

[[nodiscard]] inline bool annotation_hold_save_ready(const AnnotationSaveTabPlan& plan) noexcept {
    return plan.hold_save.enabled;
}

[[nodiscard]] AnnotationSaveTabUiActions draw_annotation_save_tab_ui(
    AnnotationSaveTabUiState state,
    const AnnotationSaveTabPlan& plan);

} // namespace mmltk::gui
