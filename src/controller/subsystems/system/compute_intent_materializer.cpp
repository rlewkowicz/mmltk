#include "src/controller/subsystems/system/compute_intent_materializer.h"

#include <concepts>
#include <exception>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/contracts/artifact.h"
#include "src/backend/data/catalog/class_catalog.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/model.h"
#include "src/controller/contracts/model_selection.h"

namespace mmltk::controller::subsystems::system {
namespace {
[[nodiscard]] ComputeIntentMaterializer::Refusal refused(const std::string_view detail) noexcept {
    return {.detail = mmltk::controller::contracts::bounded_artifact_detail(detail)};
}

[[nodiscard]] std::expected<const mmltk::controller::contracts::ArtifactSplitFact*, ComputeIntentMaterializer::Refusal>
current_artifact_split(const mmltk::controller::contracts::ArtifactInspection& inspection, const std::filesystem::path& configured_path,
                       const std::string_view unavailable_detail) noexcept {
    if (!inspection.available()) return std::unexpected(refused("artifact facts are unavailable"));
    const auto configured_name = configured_path.lexically_normal();
    if (configured_name.empty()) return std::unexpected(refused(unavailable_detail));

    const mmltk::controller::contracts::ArtifactSplitFact* selected = nullptr;
    for (std::size_t index = 0U; index != inspection.splits.size(); ++index) {
        const auto& split = inspection.splits[index];
        if (!split.valid()) return std::unexpected(refused("artifact facts are invalid"));
        const auto split_name = std::filesystem::path{split.path}.lexically_normal();
        if (split_name.empty()) return std::unexpected(refused("artifact facts are invalid"));
        for (std::size_t previous = 0U; previous != index; ++previous) {
            if (std::filesystem::path{inspection.splits[previous].path}.lexically_normal() == split_name) {
                return std::unexpected(refused("artifact split identity is duplicated"));
            }
        }
        if (split_name == configured_name) selected = &split;
    }
    if (selected == nullptr) return std::unexpected(refused(unavailable_detail));
    return selected;
}

[[nodiscard]] std::expected<const mmltk::controller::contracts::ArtifactSplitFact*, ComputeIntentMaterializer::Refusal>
current_training_split(const mmltk::controller::contracts::GuiSettingsState& settings,
                       const mmltk::controller::contracts::ArtifactInspection& inspection) noexcept {
    return current_artifact_split(inspection, settings.workflows.train.request.train_compiled_path, "training artifact is unavailable");
}

[[nodiscard]] std::expected<void, ComputeIntentMaterializer::Refusal> require_model(
    const mmltk::controller::contracts::GuiSettingsState& settings, const mmltk::controller::contracts::FeatureId workflow,
    const mmltk::controller::contracts::ModelSelection& model) noexcept {
    const auto current = ComputeIntentMaterializer::ModelInputFor(settings, workflow);
    if (!current) return std::unexpected(current.error());
    if (!model.valid() || model.key != current->key ||
        (model.key.source == mmltk::controller::contracts::ModelSelectionSource::Custom && model.artifact != current->custom_artifact))
        return std::unexpected(refused("model selection does not match current workflow settings"));
    return {};
}

void assign_model_artifact(mmltk::backend::models::rfdetr::ModelArtifactRequest& request,
                           const mmltk::controller::contracts::ModelSelection& model) {
    request.class_layout_path = model.key.class_layout_path;
    request.weights_path.clear();
    request.onnx_path.clear();
    request.tensorrt_path.clear();
    switch (model.key.input) {
        case mmltk::controller::contracts::ModelArtifactInputKind::Weights:
            request.weights_path = model.artifact;
            return;
        case mmltk::controller::contracts::ModelArtifactInputKind::Onnx:
            request.onnx_path = model.artifact;
            return;
        case mmltk::controller::contracts::ModelArtifactInputKind::TensorRt:
            request.tensorrt_path = model.artifact;
            return;
        case mmltk::controller::contracts::ModelArtifactInputKind::None:
            std::unreachable();
    }
}

void assign_export_output_facts(mmltk::backend::models::rfdetr::ModelArtifactOutputRequest& request,
                                const mmltk::controller::contracts::ExportViewState& state, std::filesystem::path output_path) {
    request.preset_name = state.preset_name;
    request.resolution = state.model_resolution;
    request.output_path = std::move(output_path);
}
}  // namespace

std::expected<ComputeIntentMaterializer::ModelInput, ComputeIntentMaterializer::Refusal> ComputeIntentMaterializer::ModelInputFor(
    const mmltk::controller::contracts::GuiSettingsState& settings, const mmltk::controller::contracts::FeatureId workflow) noexcept {
    if (!mmltk::controller::contracts::gui_settings_valid(settings)) return std::unexpected(refused("invalid settings"));
    if (mmltk::controller::contracts::valid_feature(workflow) &&
        !mmltk::controller::contracts::model_selection_workflow_supported(workflow))
        return std::unexpected(refused("workflow does not support model selection"));
    auto projection = mmltk::controller::contracts::model_settings_projection(settings, workflow);
    if (!projection || !projection->compatible) return std::unexpected(refused("model selection is incomplete"));
    const bool custom = projection->key.source == mmltk::controller::contracts::ModelSelectionSource::Custom;
    ModelInput materialized{
        .key = std::move(projection->key),
        .custom_artifact = custom ? std::move(projection->artifact) : std::string{},
    };
    if (materialized.key.source == mmltk::controller::contracts::ModelSelectionSource::Custom) {
        if (materialized.custom_artifact.empty() ||
            materialized.custom_artifact.size() > mmltk::controller::contracts::kModelArtifactCapacity)
            return std::unexpected(refused("custom model artifact is unavailable"));
    }
    if (workflow == mmltk::controller::contracts::FeatureId::Validate)
        materialized.inspection_device = settings.workflows.validate.request.device_id;
    else if (workflow == mmltk::controller::contracts::FeatureId::Predict)
        materialized.inspection_device = settings.workflows.predict.request.device_id;
    else if (workflow == mmltk::controller::contracts::FeatureId::Export)
        materialized.inspection_device = settings.workflows.export_state.device_id;
    if (!materialized.key.valid()) return std::unexpected(refused("model selection key is invalid"));
    return materialized;
}

std::expected<mmltk::backend::models::rfdetr::TrainRequest, ComputeIntentMaterializer::Refusal> ComputeIntentMaterializer::LocalTrain(
    const mmltk::controller::contracts::GuiSettingsState& settings, const mmltk::controller::contracts::ArtifactInspection& artifact,
    const mmltk::controller::contracts::ModelSelection& model) noexcept {
    auto request = settings.workflows.train.request;
    const auto selected = require_model(settings, mmltk::controller::contracts::FeatureId::Train, model);
    if (!selected) return std::unexpected(selected.error());
    const auto training = current_training_split(settings, artifact);
    if (!training) return std::unexpected(training.error());
    const auto validation = current_artifact_split(artifact, request.val_compiled_path, "validation artifact is unavailable");
    if (!validation) return std::unexpected(validation.error());
    if ((*training)->class_names != (*validation)->class_names)
        return std::unexpected(refused("training and validation class order differs"));
    request.train_compiled_path = (*training)->path;
    if (request.resume_path.empty()) {
        request.weights_path = model.artifact;
    } else {
        if (request.resume_path != model.artifact)
            return std::unexpected(refused("prepared training model does not match the selected resume checkpoint"));
        request.weights_path.clear();
    }
    request.val_compiled_path = (*validation)->path;
    if (request.test_compiled_path.empty()) {
        request.test_compiled_path.clear();
    } else {
        const auto test = current_artifact_split(artifact, request.test_compiled_path, "test artifact is unavailable");
        if (!test) return std::unexpected(test.error());
        if ((*training)->class_names != (*test)->class_names)
            return std::unexpected(refused("training and test class order differs"));
        request.test_compiled_path = (*test)->path;
    }
    try {
        mmltk::backend::models::rfdetr::validate_train_request(request);
    } catch (const std::exception& error) { return std::unexpected(refused(error.what())); }
    // CLEANUP-IGNORE: Local training returns its fully validated multi-split request; export returns a distinct model
    // request variant below.
    return request;
}

// CLEANUP-IGNORE: Validation materialization is a direct typed workflow boundary; sharing its orchestration with
// training or prediction would require an erased or detector-only generic request builder.
ComputeIntentMaterializer::ValidationMaterialization ComputeIntentMaterializer::Validation(
    const mmltk::controller::contracts::GuiSettingsState& settings, const mmltk::controller::contracts::ArtifactInspection& artifact,
    const mmltk::controller::contracts::ModelSelection& model) noexcept {
    auto request = settings.workflows.validate.request;
    const auto selected = require_model(settings, mmltk::controller::contracts::FeatureId::Validate, model);
    if (!selected) return std::unexpected(selected.error());
    const auto compiled = current_artifact_split(artifact, request.compiled_path, "selected validation artifact is unavailable");
    if (!compiled) return std::unexpected(compiled.error());
    request.compiled_path = (*compiled)->path;
    assign_model_artifact(request, model);
    request.save_engine_path.clear();
    request.source_dir.clear();
    request.recompile = false;
    request.eval_order = model.key.input == mmltk::controller::contracts::ModelArtifactInputKind::Weights ? "weights"
                         : model.key.input == mmltk::controller::contracts::ModelArtifactInputKind::Onnx ? "onnx" : "tensorrt";
    request.device_id = -1;
    request.compile_cuda_device_id = -1;
    return request;
}

ComputeIntentMaterializer::ExportMaterialization ComputeIntentMaterializer::Export(
    const mmltk::controller::contracts::GuiSettingsState& settings, const mmltk::controller::contracts::ArtifactInspection& artifact,
    const mmltk::controller::contracts::ModelSelection& model) noexcept {
    static_cast<void>(artifact);
    const auto selected = require_model(settings, mmltk::controller::contracts::FeatureId::Export, model);
    if (!selected) return std::unexpected(selected.error());
    const auto& state = settings.workflows.export_state;
    if (state.build_tensorrt) {
        mmltk::backend::models::rfdetr::BuildEngineRequest request{};
        auto& output = static_cast<mmltk::backend::models::rfdetr::ModelArtifactOutputRequest&>(request);
        assign_model_artifact(output, model);
        assign_export_output_facts(output, state, state.output_path);
        request.allow_fp16 = state.allow_fp16;
        try {
            mmltk::backend::models::rfdetr::validate_build_engine_request(request);
        } catch (const std::exception& error) { return std::unexpected(refused(error.what())); }
        request.device_id = -1;
        return mmltk::backend::models::rfdetr::ModelExportRequest{std::in_place_type<mmltk::backend::models::rfdetr::BuildEngineRequest>,
                                                                  std::move(request)};
    }

    mmltk::backend::models::rfdetr::ExportOnnxRequest request{};
    auto& output = static_cast<mmltk::backend::models::rfdetr::ModelArtifactOutputRequest&>(request);
    assign_model_artifact(output, model);
    assign_export_output_facts(output, state,
                               state.onnx_output_path.empty() ? std::filesystem::path{model.artifact}.replace_extension(".onnx")
                                                              : std::filesystem::path{state.onnx_output_path});
    // CLEANUP-IGNORE: ONNX export validation consumes fields distinct from the TensorRT branch above.
    request.opset_version = state.opset_version;
    request.simplify = state.simplify;
    try {
        mmltk::backend::models::rfdetr::validate_export_onnx_request(request);
    } catch (const std::exception& error) { return std::unexpected(refused(error.what())); }
    // CLEANUP-IGNORE: Export materialization and prediction materialization cross distinct validated domain boundaries.
    request.device_id = -1;
    return mmltk::backend::models::rfdetr::ModelExportRequest{std::in_place_type<mmltk::backend::models::rfdetr::ExportOnnxRequest>,
                                                              std::move(request)};
}

// CLEANUP-IGNORE: Prediction materialization owns source selection and prediction-only request fields after the shared
// model and artifact primitives have validated their inputs.
ComputeIntentMaterializer::PredictionMaterialization ComputeIntentMaterializer::Predict(
    const mmltk::controller::contracts::GuiSettingsState& settings, const mmltk::controller::contracts::ArtifactInspection& artifact,
    const mmltk::controller::contracts::ModelSelection& model) noexcept {
    auto request = settings.workflows.predict.request;
    const auto selected = require_model(settings, mmltk::controller::contracts::FeatureId::Predict, model);
    if (!selected) return std::unexpected(selected.error());
    request.compiled_path.clear();
    request.image_inputs.clear();
    request.video_path.clear();
    const auto& source = settings.workflows.predict.source;
    if (source.kind == mmltk::controller::contracts::SourceKind::CompiledDataset) {
        const auto compiled = current_artifact_split(artifact, source.compiled_path, "prediction dataset artifact is unavailable");
        if (!compiled) return std::unexpected(compiled.error());
        request.source_kind = mmltk::backend::models::rfdetr::PredictSourceKind::CompiledDataset;
        request.compiled_path = (*compiled)->path;
    } else if (source.kind == mmltk::controller::contracts::SourceKind::SingleImage) {
        if (source.single_image_path.empty()) return std::unexpected(refused("prediction image is unavailable"));
        request.source_kind = mmltk::backend::models::rfdetr::PredictSourceKind::ImageFiles;
        request.image_inputs.push_back({.image_path = source.single_image_path,
                                       .source_name = std::filesystem::path{source.single_image_path}.filename().string(),
                                       .image_id = 0});
    } else if (source.kind == mmltk::controller::contracts::SourceKind::VideoFile) {
        if (source.video_file_path.empty()) return std::unexpected(refused("prediction video is unavailable"));
        request.source_kind = mmltk::backend::models::rfdetr::PredictSourceKind::VideoFile;
        request.video_path = source.video_file_path;
    } else {
        return std::unexpected(refused("select a compiled dataset, image, or local video file"));
    }
    assign_model_artifact(request, model);
    request.backend = "auto";
    request.batch_size = 1U;
    request.device_id = -1;
    return request;
}

}  // namespace mmltk::controller::subsystems::system
