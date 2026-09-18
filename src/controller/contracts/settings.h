#pragma once
#include <cstdint>
#include <string>
#include <type_traits>
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/view_state.h"
#include "src/controller/contracts/workflows.h"
namespace mmltk::controller::contracts {
inline constexpr std::size_t kSettingsUiStateByteBudget = 64U * 1024U;
struct ExploreSourceFact final {
    ExploreDatasetSource selection = ExploreDatasetSource::Train;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string compiled_source;
    bool available = false;
    bool operator==(const ExploreSourceFact&) const = default;
};
struct SettingsUiState final {
    std::uint64_t revision = 0U;
    GuiSettingsState settings_state{};
    ExploreSourceFact explore_source{};
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string validation_source;
    bool operator==(const SettingsUiState&) const = default;
};
// This immutable fact separates installed settings from a default-constructed
// value. Compute systems materialize requests from an installed revision.
struct SettingsMaterializationFacts final {
    GuiSettingsState settings{};
    std::uint64_t revision = 0U;
    bool loaded = false;
    bool operator==(const SettingsMaterializationFacts&) const = default;
};
MMLTK_REFLECT_FIELDS(ExploreSourceFact)
MMLTK_REFLECT_FIELDS(SettingsUiState)
MMLTK_REFLECT_FIELDS(SettingsMaterializationFacts)
}  // namespace mmltk::controller::contracts
