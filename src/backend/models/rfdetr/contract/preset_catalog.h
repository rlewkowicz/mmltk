#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>
#include "src/backend/models/rfdetr/contract/model_topology.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/backend/models/rfdetr/contract/weight_catalog.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::models::rfdetr {
enum class ModelLicense : std::uint8_t { Apache20, PlatformModel10 };
MMLTK_REFLECT_ENUM(ModelLicense)
struct PresetCatalogEntry final {
 std::string_view preset_name;
 std::string_view display_name;
 std::string_view size_label;
 ModelTask task;
 std::uint32_t resolution;
 BackboneConfig backbone;
 std::string_view canonical_weight_filename;
 std::string_view canonical_weight_url;
 std::string_view canonical_weight_md5;
 std::uint8_t patch_size;
 std::uint8_t window_count;
 std::uint8_t positional_encoding_size;
 std::uint8_t decoder_layer_count;
 std::uint16_t query_count;
 std::uint16_t selected_query_count;
 // Hash-bound official checkpoint output width, independent of constructor defaults.
 std::uint16_t class_count;
 std::uint16_t hidden_dimension;
 std::uint8_t group_count;
 bool two_stage;
 double classification_loss_coefficient;
 double bounding_box_loss_coefficient;
 double generalized_iou_loss_coefficient;
 double mask_cross_entropy_loss_coefficient;
 double mask_dice_loss_coefficient;
 std::string_view upstream_class;
 std::uint16_t configured_class_count = 90U;
 std::uint8_t self_attention_heads = 8U;
 std::uint8_t cross_attention_heads = 16U;
 std::uint8_t sampling_points = 2U;
 std::uint16_t feedforward_dimension = 2048U;
 ModelLicense license = ModelLicense::Apache20;
 OfficialClassLayout official_class_layout = OfficialClassLayout::CocoSparse;
 ProjectorKind projector = ProjectorKind::Single;
 KeypointHeadKind keypoint_head = KeypointHeadKind::None;
 std::uint8_t keypoint_dimension_downscale = 1U;
 bool keypoint_cross_attention = true;
 bool inter_instance_keypoint_attention = false;
 std::uint16_t default_person_keypoints = 0U;
 double keypoint_l1_loss_coefficient = 0.0;
 double keypoint_findable_loss_coefficient = 0.0;
 double keypoint_visible_loss_coefficient = 0.0;
 double keypoint_nll_loss_coefficient = 0.0;
 PresetTrainingCapabilities training_capabilities{};
 constexpr bool operator==(const PresetCatalogEntry&) const noexcept = default;
};
MMLTK_REFLECT_FIELDS(PresetCatalogEntry)
inline constexpr auto kPresetCatalog = [] {
 std::array<PresetCatalogEntry, 16U> presets{{
  {"rf-detr-nano", "RF-DETR Nano", "N", ModelTask::Detection, 384U, {}, "rf-detr-nano.pth", "https://repo.roboflow.com/rfdetr/nano_coco/checkpoint_best_regular.pth",
   "fb6504cce7fbdc783f7a46991f07639f", 16U, 2U, 24U, 2U, 300U, 300U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0, "RFDETRNano"},
  {"rf-detr-small", "RF-DETR Small", "S", ModelTask::Detection, 512U, {}, "rf-detr-small.pth", "https://repo.roboflow.com/rfdetr/small_coco/checkpoint_best_regular.pth",
   "fb37061c1af7bace359c91b723a8d5c1", 16U, 2U, 32U, 3U, 300U, 300U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0, "RFDETRSmall"},
  {"rf-detr-medium", "RF-DETR Medium", "M", ModelTask::Detection, 576U, {}, "rf-detr-medium.pth", "https://repo.roboflow.com/rfdetr/medium_coco/checkpoint_best_regular.pth",
   "7223f764a87b863f02eb8d52bf0ce2ee", 16U, 2U, 36U, 4U, 300U, 300U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0, "RFDETRMedium"},
  {"rf-detr-large", "RF-DETR Large", "L", ModelTask::Detection, 704U, {}, "rf-detr-large-2026.pth", "https://repo.roboflow.com/rfdetr/rf-detr-large-2026.pth", "5cb72153541cbcb9aa6efa26222acc75", 16U,
   2U, 44U,
   // CLEANUP-IGNORE: Each immutable preset row remains a complete independently audited catalog record.
   4U, 300U, 300U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0, "RFDETRLarge"},
  {"rf-detr-seg-nano", "RF-DETR Seg Nano", "N", ModelTask::Segmentation, 312U, {}, "rf-detr-seg-nano.pt", "https://repo.roboflow.com/rfdetr/rf-detr-seg-n-ft.pth", "9995497791d0ff1664a1d9ddee9cfd20",
   12U, 1U, 26U, 4U, 100U, 100U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 5.0, 5.0, "RFDETRSegNano"},
  {"rf-detr-seg-small", "RF-DETR Seg Small", "S", ModelTask::Segmentation, 384U, {}, "rf-detr-seg-small.pt", "https://repo.roboflow.com/rfdetr/rf-detr-seg-s-ft.pth",
   "0a2a3006381d0c42853907e700eadd08", 12U, 2U, 32U,
   // CLEANUP-IGNORE: Each immutable preset row remains a complete independently audited catalog record.
   4U, 100U, 100U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 5.0, 5.0, "RFDETRSegSmall"},
  {"rf-detr-seg-medium", "RF-DETR Seg Medium", "M", ModelTask::Segmentation, 432U, {}, "rf-detr-seg-medium.pt", "https://repo.roboflow.com/rfdetr/rf-detr-seg-m-ft.pth",
   "a49af1562c3719227ad43d0ca53b4c7a", 12U, 2U, 36U, 5U, 200U, 200U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 5.0, 5.0, "RFDETRSegMedium"},
  {"rf-detr-seg-large", "RF-DETR Seg Large", "L", ModelTask::Segmentation, 504U, {}, "rf-detr-seg-large.pt", "https://repo.roboflow.com/rfdetr/rf-detr-seg-l-ft.pth",
   "275f7b094909544ed2841c94a677d07e", 12U, 2U, 42U,
   // CLEANUP-IGNORE: Each immutable preset row remains a complete independently audited catalog record.
   5U, 200U, 200U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 5.0, 5.0, "RFDETRSegLarge"},
  {"rf-detr-seg-xlarge", "RF-DETR Seg XLarge", "XL", ModelTask::Segmentation, 624U, {}, "rf-detr-seg-xlarge.pt", "https://repo.roboflow.com/rfdetr/rf-detr-seg-xl-ft.pth",
   "3693b35d0eea86ebb3e0444f4a611fba", 12U, 2U, 52U, 6U, 300U, 300U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 5.0, 5.0, "RFDETRSegXLarge"},
  {"rf-detr-seg-xxlarge", "RF-DETR Seg 2XLarge", "2XL", ModelTask::Segmentation, 768U, {}, "rf-detr-seg-xxlarge.pt", "https://repo.roboflow.com/rfdetr/rf-detr-seg-2xl-ft.pth",
   "040bc3412af840fa8a47e0ff69b552ba", 12U, 2U, 64U,
   // CLEANUP-IGNORE: Each immutable preset row remains a complete independently audited catalog record.
   6U, 300U, 300U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 5.0, 5.0, "RFDETRSeg2XLarge"},
  {"rf-detr-atto", "RF-DETR Atto", "Atto", ModelTask::Detection, 380U, {BackboneKind::PeCoreTiny, 192U, 768U, 3U, 12U, {2U, 5U, 8U, 11U}}, "rf-detr-atto.pth",
   "https://storage.googleapis.com/rfdetr/platform-licensed/rf-detr-atto.pth", "89e793e6246ac5808e2f2769ae9b4fe0", 20U, 1U, 19U, 0U, 300U, 300U, 91U, 128U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0,
   "RFDETRAtto", 90U, 4U, 8U, 2U, 1024U, ModelLicense::PlatformModel10},
  {"rf-detr-femto", "RF-DETR Femto", "Femto", ModelTask::Detection, 384U, {BackboneKind::PeCoreTiny, 192U, 768U, 3U, 12U, {2U, 5U, 8U, 11U}}, "rf-detr-femto.pth",
   "https://storage.googleapis.com/rfdetr/platform-licensed/rf-detr-femto.pth", "f1265b9cf62c8d30ae94918b794ac69e", 16U, 2U, 24U, 2U, 200U, 200U, 91U, 128U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0,
   "RFDETRFemto", 90U, 4U, 8U, 2U, 1024U, ModelLicense::PlatformModel10},
  {"rf-detr-pico", "RF-DETR Pico", "Pico", ModelTask::Detection, 560U, {BackboneKind::PeCoreTiny, 192U, 768U, 3U, 12U, {2U, 5U, 8U, 11U}}, "rf-detr-pico.pth",
   "https://storage.googleapis.com/rfdetr/platform-licensed/rf-detr-pico.pth", "ab8da50b62f1f6b3ed234e1292139b72", 20U, 2U, 28U, 3U, 200U, 200U, 91U, 128U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0,
   "RFDETRPico", 90U, 4U, 8U, 2U, 1024U, ModelLicense::PlatformModel10},
  {"rf-detr-xlarge", "RF-DETR XLarge", "XLarge", ModelTask::Detection, 700U, {BackboneKind::DinoV2Base, 768U, 3072U, 12U, 12U, {3U, 6U, 9U, 12U}}, "rf-detr-xlarge.pth",
   "https://repo.roboflow.com/rfdetr/platform-licensed/rf-detr-xlarge.pth", "6ddf834f2bc5bed3214a82f9b0aaeed7", 20U, 1U, 35U, 5U, 300U, 300U, 91U, 512U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0,
   "RFDETRXLarge", 365U, 16U, 32U, 4U, 2048U, ModelLicense::PlatformModel10},
  {"rf-detr-xxlarge", "RF-DETR 2XLarge", "2XLarge", ModelTask::Detection, 880U, {BackboneKind::DinoV2Base, 768U, 3072U, 12U, 12U, {3U, 6U, 9U, 12U}}, "rf-detr-xxlarge.pth",
   "https://repo.roboflow.com/rfdetr/platform-licensed/rf-detr-xxlarge.pth", "e3204689c1f0280427e4c33e6a2ac6cd", 20U, 2U, 44U, 5U, 300U, 300U, 91U, 512U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0,
   "RFDETR2XLarge", 365U, 16U, 32U, 4U, 2048U, ModelLicense::PlatformModel10},
  {"rf-detr-keypoint-preview", "RF-DETR Keypoint Preview", "Preview", ModelTask::Keypoints, 576U, {}, "rf-detr-keypoint-preview-xlarge.pth",
   "https://repo.roboflow.com/rfdetr/rf-detr-keypoint-preview-xlarge.pth", "6de511943ee85a547d4c5cb527daf0eb", 12U, 2U, 48U, 4U, 100U, 100U, 2U, 256U, 13U, true, 2.0, 5.0, 2.0, 1.0, 1.0,
   "RFDETRKeypointPreview", 90U, 8U, 16U, 2U, 2048U, ModelLicense::Apache20, OfficialClassLayout::PersonBackgroundFirst, ProjectorKind::DualKeypointOnly, KeypointHeadKind::GroupPose, 1U, true, false,
   17U, 1.0, 1.0, 1.0, 1.0},
 }};
 for (auto& preset : presets) { preset.training_capabilities = model_training_capabilities(preset.task, preset.decoder_layer_count); }
 return presets;
}();
[[nodiscard]] consteval bool preset_catalog_is_valid() {
 for (std::size_t index = 0U; index < kPresetCatalog.size(); ++index) {
  const auto& preset = kPresetCatalog[index];
  if (preset.preset_name.empty() || preset.display_name.empty() || preset.size_label.empty() || preset.resolution == 0U || !model_task_name(preset.task).has_value() ||
      preset.resolution > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || backbone_encoder_name(preset.backbone.kind).empty() || preset.canonical_weight_filename.empty() ||
      preset.canonical_weight_url.empty() || preset.canonical_weight_md5.size() != 32U || preset.patch_size == 0U || preset.window_count == 0U || preset.positional_encoding_size == 0U ||
      (preset.decoder_layer_count == 0U && (!preset.two_stage || preset.task != ModelTask::Detection)) || preset.query_count == 0U || preset.selected_query_count == 0U ||
      preset.selected_query_count > preset.query_count || preset.class_count == 0U || preset.hidden_dimension == 0U || preset.group_count == 0U) {
   return false;
  }
  if (preset.upstream_class.empty() || preset.self_attention_heads == 0U || preset.cross_attention_heads == 0U || preset.feedforward_dimension == 0U ||
      preset.hidden_dimension % preset.self_attention_heads != 0U || preset.hidden_dimension % preset.cross_attention_heads != 0U || !backbone_config_valid(preset.backbone) ||
      preset.resolution % (static_cast<std::uint32_t>(preset.patch_size) * preset.window_count) != 0U ||
      (preset.task == ModelTask::Keypoints) != (preset.keypoint_head == KeypointHeadKind::GroupPose) ||
      (preset.task == ModelTask::Keypoints) != (preset.projector == ProjectorKind::DualKeypointOnly) || (preset.task == ModelTask::Keypoints) != (preset.default_person_keypoints != 0U))
   return false;
  for (const auto coefficient :
   {preset.classification_loss_coefficient, preset.bounding_box_loss_coefficient, preset.generalized_iou_loss_coefficient, preset.mask_cross_entropy_loss_coefficient,
    preset.mask_dice_loss_coefficient, preset.keypoint_l1_loss_coefficient, preset.keypoint_findable_loss_coefficient, preset.keypoint_visible_loss_coefficient, preset.keypoint_nll_loss_coefficient})
   if (!std::isfinite(coefficient) || coefficient < 0.0) return false;
  for (std::size_t sibling = index + 1U; sibling < kPresetCatalog.size(); ++sibling) {
   if (preset.preset_name == kPresetCatalog[sibling].preset_name || preset.canonical_weight_filename == kPresetCatalog[sibling].canonical_weight_filename ||
       preset.upstream_class == kPresetCatalog[sibling].upstream_class) {
    return false;
   }
  }
 }
 return true;
}
static_assert(preset_catalog_is_valid());
struct RfdetrPresetCatalog final {
 using row_type = PresetCatalogEntry;
 static constexpr std::string_view identity = "rfdetr.presets";
 template <class Visitor>
 static constexpr void VisitRows(Visitor&& visitor) {
  for (std::size_t index = 0U; index < kPresetCatalog.size(); ++index) visitor(kPresetCatalog[index], index);
 }
 [[nodiscard]] static constexpr std::string_view row_key(const row_type& row) noexcept { return row.preset_name; }
 [[nodiscard]] static consteval bool valid() noexcept { return preset_catalog_is_valid(); }
};
[[nodiscard]] constexpr const PresetCatalogEntry* find_preset_catalog_entry(std::string_view preset_name) noexcept {
 if (preset_name == "rf-detr-2xlarge") preset_name = "rf-detr-xxlarge";
 if (preset_name == "rf-detr-seg-2xlarge") preset_name = "rf-detr-seg-xxlarge";
 for (const auto& preset : kPresetCatalog) {
  if (preset.preset_name == preset_name || preset.upstream_class == preset_name) return &preset;
 }
 return nullptr;
}
}  // namespace mmltk::backend::models::rfdetr
