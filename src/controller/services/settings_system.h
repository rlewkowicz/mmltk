#pragma once
#include <cstdint>
#include <functional>
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
#include "src/controller/services/settings_types.h"
#include "src/controller/subsystems/system/system_events.h"
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
    [[nodiscard]] ExploreSettingsCandidate persist_explore_filter(const ExploreSettingsCandidate&, const ExploreFilterUpdate&);
    [[nodiscard]] ExploreSettingsCandidate persist_explore_augmentation(const ExploreSettingsCandidate&, bool);
    [[nodiscard]] ExploreSettingsCandidate persist_explore_product(const ExploreSettingsCandidate&, const ExploreFilterUpdate&, bool augmentation_enabled);
    [[nodiscard]] ExploreSettingsCandidate persist_explore_detail(const ExploreSettingsCandidate&, bool);
    void persist_explore_class_catalog(const ExploreSettingsCandidate&, ExploreClassCatalogIdentity, const ExploreFilterUpdate&);

   private:
    void persist_explore_candidate(const ExploreSettingsCandidate&, std::function_ref<void(contracts::GuiSettingsState&)> mutate,
                                   std::string_view invalid_detail, bool preserve_retry_on_failure = false);
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
