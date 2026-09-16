#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/contracts/explore_filter.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/provider.h"
#include "src/controller/contracts/settings.h"
#include "src/controller/contracts/settings_commands.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/services/settings_location.h"
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

namespace mmltk::controller {
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] SettingsChanged final {
    contracts::SettingsUiState snapshot{};
};
struct ExploreSettingsCandidate final {
    std::uint64_t version = 0U;
    mmltk::backend::data::DataLoadingOptions loading;
    int device_id = 0;
    ExploreFilterPreferences preferences{};
    mmltk::backend::models::rfdetr::GpuAugmentationConfig augmentation{};
    bool augmentation_preview_enabled = false;
    bool show_original_dimensions = false;
};
// Explicit domain edit data; SettingsSystem alone admits and mutates the candidate.
struct ExploreSettingsEdit final {
    std::optional<ExploreFilterUpdate> preferences{};
    std::optional<bool> augmentation_enabled{};
    std::optional<bool> show_original_dimensions{};
    std::optional<ExploreClassCatalogIdentity> class_catalog_identity{};
};
class SettingsSystem final {
   public:
    using event_type = std::variant<SettingsChanged>;
    using settings_surface = SettingsSurface<&contracts::SettingsUiState::settings_state, &contracts::default_gui_settings_state>;
    explicit SettingsSystem(SystemEventSink<event_type> events = {});
    [[nodiscard]] services::SettingsMutationResult Load(services::SettingsLocation, std::optional<bool> h2d_dataloader_override = std::nullopt);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::SettingsUiState Update(contracts::SettingsUpdateRequest);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::SettingsUiState Reset(contracts::SettingsResetRequest);
    [[nodiscard]] services::SettingsMutationResult Retry();
    [[= contracts::reflection::Snapshot{contracts::kSettingsUiStateByteBudget}]] [[nodiscard]] contracts::SettingsUiState snapshot() const;
    void require_loaded() const;
    [[nodiscard]] contracts::SettingsMaterializationFacts materialization_facts() const;
    [[nodiscard]] contracts::ProviderPreferences provider_preferences() const;
    void RestoreTrainingCheckpoint(mmltk::backend::models::rfdetr::TrainRequest, const std::filesystem::path&);
    [[nodiscard]] ExploreSettingsCandidate explore_settings_candidate() const;
    [[nodiscard]] ExploreSettingsCandidate Update(const ExploreSettingsCandidate&, const ExploreSettingsEdit&);

   private:
    [[nodiscard]] contracts::GuiSettingsState mutation_candidate() const;
    [[nodiscard]] services::SettingsMutationResult persist(contracts::GuiSettingsState, bool preserve_retry_on_failure = false);
    void publish(const services::SettingsMutationResult&) noexcept;
    std::mutex mutation_mutex_;
    mutable std::mutex mutex_;
    SystemEventSink<event_type> events_;
    contracts::SettingsUiState state_{};
    contracts::GuiSettingsState retry_snapshot_{};
    std::optional<bool> h2d_dataloader_override_{};
    services::SettingsLocation location_{std::string_view{}};
    std::uint64_t candidate_version_ = 0U;
    bool loaded_ = false;
    bool retryable_ = false;
    services::SettingsMutationResult terminal_{};
};
MMLTK_REFLECT_FIELDS(SettingsChanged)
}  // namespace mmltk::controller
