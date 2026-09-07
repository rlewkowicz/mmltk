#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "src/backend/data/compiled_format_limits.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/artifact_catalog.h"

namespace mmltk::controller {

inline constexpr std::size_t kExploreClassCapacity = mmltk::backend::data::MAX_CLASSES;

enum class ExploreOrder : std::uint8_t {
    Sequential = 0,
    Shuffled = 1,
};

enum class ExploreClassSelectionMode : std::uint8_t {
    All = 0,
    None = 1,
    Subset = 2,
};

struct ExploreClassSelection final {
    ExploreClassSelectionMode mode = ExploreClassSelectionMode::All;
    [[= mmltk::frameworks::reflection::MaxItems{kExploreClassCapacity}]] std::vector<std::uint32_t> classes{};
    bool operator==(const ExploreClassSelection&) const = default;
};

struct ExploreOverlay final {
    ExploreClassSelection class_selection{};
    bool show_boxes = true;
    bool show_masks = true;
    bool show_labels = true;
    bool operator==(const ExploreOverlay&) const = default;
};
struct ExploreFilter final {
    ExploreClassSelection class_selection{};
    std::uint32_t minimum_instances = 0U;
    std::uint32_t maximum_instances = 10'000U;
    std::uint64_t minimum_compiled_index = 0U;
    std::uint64_t maximum_compiled_index = std::numeric_limits<std::uint64_t>::max();
    ExploreOrder order = ExploreOrder::Sequential;
    std::uint64_t shuffle_seed = 0U;
    bool require_boxes = false;
    bool require_masks = false;
    bool operator==(const ExploreFilter&) const = default;
};
struct ExploreFilterUpdate final {
    ExploreFilter filter{};
    ExploreOverlay overlay{};
};

using ExploreClassCatalogIdentity = std::uint64_t;

[[nodiscard]] inline ExploreClassCatalogIdentity explore_class_catalog_identity(
    const std::span<const contracts::ArtifactClassName> class_names) noexcept {
    constexpr ExploreClassCatalogIdentity offset_basis = 14'695'981'039'346'656'037ULL;
    constexpr ExploreClassCatalogIdentity prime = 1'099'511'628'211ULL;
    ExploreClassCatalogIdentity identity = offset_basis;
    const auto mix = [&identity](const std::uint8_t value) {
        identity ^= value;
        identity *= prime;
    };
    const auto mix_size = [&mix](std::uint64_t value) {
        for (std::size_t byte = 0U; byte != sizeof(value); ++byte) {
            mix(static_cast<std::uint8_t>(value & 0xffU));
            value >>= 8U;
        }
    };
    mix_size(static_cast<std::uint64_t>(class_names.size()));
    for (const auto& class_name : class_names) {
        mix_size(static_cast<std::uint64_t>(class_name.value.size()));
        for (const unsigned char value : class_name.value)
            mix(value);
    }
    return identity == 0U ? 1U : identity;
}

struct ExploreFilterPreferences final {
    ExploreClassCatalogIdentity class_catalog_identity = 0U;
    ExploreFilterUpdate policy{};
};

MMLTK_REFLECT_FIELDS(ExploreClassSelection)
MMLTK_REFLECT_FIELDS(ExploreOverlay)
MMLTK_REFLECT_FIELDS(ExploreFilter)
MMLTK_REFLECT_FIELDS(ExploreFilterUpdate)
MMLTK_REFLECT_ENUM(ExploreOrder)
MMLTK_REFLECT_ENUM(ExploreClassSelectionMode)

}  // namespace mmltk::controller
