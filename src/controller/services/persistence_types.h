#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "src/controller/contracts/gui_settings_states.h"
namespace mmltk::controller::services {
enum class PersistenceTerminal : std::uint8_t {
    Succeeded,
    Failed,
    Cancelled,
};
using PersistenceSettingsSnapshot = std::vector<mmltk::controller::contracts::GuiSettingsState>;
struct PersistenceLoadResult final {
    PersistenceTerminal terminal = PersistenceTerminal::Failed;
    PersistenceSettingsSnapshot settings{};
    std::uint64_t revision_frontier = 0U;
    std::string detail{};
    [[nodiscard]] bool succeeded() const noexcept { return terminal == PersistenceTerminal::Succeeded && settings.size() == 1U; }
};
struct PersistenceSaveResult final {
    PersistenceTerminal terminal = PersistenceTerminal::Failed;
    std::uint64_t revision = 0U;
    std::string detail{};
    [[nodiscard]] bool succeeded() const noexcept { return terminal == PersistenceTerminal::Succeeded; }
};
[[nodiscard]] PersistenceSettingsSnapshot make_persistence_settings_snapshot(mmltk::controller::contracts::GuiSettingsState settings);
[[nodiscard]] const mmltk::controller::contracts::GuiSettingsState* view_persistence_settings(const PersistenceSettingsSnapshot& snapshot) noexcept;
[[nodiscard]] bool take_persistence_settings(PersistenceSettingsSnapshot& snapshot, mmltk::controller::contracts::GuiSettingsState& destination) noexcept;
}  // namespace mmltk::controller::services
namespace mmltk::controller::services {
inline PersistenceSettingsSnapshot make_persistence_settings_snapshot(mmltk::controller::contracts::GuiSettingsState settings) {
    PersistenceSettingsSnapshot snapshot;
    snapshot.emplace_back(std::move(settings));
    return snapshot;
}
inline const mmltk::controller::contracts::GuiSettingsState* view_persistence_settings(const PersistenceSettingsSnapshot& snapshot) noexcept {
    return snapshot.size() == 1U ? &snapshot.front() : nullptr;
}
inline bool take_persistence_settings(PersistenceSettingsSnapshot& snapshot, mmltk::controller::contracts::GuiSettingsState& destination) noexcept {
    if (snapshot.size() != 1U) return false;
    destination = std::move(snapshot.front());
    snapshot.clear();
    return true;
}
}  // namespace mmltk::controller::services
