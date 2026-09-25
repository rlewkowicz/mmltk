#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <meta>
#include "mmltk/frameworks/reflection/member_relation.h"
#include <string_view>
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::models::rfdetr {
enum class TrainOptimizerKind : std::uint8_t {
 AdamW,
 Muon,
 SGD,
};
enum class TrainLrSchedulerKind : std::uint8_t {
 Step,
 Cosine,
 UltralyticsLinear,
};
[[nodiscard]] constexpr std::string_view cli_enum_spelling(const TrainOptimizerKind optimizer) noexcept {
 switch (optimizer) {
  case TrainOptimizerKind::AdamW: return "adamw";
  case TrainOptimizerKind::Muon: return "muon";
  case TrainOptimizerKind::SGD: return "sgd";
 }
 return {};
}
[[nodiscard]] constexpr std::string_view cli_enum_spelling(const TrainLrSchedulerKind scheduler) noexcept {
 switch (scheduler) {
  case TrainLrSchedulerKind::Step: return "step";
  case TrainLrSchedulerKind::Cosine: return "cosine";
  case TrainLrSchedulerKind::UltralyticsLinear: return "ultralytics-linear";
 }
 return {};
}
[[nodiscard]] constexpr std::optional<TrainLrSchedulerKind> train_lr_scheduler_from_spelling(const std::string_view spelling) noexcept {
 constexpr std::array candidates{TrainLrSchedulerKind::Step, TrainLrSchedulerKind::Cosine, TrainLrSchedulerKind::UltralyticsLinear};
 for (const TrainLrSchedulerKind candidate : candidates) {
  if (cli_enum_spelling(candidate) == spelling) return candidate;
 }
 return std::nullopt;
}
struct TrainRecipeCatalogEntry final {
 TrainOptimizerKind optimizer = TrainOptimizerKind::AdamW;
 double lr = 1.0e-4;
 double lr_encoder = 1.5e-4;
 double lr_component_decay = 0.7;
 double encoder_layer_decay = 0.8;
 double momentum = 0.95;
 double weight_decay = 1.0e-4;
 double warmup_epochs = 0.0;
 double warmup_momentum = 0.0;
 double lr_min_factor = 0.0;
 int lr_drop = 100;
 TrainLrSchedulerKind lr_scheduler = TrainLrSchedulerKind::Step;
 bool nesterov = false;
 double warmup_bias_lr = 0.0;
 constexpr bool operator==(const TrainRecipeCatalogEntry&) const noexcept = default;
};
MMLTK_REFLECT_ENUM(TrainOptimizerKind)
MMLTK_REFLECT_ENUM(TrainLrSchedulerKind)
MMLTK_REFLECT_FIELDS(TrainRecipeCatalogEntry)
inline constexpr std::array<TrainRecipeCatalogEntry, 3U> kTrainRecipeCatalog{{
 {.optimizer = TrainOptimizerKind::AdamW,
  .lr = 1.0e-4,
  .lr_encoder = 1.5e-4,
  .lr_component_decay = 0.7,
  .encoder_layer_decay = 0.8,
  .momentum = 0.95,
  .weight_decay = 1.0e-4,
  .warmup_epochs = 0.0,
  .warmup_momentum = 0.0,
  .lr_min_factor = 0.0,
  .lr_drop = 100,
  .lr_scheduler = TrainLrSchedulerKind::Step},
 {.optimizer = TrainOptimizerKind::Muon,
  .lr = 2.0e-4,
  .lr_encoder = 3.0e-4,
  .lr_component_decay = 0.7,
  .encoder_layer_decay = 0.8,
  .momentum = 0.9,
  .weight_decay = 5.0e-4,
  .warmup_epochs = 3.0,
  .warmup_momentum = 0.8,
  .lr_min_factor = 0.01,
  .lr_drop = 100,
  .lr_scheduler = TrainLrSchedulerKind::Cosine},
 {.optimizer = TrainOptimizerKind::SGD, .lr = .01, .lr_encoder = .001, .lr_component_decay = .7, .encoder_layer_decay = .8, .momentum = .9, .weight_decay = .0001, .warmup_epochs = 3, .warmup_momentum = .8, .lr_min_factor = .01, .lr_drop = 100, .lr_scheduler = TrainLrSchedulerKind::UltralyticsLinear, .nesterov = false, .warmup_bias_lr = .1},
}};
[[nodiscard]] consteval bool train_recipe_catalog_is_valid() {
 for (std::size_t index = 0U; index < kTrainRecipeCatalog.size(); ++index) {
  const auto& recipe = kTrainRecipeCatalog[index];
  if (cli_enum_spelling(recipe.optimizer).empty() || cli_enum_spelling(recipe.lr_scheduler).empty() || recipe.lr < 0.0 || recipe.lr_encoder < 0.0 || recipe.momentum < 0.0 || recipe.momentum > 1.0 ||
      recipe.weight_decay < 0.0)
   return false;
  for (std::size_t sibling = index + 1U; sibling < kTrainRecipeCatalog.size(); ++sibling) {
   if (recipe.optimizer == kTrainRecipeCatalog[sibling].optimizer) return false;
  }
 }
 return true;
}
static_assert(train_recipe_catalog_is_valid());
[[nodiscard]] constexpr const TrainRecipeCatalogEntry& train_recipe(const TrainOptimizerKind optimizer) noexcept {
 for (const auto& recipe : kTrainRecipeCatalog) {
  if (recipe.optimizer == optimizer) return recipe;
 }
 return kTrainRecipeCatalog.front();
}
struct TrainRecipeCatalog final {
 using row_type = TrainRecipeCatalogEntry;
 // CLEANUP-IGNORE: Distinct domain catalogs intentionally implement the same reflection catalog protocol.
 static constexpr std::string_view identity = "rfdetr.train-recipes";
 template <class Visitor>
 static constexpr void VisitRows(Visitor&& visitor) {
  for (std::size_t index = 0U; index < kTrainRecipeCatalog.size(); ++index) visitor(kTrainRecipeCatalog[index], index);
 }
 [[nodiscard]] static constexpr std::string_view row_key(const row_type& row) noexcept { return cli_enum_spelling(row.optimizer); }
 [[nodiscard]] static consteval bool valid() noexcept { return train_recipe_catalog_is_valid(); }
};
inline constexpr std::size_t kTrainRecipeFieldCount = std::meta::nonstatic_data_members_of(^^TrainRecipeCatalogEntry, std::meta::access_context::current()).size() - 1U;
static_assert(kTrainRecipeFieldCount < 16U);
inline constexpr std::uint16_t kTrainRecipeOverrideBits = (std::uint16_t{1U} << kTrainRecipeFieldCount) - 1U;
class[[= mmltk::frameworks::reflection::OpaqueRelationStorage{}]] TrainRecipeOverrideState final {
public:
 constexpr TrainRecipeOverrideState() noexcept = default;
 constexpr bool operator==(const TrainRecipeOverrideState&) const noexcept = default;

private:
 [[= mmltk::frameworks::reflection::Maximum<std::uint16_t>{kTrainRecipeOverrideBits}]] std::uint16_t mask = 0U;
 friend struct mmltk::frameworks::reflection::catalog_provider_relation<TrainRecipeCatalog>;
};
struct TrainRecipeSettings final {
 constexpr bool operator==(const TrainRecipeSettings&) const = default;
 [[= mmltk::frameworks::reflection::CatalogProvider<TrainRecipeCatalog>{}]] TrainOptimizerKind optimizer = TrainOptimizerKind::AdamW;
 TrainLrSchedulerKind lr_scheduler = TrainLrSchedulerKind::Step;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int lr_drop = 100;
 [[= mmltk::frameworks::reflection::Minimum<double>{
  0.0}]][[= mmltk::frameworks::reflection::Finite{}]][[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::LearningRate>{}]] double lr = 1.0e-4;
 [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Finite{}]][[= mmltk::frameworks::reflection::Presentation<
  // CLEANUP-IGNORE: Each learning-rate field needs an independently addressable generated identity.
  mmltk::frameworks::reflection::PresentationKind::LearningRate>{}]] double lr_encoder = 1.5e-4;
 [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
  [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double lr_component_decay = 0.7;
 [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
  [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double encoder_layer_decay = 0.8;
 [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
  [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double momentum = 0.95;
 [[= mmltk::frameworks::reflection::Minimum<double>{
  0.0}]][[= mmltk::frameworks::reflection::Finite{}]][[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double weight_decay = 1.0e-4;
 // CLEANUP-IGNORE: Warmup remains a separate constrained setting rather than an indexed optimizer scalar.
 [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Finite{}]] double warmup_epochs = 0.0;
 [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
  [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double warmup_momentum = 0.0;
 [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
  [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double lr_min_factor = 0.0;
 bool nesterov = false;
 [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Finite{}]] double warmup_bias_lr = 0.0;
 TrainRecipeOverrideState overrides;
};
MMLTK_REFLECT_FIELDS(TrainRecipeOverrideState)
MMLTK_REFLECT_FIELDS(TrainRecipeSettings)
}  // namespace mmltk::backend::models::rfdetr
namespace mmltk::frameworks::reflection {
template <>
struct catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>
 {
 using source_type = mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry;
 using destination_type = mmltk::backend::models::rfdetr::TrainRecipeSettings;
 inline static constexpr std::size_t member_count = mmltk::backend::models::rfdetr::kTrainRecipeFieldCount;
 template <class Visitor>
 static constexpr void VisitMembers(Visitor&& visitor) {
  template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^source_type, std::meta::access_context::current()))) {
   if constexpr (member != ^^source_type::optimizer) {
    constexpr auto destination = [] consteval {
     for (const auto candidate : std::meta::nonstatic_data_members_of(^^destination_type, std::meta::access_context::current())) {
      if (std::meta::identifier_of(candidate) == std::meta::identifier_of(member)) return candidate;
     }
     throw "recipe catalog member has no canonical setting";
    }();
    using Entry = MemberRelationEntry<member_path<&[:member:]>, member_path<&[:destination:]>>;
    static_assert(Entry::transform::template accepts<typename[:std::meta::type_of(member):], typename[:std::meta::type_of(destination):]>());
    visitor.template operator()<Entry>();
   }
  }
 }
 [[nodiscard]] static consteval bool valid() { return member_count > 0; }
 using provider_type = mmltk::backend::models::rfdetr::TrainRecipeCatalog;
 using override_state_type = mmltk::backend::models::rfdetr::TrainRecipeOverrideState;
 inline static constexpr auto source_selector = member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::optimizer>;
 inline static constexpr auto destination_selector = member_path<&mmltk::backend::models::rfdetr::TrainRecipeSettings::optimizer>;
 inline static constexpr auto destination_override_state = member_path<&mmltk::backend::models::rfdetr::TrainRecipeSettings::overrides>;
 inline static constexpr std::uint16_t valid_bits = mmltk::backend::models::rfdetr::kTrainRecipeOverrideBits;
 template <auto Destination>
 [[nodiscard]] static constexpr bool overridden(const override_state_type& state) noexcept {
  return (state.mask & bit<Destination>()) != 0U;
 }
 template <auto Destination>
 static constexpr void set_override(override_state_type& state) noexcept {
  state.mask = static_cast<std::uint16_t>(state.mask | bit<Destination>());
 }
 template <auto Destination>
 static constexpr void clear_override(override_state_type& state) noexcept {
  state.mask = static_cast<std::uint16_t>(state.mask & ~bit<Destination>());
 }
 [[nodiscard]] static consteval bool audit() {
  if (!valid()) return false;
  if constexpr (!std::same_as<accessor_value_t<source_type, source_selector>, accessor_value_t<destination_type, destination_selector>>) return false;
  const auto source_selector_identity = accessor_member_identity<source_type, source_selector>();
  const auto destination_selector_identity = accessor_member_identity<destination_type, destination_selector>();
  bool selectors_are_not_members = true;
  std::uint16_t claimed = 0U;
  VisitMembers([&]<class Entry>() {
   selectors_are_not_members = selectors_are_not_members && accessor_member_identity<source_type, Entry::source>() != source_selector_identity &&
                               accessor_member_identity<destination_type, Entry::destination>() != destination_selector_identity;
   claimed = static_cast<std::uint16_t>(claimed | bit<Entry::destination>());
  });
  return selectors_are_not_members && claimed == valid_bits;
 }

private:
 template <auto Destination>
 [[nodiscard]] static consteval std::uint16_t bit() {
  const auto identity = accessor_member_identity<destination_type, Destination>();
  std::size_t ordinal = 0U;
  std::size_t found = member_count;
  VisitMembers([&]<class Entry>() {
   if (accessor_member_identity<destination_type, Entry::destination>() == identity) found = ordinal;
   ++ordinal;
  });
  if (found == member_count) throw "destination accessor is not a Train recipe relation member";
  return static_cast<std::uint16_t>(std::uint16_t{1U} << found);
 }
};
static_assert(catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>::audit());
}  // namespace mmltk::frameworks::reflection
namespace mmltk::backend::models::rfdetr {
inline constexpr void apply_train_recipe(TrainRecipeSettings& target, const TrainRecipeCatalogEntry& recipe, const TrainRecipeOverrideState& overrides) {
 using Relation = mmltk::frameworks::reflection::catalog_provider_relation<TrainRecipeCatalog>;
 Relation::VisitMembers([&]<class Entry>() {
  if (!Relation::template overridden<Entry::destination>(overrides)) {
   Entry::transform::apply(
    mmltk::frameworks::reflection::access<TrainRecipeSettings, Entry::destination>(target), mmltk::frameworks::reflection::access<const TrainRecipeCatalogEntry, Entry::source>(recipe));
  }
 });
}
inline constexpr void resolve_train_recipe(TrainRecipeSettings& value) { apply_train_recipe(value, train_recipe(value.optimizer), value.overrides); }
inline constexpr void reset_train_recipe(TrainRecipeSettings& value) { value.overrides = {}; resolve_train_recipe(value); }
[[nodiscard]] inline bool train_recipe_valid(const TrainRecipeSettings& value) noexcept {
 return !mmltk::frameworks::reflection::validate_reflected_fields(value) &&
  (value.lr_scheduler != TrainLrSchedulerKind::UltralyticsLinear || value.optimizer == TrainOptimizerKind::SGD) &&
  (!value.nesterov || (value.optimizer == TrainOptimizerKind::SGD && value.momentum > 0));
}
}  // namespace mmltk::backend::models::rfdetr
