#pragma once
#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "src/backend/models/rfdetr/contract/evaluation_metrics.h"
#include "src/controller/contracts/annotation.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_document.h"
namespace mmltk::controller {
struct ValidationSampleIdentity final {
    std::uint64_t generation = 0U;
    std::uint32_t dataset_index = 0U;
    auto operator<=>(const ValidationSampleIdentity&) const = default;
};
struct ValidationLabel final {
    contracts::AnnotationBox box{};
    contracts::AnnotationColor color{};
    std::array<std::uint8_t, 3> rgb{};
    std::uint32_t category = 0U;
    bool ground_truth = false;
    float confidence = 0.0F;
    [[= mmltk::frameworks::reflection::MaxBytes{256U}]] std::string name;
};
struct ValidationSampleMetadata final {
    ValidationSampleIdentity identity{};
    bool available = false;
    VisualRegion crop{};
    VisualExtent original_extent{};
    [[= mmltk::frameworks::reflection::MaxItems{2U * contracts::kAnnotationObjectCapacity}]] std::vector<ValidationLabel> labels;
};
struct ValidationOverlays final {
    bool prediction_boxes = true, prediction_masks = true;
    bool ground_truth_boxes = true, ground_truth_masks = true;
    bool prediction_layer = true, ground_truth_layer = true;
    bool operator==(const ValidationOverlays&) const = default;
};
struct ValidationOverlaySelection final {
    std::uint64_t revision = 0U;
    ValidationOverlays value{};
    bool operator==(const ValidationOverlaySelection&) const = default;
};
struct ValidationImageMetadata {
    VisualFrame frame{};
    ValidationOverlays overlays{};
    std::uint64_t content_identity = 0U;
    bool detail = false;
    std::optional<ValidationSampleIdentity> selected;
    VisualDocumentFacts document{};
    std::array<ValidationSampleMetadata, mmltk::backend::models::rfdetr::kValidationSampleCapacity> samples{};
};
struct ValidationSnapshot final {
    VisualFrame frame{};
    std::uint64_t content_identity = 0U;
    bool detail = false;
    std::optional<ValidationSampleIdentity> selected;
    VisualDocumentFacts document{};
    std::array<ValidationSampleIdentity, mmltk::backend::models::rfdetr::kValidationSampleCapacity> sample_identities{};
    std::array<bool, mmltk::backend::models::rfdetr::kValidationSampleCapacity> sample_available{};
    contracts::ComputeUiState operation{};
    std::optional<mmltk::backend::models::rfdetr::EvalSummary> metrics;
    std::uint32_t detail_rows = 0U;
    ValidationOverlays overlays{};
    ValidationOverlaySelection overlay_selection{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] ValidationChanged final {
    ValidationSnapshot snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] ValidationProgress final {
    contracts::ComputeUiState operation{};
};
MMLTK_REFLECT_FIELDS(ValidationSampleIdentity)
MMLTK_REFLECT_FIELDS(ValidationLabel)
MMLTK_REFLECT_FIELDS(ValidationSampleMetadata)
MMLTK_REFLECT_FIELDS(ValidationOverlays)
MMLTK_REFLECT_FIELDS(ValidationOverlaySelection)
MMLTK_REFLECT_FIELDS(ValidationImageMetadata)
MMLTK_REFLECT_FIELDS(ValidationSnapshot)
MMLTK_REFLECT_FIELDS(ValidationChanged)
MMLTK_REFLECT_FIELDS(ValidationProgress)
}  // namespace mmltk::controller
