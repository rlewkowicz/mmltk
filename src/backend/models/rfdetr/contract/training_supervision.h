#pragma once
#include "src/frameworks/reflection/declaration_annotations.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::models::rfdetr {
enum class TrainAssignmentKind : std::uint8_t {
 Hungarian,
 MatchFree,
};
MMLTK_REFLECT_ENUM(TrainAssignmentKind)
[[nodiscard]] constexpr std::string_view cli_enum_spelling(const TrainAssignmentKind assignment) noexcept {
 switch (assignment) {
  case TrainAssignmentKind::Hungarian: return "hungarian";
  case TrainAssignmentKind::MatchFree: return "match-free";
 }
 return {};
}
[[nodiscard]] constexpr std::optional<TrainAssignmentKind> train_assignment_from_spelling(const std::string_view spelling) noexcept {
 if (spelling == cli_enum_spelling(TrainAssignmentKind::Hungarian)) return TrainAssignmentKind::Hungarian;
 if (spelling == cli_enum_spelling(TrainAssignmentKind::MatchFree)) return TrainAssignmentKind::MatchFree;
 return std::nullopt;
}
inline constexpr float kSupervisionOpenUnitMinimum = std::numeric_limits<float>::denorm_min();
inline constexpr float kSupervisionOpenUnitMaximum = std::nextafter(1.0F, 0.0F);
inline constexpr std::uint16_t kMaximumDenoisingGroups = 64U;
struct MatchFreeSupervisionConfig {
 MMLTK_MINIMUM(float, kSupervisionOpenUnitMinimum) MMLTK_MAXIMUM(float, kSupervisionOpenUnitMaximum) MMLTK_FINITE float rho = 0.5F;
 MMLTK_MINIMUM(float, 0.0F) MMLTK_FINITE float correspondence_weight = 1.0F;
 MMLTK_MINIMUM(float, 0.0F) MMLTK_FINITE float query_weight = 1.0F;
 constexpr bool operator==(const MatchFreeSupervisionConfig&) const noexcept = default;
};
struct DenoisingSupervisionConfig {
 bool enabled = false;
 MMLTK_MINIMUM(std::uint16_t, 1U) MMLTK_MAXIMUM(std::uint16_t, kMaximumDenoisingGroups) std::uint16_t groups = 5U;
 MMLTK_MINIMUM(float, 0.0F) MMLTK_MAXIMUM(float, 1.0F) MMLTK_FINITE float label_noise_ratio = 0.2F;
 MMLTK_MINIMUM(float, kSupervisionOpenUnitMinimum) MMLTK_MAXIMUM(float, kSupervisionOpenUnitMaximum) MMLTK_FINITE float center_noise_scale = 0.4F;
 MMLTK_MINIMUM(float, kSupervisionOpenUnitMinimum) MMLTK_MAXIMUM(float, kSupervisionOpenUnitMaximum) MMLTK_FINITE float size_noise_scale = 0.4F;
 constexpr bool operator==(const DenoisingSupervisionConfig&) const noexcept = default;
};
struct TrainingSupervisionConfig {
 TrainAssignmentKind assignment = TrainAssignmentKind::Hungarian;
 MatchFreeSupervisionConfig match_free;
 DenoisingSupervisionConfig denoising;
 constexpr bool operator==(const TrainingSupervisionConfig&) const noexcept = default;
};
MMLTK_REFLECT_FIELDS(MatchFreeSupervisionConfig)
MMLTK_REFLECT_FIELDS(DenoisingSupervisionConfig)
MMLTK_REFLECT_FIELDS(TrainingSupervisionConfig)
static_assert(mmltk::frameworks::reflection::reflected_defaults_are_valid<TrainingSupervisionConfig>());
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<TrainingSupervisionConfig>());
[[nodiscard]] constexpr bool training_supervision_config_valid(const TrainingSupervisionConfig& config) noexcept {
 const bool selected_objective = config.assignment != TrainAssignmentKind::MatchFree || config.match_free.correspondence_weight > 0.0F || config.match_free.query_weight > 0.0F;
 return !mmltk::frameworks::reflection::validate_reflected_fields(config).has_value() && mmltk::frameworks::reflection::enum_contains(config.assignment) && selected_objective;
}
[[nodiscard]] constexpr bool training_supervision_enabled(const TrainingSupervisionConfig& config) noexcept { return config.assignment == TrainAssignmentKind::MatchFree || config.denoising.enabled; }
[[nodiscard]] constexpr bool training_supervision_query_layout_valid(
 const TrainingSupervisionConfig& config, const std::size_t ordinary_queries, const std::size_t ordinary_groups, const std::size_t maximum_targets) noexcept {
 if (ordinary_groups != 0U && ordinary_queries > std::numeric_limits<std::size_t>::max() / ordinary_groups) return false;
 const std::size_t ordinary_count = ordinary_queries * ordinary_groups;
 const std::size_t denoising_groups = config.denoising.enabled ? config.denoising.groups : 0U;
 if (denoising_groups != 0U && maximum_targets > std::numeric_limits<std::size_t>::max() / denoising_groups) return false;
 const std::size_t denoising_count = denoising_groups * maximum_targets;
 return denoising_count <= std::numeric_limits<std::size_t>::max() - ordinary_count;
}
}  // namespace mmltk::backend::models::rfdetr
