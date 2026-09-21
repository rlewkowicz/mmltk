#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::models::rfdetr {
enum class ModelTask : std::uint8_t {
 Detection,
 Segmentation,
};
MMLTK_REFLECT_ENUM(ModelTask)
[[nodiscard]] constexpr std::optional<std::string_view> model_task_name(const ModelTask task) noexcept {
 switch (task) {
  case ModelTask::Detection: return "detection";
  case ModelTask::Segmentation: return "segmentation";
 }
 return std::nullopt;
}
struct PresetCatalogEntry final {
 std::string_view preset_name;
 std::string_view display_name;
 std::string_view size_label;
 ModelTask task;
 std::uint32_t resolution;
 std::string_view encoder;
 std::string_view canonical_weight_filename;
 std::string_view canonical_weight_url;
 std::string_view canonical_weight_md5;
 std::uint8_t patch_size;
 std::uint8_t window_count;
 std::uint8_t positional_encoding_size;
 std::uint8_t decoder_layer_count;
 std::uint16_t query_count;
 std::uint16_t selected_query_count;
 std::uint16_t class_count;
 std::uint16_t hidden_dimension;
 std::uint8_t group_count;
 bool two_stage;
 double classification_loss_coefficient;
 double bounding_box_loss_coefficient;
 double generalized_iou_loss_coefficient;
 double mask_cross_entropy_loss_coefficient;
 double mask_dice_loss_coefficient;
 constexpr bool operator==(const PresetCatalogEntry&) const noexcept = default;
};
MMLTK_REFLECT_FIELDS(PresetCatalogEntry)
inline constexpr std::array<PresetCatalogEntry, 10U> kPresetCatalog{{
 {"rf-detr-nano",
  "RF-DETR Nano",
  "N",
  ModelTask::Detection,
  384U,
  "dinov2_windowed_small",
  "rf-detr-nano.pth",
  "https://storage.googleapis.com/rfdetr/nano_coco/checkpoint_best_regular.pth",
  "fb6504cce7fbdc783f7a46991f07639f",
  16U,
  2U,
  24U,
  2U,
  300U,
  300U,
  91U,
  256U,
  13U,
  true,
  1.0,
  5.0,
  2.0,
  1.0,
  1.0},
 {"rf-detr-small",
  "RF-DETR Small",
  "S",
  ModelTask::Detection,
  512U,
  "dinov2_windowed_small",
  "rf-detr-small.pth",
  "https://storage.googleapis.com/rfdetr/small_coco/checkpoint_best_regular.pth",
  "fb37061c1af7bace359c91b723a8d5c1",
  16U,
  2U,
  32U,
  3U,
  300U,
  300U,
  91U,
  256U,
  13U,
  true,
  1.0,
  5.0,
  2.0,
  1.0,
  1.0},
 {"rf-detr-medium",
  "RF-DETR Medium",
  "M",
  ModelTask::Detection,
  576U,
  "dinov2_windowed_small",
  "rf-detr-medium.pth",
  "https://storage.googleapis.com/rfdetr/medium_coco/checkpoint_best_regular.pth",
  "7223f764a87b863f02eb8d52bf0ce2ee",
  16U,
  2U,
  36U,
  4U,
  300U,
  300U,
  91U,
  256U,
  13U,
  true,
  1.0,
  5.0,
  2.0,
  1.0,
  1.0},
 {"rf-detr-large", "RF-DETR Large", "L", ModelTask::Detection, 704U, "dinov2_windowed_small", "rf-detr-large-2026.pth",
  "https://storage.googleapis.com/rfdetr/rf-detr-large-2026.pth", "5cb72153541cbcb9aa6efa26222acc75", 16U, 2U, 44U,
  // CLEANUP-IGNORE: Each immutable preset row remains a complete independently audited catalog record.
  4U, 300U, 300U, 91U, 256U, 13U, true, 1.0, 5.0, 2.0, 1.0, 1.0},
 {"rf-detr-seg-nano",
  "RF-DETR Seg Nano",
  "N",
  ModelTask::Segmentation,
  312U,
  "dinov2_windowed_small",
  "rf-detr-seg-nano.pt",
  "https://storage.googleapis.com/rfdetr/rf-detr-seg-n-ft.pth",
  "9995497791d0ff1664a1d9ddee9cfd20",
  12U,
  1U,
  26U,
  4U,
  100U,
  100U,
  91U,
  256U,
  13U,
  true,
  5.0,
  5.0,
  2.0,
  5.0,
  5.0},
 {"rf-detr-seg-small", "RF-DETR Seg Small", "S", ModelTask::Segmentation, 384U, "dinov2_windowed_small", "rf-detr-seg-small.pt",
  "https://storage.googleapis.com/rfdetr/rf-detr-seg-s-ft.pth", "0a2a3006381d0c42853907e700eadd08", 12U, 2U, 32U,
  // CLEANUP-IGNORE: Each immutable preset row remains a complete independently audited catalog record.
  4U, 100U, 100U, 91U, 256U, 13U, true, 5.0, 5.0, 2.0, 5.0, 5.0},
 {"rf-detr-seg-medium",
  "RF-DETR Seg Medium",
  "M",
  ModelTask::Segmentation,
  432U,
  "dinov2_windowed_small",
  "rf-detr-seg-medium.pt",
  "https://storage.googleapis.com/rfdetr/rf-detr-seg-m-ft.pth",
  "a49af1562c3719227ad43d0ca53b4c7a",
  12U,
  2U,
  36U,
  5U,
  200U,
  200U,
  91U,
  256U,
  13U,
  true,
  5.0,
  5.0,
  2.0,
  5.0,
  5.0},
 {"rf-detr-seg-large", "RF-DETR Seg Large", "L", ModelTask::Segmentation, 504U, "dinov2_windowed_small", "rf-detr-seg-large.pt",
  "https://storage.googleapis.com/rfdetr/rf-detr-seg-l-ft.pth", "275f7b094909544ed2841c94a677d07e", 12U, 2U, 42U,
  // CLEANUP-IGNORE: Each immutable preset row remains a complete independently audited catalog record.
  5U, 200U, 200U, 91U, 256U, 13U, true, 5.0, 5.0, 2.0, 5.0, 5.0},
 {"rf-detr-seg-xlarge",
  "RF-DETR Seg XLarge",
  "XL",
  ModelTask::Segmentation,
  624U,
  "dinov2_windowed_small",
  "rf-detr-seg-xlarge.pt",
  "https://storage.googleapis.com/rfdetr/rf-detr-seg-xl-ft.pth",
  "3693b35d0eea86ebb3e0444f4a611fba",
  12U,
  2U,
  52U,
  6U,
  300U,
  300U,
  91U,
  256U,
  13U,
  true,
  5.0,
  5.0,
  2.0,
  5.0,
  5.0},
 {"rf-detr-seg-xxlarge", "RF-DETR Seg 2XLarge", "2XL", ModelTask::Segmentation, 768U, "dinov2_windowed_small", "rf-detr-seg-xxlarge.pt",
  "https://storage.googleapis.com/rfdetr/rf-detr-seg-2xl-ft.pth", "040bc3412af840fa8a47e0ff69b552ba", 12U, 2U, 64U,
  // CLEANUP-IGNORE: Each immutable preset row remains a complete independently audited catalog record.
  6U, 300U, 300U, 91U, 256U, 13U, true, 5.0, 5.0, 2.0, 5.0, 5.0},
}};
[[nodiscard]] consteval bool preset_catalog_is_valid() {
 for (std::size_t index = 0U; index < kPresetCatalog.size(); ++index) {
  const auto& preset = kPresetCatalog[index];
  if (preset.preset_name.empty() || preset.display_name.empty() || preset.size_label.empty() || preset.resolution == 0U ||
      !model_task_name(preset.task).has_value() || preset.resolution > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || preset.encoder.empty() ||
      preset.canonical_weight_filename.empty() || preset.canonical_weight_url.empty() || preset.canonical_weight_md5.size() != 32U || preset.patch_size == 0U ||
      preset.window_count == 0U || preset.positional_encoding_size == 0U || preset.decoder_layer_count == 0U || preset.query_count == 0U ||
      preset.selected_query_count == 0U || preset.class_count == 0U || preset.hidden_dimension == 0U || preset.group_count == 0U) {
   return false;
  }
  for (std::size_t sibling = index + 1U; sibling < kPresetCatalog.size(); ++sibling) {
   if (preset.preset_name == kPresetCatalog[sibling].preset_name || preset.canonical_weight_filename == kPresetCatalog[sibling].canonical_weight_filename) {
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
[[nodiscard]] constexpr const PresetCatalogEntry* find_preset_catalog_entry(const std::string_view preset_name) noexcept {
 for (const auto& preset : kPresetCatalog) {
  if (preset.preset_name == preset_name) return &preset;
 }
 return nullptr;
}
}  // namespace mmltk::backend::models::rfdetr
