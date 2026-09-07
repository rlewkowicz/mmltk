#include "src/controller/services/settings_system.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "src/controller/contracts/default_state.h"
#include "src/controller/services/persistence_storage.h"
#include "src/controller/subsystems/system/local_run.h"

namespace mmltk::controller {
namespace {

[[nodiscard]] bool flat_value_is_null(const mmltk::frameworks::serialization::wire::FlatValue& value) {
    bool null = false;
    value.visit([&]<class Item>(const Item&) { null = std::same_as<std::remove_cvref_t<Item>, std::monostate>; });
    return null;
}

[[nodiscard]] bool apply_train_recipe_relation(contracts::GuiSettingsState& candidate,
                                               const std::span<const contracts::SettingsValueUpdate> updates) {
    using Relation = mmltk::frameworks::reflection::catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>;
    constexpr auto selector =
        mmltk::frameworks::reflection::member_path<&contracts::GuiSettingsState::workflows, &contracts::WorkflowSettingsState::train,
                                                   &contracts::TrainViewState::request,
                                                   &mmltk::backend::models::rfdetr::TrainRequest::optimizer>;
    auto& request = candidate.workflows.train.request;
    const auto& recipe = mmltk::backend::models::rfdetr::train_recipe(request.optimizer);
    bool valid = true;
    std::array<bool, contracts::kMaxSettingsUpdates> relation_updates{};
    Relation::VisitMembers([&]<class Entry>() {
        constexpr auto destination =
            mmltk::frameworks::reflection::rebase_member_path<contracts::GuiSettingsState, mmltk::backend::models::rfdetr::TrainRequest>(
                selector, Entry::destination);
        constexpr auto path = mmltk::frameworks::reflection::reflected_member_path<contracts::GuiSettingsState, destination>();
        for (std::size_t update_index = 0U; update_index < updates.size(); ++update_index) {
            const auto& update = updates[update_index];
            if (update.path != path.view()) continue;
            relation_updates[update_index] = true;
            if (flat_value_is_null(update.value)) {
                Entry::transform::apply(
                    mmltk::frameworks::reflection::access<mmltk::backend::models::rfdetr::TrainRequest, Entry::destination>(request),
                    mmltk::frameworks::reflection::access<const mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry, Entry::source>(
                        recipe));
                Relation::template clear_override<Entry::destination>(request.recipe_overrides);
            } else {
                Relation::template set_override<Entry::destination>(request.recipe_overrides);
            }
        }
    });
    for (std::size_t index = 0U; index < updates.size(); ++index) {
        if (flat_value_is_null(updates[index].value) && !relation_updates[index]) valid = false;
        for (std::size_t prior = 0U; prior < index; ++prior)
            valid = valid && updates[index].path != updates[prior].path;
    }
    return valid;
}

[[nodiscard]] ExploreFilterPreferences explore_preferences(const contracts::ExploreViewState& explore) {
    ExploreFilterPreferences result{
        .class_catalog_identity = explore.class_catalog_identity,
    };
    contracts::ExploreSettingsProjection::Visit([&]<auto Setting, auto Filter>() {
        mmltk::frameworks::reflection::access<ExploreFilterUpdate, Filter>(result.policy) =
            mmltk::frameworks::reflection::access<const contracts::ExploreViewState, Setting>(explore);
    });
    const auto project = [](const auto& values) {
        ExploreClassSelection selection;
        const auto selected = std::ranges::count(values, true);
        if (selected == static_cast<std::ptrdiff_t>(values.size())) {
            selection.mode = ExploreClassSelectionMode::All;
        } else if (selected == 0) {
            selection.mode = ExploreClassSelectionMode::None;
        } else {
            selection.mode = ExploreClassSelectionMode::Subset;
            for (std::size_t index = 0U; index != values.size(); ++index)
                if (values[index]) selection.classes.push_back(static_cast<std::uint32_t>(index));
        }
        return selection;
    };
    result.policy.filter.class_selection = project(explore.sample_classes);
    result.policy.overlay.class_selection = project(explore.overlay_classes);
    return result;
}

void install_explore_preferences(contracts::ExploreViewState& explore, const ExploreFilterUpdate& request) {
    contracts::ExploreSettingsProjection::Visit([&]<auto Setting, auto Filter>() {
        mmltk::frameworks::reflection::access<contracts::ExploreViewState, Setting>(explore) =
            mmltk::frameworks::reflection::access<const ExploreFilterUpdate, Filter>(request);
    });
    const auto install = [](auto& target, const ExploreClassSelection& selection) {
        target.fill(selection.mode == ExploreClassSelectionMode::All);
        if (selection.mode != ExploreClassSelectionMode::Subset) return;
        target.fill(false);
        for (const auto index : selection.classes) {
            if (index >= target.size()) throw contracts::InvalidIntentError("Explore class is outside settings capacity");
            target[index] = true;
        }
    };
    install(explore.sample_classes, request.filter.class_selection);
    install(explore.overlay_classes, request.overlay.class_selection);
}

void select_data_loading(contracts::GuiSettingsState& state, const bool h2d) {
    mmltk::frameworks::reflection::visit_materialized_members<contracts::WorkflowSettingsState>([&]<class Declaration>(const auto&) {
        auto& workflow = state.workflows.*Declaration::pointer;
        if constexpr (requires { workflow.h2d_dataloader; }) {
            workflow.h2d_dataloader = h2d;
        } else if constexpr (requires { workflow.request.h2d_dataloader; }) {
            workflow.request.h2d_dataloader = h2d;
        }
    });
}

}  // namespace

SettingsSystem::SettingsSystem(SystemEventSink<event_type> events) : events_(std::move(events)) {}

services::SettingsMutationResult SettingsSystem::Load(services::SettingsLocation location,
                                                      const std::optional<bool> h2d_dataloader_override) {
    services::SettingsMutationResult result;
    {
        std::scoped_lock mutation_lock(mutation_mutex_);
        if (!location.valid()) {
            std::scoped_lock lock(mutex_);
            terminal_ = {services::SettingsTerminal::Rejected, state_.revision, "invalid settings location"};
            result = terminal_;
        } else {
            auto loaded = services::load_persistence_settings(location);
            const auto* value = services::view_persistence_settings(loaded.settings);
            std::scoped_lock lock(mutex_);
            if (candidate_version_ == std::numeric_limits<std::uint64_t>::max()) {
                loaded_ = false;
                retryable_ = false;
                terminal_ = {services::SettingsTerminal::PersistenceFailed, state_.revision, "settings candidate version exhausted"};
            } else if (!loaded.succeeded() || value == nullptr || !contracts::gui_settings_valid(*value)) {
                loaded_ = false;
                retryable_ = false;
                ++candidate_version_;
                terminal_ = {services::SettingsTerminal::PersistenceFailed, state_.revision,
                             contracts::bounded_compute_error(loaded.detail)};
            } else {
                state_.settings_state = *value;
                h2d_dataloader_override_ = h2d_dataloader_override;
                state_.revision = std::max<std::uint64_t>(1U, std::max(state_.revision, loaded.revision_frontier));
                location_ = std::move(location);
                loaded_ = true;
                retryable_ = false;
                ++candidate_version_;
                terminal_ = {services::SettingsTerminal::Applied, state_.revision, {}};
            }
            result = terminal_;
        }
    }
    publish(result);
    return result;
}

contracts::SettingsUiState SettingsSystem::Update(contracts::SettingsUpdateRequest request) {
    if (request.updates.empty()) throw contracts::InvalidIntentError("invalid settings update");
    services::SettingsMutationResult result;
    {
        std::scoped_lock mutation_lock(mutation_mutex_);
        auto candidate = mutation_candidate();
        contracts::SettingsUpdateRequest ordinary;
        for (const auto& update : request.updates) {
            if (!flat_value_is_null(update.value)) ordinary.updates.push_back(update);
        }
        if ((!ordinary.updates.empty() && !contracts::apply_gui_settings_values(candidate, std::span{ordinary.updates})) ||
            !apply_train_recipe_relation(candidate, std::span{request.updates}) || !contracts::gui_settings_valid(candidate))
            throw contracts::InvalidIntentError("invalid settings update");
        result = persist(std::move(candidate));
    }
    publish(result);
    if (!result.applied()) throw contracts::FailedError(result.detail);
    return snapshot();
}

contracts::SettingsUiState SettingsSystem::Reset(contracts::SettingsResetRequest) {
    services::SettingsMutationResult result;
    {
        std::scoped_lock mutation_lock(mutation_mutex_);
        {
            std::scoped_lock lock(mutex_);
            if (!loaded_) throw contracts::UnavailableError("settings are not loaded");
        }
        result = persist(contracts::default_gui_settings_state());
    }
    publish(result);
    if (!result.applied()) throw contracts::FailedError(result.detail);
    return snapshot();
}

services::SettingsMutationResult SettingsSystem::Retry() {
    services::SettingsMutationResult result;
    {
        std::scoped_lock mutation_lock(mutation_mutex_);
        contracts::GuiSettingsState candidate;
        {
            std::scoped_lock lock(mutex_);
            if (!retryable_) return terminal_;
            candidate = retry_snapshot_;
        }
        result = persist(std::move(candidate));
    }
    publish(result);
    return result;
}

contracts::GuiSettingsState SettingsSystem::mutation_candidate() const {
    std::scoped_lock lock(mutex_);
    if (!loaded_) throw contracts::UnavailableError("settings are not loaded");
    return state_.settings_state;
}

services::SettingsMutationResult SettingsSystem::persist(contracts::GuiSettingsState candidate, const bool preserve_retry_on_failure) {
    services::SettingsMutationResult result;
    services::SettingsLocation location{std::string_view{}};
    std::uint64_t revision = 0;
    std::uint64_t candidate_version = 0U;
    {
        std::scoped_lock lock(mutex_);
        if (!loaded_ || !location_.valid()) {
            terminal_ = {services::SettingsTerminal::NotLoaded, state_.revision, "settings are not loaded"};
        } else if (state_.revision == std::numeric_limits<std::uint64_t>::max()) {
            terminal_ = {services::SettingsTerminal::PersistenceFailed, state_.revision, "settings revision exhausted"};
        } else if (candidate_version_ == std::numeric_limits<std::uint64_t>::max()) {
            terminal_ = {services::SettingsTerminal::PersistenceFailed, state_.revision, "settings candidate version exhausted"};
        } else {
            location = location_;
            revision = state_.revision + 1U;
            candidate_version = candidate_version_ + 1U;
        }
        result = terminal_;
    }
    if (revision == 0) return result;
    const auto saved = services::save_persistence_settings(location, services::make_persistence_settings_snapshot(candidate), revision);
    {
        std::scoped_lock lock(mutex_);
        if (!saved.succeeded() || saved.revision != revision) {
            if (!preserve_retry_on_failure) {
                retry_snapshot_ = std::move(candidate);
                retryable_ = true;
                candidate_version_ = candidate_version;
            }
            terminal_ = {services::SettingsTerminal::PersistenceFailed, state_.revision, contracts::bounded_compute_error(saved.detail)};
        } else {
            state_.settings_state = std::move(candidate);
            state_.revision = revision;
            retry_snapshot_ = {};
            retryable_ = false;
            candidate_version_ = candidate_version;
            terminal_ = {services::SettingsTerminal::Applied, revision, {}};
        }
        result = terminal_;
    }
    return result;
}

void SettingsSystem::publish(const services::SettingsMutationResult& result) noexcept {
    if (result.applied()) direct::PublishLazyNoexcept(events_, [this] { return event_type{SettingsChanged{snapshot()}}; });
}

contracts::SettingsUiState SettingsSystem::snapshot() const {
    std::scoped_lock lock(mutex_);
    auto result = state_;
    if (h2d_dataloader_override_) select_data_loading(result.settings_state, *h2d_dataloader_override_);
    result.explore_source = contracts::resolve_explore_source(result.settings_state);
    return result;
}

void SettingsSystem::require_loaded() const {
    std::scoped_lock lock(mutex_);
    if (!loaded_) throw contracts::UnavailableError("settings are not loaded");
}

contracts::SettingsMaterializationFacts SettingsSystem::materialization_facts() const {
    std::scoped_lock lock(mutex_);
    auto settings = state_.settings_state;
    if (h2d_dataloader_override_) select_data_loading(settings, *h2d_dataloader_override_);
    return {std::move(settings), state_.revision, loaded_};
}
contracts::ProviderPreferences SettingsSystem::provider_preferences() const {
    std::scoped_lock lock(mutex_);
    contracts::ProviderPreferences result;
    if (!loaded_) return result;
    const auto& train = state_.settings_state.workflows.train;
    result.image = train.remote_container_image;
    result.template_text = train.remote_launch_template;
    for (std::size_t index = 0; index < train.remote_family_enabled.size(); ++index)
        if (train.remote_family_enabled[index])
            if (const auto family = contracts::provider_gpu_family_from_index(index)) result.families.push_back(*family);
    return result;
}

ExploreSettingsCandidate SettingsSystem::explore_settings_candidate() const {
    std::scoped_lock lock(mutex_);
    if (!loaded_) throw contracts::UnavailableError("settings are not loaded");
    const auto& settings = retryable_ ? retry_snapshot_ : state_.settings_state;
    auto loading = static_cast<mmltk::backend::data::DataLoadingOptions>(settings.workflows.explore);
    if (h2d_dataloader_override_) loading.h2d_dataloader = *h2d_dataloader_override_;
    return {
        .version = candidate_version_,
        .loading = loading,
        .device_id = settings.workflows.explore.device_id,
        .preferences = explore_preferences(settings.workflows.explore),
        .augmentation = settings.workflows.train.request.gpu_augmentation,
        .augmentation_preview_enabled = settings.workflows.train.visualize_augmentation_in_explore,
        .show_original_dimensions = settings.workflows.explore.show_original_dimensions,
    };
}

void SettingsSystem::persist_explore_candidate(const ExploreSettingsCandidate& installed,
                                               const std::function_ref<void(contracts::GuiSettingsState&)> mutate,
                                               const std::string_view invalid_detail, const bool preserve_retry_on_failure) {
    services::SettingsMutationResult result;
    {
        std::scoped_lock mutation_lock(mutation_mutex_);
        contracts::GuiSettingsState candidate;
        {
            std::scoped_lock lock(mutex_);
            if (!loaded_) throw contracts::UnavailableError("settings are not loaded");
            if (installed.version != candidate_version_) throw contracts::BusyError("Explore settings candidate is stale");
            candidate = retryable_ ? retry_snapshot_ : state_.settings_state;
        }
        mutate(candidate);
        if (!contracts::gui_settings_valid(candidate)) throw contracts::InvalidIntentError(std::string(invalid_detail));
        result = persist(std::move(candidate), preserve_retry_on_failure);
    }
    publish(result);
    if (!result.applied()) throw contracts::FailedError(result.detail);
}

ExploreSettingsCandidate SettingsSystem::persist_explore_augmentation(const ExploreSettingsCandidate& installed, const bool enabled) {
    persist_explore_candidate(
        installed, [enabled](auto& candidate) { candidate.workflows.train.visualize_augmentation_in_explore = enabled; },
        "Explore augmentation settings are invalid");
    return explore_settings_candidate();
}

ExploreSettingsCandidate SettingsSystem::persist_explore_detail(const ExploreSettingsCandidate& installed,
                                                                const bool show_original_dimensions) {
    persist_explore_candidate(
        installed,
        [show_original_dimensions](auto& candidate) { candidate.workflows.explore.show_original_dimensions = show_original_dimensions; },
        "Explore detail settings are invalid");
    return explore_settings_candidate();
}

ExploreSettingsCandidate SettingsSystem::persist_explore_product(const ExploreSettingsCandidate& installed,
                                                                 const ExploreFilterUpdate& policy, const bool augmentation_enabled) {
    persist_explore_candidate(
        installed,
        [&](auto& candidate) {
            install_explore_preferences(candidate.workflows.explore, policy);
            candidate.workflows.train.visualize_augmentation_in_explore = augmentation_enabled;
        },
        "Explore product preferences are invalid");
    return explore_settings_candidate();
}

ExploreSettingsCandidate SettingsSystem::persist_explore_filter(const ExploreSettingsCandidate& installed,
                                                                const ExploreFilterUpdate& request) {
    persist_explore_candidate(
        installed, [&request](auto& candidate) { install_explore_preferences(candidate.workflows.explore, request); },
        "Explore filter preferences are invalid");
    return explore_settings_candidate();
}

void SettingsSystem::persist_explore_class_catalog(const ExploreSettingsCandidate& settings_candidate,
                                                   const ExploreClassCatalogIdentity identity, const ExploreFilterUpdate& preferences) {
    persist_explore_candidate(
        settings_candidate,
        [&preferences, identity](auto& candidate) {
            auto& explore = candidate.workflows.explore;
            install_explore_preferences(explore, preferences);
            explore.class_catalog_identity = identity;
        },
        "Explore preferences are invalid", true);
}

}  // namespace mmltk::controller
