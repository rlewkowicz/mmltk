#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::models::rfdetr {
enum class TrainOptimizerKind : std::uint8_t {
    AdamW,
    Muon,
};
enum class TrainLrSchedulerKind : std::uint8_t {
    Step,
    Cosine,
};
[[nodiscard]] constexpr std::string_view cli_enum_spelling(const TrainOptimizerKind optimizer) noexcept {
    switch (optimizer) {
        case TrainOptimizerKind::AdamW: return "adamw";
        case TrainOptimizerKind::Muon: return "muon";
    }
    return {};
}
[[nodiscard]] constexpr std::string_view cli_enum_spelling(const TrainLrSchedulerKind scheduler) noexcept {
    switch (scheduler) {
        case TrainLrSchedulerKind::Step: return "step";
        case TrainLrSchedulerKind::Cosine: return "cosine";
    }
    return {};
}
[[nodiscard]] constexpr std::optional<TrainLrSchedulerKind> train_lr_scheduler_from_spelling(const std::string_view spelling) noexcept {
    constexpr std::array candidates{TrainLrSchedulerKind::Step, TrainLrSchedulerKind::Cosine};
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
    constexpr bool operator==(const TrainRecipeCatalogEntry&) const noexcept = default;
};
MMLTK_REFLECT_ENUM(TrainOptimizerKind)
MMLTK_REFLECT_ENUM(TrainLrSchedulerKind)
MMLTK_REFLECT_FIELDS(TrainRecipeCatalogEntry)
inline constexpr std::array<TrainRecipeCatalogEntry, 2U> kTrainRecipeCatalog{{
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
}};
[[nodiscard]] consteval bool train_recipe_catalog_is_valid() {
    for (std::size_t index = 0U; index < kTrainRecipeCatalog.size(); ++index) {
        const auto& recipe = kTrainRecipeCatalog[index];
        if (cli_enum_spelling(recipe.optimizer).empty() || cli_enum_spelling(recipe.lr_scheduler).empty() || recipe.lr < 0.0 || recipe.lr_encoder < 0.0 ||
            recipe.momentum < 0.0 || recipe.momentum > 1.0 || recipe.weight_decay < 0.0)
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
}  // namespace mmltk::backend::models::rfdetr
