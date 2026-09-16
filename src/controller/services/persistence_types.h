#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <optional>
#include "src/controller/contracts/gui_settings_states.h"
namespace mmltk::controller::services {
enum class PersistenceTerminal : std::uint8_t {
    Succeeded,
    Failed,
    Cancelled,
};
enum class SettingsStoreWriteStage : unsigned char { Parent, Open, Flush, Rename, Validation };
struct PersistenceLoadResult final {
    PersistenceTerminal terminal = PersistenceTerminal::Failed;
    std::unique_ptr<mmltk::controller::contracts::GuiSettingsState> settings{};
    std::uint64_t revision_frontier = 0U;
    std::string detail{};
    std::optional<SettingsStoreWriteStage> stage{};
    [[nodiscard]] bool succeeded() const noexcept { return terminal == PersistenceTerminal::Succeeded && settings != nullptr; }
};
struct PersistenceSaveResult final {
    PersistenceTerminal terminal = PersistenceTerminal::Failed;
    std::uint64_t revision = 0U;
    std::string detail{};
    std::optional<SettingsStoreWriteStage> stage{};
    [[nodiscard]] bool succeeded() const noexcept { return terminal == PersistenceTerminal::Succeeded; }
};
}  // namespace mmltk::controller::services
