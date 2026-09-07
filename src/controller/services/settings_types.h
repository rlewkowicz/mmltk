#pragma once

#include <cstdint>
#include <string>

namespace mmltk::controller::services {

enum class SettingsTerminal : std::uint8_t {
    Applied,
    Rejected,
    PersistenceFailed,
    NotLoaded,
};

struct SettingsMutationResult final {
    SettingsTerminal terminal = SettingsTerminal::NotLoaded;
    std::uint64_t revision = 0U;
    std::string detail{};

    [[nodiscard]] bool applied() const noexcept { return terminal == SettingsTerminal::Applied; }
};

}  // namespace mmltk::controller::services
