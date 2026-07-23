#pragma once

#include "gui/annotation/editor.h"
#include "gui/annotation_core.h"
#include "gui/canvas_layers.h"
#include "gui/rfdetr_workflows.h"
#include "mmltk/runtime/async_runtime.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mmltk::gui {

struct AnnotationWorkflowAssistLaunchRequest {
    const AnnotationFrame* frame = nullptr;
    bool assist_running = false;
    ModelInputMode model_input = ModelInputMode::None;
};

struct AnnotationWorkflowAssistLaunchDecision {
    bool can_launch = false;
    std::string error_message;
};

struct AnnotationWorkflowAssistDispatchPlan {
    bool can_launch = false;
    bool refresh_live_frame = false;
    std::string error_message;
};

struct AnnotationWorkflowAssistSeed {
    std::string class_name;
    AnnotationBox box{};
    std::vector<std::uint8_t> mask;
    AnnotationMaskRegion mask_region{};
    std::uint64_t seed_frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> seed_live_frame_id;
    bool has_mask = false;
};

struct AnnotationWorkflowAssistOutcome {
    std::vector<AnnotationWorkflowAssistSeed> seeds;
    std::string summary;
    std::uint64_t frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> live_frame_id;
};

struct AnnotationWorkflowAssistExecutionRequest {
    AnnotationFrame frame;
    AnnotateViewState annotate{};
    std::string preset_name;
};

struct AnnotationWorkflowAssistLaunchState {
    bool* running = nullptr;
    std::string* summary = nullptr;
    std::string* error = nullptr;
};

[[nodiscard]] inline AnnotationWorkflowAssistOutcome run_annotation_workflow_assist(
    const AnnotationWorkflowAssistExecutionRequest& request);

struct AnnotationWorkflowAssistApplyResult {
    bool changed = false;
    std::optional<std::size_t> first_inserted_index;
};

[[nodiscard]] inline AnnotationWorkflowAssistLaunchDecision decide_annotation_workflow_assist_launch(
    const AnnotationWorkflowAssistLaunchRequest& request) {
    if (request.frame == nullptr || request.assist_running) {
        return {};
    }
    if (request.model_input == ModelInputMode::None) {
        return AnnotationWorkflowAssistLaunchDecision{
            false,
            "Annotation assist requires model backing. Use None for manual "
            "full-box masks, or select Weights, ONNX, or TensorRT.",
        };
    }
    return AnnotationWorkflowAssistLaunchDecision{
        true,
        {},
    };
}

[[nodiscard]] inline AnnotationWorkflowAssistDispatchPlan plan_annotation_workflow_assist_dispatch(
    const AnnotationWorkflowAssistLaunchRequest& request, const bool annotation_live_running) {
    AnnotationWorkflowAssistLaunchDecision decision = decide_annotation_workflow_assist_launch(request);
    return AnnotationWorkflowAssistDispatchPlan{
        decision.can_launch,
        decision.can_launch && annotation_live_running,
        decision.error_message,
    };
}

[[nodiscard]] inline std::optional<AnnotationWorkflowAssistExecutionRequest>
make_annotation_workflow_assist_execution_request(const AnnotationFrame& frame_snapshot,
                                                  const AnnotateViewState& annotate, std::string preset_name,
                                                  std::string* error_message = nullptr) {
    try {
        AnnotationWorkflowAssistExecutionRequest request;
        request.frame = annotate.source.kind == SourceKind::VideoStream
                            ? extract_annotation_frame_region(frame_snapshot, resolved_video_crop_box(annotate.source))
                            : frame_snapshot;
        request.annotate = annotate;
        request.preset_name = std::move(preset_name);
        if (error_message != nullptr) {
            error_message->clear();
        }
        return request;
    } catch (const std::exception& error) {
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return std::nullopt;
    }
}

[[nodiscard]] inline AnnotationWorkflowAssistOutcome make_annotation_workflow_assist_outcome(
    const AnnotationFrame& assist_frame, const mmltk::rfdetr::PredictionRunResult& result) {
    if (result.records.size() != 1U) {
        throw std::runtime_error("annotation assist expected exactly one prediction record");
    }

    AnnotationWorkflowAssistOutcome outcome;
    outcome.summary = rfdetr_workflows::summarize_prediction_result(result);
    outcome.frame_id = assist_frame.frame_id;
    outcome.live_frame_id = assist_frame.live_frame_id;
    for (const mmltk::rfdetr::Prediction& prediction : result.records.front().detections) {
        if (prediction.category_id <= 0) {
            continue;
        }

        AnnotationWorkflowAssistSeed seed;
        const auto class_index = static_cast<std::size_t>(prediction.category_id - 1);
        if (class_index < result.class_names.size()) {
            seed.class_name = result.class_names[class_index];
        } else {
            seed.class_name = std::to_string(prediction.category_id);
        }
        const AnnotationBox local_box =
            rfdetr_workflows::prediction_to_annotation_box(prediction, assist_frame.width, assist_frame.height);
        seed.box = annotation_box_from_frame(assist_frame, local_box);
        if (prediction.has_mask) {
            seed.mask = decode_annotation_prediction_mask(prediction.mask, assist_frame.width, assist_frame.height);
            seed.mask_region = annotation_mask_region_from_frame(assist_frame);
            seed.seed_frame_id = assist_frame.frame_id;
            seed.seed_live_frame_id = assist_frame.live_frame_id;
            seed.has_mask = true;
        }
        outcome.seeds.push_back(std::move(seed));
    }
    return outcome;
}

[[nodiscard]] inline std::vector<AnnotationObject> make_annotation_workflow_assist_objects(
    AnnotationCategories& categories, const std::vector<AnnotationWorkflowAssistSeed>& seeds,
    const std::size_t first_object_index) {
    std::vector<AnnotationObject> objects;
    objects.reserve(seeds.size());
    for (std::size_t index = 0; index < seeds.size(); ++index) {
        const AnnotationWorkflowAssistSeed& seed = seeds[index];
        AnnotationObject object;
        object.object_id = "assist-" + std::to_string(first_object_index + index + 1U);
        object.category_index = ensure_annotation_category(categories, seed.class_name);
        if (seed.has_mask) {
            object.shape = AnnotationMaskShape{
                seed.box, seed.mask_region, seed.mask, seed.seed_frame_id, seed.seed_live_frame_id,
            };
        } else {
            object.shape = AnnotationBoxShape{seed.box};
        }
        objects.push_back(std::move(object));
    }
    return objects;
}

[[nodiscard]] inline bool annotation_workflow_assist_outcome_matches_frame(
    const AnnotationFrame* frame, const AnnotationWorkflowAssistOutcome& outcome) noexcept {
    return frame != nullptr && annotation_frame_matches_saved_identity(*frame, outcome.frame_id, outcome.live_frame_id);
}

[[nodiscard]] inline AnnotationWorkflowAssistApplyResult apply_annotation_workflow_assist_outcome(
    AnnotationDocument& document, AnnotationSession& session, AnnotationCategories& categories,
    const AnnotationWorkflowAssistOutcome& outcome) {
    AnnotationWorkflowAssistApplyResult result;
    result.first_inserted_index = document.size();
    std::vector<AnnotationObject> objects =
        make_annotation_workflow_assist_objects(categories, outcome.seeds, *result.first_inserted_index);
    for (AnnotationObject& object : objects) {
        result.changed = AnnotationEditor::insert_object(document, session, std::move(object)) || result.changed;
    }
    if (!result.changed) {
        result.first_inserted_index.reset();
        return result;
    }

    select_object(session, document, result.first_inserted_index);
    return result;
}

template <typename EnsureLiveFramePixelsFn, typename MakeExecutionRequestFn, typename OnSuccessFn, typename OnErrorFn>
[[nodiscard]] inline bool launch_annotation_workflow_assist(
    const AnnotationWorkflowAssistLaunchRequest& request, const bool annotation_live_running,
    AnnotationWorkflowAssistLaunchState state, mmltk::runtime::BackgroundExecutor& background_executor,
    mmltk::runtime::UiCallbackQueue& ui_callbacks, EnsureLiveFramePixelsFn&& ensure_live_frame_pixels,
    MakeExecutionRequestFn&& make_execution_request, OnSuccessFn&& on_success, OnErrorFn&& on_error) {
    const AnnotationWorkflowAssistDispatchPlan plan =
        plan_annotation_workflow_assist_dispatch(request, annotation_live_running);
    if (!plan.can_launch) {
        if (state.error != nullptr && !plan.error_message.empty()) {
            *state.error = plan.error_message;
        }
        if (state.summary != nullptr) {
            state.summary->clear();
        }
        return false;
    }

    if (plan.refresh_live_frame) {
        std::string frame_error;
        if (!std::forward<EnsureLiveFramePixelsFn>(ensure_live_frame_pixels)(&frame_error)) {
            if (state.error != nullptr) {
                *state.error = std::move(frame_error);
            }
            if (state.summary != nullptr) {
                state.summary->clear();
            }
            return false;
        }
    }

    std::optional<AnnotationWorkflowAssistExecutionRequest> assist_request =
        std::forward<MakeExecutionRequestFn>(make_execution_request)();
    if (!assist_request.has_value()) {
        if (state.running != nullptr) {
            *state.running = false;
        }
        return false;
    }

    if (state.running != nullptr) {
        *state.running = true;
    }
    if (state.error != nullptr) {
        state.error->clear();
    }
    if (state.summary != nullptr) {
        state.summary->clear();
    }

    mmltk::runtime::submit_background_task(
        background_executor, ui_callbacks,
        [assist_request = std::move(*assist_request)]() mutable {
            return run_annotation_workflow_assist(assist_request);
        },
        [state, on_success = std::forward<OnSuccessFn>(on_success)](AnnotationWorkflowAssistOutcome outcome) mutable {
            if (state.running != nullptr) {
                *state.running = false;
            }
            if (state.error != nullptr) {
                state.error->clear();
            }
            if (state.summary != nullptr) {
                *state.summary = outcome.summary;
            }
            on_success(std::move(outcome));
        },
        [state, on_error = std::forward<OnErrorFn>(on_error)](const std::string& error) mutable {
            if (state.running != nullptr) {
                *state.running = false;
            }
            if (state.error != nullptr) {
                *state.error = error;
            }
            if (state.summary != nullptr) {
                state.summary->clear();
            }
            on_error(error);
        });
    return true;
}

[[nodiscard]] inline AnnotationWorkflowAssistOutcome run_annotation_workflow_assist(
    const AnnotationWorkflowAssistExecutionRequest& request) {
    const std::filesystem::path temp_path =
        std::filesystem::temp_directory_path() / ("mmltk-annotate-" + std::to_string(request.frame.frame_id) + ".png");
    try {
        write_annotation_frame_png(temp_path, request.frame);
        mmltk::rfdetr::PredictImageInput input;
        input.image_path = temp_path;
        input.source_name = request.frame.source_name;
        input.image_id = static_cast<int64_t>(request.frame.frame_id);
        const mmltk::rfdetr::PredictOptions options =
            rfdetr_workflows::build_annotate_predict_options(request.annotate, request.preset_name, std::move(input));
        const mmltk::rfdetr::PredictionRunResult result = run_prediction(options);
        std::error_code ignored_error;
        std::filesystem::remove(temp_path, ignored_error);
        return make_annotation_workflow_assist_outcome(request.frame, result);
    } catch (...) {
        std::error_code ignored_error;
        std::filesystem::remove(temp_path, ignored_error);
        throw;
    }
}

}  
