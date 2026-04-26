#pragma once

#include "gui/annotation/workflow_runtime.h"
#include "gui/rfdetr_workflows.h"
#include "mmltk/runtime/async_runtime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mmltk::gui {

inline void reset_annotation_workflow_save_progress(AnnotationWorkflowRuntime& runtime) noexcept {
    reset_annotation_save_progress(&runtime.save_controller);
}

inline void stop_annotation_workflow_hold_save(AnnotationWorkflowRuntime& runtime) noexcept {
    stop_annotation_hold_save(&runtime.save_controller);
}

inline void mark_annotation_hold_save_overflow(AnnotationWorkflowRuntime& runtime) noexcept {
    runtime.save_controller.hold_save = false;
    runtime.save_controller.hold_save_blocked = true;
}

inline void clear_annotation_workflow_save_queue(AnnotationWorkflowRuntime& runtime) noexcept {
    clear_annotation_save_queue(&runtime.save_controller);
}

inline void note_annotation_workflow_save_started(AnnotationWorkflowRuntime& runtime,
                                                  const AnnotationFrame& frame) noexcept {
    note_annotation_save_started(&runtime.save_controller, frame);
}

inline void queue_annotation_workflow_save(AnnotationWorkflowRuntime& runtime, AnnotationFrame frame,
                                           std::vector<AnnotationObject> objects,
                                           std::shared_ptr<const AnnotationProjectedScene> projected_scene = {}) {
    queue_annotation_save(&runtime.save_controller, std::move(frame), std::move(objects), std::move(projected_scene));
}

[[nodiscard]] inline std::optional<AnnotationQueuedSave> take_annotation_workflow_queued_save(
    AnnotationWorkflowRuntime& runtime) {
    return take_queued_annotation_save(&runtime.save_controller);
}

[[nodiscard]] inline AnnotationSaveTabUiCommitResult apply_annotation_workflow_save_tab_ui_actions(
    AnnotationWorkflowRuntime& runtime, const bool live_video, const AnnotationSaveTabUiActions& actions) noexcept {
    return apply_annotation_save_tab_ui_actions(&runtime.save_controller, live_video, actions);
}

[[nodiscard]] inline AnnotationSavePipelinePlan plan_annotation_workflow_save_pipeline(
    const AnnotationWorkflowRuntime& runtime, const AnnotationSaveTabPlan& tab_plan,
    const AnnotationFrame* current_frame, const bool save_running) noexcept {
    return plan_annotation_save_pipeline(AnnotationSavePipelineRequest{
        tab_plan,
        &runtime.save_controller,
        current_frame,
        save_running,
    });
}

struct AnnotationWorkflowSaveRequest {
    const AnnotationWorkflowRuntime* runtime = nullptr;
    SourceKind source_kind = SourceKind::CompiledDataset;
    bool annotation_live_running = false;
    bool save_running = false;
    const AnnotationFrame* frame = nullptr;
    bool has_resolved_instances = false;
    std::size_t current_input_index = 0;
    std::size_t input_count = 0;
};

[[nodiscard]] inline AnnotationWorkflowSaveRequest make_annotation_workflow_save_request(
    const AnnotationWorkflowRuntime* runtime, const SourceKind source_kind, const bool annotation_live_running,
    const bool save_running, const AnnotationFrame* frame, const bool has_resolved_instances,
    const std::size_t current_input_index, const std::size_t input_count) noexcept {
    return AnnotationWorkflowSaveRequest{
        runtime,
        source_kind,
        annotation_live_running,
        save_running,
        frame,
        has_resolved_instances,
        current_input_index,
        input_count,
    };
}

[[nodiscard]] inline AnnotationWorkflowSaveRequest make_annotation_workflow_save_request(
    const AnnotationWorkflowRuntime* runtime, const AnnotateViewState& annotate, const bool annotation_live_running,
    const bool save_running, const AnnotationFrame* frame, const bool has_resolved_instances,
    const std::size_t current_input_index, const std::size_t input_count) noexcept {
    return make_annotation_workflow_save_request(runtime, annotate.source.kind, annotation_live_running, save_running,
                                                 frame, has_resolved_instances, current_input_index, input_count);
}

struct AnnotationWorkflowSaveInputs {
    SourceKind source_kind = SourceKind::CompiledDataset;
    bool annotation_live_running = false;
    bool save_running = false;
    bool hold_save = false;
    bool hold_save_blocked = false;
    const AnnotationFrame* frame = nullptr;
    bool has_resolved_instances = false;
    std::size_t current_input_index = 0;
    std::size_t input_count = 0;
};

[[nodiscard]] inline AnnotationWorkflowSaveInputs make_annotation_workflow_save_inputs(
    const AnnotationWorkflowSaveRequest& request) noexcept {
    const AnnotationSaveControllerState* save_controller =
        request.runtime != nullptr ? std::addressof(request.runtime->save_controller) : nullptr;
    return AnnotationWorkflowSaveInputs{
        request.source_kind,
        request.annotation_live_running,
        request.save_running,
        save_controller != nullptr ? save_controller->hold_save : false,
        save_controller != nullptr ? save_controller->hold_save_blocked : false,
        request.frame,
        request.has_resolved_instances,
        request.current_input_index,
        request.input_count,
    };
}

[[nodiscard]] inline AnnotationSaveTabState make_annotation_save_tab_state(
    const AnnotationWorkflowSaveInputs& inputs) noexcept {
    return AnnotationSaveTabState{
        inputs.source_kind,
        inputs.annotation_live_running,
        inputs.save_running,
        inputs.hold_save,
        inputs.hold_save_blocked,
        inputs.frame != nullptr,
        inputs.has_resolved_instances,
        inputs.current_input_index,
        inputs.input_count,
    };
}

[[nodiscard]] inline AnnotationSaveTabPlan make_annotation_save_tab_plan(
    const AnnotationWorkflowSaveInputs& inputs) noexcept {
    return plan_annotation_save_tab(make_annotation_save_tab_state(inputs));
}

struct AnnotationWorkflowPollPlan {
    AnnotationWorkflowSaveInputs save_inputs;
    AnnotationSaveTabPlan save_tab_plan;
    AnnotationSavePipelinePlan pipeline_plan;
};

[[nodiscard]] inline AnnotationWorkflowPollPlan plan_annotation_workflow_poll(
    const AnnotationWorkflowSaveRequest& request) noexcept {
    const AnnotationWorkflowSaveInputs save_inputs = make_annotation_workflow_save_inputs(request);
    const AnnotationSaveTabPlan save_tab_plan = make_annotation_save_tab_plan(save_inputs);
    return AnnotationWorkflowPollPlan{
        save_inputs,
        save_tab_plan,
        plan_annotation_save_pipeline(AnnotationSavePipelineRequest{
            save_tab_plan,
            request.runtime != nullptr ? std::addressof(request.runtime->save_controller) : nullptr,
            request.frame,
            request.save_running,
        }),
    };
}

struct AnnotationWorkflowSaveNowDecision {
    bool allowed = false;
    bool refresh_live_frame = false;
    bool live_mode = false;
    bool advance_after_save = false;
};

struct AnnotationWorkflowSaveLaunchState {
    bool* running = nullptr;
    std::string* summary = nullptr;
    std::string* error = nullptr;
    bool* categories_loaded = nullptr;
    std::string* categories_output_dir = nullptr;
};

struct AnnotationBackgroundSaveRequest {
    AnnotationSaveConfig config;
    AnnotationCategories categories;
    AnnotationSaveSnapshot snapshot;
    bool live_mode = false;
};

struct AnnotationBackgroundSaveRequestInputs {
    std::string output_dir;
    std::string split;
    AnnotationCategories categories;
    AnnotationSaveSnapshot snapshot;
    bool live_mode = false;
};

struct AnnotationBackgroundSaveOutcome {
    AnnotationSaveResult save;
    AnnotationCategories categories;
    std::string summary;
};

[[nodiscard]] inline std::optional<AnnotationBackgroundSaveRequest> make_annotation_background_save_request(
    AnnotationBackgroundSaveRequestInputs inputs, std::string* error_message = nullptr);

[[nodiscard]] inline AnnotationBackgroundSaveOutcome run_annotation_background_save(
    AnnotationBackgroundSaveRequest request);

[[nodiscard]] inline AnnotationWorkflowSaveNowDecision decide_annotation_save_now(
    const AnnotationSaveTabPlan& plan) noexcept {
    return AnnotationWorkflowSaveNowDecision{
        plan.save_now.enabled,
        plan.save_now.requires_live_frame_refresh,
        plan.live_save.enabled,
        plan.save_now.should_advance_image_folder,
    };
}

template <typename PrepareSaveSnapshotFn, typename LaunchSaveFn, typename AdvanceAfterSaveFn>
[[nodiscard]] inline bool dispatch_annotation_workflow_save_now(const AnnotationSaveTabPlan& plan,
                                                                PrepareSaveSnapshotFn&& prepare_save_snapshot,
                                                                LaunchSaveFn&& launch_save,
                                                                AdvanceAfterSaveFn&& advance_after_save) {
    const AnnotationWorkflowSaveNowDecision decision = decide_annotation_save_now(plan);
    if (!decision.allowed) {
        return false;
    }

    std::optional<AnnotationSaveSnapshot> save_snapshot =
        std::forward<PrepareSaveSnapshotFn>(prepare_save_snapshot)(decision.refresh_live_frame);
    if (!save_snapshot.has_value()) {
        return false;
    }
    if (!std::forward<LaunchSaveFn>(launch_save)(std::move(*save_snapshot), decision.live_mode)) {
        return false;
    }
    if (decision.advance_after_save) {
        std::forward<AdvanceAfterSaveFn>(advance_after_save)();
    }
    return true;
}

template <typename LogErrorFn>
[[nodiscard]] inline bool launch_annotation_workflow_save(AnnotationWorkflowRuntime& runtime,
                                                          AnnotateViewState& annotate, AnnotationCategories& categories,
                                                          AnnotationSaveSnapshot save_snapshot, const bool live_mode,
                                                          AnnotationWorkflowSaveLaunchState state,
                                                          mmltk::runtime::BackgroundExecutor& background_executor,
                                                          mmltk::runtime::UiCallbackQueue& ui_callbacks,
                                                          LogErrorFn&& log_error) {
    if (state.running != nullptr && *state.running) {
        return false;
    }
    if (state.error != nullptr) {
        state.error->clear();
    }
    if (state.summary != nullptr) {
        state.summary->clear();
    }

    std::optional<AnnotationBackgroundSaveRequest> save_request = make_annotation_background_save_request(
        AnnotationBackgroundSaveRequestInputs{
            annotate.output_dir,
            annotate.split,
            categories,
            std::move(save_snapshot),
            live_mode,
        },
        state.error);
    if (!save_request.has_value()) {
        if (state.running != nullptr) {
            *state.running = false;
        }
        return false;
    }

    if (state.running != nullptr) {
        *state.running = true;
    }

    note_annotation_workflow_save_started(runtime, save_request->snapshot.frame);
    mmltk::runtime::submit_background_task(
        background_executor, ui_callbacks,
        [save_request = std::move(*save_request)]() mutable {
            return run_annotation_background_save(std::move(save_request));
        },
        [state, categories_ptr = std::addressof(categories),
         annotate_output_dir = annotate.output_dir](AnnotationBackgroundSaveOutcome outcome) mutable {
            if (categories_ptr != nullptr) {
                *categories_ptr = std::move(outcome.categories);
            }
            if (state.categories_loaded != nullptr) {
                *state.categories_loaded = true;
            }
            if (state.categories_output_dir != nullptr) {
                *state.categories_output_dir = annotate_output_dir;
            }
            if (state.summary != nullptr) {
                *state.summary = std::move(outcome.summary);
            }
            if (state.error != nullptr) {
                state.error->clear();
            }
            if (state.running != nullptr) {
                *state.running = false;
            }
        },
        [state, log_error = std::forward<LogErrorFn>(log_error)](const std::string& error) mutable {
            if (state.error != nullptr) {
                *state.error = error;
            }
            if (state.summary != nullptr) {
                state.summary->clear();
            }
            if (state.running != nullptr) {
                *state.running = false;
            }
            log_error(error);
        });
    return true;
}

using AnnotationWorkflowSaveSnapshotRequest = AnnotationSaveSnapshotRequest;

[[nodiscard]] inline AnnotationWorkflowSaveSnapshotRequest make_annotation_workflow_save_snapshot_request(
    const bool refresh_live_frame, const AnnotationFrame* frame, const std::uint64_t document_generation,
    const std::vector<AnnotationObject>* document_objects, const bool has_resolved_instances,
    std::string* error_message = nullptr, const char* resolve_error_context = "annotate save resolve error") noexcept {
    return AnnotationWorkflowSaveSnapshotRequest{
        refresh_live_frame,     frame,         document_generation,   document_objects,
        has_resolved_instances, error_message, resolve_error_context,
    };
}

struct AnnotationWorkflowCurrentSaveSnapshotRequest {
    AnnotationWorkflowRuntime* runtime = nullptr;
    AnnotationDocument* document = nullptr;
    AnnotationSession* session = nullptr;
    const std::optional<AnnotationFrame>* frame = nullptr;
    const AnnotationCategories* categories = nullptr;
    std::vector<AnnotationResolvedObject>* resolved_objects = nullptr;
    bool refresh_live_frame = false;
    bool clear_resolved_objects_on_error = false;
    std::string* error_message = nullptr;
    const char* resolve_error_context = "annotate save resolve error";
};

template <typename EnsureLiveFramePixelsFn, typename LogResolveErrorFn>
[[nodiscard]] inline std::optional<AnnotationSaveSnapshot> make_annotation_workflow_current_save_snapshot(
    const AnnotationWorkflowCurrentSaveSnapshotRequest& request, EnsureLiveFramePixelsFn&& ensure_live_frame_pixels,
    LogResolveErrorFn&& log_resolve_error) {
    if (request.runtime == nullptr || request.document == nullptr || request.session == nullptr ||
        request.frame == nullptr || request.categories == nullptr || request.resolved_objects == nullptr) {
        return std::nullopt;
    }

    const AnnotationFrame* current_frame = annotation_frame_ptr(*request.frame);
    if (current_frame == nullptr || request.resolved_objects->empty()) {
        return std::nullopt;
    }

    const AnnotationDocumentSnapshot document_snapshot = request.document->snapshot();
    return make_annotation_workflow_save_snapshot(
        make_annotation_workflow_save_snapshot_request(
            request.refresh_live_frame, current_frame, document_snapshot.generation, &document_snapshot.objects,
            !request.resolved_objects->empty(), request.error_message, request.resolve_error_context),
        std::forward<EnsureLiveFramePixelsFn>(ensure_live_frame_pixels),
        [&request, &log_resolve_error](const bool live_mode, const char* error_context,
                                       const bool clear_resolved_instances_on_error,
                                       std::shared_ptr<const AnnotationProjectedScene>* projected_scene_snapshot) {
            if (projected_scene_snapshot != nullptr) {
                projected_scene_snapshot->reset();
            }

            AnnotationWorkflowResolvedObjectsResult resolved = resolve_annotation_workflow_objects(
                *request.runtime, *request.document, *request.session, *request.frame, *request.categories, live_mode);
            if (!resolved.ok) {
                if (clear_resolved_instances_on_error || request.clear_resolved_objects_on_error) {
                    request.resolved_objects->clear();
                }
                if (request.error_message != nullptr) {
                    *request.error_message = resolved.error_message;
                }
                std::forward<LogResolveErrorFn>(log_resolve_error)(
                    error_context != nullptr ? error_context : request.resolve_error_context, resolved.error_message);
                return false;
            }

            *request.resolved_objects = std::move(resolved.resolved_objects);
            if (projected_scene_snapshot != nullptr) {
                *projected_scene_snapshot = std::move(resolved.projected_scene);
            }
            if (request.error_message != nullptr) {
                request.error_message->clear();
            }
            return true;
        },
        [&request, document_generation = document_snapshot.generation]() {
            return resolve_annotation_projected_scene_for_document_generation(
                *request.runtime, *request.document, *request.session, *request.frame, document_generation);
        });
}

template <typename EnsureLiveFramePixelsFn, typename ResolveCurrentObjectsFn, typename CurrentProjectedSceneFn>
[[nodiscard]] inline std::optional<AnnotationSaveSnapshot> make_annotation_workflow_save_snapshot(
    const AnnotationWorkflowSaveSnapshotRequest& request, EnsureLiveFramePixelsFn&& ensure_live_frame_pixels,
    ResolveCurrentObjectsFn&& resolve_current_annotation_objects, CurrentProjectedSceneFn&& current_projected_scene) {
    if (request.frame == nullptr || request.document_objects == nullptr || !request.has_resolved_instances) {
        return std::nullopt;
    }

    return make_annotation_save_snapshot(
        AnnotationSaveSnapshotRequest{
            request.refresh_live_frame,
            request.frame,
            request.document_generation,
            request.document_objects,
            request.has_resolved_instances,
            request.error_message,
            request.resolve_error_context,
        },
        std::forward<EnsureLiveFramePixelsFn>(ensure_live_frame_pixels),
        std::forward<ResolveCurrentObjectsFn>(resolve_current_annotation_objects),
        std::forward<CurrentProjectedSceneFn>(current_projected_scene));
}

[[nodiscard]] inline std::optional<AnnotationBackgroundSaveRequest> make_annotation_background_save_request(
    AnnotationBackgroundSaveRequestInputs inputs, std::string* error_message) {
    if (inputs.output_dir.empty()) {
        if (error_message != nullptr) {
            *error_message = "annotation output root must not be empty";
        }
        return std::nullopt;
    }
    if (error_message != nullptr) {
        error_message->clear();
    }

    return std::make_optional<AnnotationBackgroundSaveRequest>(AnnotationBackgroundSaveRequest{
        AnnotationSaveConfig{
            std::move(inputs.output_dir),
            inputs.split.empty() ? std::string("train") : std::move(inputs.split),
        },
        std::move(inputs.categories),
        std::move(inputs.snapshot),
        inputs.live_mode,
    });
}

[[nodiscard]] inline AnnotationBackgroundSaveOutcome run_annotation_background_save(
    AnnotationBackgroundSaveRequest request) {
    AnnotationBackgroundSaveOutcome outcome;
    outcome.categories = std::move(request.categories);
    outcome.save =
        save_annotation_scene(request.config, request.snapshot.frame, outcome.categories, request.snapshot.objects,
                              request.live_mode, request.snapshot.projected_scene.get());
    outcome.summary = rfdetr_workflows::summarize_annotation_save_result(outcome.save);
    return outcome;
}

}  // namespace mmltk::gui
