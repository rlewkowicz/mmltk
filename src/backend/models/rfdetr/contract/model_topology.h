#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::models::rfdetr {
enum class ModelTask : std::uint8_t {
 Detection,
 Segmentation,
 Keypoints,
};
MMLTK_REFLECT_ENUM(ModelTask)
[[nodiscard]] constexpr std::optional<std::string_view> model_task_name(const ModelTask task) noexcept {
 switch (task) {
  case ModelTask::Detection: return "detection";
  case ModelTask::Segmentation: return "segmentation";
  case ModelTask::Keypoints: return "keypoints";
 }
 return std::nullopt;
}
enum class BackboneKind : std::uint8_t { DinoV2Small, DinoV2Base, PeCoreTiny };
enum class ProjectorKind : std::uint8_t { Single, DualKeypointOnly };
enum class KeypointHeadKind : std::uint8_t { None, GroupPose };
MMLTK_REFLECT_ENUM(BackboneKind)
MMLTK_REFLECT_ENUM(ProjectorKind)
MMLTK_REFLECT_ENUM(KeypointHeadKind)
struct BackboneConfig final {
 BackboneKind kind = BackboneKind::DinoV2Small;
 std::uint16_t dimension = 384U;
 std::uint16_t feedforward_dimension = 1536U;
 std::uint8_t heads = 6U;
 std::uint8_t layers = 12U;
 std::array<std::uint16_t, 4U> features{3U, 6U, 9U, 12U};
 constexpr bool operator==(const BackboneConfig&) const noexcept = default;
};
MMLTK_REFLECT_FIELDS(BackboneConfig)
[[nodiscard]] constexpr bool backbone_config_valid(const BackboneConfig& config) noexcept {
 if (!mmltk::frameworks::reflection::enum_contains(config.kind) || config.dimension == 0U || config.feedforward_dimension == 0U ||
     config.heads == 0U || config.dimension % config.heads != 0U || config.layers == 0U) return false;
 const bool zero_based = config.kind == BackboneKind::PeCoreTiny;
 for (std::size_t index = 0U; index < config.features.size(); ++index) {
  const auto feature = config.features[index];
  if ((zero_based ? feature >= config.layers : feature == 0U || feature > config.layers) || (index != 0U && feature <= config.features[index - 1U])) return false;
 }
 return true;
}
[[nodiscard]] constexpr std::string_view backbone_encoder_name(BackboneKind kind) noexcept {
 switch (kind) {
  case BackboneKind::DinoV2Small: return "dinov2_windowed_small";
  case BackboneKind::DinoV2Base: return "dinov2_windowed_base";
  case BackboneKind::PeCoreTiny: return "pe_core_t";
 }
 return {};
}
}  // namespace mmltk::backend::models::rfdetr
