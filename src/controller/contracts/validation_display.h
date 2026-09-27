#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
namespace mmltk::controller {
struct ValidationOverlays final {
 bool prediction_boxes = true, prediction_masks = true;
 bool ground_truth_boxes = true, ground_truth_masks = true;
 bool prediction_layer = true, ground_truth_layer = true;
 bool operator==(const ValidationOverlays&) const = default;
};
MMLTK_REFLECT_FIELDS(ValidationOverlays)
}  // namespace mmltk::controller
namespace mmltk::controller::contracts {
struct ValidationDisplaySettings final {
 MMLTK_MINIMUM(float, 0.0F) MMLTK_MAXIMUM(float, 1.0F) MMLTK_FINITE float confidence_threshold = 0.4F;
 bool operator==(const ValidationDisplaySettings&) const = default;
};
struct ValidationRunPreview final {
 ValidationOverlays overlays{};
 ValidationDisplaySettings display{};
 bool ground_truth_labels = true;
 bool prediction_labels = true;
 bool operator==(const ValidationRunPreview&) const = default;
};
MMLTK_REFLECT_FIELDS(ValidationRunPreview)
MMLTK_REFLECT_FIELDS(ValidationDisplaySettings)
}  // namespace mmltk::controller::contracts
