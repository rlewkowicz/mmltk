#include "src/controller/services/settings_store.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/common/io/json_file.h"
#include "src/controller/contracts/default_state.h"
#include "src/controller/contracts/gui_settings.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/provider.h"
#include "src/controller/contracts/settings.h"
#include "src/controller/contracts/settings_commands.h"
#include "src/controller/contracts/settings_vocabulary.h"
#include "src/controller/contracts/view_state.h"
#include "src/controller/contracts/workflows.h"

namespace mmltk::controller::services {
namespace common_io = mmltk::common::io;
namespace contracts = mmltk::controller::contracts;

namespace {

[[nodiscard]] SettingsStoreWriteStage settings_write_stage(const common_io::JsonWriteStage stage) noexcept {
    switch (stage) {
        case common_io::JsonWriteStage::kOpen:
            return SettingsStoreWriteStage::Open;
        case common_io::JsonWriteStage::kFlush:
            return SettingsStoreWriteStage::Flush;
        case common_io::JsonWriteStage::kRename:
            return SettingsStoreWriteStage::Rename;
    }
    return SettingsStoreWriteStage::Open;
}

constexpr std::string_view kRevisionField{"settings_revision"};

[[nodiscard]] std::uint64_t read_revision(nlohmann::json& normalized) {
    const auto entry = normalized.find(kRevisionField);
    if (entry == normalized.end()) return 0U;
    if (!entry->is_number_unsigned()) {
        throw SettingsStoreError{SettingsStoreWriteStage::Validation, "settings revision metadata must be an unsigned integer"};
    }
    const std::uint64_t revision = entry->get<std::uint64_t>();
    normalized.erase(entry);
    return revision;
}

[[nodiscard]] std::optional<std::uint64_t> inspect_raw_revision(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;
    const nlohmann::json raw = nlohmann::json::parse(file, nullptr, false);
    if (raw.is_discarded() || !raw.is_object() || !raw.contains(kRevisionField)) return std::nullopt;
    const nlohmann::json& revision = raw.at(kRevisionField);
    if (!revision.is_number_unsigned()) {
        throw SettingsStoreError{SettingsStoreWriteStage::Validation, "settings revision metadata must be an unsigned integer"};
    }
    return revision.get<std::uint64_t>();
}

}  // namespace

StoredSettings SettingsStore::load(const std::filesystem::path& path) {
    contracts::GuiSettingsState state = contracts::default_gui_settings_state();
    nlohmann::json normalized;
    std::error_code exists_error;
    const bool existed = std::filesystem::exists(path, exists_error) && !exists_error;
    const std::optional<std::uint64_t> raw_revision = existed ? inspect_raw_revision(path) : std::nullopt;
    const bool loaded = contracts::load_gui_settings_file(path.string(), state, &normalized, nullptr);
    if (!loaded && !existed) return {.settings = std::move(state), .revision_frontier = 0U};
    const bool normalized_has_revision = normalized.contains(kRevisionField);
    const std::uint64_t revision = loaded && normalized_has_revision ? read_revision(normalized) : raw_revision.value_or(0U);
    const bool settings_repair = loaded && contracts::snapshot_gui_settings(state) != normalized;
    const bool revision_repair = loaded && raw_revision.has_value() && !normalized_has_revision;
    const StoredSettings record{.settings = state, .revision_frontier = revision};
    if ((loaded && (settings_repair || revision_repair)) || (!loaded && existed)) save(path, record.settings, record.revision_frontier);
    return record;
}

void SettingsStore::save(const std::filesystem::path& path, const contracts::GuiSettingsState& settings,
                         const std::uint64_t revision_frontier) {
    if (!contracts::gui_settings_valid(settings)) {
        throw SettingsStoreError{SettingsStoreWriteStage::Validation, "refusing to persist invalid typed GUI settings"};
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw SettingsStoreError{SettingsStoreWriteStage::Parent,
                                     "failed to create settings directory `" + parent.string() + "`: " + error.message()};
        }
    }
    nlohmann::json document = contracts::snapshot_gui_settings(settings);
    document[std::string{kRevisionField}] = revision_frontier;
    const auto failure = common_io::write_json_text_file_atomic(path, document.dump(2));
    if (failure.has_value()) {
        std::error_code cleanup_error;
        std::filesystem::remove(path.string() + ".tmp", cleanup_error);
        throw SettingsStoreError{settings_write_stage(*failure), "failed to persist GUI settings `" + path.string() + "` at stage " +
                                                                     std::string(common_io::to_string(*failure))};
    }
}

}  // namespace mmltk::controller::services
