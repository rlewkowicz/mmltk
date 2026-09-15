#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/model_selection.h"
#include "src/controller/contracts/model_selection_types.h"
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include "src/controller/contracts/terminal_presentation.h"

#include "src/controller/contracts/workflows.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::controller::contracts {

inline constexpr std::size_t kModelUiStateByteBudget = 16384U;
inline constexpr std::size_t kModelDetailCapacity = 4096U;

[[nodiscard]] inline std::string bounded_model_detail(const std::string_view value) {
    return std::string{value.substr(0U, kModelDetailCapacity)};
}

enum class ModelProgressStage : std::uint8_t {
    Idle,
    InspectingCache,
    Downloading,
    Verifying,
};

struct ModelProgress final {
    ModelProgressStage stage = ModelProgressStage::Idle;
    [[= mmltk::frameworks::reflection::MaxBytes{kModelDetailCapacity}]] std::string activity;
    std::uint64_t completed = 0U;
    std::uint64_t total = 0U;
    bool total_known = false;
    [[nodiscard]] bool valid() const noexcept {
        return activity.size() <= kModelDetailCapacity && (!total_known || (total != 0U && completed <= total)) &&
               (total_known || total == 0U);
    }
    bool operator==(const ModelProgress&) const = default;
};

struct[[= reflection::feature_scope(FeatureId::Train, FeatureId::Validate, FeatureId::Predict, FeatureId::Export)]]
    ModelSelectionRequest final {
    [[= mmltk::frameworks::reflection::CatalogProvider<ModelSelectionCompatibilityCatalog>{}]] FeatureId workflow = FeatureId::Train;
    [[nodiscard]] bool valid() const noexcept { return model_selection_workflow_supported(workflow); }
};

struct ModelSelection final {
    ModelSelectionKey key{};
    [[= mmltk::frameworks::reflection::MaxBytes{kModelArtifactCapacity}]] std::string artifact;
    mmltk::backend::models::rfdetr::ModelClassLayoutSummary class_layout;
    [[nodiscard]] bool valid() const noexcept { return key.valid() && !artifact.empty() && artifact.size() <= kModelArtifactCapacity; }
    bool operator==(const ModelSelection&) const = default;
};

enum class ModelSelectionOutcome : std::uint8_t { Idle, Accepted, Rejected, Cancelled, CancellationRequested };
inline constexpr std::array kModelSelectionPresentations{
    terminal_presentation::Policy{ModelSelectionOutcome::Idle, terminal_presentation::Classification::Refused, "model_selection.idle",
                                  "No model selection has completed."},
    terminal_presentation::Policy{ModelSelectionOutcome::Accepted, terminal_presentation::Classification::Success,
                                  "model_selection.accepted", ""},
    terminal_presentation::Policy{ModelSelectionOutcome::Rejected, terminal_presentation::Classification::Refused,
                                  "model_selection.rejected", "The model selection was rejected."},
    terminal_presentation::Policy{ModelSelectionOutcome::Cancelled, terminal_presentation::Classification::Cancelled,
                                  "model_selection.cancelled", "The model selection was cancelled."},
    terminal_presentation::Policy{ModelSelectionOutcome::CancellationRequested, terminal_presentation::Classification::Cancelled,
                                  "model_selection.cancellation_requested", "Model selection cancellation was requested."},
};
static_assert(terminal_presentation::complete(kModelSelectionPresentations));
[[nodiscard]] consteval const auto& materialized_terminal_presentation_policy(std::type_identity<ModelSelectionOutcome>) {
    return kModelSelectionPresentations;
}
struct ModelSelectionResult final {
    ModelSelectionOutcome outcome = ModelSelectionOutcome::Idle;
    [[= mmltk::frameworks::reflection::MaxBytes{kModelDetailCapacity}]] std::string detail;
    bool operator==(const ModelSelectionResult&) const = default;
};
struct ModelUiState final {
    std::uint64_t generation = 0U;
    bool active = false;
    ModelProgress progress{};
    ModelSelection selection{};
    ModelSelectionResult terminal{};
    bool operator==(const ModelUiState&) const = default;
};

MMLTK_REFLECT_FIELDS(ModelSelection)
MMLTK_REFLECT_FIELDS(ModelSelectionRequest)
MMLTK_REFLECT_FIELDS(ModelProgress)
MMLTK_REFLECT_FIELDS(ModelSelectionResult)
MMLTK_REFLECT_FIELDS(ModelUiState)
MMLTK_REFLECT_ENUM(ModelProgressStage)
MMLTK_REFLECT_ENUM(ModelSelectionOutcome)

}  // namespace mmltk::controller::contracts
