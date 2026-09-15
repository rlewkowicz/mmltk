#pragma once

#include <expected>
#include <string>
#include "src/frameworks/reflection/field_policy.h"

#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/contracts/artifact.h"
#include "src/backend/data/catalog/class_catalog.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/model.h"
#include "src/controller/contracts/workflows.h"
namespace mmltk::controller::subsystems::system {
// This is deliberately a pure boundary: it maps immutable system facts to a
// private service request, and has neither runtime authority nor effects.
struct ComputeIntentMaterializer final {
    struct Refusal final {
        [[= mmltk::frameworks::reflection::MaxBytes{mmltk::controller::contracts::kArtifactErrorCapacity}]] std::string detail;
    };
    using ValidationMaterialization = std::expected<mmltk::backend::models::rfdetr::ValidateRequest, Refusal>;
    using ExportMaterialization = std::expected<mmltk::backend::models::rfdetr::ModelExportRequest, Refusal>;
    using PredictionMaterialization = std::expected<mmltk::backend::models::rfdetr::PredictRequest, Refusal>;
    struct ModelInput final {
        mmltk::controller::contracts::ModelSelectionKey key{};
        std::string custom_artifact;
        int inspection_device = 0;
    };
    [[nodiscard]] static std::expected<ModelInput, Refusal> ModelInputFor(const mmltk::controller::contracts::GuiSettingsState& settings,
                                                                          mmltk::controller::contracts::FeatureId workflow) noexcept;
    [[nodiscard]] static std::expected<mmltk::backend::models::rfdetr::TrainRequest, Refusal> LocalTrain(
        const mmltk::controller::contracts::GuiSettingsState& settings, const mmltk::controller::contracts::ArtifactInspection& artifact,
        const mmltk::controller::contracts::ModelSelection& model) noexcept;
    [[nodiscard]] static ValidationMaterialization Validation(const mmltk::controller::contracts::GuiSettingsState& settings,
                                                              const mmltk::controller::contracts::ArtifactInspection& artifact,
                                                              const mmltk::controller::contracts::ModelSelection& model) noexcept;
    // CLEANUP-IGNORE: Each typed materializer method is an explicit domain boundary with a distinct result vocabulary.
    [[nodiscard]] static ExportMaterialization Export(const mmltk::controller::contracts::GuiSettingsState& settings,
                                                      const mmltk::controller::contracts::ArtifactInspection& artifact,
                                                      const mmltk::controller::contracts::ModelSelection& model) noexcept;
    [[nodiscard]] static PredictionMaterialization Predict(const mmltk::controller::contracts::GuiSettingsState& settings,
                                                           const mmltk::controller::contracts::ArtifactInspection& artifact,
                                                           const mmltk::controller::contracts::ModelSelection& model) noexcept;
};

}  // namespace mmltk::controller::subsystems::system
