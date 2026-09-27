#pragma once
#include <cstdint>
#include <string_view>
#include "src/controller/services/persistence_types.h"
namespace mmltk::controller::services {
// Owns parse/repair, revision admission, durable writes and failure translation.
class SettingsStore final {
public:
 [[nodiscard]] static PersistenceLoadResult load(std::string_view) noexcept;
 [[nodiscard]] static PersistenceSaveResult save(std::string_view, const mmltk::controller::contracts::GuiSettingsState&, std::uint64_t revision) noexcept;
};
}  // namespace mmltk::controller::services
