#pragma once
#include <cstdint>
#include <expected>
#include <span>
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/settings.h"
#include "src/controller/contracts/settings_commands.h"
namespace mmltk::controller::contracts {
enum class SettingsMutationError : std::uint8_t {
    InvalidPath,
    TypeMismatch,
    OutOfRange,
    CrossFieldViolation,
};
[[nodiscard]] inline GuiSettingsState default_gui_settings_state() {
    GuiSettingsState state{};
    state.apply_defaults();
    return state;
}
[[nodiscard]] std::expected<void, SettingsMutationError> apply_gui_settings_values(GuiSettingsState& state, std::span<const SettingsValueUpdate> updates);
[[nodiscard]] bool gui_settings_valid(const GuiSettingsState& state) noexcept;
[[nodiscard]] ExploreSourceFact resolve_explore_source(const GuiSettingsState& state);
}  // namespace mmltk::controller::contracts
