#pragma once
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include "src/backend/models/rfdetr/contract/model_topology.h"
#include "src/frameworks/reflection/reflected_declarations.h"
#include "src/frameworks/serialization/cbor_wire.h"
namespace mmltk::backend::models::rfdetr::detail::training_identity_v1 {
// Persisted identity values from application 1c13fec8b04036661a4028b8807d0e1cff42407d:
// contract/model_config.h, contract/training_supervision.h, contract/class_layout.h,
// data/catalog/class_catalog.h; encoding in training/train.cpp and core/class_layout.cpp.
// These declarations are serialization-only. Their fields, order, scalar types,
// enum spellings and limits must not follow changes to the execution vocabulary.
inline constexpr mmltk::frameworks::serialization::wire::Limits kSupervisionLimits{.max_bytes = 4096U, .max_items = 128U, .max_depth = 16U};
inline constexpr mmltk::frameworks::serialization::wire::Limits kClassLayoutLimits{.max_bytes = 1024U * 1024U, .max_items = 1024U * 1024U, .max_depth = 64U};
inline constexpr float kOpenUnitMinimum = std::numeric_limits<float>::denorm_min();
inline constexpr float kOpenUnitMaximum = std::nextafter(1.0F, 0.0F);
enum class TrainAssignmentKind : std::uint8_t { Hungarian, MatchFree };
enum class ClassSlotRole : std::uint8_t { Foreground, Background, Unused, Unresolved };
enum class ClassScoreEncoding : std::uint8_t { SigmoidLogits, SoftmaxLogits };
enum class NoObjectEncoding : std::uint8_t { AllNegative, ExplicitBackground, Unspecified };
enum class ClassLayoutOrigin : std::uint8_t { Unresolved, NativeTraining, Embedded, DigestDescriptor, VerifiedAsset };
MMLTK_REFLECT_ENUM(TrainAssignmentKind)
MMLTK_REFLECT_ENUM(ClassSlotRole)
MMLTK_REFLECT_ENUM(ClassScoreEncoding)
MMLTK_REFLECT_ENUM(NoObjectEncoding)
MMLTK_REFLECT_ENUM(ClassLayoutOrigin)
struct MatchFreeSupervisionConfig final {
 MMLTK_MINIMUM(float, kOpenUnitMinimum) MMLTK_MAXIMUM(float, kOpenUnitMaximum) MMLTK_FINITE float rho = 0.5F;
 MMLTK_MINIMUM(float, 0.0F) MMLTK_FINITE float correspondence_weight = 1.0F;
 MMLTK_MINIMUM(float, 0.0F) MMLTK_FINITE float query_weight = 1.0F;
};
struct DenoisingSupervisionConfig final {
 bool enabled = false;
 MMLTK_MINIMUM(std::uint16_t, 1U) MMLTK_MAXIMUM(std::uint16_t, 64U) std::uint16_t groups = 5U;
 MMLTK_MINIMUM(float, 0.0F) MMLTK_MAXIMUM(float, 1.0F) MMLTK_FINITE float label_noise_ratio = 0.2F;
 MMLTK_MINIMUM(float, kOpenUnitMinimum) MMLTK_MAXIMUM(float, kOpenUnitMaximum) MMLTK_FINITE float center_noise_scale = 0.4F;
 MMLTK_MINIMUM(float, kOpenUnitMinimum) MMLTK_MAXIMUM(float, kOpenUnitMaximum) MMLTK_FINITE float size_noise_scale = 0.4F;
};
struct TrainingSupervisionConfig final {
 TrainAssignmentKind assignment = TrainAssignmentKind::Hungarian;
 MatchFreeSupervisionConfig match_free;
 DenoisingSupervisionConfig denoising;
};
MMLTK_REFLECT_FIELDS(MatchFreeSupervisionConfig)
MMLTK_REFLECT_FIELDS(DenoisingSupervisionConfig)
MMLTK_REFLECT_FIELDS(TrainingSupervisionConfig)
struct Configuration final {
 std::string preset_name;
 int resolution = 0;
 bool segmentation = false;
 int num_classes = 0;
 int num_queries = 0;
 int num_select = 0;
 int dec_layers = 0;
 int group_detr = 1;
 bool two_stage = true;
 int hidden_dim = 256;
 int patch_size = 16;
 int num_windows = 2;
 int positional_encoding_size = 24;
 int sa_nheads = 8;
 int ca_nheads = 16;
 int dec_n_points = 2;
 int dim_feedforward = 2048;
 bool bbox_reparam = true;
 bool lite_refpoint_refine = true;
 double cls_loss_coef = 1.0;
 double bbox_loss_coef = 5.0;
 double giou_loss_coef = 2.0;
 double mask_ce_loss_coef = 1.0;
 double mask_dice_loss_coef = 1.0;
 bool sum_group_losses = false;
 bool use_varifocal_loss = false;
 bool use_position_supervised_loss = false;
 bool ia_bce_loss = true;
 bool aux_loss = true;
 std::int64_t mask_point_sample_ratio = 16;
 double focal_alpha = 0.25;
 double set_cost_class = 2.0;
 double set_cost_bbox = 5.0;
 double set_cost_giou = 2.0;
 TrainingSupervisionConfig training_supervision;
};
MMLTK_REFLECT_FIELDS(Configuration)
struct ClassName final {
 MMLTK_MAX_BYTES(256U) std::string value;
};
struct OrderedClassCatalog final {
 MMLTK_MAX_ITEMS(256U) std::vector<ClassName> names;
};
struct ModelClassSlot final {
 ClassSlotRole role = ClassSlotRole::Unresolved;
 std::optional<std::uint32_t> foreground_index;
};
struct ClassLayoutProvenance final {
 ClassLayoutOrigin origin = ClassLayoutOrigin::Unresolved;
 MMLTK_MAX_BYTES(1024U) std::string producer;
 MMLTK_MAX_BYTES(64U) std::string artifact_sha256;
};
struct ModelClassLayout final {
 std::uint32_t version = 1U;
 OrderedClassCatalog foreground;
 OrderedClassCatalog class_name_evidence;
 MMLTK_MAX_ITEMS(4096U) std::vector<ModelClassSlot> slots;
 ClassScoreEncoding scores = ClassScoreEncoding::SigmoidLogits;
 NoObjectEncoding no_object = NoObjectEncoding::Unspecified;
 ClassLayoutProvenance provenance;
 bool supervision_in_foreground_order = false;
};
MMLTK_REFLECT_FIELDS(ClassName)
MMLTK_REFLECT_FIELDS(OrderedClassCatalog)
MMLTK_REFLECT_FIELDS(ModelClassSlot)
MMLTK_REFLECT_FIELDS(ClassLayoutProvenance)
MMLTK_REFLECT_FIELDS(ModelClassLayout)
// Unlike the exact-type project_record facility, this private historical
// conversion recursively projects nested records and enums, with one renamed
// semantic field. Only the frozen destination supplies the member inventory.
template <class Destination, class Source>
[[nodiscard]] Destination project(const Source& source) {
 if constexpr (std::same_as<Destination, Source>) {
  return source;
 } else if constexpr (std::same_as<Destination, bool> && std::same_as<Source, ModelTask>) {
  switch (source) {
   case ModelTask::Detection: return false;
   case ModelTask::Segmentation: return true;
   default: throw std::invalid_argument("baseline session identity cannot represent the requested model/schema");
  }
 } else if constexpr (std::is_enum_v<Destination> && std::is_enum_v<Source>) {
  static_assert(std::same_as<std::underlying_type_t<Destination>, std::underlying_type_t<Source>>);
  constexpr const auto& targets = materialized_enum_entries(std::type_identity<Destination>{});
  template for (constexpr auto target : targets) {
   constexpr auto corresponding = [=] consteval {
    for (const auto candidate : mmltk::frameworks::reflection::enum_entries<Source>())
     if (candidate.name == target.name) return candidate.value;
    throw "historical identity enumerator requires an explicit semantic conversion";
   }();
   if (source == corresponding) return target.value;
  }
  throw std::invalid_argument("baseline session identity cannot represent the requested enum");
 } else if constexpr (requires(Destination value) { value.reserve(0U); }) {
  static_assert(std::same_as<Destination, std::vector<typename Destination::value_type>>);
  static_assert(std::same_as<Source, std::vector<typename Source::value_type>>);
  Destination result;
  result.reserve(source.size());
  for (const auto& value : source) result.push_back(project<typename Destination::value_type>(value));
  return result;
 } else {
  static_assert(std::is_aggregate_v<Destination> && std::is_aggregate_v<Source>, "historical identity scalar types must match exactly");
  Destination result;
  template for (constexpr auto target : std::define_static_array(std::meta::nonstatic_data_members_of(^^Destination, std::meta::access_context::current()))) {
   constexpr auto current = [=] consteval {
    constexpr auto name = std::same_as<Destination, Configuration> && std::meta::identifier_of(target) == "segmentation" ? "task" : std::meta::identifier_of(target);
    for (const auto candidate : std::meta::nonstatic_data_members_of(^^Source, std::meta::access_context::current()))
     if (std::meta::identifier_of(candidate) == name) return candidate;
    throw "historical identity member requires an explicit semantic conversion";
   }();
   result.[:target:] = project<typename[:std::meta::type_of(target):]>(source.[:current:]);
  }
  return result;
 }
}
}  // namespace mmltk::backend::models::rfdetr::detail::training_identity_v1
