#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "src/controller/services/persistence_storage.h"
#include "src/controller/services/settings_store.h"

namespace mmltk::controller::services {
namespace {

[[nodiscard]] std::string bounded_detail(const std::exception& error) {
    constexpr std::size_t kMaximumDetailBytes = 256U;
    std::string detail = error.what();
    if (detail.size() > kMaximumDetailBytes) detail.resize(kMaximumDetailBytes);
    return detail;
}

}  // namespace

PersistenceLoadResult load_persistence_settings(const SettingsLocation& location) noexcept {
    try {
        const std::filesystem::path path{location.value()};
        StoredSettings record = SettingsStore::load(path);
        return {.terminal = PersistenceTerminal::Succeeded,
                .settings = make_persistence_settings_snapshot(std::move(record.settings)),
                .revision_frontier = record.revision_frontier,
                .detail = {}};
    } catch (const std::exception& error) {
        return {.terminal = PersistenceTerminal::Failed, .settings = {}, .revision_frontier = 0U, .detail = bounded_detail(error)};
    } catch (...) {
        return {.terminal = PersistenceTerminal::Failed, .settings = {}, .revision_frontier = 0U, .detail = "settings load failed"};
    }
}

PersistenceSaveResult save_persistence_settings(const SettingsLocation& location, const PersistenceSettingsSnapshot& settings,
                                                const std::uint64_t revision) noexcept {
    try {
        const auto* const settings_state = view_persistence_settings(settings);
        if (settings_state == nullptr) {
            return {.terminal = PersistenceTerminal::Failed, .revision = revision, .detail = "settings save received no snapshot"};
        }
        const std::filesystem::path path{location.value()};
        const StoredSettings current = SettingsStore::load(path);
        if (revision <= current.revision_frontier) {
            return {.terminal = PersistenceTerminal::Failed,
                    .revision = revision,
                    .detail = "settings revision is not newer than durable frontier"};
        }
        SettingsStore::save(path, *settings_state, revision);
        return {.terminal = PersistenceTerminal::Succeeded, .revision = revision, .detail = {}};
    } catch (const std::exception& error) {
        return {.terminal = PersistenceTerminal::Failed, .revision = revision, .detail = bounded_detail(error)};
    } catch (...) { return {.terminal = PersistenceTerminal::Failed, .revision = revision, .detail = "settings save failed"}; }
}

}  // namespace mmltk::controller::services
