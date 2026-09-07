#pragma once

#include <cstdint>

#include "src/controller/services/persistence_types.h"
#include "src/controller/services/settings_location.h"

namespace mmltk::controller::services {

[[nodiscard]] PersistenceLoadResult load_persistence_settings(const SettingsLocation& location) noexcept;
[[nodiscard]] PersistenceSaveResult save_persistence_settings(const SettingsLocation& location, const PersistenceSettingsSnapshot& settings,
                                                              std::uint64_t revision) noexcept;

}  // namespace mmltk::controller::services
