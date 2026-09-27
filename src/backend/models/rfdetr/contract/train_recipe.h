#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include "mmltk/frameworks/reflection/member_path.h"
#include "mmltk/frameworks/reflection/member_relation.h"
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
struct TrainRecipeCatalog;
// Declaration order is the persisted override-bit order for both recipe scopes.
struct TrainRecipeValues {
 constexpr bool operator==(const TrainRecipeValues&) const = default;
 MMLTK_CATALOG(TrainRecipeCatalog) TrainOptimizerKind optimizer = TrainOptimizerKind::AdamW;
 MMLTK_MINIMUM(double, 0.0) MMLTK_FINITE MMLTK_PRESENTATION(mmltk::frameworks::reflection::PresentationKind::LearningRate) double lr = 1.0e-4;
 MMLTK_MINIMUM(double, 0.0)
 MMLTK_FINITE MMLTK_PRESENTATION(
  // CLEANUP-IGNORE: Each learning-rate field needs an independently addressable generated identity.
  mmltk::frameworks::reflection::PresentationKind::LearningRate) double lr_encoder = 1.5e-4;
 MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0) MMLTK_FINITE MMLTK_PRESENTATION(mmltk::frameworks::reflection::PresentationKind::Decay) double lr_component_decay = 0.7;
 MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0) MMLTK_FINITE MMLTK_PRESENTATION(mmltk::frameworks::reflection::PresentationKind::Decay) double encoder_layer_decay = 0.8;
 MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0) MMLTK_FINITE MMLTK_PRESENTATION(mmltk::frameworks::reflection::PresentationKind::Decay) double momentum = 0.95;
 MMLTK_MINIMUM(double, 0.0) MMLTK_FINITE MMLTK_PRESENTATION(mmltk::frameworks::reflection::PresentationKind::Decay) double weight_decay = 1.0e-4;
 // CLEANUP-IGNORE: Warmup remains a separate constrained setting rather than an indexed optimizer scalar.
 MMLTK_MINIMUM(double, 0.0) MMLTK_FINITE double warmup_epochs = 0.0;
 MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0) MMLTK_FINITE MMLTK_PRESENTATION(mmltk::frameworks::reflection::PresentationKind::Decay) double warmup_momentum = 0.0;
 MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0) MMLTK_FINITE MMLTK_PRESENTATION(mmltk::frameworks::reflection::PresentationKind::Decay) double lr_min_factor = 0.0;
 MMLTK_MINIMUM(int, 0) int lr_drop = 100;
 TrainLrSchedulerKind lr_scheduler = TrainLrSchedulerKind::Step;
 bool nesterov = false;
 MMLTK_MINIMUM(double, 0.0) MMLTK_FINITE double warmup_bias_lr = 0.0;
};
MMLTK_REFLECT_ENUM(TrainOptimizerKind)
MMLTK_REFLECT_ENUM(TrainLrSchedulerKind)
MMLTK_REFLECT_FIELDS(TrainRecipeValues)
struct TrainRecipeCatalogEntry final : TrainRecipeValues {
 constexpr bool operator==(const TrainRecipeCatalogEntry&) const noexcept = default;
};
MMLTK_REFLECT_FIELDS(TrainRecipeCatalogEntry)
[[nodiscard]] constexpr bool train_recipe_values_valid(const TrainRecipeValues& value) noexcept {
 return !mmltk::frameworks::reflection::validate_reflected_fields(value) && (value.lr_scheduler != TrainLrSchedulerKind::UltralyticsLinear || value.optimizer == TrainOptimizerKind::SGD) &&
        (!value.nesterov || (value.optimizer == TrainOptimizerKind::SGD && value.momentum > 0));
}
inline constexpr std::array<TrainRecipeCatalogEntry, 3U> kTrainRecipeCatalog{{
 {{.optimizer = TrainOptimizerKind::AdamW, .lr = 1.0e-4, .lr_encoder = 1.5e-4, .lr_component_decay = 0.7, .encoder_layer_decay = 0.8, .momentum = 0.95, .weight_decay = 1.0e-4, .warmup_epochs = 0.0, .warmup_momentum = 0.0, .lr_min_factor = 0.0, .lr_drop = 100, .lr_scheduler = TrainLrSchedulerKind::Step}},
 {{.optimizer = TrainOptimizerKind::Muon, .lr = 2.0e-4, .lr_encoder = 3.0e-4, .lr_component_decay = 0.7, .encoder_layer_decay = 0.8, .momentum = 0.9, .weight_decay = 5.0e-4, .warmup_epochs = 3.0, .warmup_momentum = 0.8, .lr_min_factor = 0.01, .lr_drop = 100, .lr_scheduler = TrainLrSchedulerKind::Cosine}},
 {{.optimizer = TrainOptimizerKind::SGD, .lr = .01, .lr_encoder = .001, .lr_component_decay = .7, .encoder_layer_decay = .8, .momentum = .9, .weight_decay = .0001, .warmup_epochs = 3, .warmup_momentum = .8, .lr_min_factor = .01, .lr_drop = 100, .lr_scheduler = TrainLrSchedulerKind::UltralyticsLinear, .nesterov = false, .warmup_bias_lr = .1}},
}};
[[nodiscard]] consteval bool train_recipe_catalog_is_valid() {
 for (std::size_t index = 0U; index < kTrainRecipeCatalog.size(); ++index) {
  const auto& recipe = kTrainRecipeCatalog[index];
  if (!train_recipe_values_valid(recipe)) return false;
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
inline constexpr std::size_t kTrainRecipeFieldCount = mmltk::frameworks::reflection::field_declarations<TrainRecipeValues>().size() - 1U;
static_assert(kTrainRecipeFieldCount > 0U && kTrainRecipeFieldCount <= std::numeric_limits<std::uint16_t>::digits, "Train recipe override mask overflow");
inline constexpr std::uint16_t kTrainRecipeOverrideBits = (std::uint16_t{1U} << kTrainRecipeFieldCount) - 1U;
class[[= mmltk::frameworks::reflection::OpaqueRelationStorage{}]] TrainRecipeOverrideState final {
public:
 constexpr TrainRecipeOverrideState() noexcept = default;
 constexpr bool operator==(const TrainRecipeOverrideState&) const noexcept = default;

private:
 MMLTK_MAXIMUM(std::uint16_t, kTrainRecipeOverrideBits) std::uint16_t mask = 0U;
 friend struct mmltk::frameworks::reflection::catalog_provider_relation<TrainRecipeCatalog>;
};
struct TrainRecipeSettings final : TrainRecipeValues {
 constexpr bool operator==(const TrainRecipeSettings&) const = default;
 TrainRecipeOverrideState overrides;
};
MMLTK_REFLECT_FIELDS(TrainRecipeOverrideState)
MMLTK_REFLECT_FIELDS(TrainRecipeSettings)
}  // namespace mmltk::backend::models::rfdetr
namespace mmltk::frameworks::reflection {
template <>
struct catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog> {
 using value_type = mmltk::backend::models::rfdetr::TrainRecipeValues;
 using source_type = mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry;
 using destination_type = mmltk::backend::models::rfdetr::TrainRecipeSettings;
 inline static constexpr std::size_t member_count = mmltk::backend::models::rfdetr::kTrainRecipeFieldCount;
 template <class Visitor>
 static constexpr void VisitMembers(Visitor&& visitor) {
  visit_materialized_members<value_type>([&]<class Declaration>(const auto&) {
   if constexpr (accessor_member_identity<value_type, member_path<Declaration::pointer>>() != accessor_member_identity<value_type, member_path<&value_type::optimizer>>()) {
    constexpr auto path = member_path<Declaration::pointer>;
    using Entry = MemberRelationEntry<path, path>;
    visitor.template operator()<Entry>();
   }
  });
 }
 [[nodiscard]] static consteval bool valid() {
  using Values = RemoveCvRef<decltype(field_declarations<value_type>())>;
  using Source = RemoveCvRef<decltype(field_declarations<source_type>())>;
  using Destination = RemoveCvRef<decltype(field_declarations<destination_type>())>;
  // Both public forms must contain exactly the canonical values, with only the
  // opaque override state added by the settings form.
  if constexpr (Values::base_types::count != 0U || !std::same_as<typename Source::base_types, MaterializedBaseList<value_type>> ||
                !std::same_as<typename Destination::base_types, MaterializedBaseList<value_type>> || Source::size() != 0U || Destination::size() != 1U)
   return false;
  std::size_t selectors = 0U;
  std::size_t values = 0U;
  visit_materialized_members<value_type>([&]<class Declaration>(const auto&) {
   if constexpr (accessor_member_identity<value_type, member_path<Declaration::pointer>>() == accessor_member_identity<value_type, member_path<&value_type::optimizer>>()) {
    ++selectors;
   } else {
    ++values;
   }
  });
  bool storage = false;
  visit_materialized_members<destination_type>([&]<class Declaration>(const auto&) {
   if constexpr (std::same_as<typename Declaration::member_type, override_state_type>) storage = Declaration::pointer == &destination_type::overrides;
  });
  return selectors == 1U && values == member_count && storage;
 }
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
  constexpr auto storage_identity = accessor_member_identity<destination_type, destination_override_state>();
  std::array<ReflectedMemberIdentity, member_count> destinations{};
  std::size_t count = 0U;
  bool complete = true;
  std::uint16_t claimed = 0U;
  VisitMembers([&]<class Entry>() {
   constexpr auto source = accessor_member_identity<source_type, Entry::source>();
   constexpr auto destination = accessor_member_identity<destination_type, Entry::destination>();
   constexpr auto mask = bit<Entry::destination>();
   static_assert(std::same_as<decltype(Entry::source), decltype(Entry::destination)>);
   static_assert(Entry::transform::template accepts<accessor_value_t<source_type, Entry::source>, accessor_value_t<destination_type, Entry::destination>>());
   complete = complete && source.valid() && destination.valid() && source != source_selector_identity && destination != destination_selector_identity && destination != storage_identity &&
              (claimed & mask) == 0U;
   for (const auto& prior : destinations) complete = complete && prior != destination;
   if (count < destinations.size()) destinations[count] = destination;
   ++count;
   claimed = static_cast<std::uint16_t>(claimed | mask);
  });
  return complete && count == member_count && claimed == valid_bits;
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
inline constexpr void reset_train_recipe(TrainRecipeSettings& value) {
 value.overrides = {};
 resolve_train_recipe(value);
}
[[nodiscard]] inline bool train_recipe_valid(const TrainRecipeSettings& value) noexcept {
 return train_recipe_values_valid(value) && !mmltk::frameworks::reflection::validate_reflected_fields(value.overrides);
}
}  // namespace mmltk::backend::models::rfdetr
