#pragma once
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include "src/controller/contracts/gui_settings_states.h"
namespace mmltk::controller::services {
enum class SettingsStoreWriteStage : unsigned char { Parent, Open, Flush, Rename, Validation };
class SettingsStoreError final : public std::runtime_error {
   public:
    SettingsStoreError(const SettingsStoreWriteStage value, std::string message) : std::runtime_error(std::move(message)), stage(value) {}
    const SettingsStoreWriteStage stage;
};
struct StoredSettings final {
    mmltk::controller::contracts::GuiSettingsState settings{};
    std::uint64_t revision_frontier = 0U;
};
// Durable typed boundary shared by application startup and domain callers.
class SettingsStore final {
   public:
    [[nodiscard]] static StoredSettings load(const std::filesystem::path& path);
    static void save(const std::filesystem::path& path, const mmltk::controller::contracts::GuiSettingsState& settings, std::uint64_t revision_frontier);
};
}  // namespace mmltk::controller::services
