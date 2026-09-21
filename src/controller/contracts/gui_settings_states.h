#pragma once
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <type_traits>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/default_state.h"
#include "src/controller/contracts/view_state.h"
#include "src/controller/contracts/workflows.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::controller::contracts {
struct WorkflowSettingsState {
 [[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Train)]] TrainViewState train;
 [[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Validate)]] ValidateViewState validate;
 [[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Predict,
                                                             mmltk::controller::contracts::FeatureId::Live)]] PredictViewState predict;
 // CLEANUP-IGNORE: Each reflected workflow member carries its own canonical feature scope for generated settings
 // projection.
 [[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Annotate)]] AnnotateViewState annotate;
 [[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Export)]] ExportViewState export_state;
 [[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Explore)]] ExploreViewState explore;
 void apply_defaults() { *this = WorkflowSettingsState{}; }
};
// Canonical in-memory settings value. Persistence translates this aggregate at
// the gui.json boundary; systems exchange immutable snapshots of this type.
struct GuiSettingsState {
 [[= mmltk::frameworks::serialization::DefaultValue<mmltk::controller::contracts::FeatureId::Train>{}]] mmltk::controller::contracts::FeatureId current_view =
  mmltk::controller::contracts::FeatureId::Train;
 UiSettingsState ui;
 WorkflowSettingsState workflows;
 void apply_defaults() { *this = GuiSettingsState{}; }
 [[nodiscard]] bool operator==(const GuiSettingsState&) const noexcept;
};
MMLTK_REFLECT_FIELDS(WorkflowSettingsState)
MMLTK_REFLECT_FIELDS(GuiSettingsState)
}  // namespace mmltk::controller::contracts
