#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>
#include <optional>
#include <string_view>
#include <tuple>
#include "mmltk/frameworks/reflection/materializer.h"
#include "mmltk/frameworks/reflection/member_path.h"
#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/backend/models/catalog/artifacts.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/model_selection_types.h"
#include "src/controller/contracts/workflows.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::controller::contracts {
using mmltk::backend::models::catalog::ModelArtifactInputKind;
struct ModelSelectionCompatibility final {
 std::string_view key;
 FeatureId workflow = FeatureId::Train;
 ModelArtifactInputKind input = ModelArtifactInputKind::None;
 bool canonical_allowed = false;
 bool custom_allowed = false;
 std::optional<bool> required_export_build_tensorrt;
 std::string_view artifact_field_path;
 std::string_view dialog_title;
 std::string_view dialog_filter;
 std::string_view dialog_pattern;
 constexpr bool operator==(const ModelSelectionCompatibility&) const noexcept = default;
};
MMLTK_REFLECT_FIELDS(ModelSelectionCompatibility)
struct ModelArtifactDialog final {
 std::string_view title;
 std::string_view filter;
 std::string_view pattern;
};
MMLTK_REFLECT_FIELDS(ModelArtifactDialog)
[[nodiscard]] constexpr ModelArtifactDialog model_artifact_dialog(const ModelArtifactInputKind input) noexcept {
 switch (input) {
  case ModelArtifactInputKind::Weights: return {"Select custom model weights", "Weights", "*.pt *.pth *.ckpt *.safetensors"};
  case ModelArtifactInputKind::Onnx: return {"Select custom ONNX model", "ONNX files", "*.onnx"};
  case ModelArtifactInputKind::TensorRt: return {"Select custom TensorRT model", "TensorRT files", "*.engine *.trt"};
  case ModelArtifactInputKind::None: return {};
 }
 return {};
}
[[nodiscard]] constexpr ModelSelectionCompatibility custom_model_compatibility(const std::string_view key, const FeatureId workflow,
                                                                               const ModelArtifactInputKind input, const bool canonical_allowed,
                                                                               const std::optional<bool> required_export_build_tensorrt,
                                                                               const std::string_view artifact_field_path) noexcept {
 const auto dialog = model_artifact_dialog(input);
 return {
  key, workflow, input, canonical_allowed, true, required_export_build_tensorrt, artifact_field_path, dialog.title, dialog.filter, dialog.pattern,
 };
}
struct ModelSelectionTextTransform final {
 template <class Source, class Destination>
 [[nodiscard]] static consteval bool accepts() {
  return (std::same_as<std::remove_cvref_t<Source>, std::filesystem::path> || std::same_as<std::remove_cvref_t<Source>, std::string>) &&
         std::same_as<std::remove_cvref_t<Destination>, std::string>;
 }
 template <class Destination, class Source>
 static void apply(Destination& destination, const Source& source) {
  static_assert(accepts<Source, Destination>());
  if constexpr (std::same_as<std::remove_cvref_t<Source>, std::filesystem::path>)
   destination = source.native();
  else
   destination = source;
 }
};
struct ModelSelectionResolutionTransform final {
 template <class Source, class Destination>
 [[nodiscard]] static consteval bool accepts() {
  return std::same_as<std::remove_cvref_t<Source>, std::int32_t> && std::same_as<std::remove_cvref_t<Destination>, std::uint32_t>;
 }
 template <class Destination, class Source>
 static constexpr void apply(Destination& destination, const Source& source) noexcept {
  static_assert(accepts<Source, Destination>());
  destination = static_cast<std::uint32_t>(source);
 }
};
using mmltk::frameworks::reflection::member_path;
using mmltk::frameworks::reflection::MemberRelationEntry;
template <FeatureId Workflow, auto Source, auto Input, auto Preset, auto Resolution, auto Artifact, auto Layout, auto... Predicate>
struct ModelSelectionRelationRow {
 static constexpr FeatureId workflow = Workflow;
 static constexpr auto source = Source;
 static constexpr auto input = Input;
 static constexpr auto preset = Preset;
 static constexpr auto resolution = Resolution;
 static constexpr auto artifact = Artifact;
 static constexpr auto class_layout = Layout;
 static constexpr auto predicate = std::tuple{Predicate...};
 static constexpr auto workflow_destination = member_path<&ModelSelectionKey::workflow>;
 static constexpr std::size_t key_field_count = [] {
  std::size_t count = 0U;
  mmltk::frameworks::reflection::visit_materialized_members<ModelSelectionKey>([&]<class>(const auto&) { ++count; });
  return count;
 }();
 using key_relation = mmltk::frameworks::reflection::StaticMemberRelation<
  GuiSettingsState, ModelSelectionKey, key_field_count - 1U, MemberRelationEntry<Source, member_path<&ModelSelectionKey::source>>,
  MemberRelationEntry<Input, member_path<&ModelSelectionKey::input>>,
  MemberRelationEntry<Preset, member_path<&ModelSelectionKey::preset>, ModelSelectionTextTransform>,
  MemberRelationEntry<Resolution, member_path<&ModelSelectionKey::resolution>, ModelSelectionResolutionTransform>,
  MemberRelationEntry<Layout, member_path<&ModelSelectionKey::class_layout_path>, ModelSelectionTextTransform>>;
 using artifact_relation = mmltk::frameworks::reflection::StaticMemberRelation<
  GuiSettingsState, ModelSettingsProjection, 1U, MemberRelationEntry<Artifact, member_path<&ModelSettingsProjection::artifact>, ModelSelectionTextTransform>>;
 inline static constexpr auto artifact_field_path = mmltk::frameworks::reflection::reflected_member_path<GuiSettingsState, Artifact>();
 [[nodiscard]] static consteval bool valid() {
  using namespace mmltk::frameworks::reflection;
  if (!key_relation::valid() || !artifact_relation::valid()) return false;
  bool complete = true;
  std::size_t count = 0U;
  visit_materialized_members<ModelSelectionKey>([&]<class Declaration>(const auto&) {
   constexpr auto identity = accessor_member_identity<ModelSelectionKey, Declaration::pointer>();
   std::size_t matches = identity == accessor_member_identity<ModelSelectionKey, workflow_destination>() ? 1U : 0U;
   key_relation::VisitMembers([&]<class Entry>() { matches += identity == accessor_member_identity<ModelSelectionKey, Entry::destination>() ? 1U : 0U; });
   complete = complete && matches == 1U;
   ++count;
  });
  return complete && count == key_relation::member_count + 1U;
 }
 [[nodiscard]] static bool predicate_matches(const GuiSettingsState& settings, const ModelSelectionCompatibility& row) {
  if constexpr (sizeof...(Predicate) == 0U)
   return !row.required_export_build_tensorrt;
  else {
   static_assert(sizeof...(Predicate) == 1U);
   return row.required_export_build_tensorrt && std::get<0>(predicate)(settings) == *row.required_export_build_tensorrt;
  }
 }
};
using mmltk::frameworks::reflection::reflected_member_path;
struct TrainWeightsModelSelection final
    : ModelSelectionRelationRow<FeatureId::Train, member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::model_source>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::model_input>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                            &mmltk::backend::models::rfdetr::TrainRequest::preset_name>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                            &mmltk::backend::models::rfdetr::TrainRequest::resolution>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                            &mmltk::backend::models::rfdetr::TrainRequest::weights_path>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                            &mmltk::backend::models::rfdetr::TrainRequest::class_layout_path>> {
 inline static constexpr ModelArtifactInputKind input_kind = ModelArtifactInputKind::Weights;
 inline static constexpr ModelSelectionCompatibility compatibility =
  custom_model_compatibility("train.weights", workflow, input_kind, true, std::nullopt, artifact_field_path.view());
};
template <auto Artifact, ModelArtifactInputKind Input>
// CLEANUP-IGNORE: Each workflow row declares distinct reflected paths into its own request type.
struct ValidateModelSelection
    : ModelSelectionRelationRow<FeatureId::Validate,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::model_source>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::model_input>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                            &mmltk::backend::models::rfdetr::ValidateRequest::preset_name>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                            &mmltk::backend::models::rfdetr::ValidateRequest::resolution>,
                                Artifact,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                            &mmltk::backend::models::rfdetr::ValidateRequest::class_layout_path>> {
 inline static constexpr ModelArtifactInputKind input_kind = Input;
};
struct ValidateWeightsModelSelection final
    : ValidateModelSelection<member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                         &mmltk::backend::models::rfdetr::ValidateRequest::weights_path>,
                             ModelArtifactInputKind::Weights> {
 inline static constexpr ModelSelectionCompatibility compatibility =
  custom_model_compatibility("validate.weights", workflow, input_kind, true, std::nullopt, artifact_field_path.view());
};
struct ValidateOnnxModelSelection final
    : ValidateModelSelection<member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                         &mmltk::backend::models::rfdetr::ValidateRequest::onnx_path>,
                             ModelArtifactInputKind::Onnx> {
 inline static constexpr ModelSelectionCompatibility compatibility =
  custom_model_compatibility("validate.onnx", workflow, input_kind, false, std::nullopt, artifact_field_path.view());
};
struct ValidateTensorRtModelSelection final
    : ValidateModelSelection<member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                         &mmltk::backend::models::rfdetr::ValidateRequest::tensorrt_path>,
                             ModelArtifactInputKind::TensorRt> {
 inline static constexpr ModelSelectionCompatibility compatibility =
  // CLEANUP-IGNORE: Compatibility rows retain distinct stable identities and artifact policies.
  custom_model_compatibility("validate.tensorrt", workflow, input_kind, false, std::nullopt, artifact_field_path.view());
};
template <auto Artifact, ModelArtifactInputKind Input>
// CLEANUP-IGNORE: Each workflow row declares distinct reflected paths into its own request type.
struct PredictModelSelection
    : ModelSelectionRelationRow<FeatureId::Predict, member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::model_source>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::model_input>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                            &mmltk::backend::models::rfdetr::PredictRequest::preset_name>,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                            &mmltk::backend::models::rfdetr::PredictRequest::resolution>,
                                Artifact,
                                member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                            &mmltk::backend::models::rfdetr::PredictRequest::class_layout_path>> {
 inline static constexpr ModelArtifactInputKind input_kind = Input;
};
struct PredictWeightsModelSelection final
    : PredictModelSelection<member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                        &mmltk::backend::models::rfdetr::PredictRequest::weights_path>,
                            ModelArtifactInputKind::Weights> {
 inline static constexpr ModelSelectionCompatibility compatibility =
  // CLEANUP-IGNORE: Compatibility rows retain distinct stable identities and artifact policies.
  custom_model_compatibility("predict.weights", workflow, input_kind, true, std::nullopt, artifact_field_path.view());
};
struct PredictOnnxModelSelection final
    : PredictModelSelection<member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                        &mmltk::backend::models::rfdetr::PredictRequest::onnx_path>,
                            ModelArtifactInputKind::Onnx> {
 inline static constexpr ModelSelectionCompatibility compatibility =
  // CLEANUP-IGNORE: Compatibility rows retain distinct stable identities and artifact policies.
  custom_model_compatibility("predict.onnx", workflow, input_kind, false, std::nullopt, artifact_field_path.view());
};
struct PredictTensorRtModelSelection final
    : PredictModelSelection<member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                        &mmltk::backend::models::rfdetr::PredictRequest::tensorrt_path>,
                            ModelArtifactInputKind::TensorRt> {
 inline static constexpr ModelSelectionCompatibility compatibility =
  custom_model_compatibility("predict.tensorrt", workflow, input_kind, false, std::nullopt, artifact_field_path.view());
};
template <auto Artifact, auto Predicate, ModelArtifactInputKind Input>
struct ExportModelSelection
    : ModelSelectionRelationRow<
       FeatureId::Export, member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state, &ExportViewState::model_source>,
       member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state, &ExportViewState::model_input>,
       member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state, &ExportViewState::preset_name>,
       member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state, &ExportViewState::model_resolution>, Artifact,
       member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state, &ExportViewState::class_layout_path>, Predicate> {
 inline static constexpr ModelArtifactInputKind input_kind = Input;
};
struct ExportWeightsModelSelection final
    : ExportModelSelection<member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state, &ExportViewState::weights_path>,
                           member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state, &ExportViewState::build_tensorrt>,
                           ModelArtifactInputKind::Weights> {
 inline static constexpr ModelSelectionCompatibility compatibility =
  custom_model_compatibility("export.weights", workflow, input_kind, true, false, artifact_field_path.view());
};
struct ExportOnnxModelSelection final
    : ExportModelSelection<member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state, &ExportViewState::onnx_input_path>,
                           member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state, &ExportViewState::build_tensorrt>,
                           ModelArtifactInputKind::Onnx> {
 inline static constexpr ModelSelectionCompatibility compatibility =
  custom_model_compatibility("export.onnx", workflow, input_kind, false, true, artifact_field_path.view());
};
struct ModelSelectionRelation final {
 template <class Visitor>
 static constexpr void VisitRows(Visitor&& visitor) {
  Visit<TrainWeightsModelSelection>(visitor);
  Visit<ValidateWeightsModelSelection>(visitor);
  Visit<ValidateOnnxModelSelection>(visitor);
  Visit<ValidateTensorRtModelSelection>(visitor);
  Visit<PredictWeightsModelSelection>(visitor);
  Visit<PredictOnnxModelSelection>(visitor);
  Visit<PredictTensorRtModelSelection>(visitor);
  Visit<ExportWeightsModelSelection>(visitor);
  Visit<ExportOnnxModelSelection>(visitor);
 }

private:
 template <class Row, class Visitor>
 static constexpr void Visit(Visitor& visitor) {
  static_assert(Row::workflow == Row::compatibility.workflow);
  static_assert(Row::input_kind == Row::compatibility.input);
  static_assert((std::tuple_size_v<decltype(Row::predicate)> == 1U) == Row::compatibility.required_export_build_tensorrt.has_value());
  if constexpr (std::tuple_size_v<decltype(Row::predicate)> != 0U) {
   static_assert(std::same_as<mmltk::frameworks::reflection::accessor_value_t<GuiSettingsState, std::get<0>(Row::predicate)>, bool>);
  }
  static_assert(Row::valid());
  visitor.template operator()<Row>(Row::compatibility);
 }
};
inline constexpr std::array<ModelSelectionCompatibility, 9U> kModelSelectionCompatibility = [] {
 std::array<ModelSelectionCompatibility, 9U> rows{};
 std::size_t index = 0U;
 ModelSelectionRelation::VisitRows([&]<class Row>(const ModelSelectionCompatibility& compatibility) { rows[index++] = compatibility; });
 return rows;
}();
[[nodiscard]] consteval bool model_selection_compatibility_catalog_is_valid() {
 if (kModelSelectionCompatibility.size() != 9U) return false;
 for (std::size_t index = 0U; index < kModelSelectionCompatibility.size(); ++index) {
  const auto& row = kModelSelectionCompatibility[index];
  if (row.key.empty() || (!row.canonical_allowed && !row.custom_allowed) || (row.canonical_allowed && row.input != ModelArtifactInputKind::Weights) ||
      (row.workflow == FeatureId::Export) != row.required_export_build_tensorrt.has_value() || row.input == ModelArtifactInputKind::None ||
      row.artifact_field_path.empty() || row.dialog_title.empty() || row.dialog_filter.empty() || row.dialog_pattern.empty()) {
   return false;
  }
  for (std::size_t sibling = index + 1U; sibling < kModelSelectionCompatibility.size(); ++sibling) {
   const auto& other = kModelSelectionCompatibility[sibling];
   if (row.key == other.key ||
       (row.workflow == other.workflow && row.input == other.input && row.required_export_build_tensorrt == other.required_export_build_tensorrt)) {
    return false;
   }
  }
 }
 return true;
}
static_assert(model_selection_compatibility_catalog_is_valid());
struct ModelSelectionCompatibilityCatalog final {
 using row_type = ModelSelectionCompatibility;
 // CLEANUP-IGNORE: This reflected compatibility catalog and the RF-DETR preset catalog expose the same required
 // catalog protocol over different canonical row schemas.
 static constexpr std::string_view identity = "model.selection.compatibility";
 template <class Visitor>
 static constexpr void VisitRows(Visitor&& visitor) {
  for (std::size_t index = 0U; index < kModelSelectionCompatibility.size(); ++index) visitor(kModelSelectionCompatibility[index], index);
 }
 [[nodiscard]] static constexpr std::string_view row_key(const row_type& row) noexcept { return row.key; }
 [[nodiscard]] static consteval bool valid() noexcept { return model_selection_compatibility_catalog_is_valid(); }
};
[[nodiscard]] constexpr const ModelSelectionCompatibility* find_model_selection_compatibility(const FeatureId workflow,
                                                                                              const ModelArtifactInputKind input) noexcept {
 for (const auto& row : kModelSelectionCompatibility) {
  if (row.workflow == workflow && row.input == input) return &row;
 }
 return nullptr;
}
[[nodiscard]] constexpr const ModelSelectionCompatibility* find_model_selection_compatibility(const FeatureId workflow, const ModelArtifactInputKind input,
                                                                                              const bool export_build_tensorrt) noexcept {
 const auto* row = find_model_selection_compatibility(workflow, input);
 if (row == nullptr) return nullptr;
 return !row->required_export_build_tensorrt.has_value() || *row->required_export_build_tensorrt == export_build_tensorrt ? row : nullptr;
}
[[nodiscard]] constexpr const ModelSelectionCompatibility* find_export_model_selection_compatibility(const bool build_tensorrt) noexcept {
 for (const auto& row : kModelSelectionCompatibility) {
  if (row.workflow == FeatureId::Export && row.required_export_build_tensorrt == build_tensorrt) return &row;
 }
 return nullptr;
}
[[nodiscard]] constexpr bool model_selection_source_allowed(const ModelSelectionCompatibility& compatibility, const ModelSelectionSource source) noexcept {
 switch (source) {
  case ModelSelectionSource::Canonical: return compatibility.canonical_allowed;
  case ModelSelectionSource::Custom: return compatibility.custom_allowed;
 }
 return false;
}
[[nodiscard]] constexpr bool model_selection_compatible(const FeatureId workflow, const ModelSelectionSource source,
                                                        const ModelArtifactInputKind input) noexcept {
 const auto* compatibility = find_model_selection_compatibility(workflow, input);
 return compatibility != nullptr && model_selection_source_allowed(*compatibility, source);
}
[[nodiscard]] constexpr bool model_selection_compatible(const FeatureId workflow, const ModelSelectionSource source, const ModelArtifactInputKind input,
                                                        const bool export_build_tensorrt) noexcept {
 const auto* compatibility = find_model_selection_compatibility(workflow, input, export_build_tensorrt);
 return compatibility != nullptr && model_selection_source_allowed(*compatibility, source);
}
[[nodiscard]] constexpr bool model_selection_workflow_supported(const FeatureId workflow) noexcept {
 for (const auto& compatibility : kModelSelectionCompatibility) {
  if (compatibility.workflow == workflow) return true;
 }
 return false;
}
// Select the first predicate-compatible row for shared draft fields, even when
// input is None. Only a matching input contributes an artifact or compatibility.
[[nodiscard]] inline std::optional<ModelSettingsProjection> model_settings_projection(const GuiSettingsState& settings, const FeatureId workflow) {
 std::optional<ModelSettingsProjection> result;
 ModelSelectionRelation::VisitRows([&]<class Row>(const ModelSelectionCompatibility& row) {
  if (row.workflow != workflow || !Row::predicate_matches(settings, row)) return;
  if (!result) {
   result.emplace();
   Row::key_relation::Project(settings, result->key);
   Row::workflow_destination(result->key) = Row::workflow;
   if constexpr (std::tuple_size_v<decltype(Row::predicate)> != 0U) result->export_build_tensorrt = std::get<0>(Row::predicate)(settings);
  }
  if (Row::input(settings) != row.input) return;
  Row::artifact_relation::Project(settings, *result);
  result->compatible = model_selection_source_allowed(row, result->key.source);
 });
 return result;
}
}  // namespace mmltk::controller::contracts
