#include "src/controller/services/tests/support/settings_test_fixture.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/controller/services/file_dialog_system.h"
#include "src/controller/services/settings_system.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "mmltk/frameworks/reflection/member_relation.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {

void queue_failed_dark_mode_update(SettingsSystem& settings, const std::filesystem::path& root) {
    const auto settings_path = root / "settings.json";
    REQUIRE(std::filesystem::remove(settings_path));
    REQUIRE(std::filesystem::create_directory(settings_path));
    contracts::SettingsUpdateRequest request;
    request.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "ui.dark_mode",
        .value = mmltk::frameworks::serialization::wire::FlatValue{true},
    });
    CHECK_THROWS_AS(settings.Update(std::move(request)), contracts::FailedError);
    CHECK_FALSE(settings.snapshot().settings_state.ui.dark_mode);
}
class FakeDialogRuntime final : public FileDialogRuntime {
   public:
    FakeDialogRuntime(std::shared_ptr<mmltk::testsupport::StopGate> gate, const bool fail) : gate_(std::move(gate)), fail_(fail) {}
    services::FileDialogSelection Open(const services::ResolvedFileDialog& dialog, const std::stop_token stop) override {
        if (!gate_->Wait(stop)) return {.target = dialog.target};
        if (fail_) throw contracts::FailedError("file dialog failed");
        return {.target = dialog.target, .result = services::FileDialogSelected{"/tmp/input"}};
    }

   private:
    std::shared_ptr<mmltk::testsupport::StopGate> gate_;
    bool fail_ = false;
};
TEST_CASE("ordinary settings and file dialog expose direct state, Busy, Stop, and reconstruction", "[controller][systems][services]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-services");
    std::size_t settings_events = 0U;
    SettingsSystem settings{[&settings_events](SettingsSystem::event_type) { ++settings_events; }};
    REQUIRE(settings.Load(install_settings(root)).applied());
    CHECK(settings.snapshot().revision == 1U);
    CHECK(settings_events == 1U);
    const auto explore_preferences = settings.explore_settings_candidate().preferences.policy;
    CHECK(explore_preferences.filter.minimum_instances == 0U);
    CHECK(explore_preferences.filter.maximum_instances == 10'000U);
    CHECK(explore_preferences.filter.order == ExploreOrder::Sequential);
    CHECK(explore_preferences.filter.minimum_compiled_index == 0U);
    CHECK(explore_preferences.filter.maximum_compiled_index == std::numeric_limits<std::uint64_t>::max());
    CHECK(explore_preferences.overlay.class_selection.mode == ExploreClassSelectionMode::All);
    auto gate = std::make_shared<mmltk::testsupport::StopGate>();
    std::atomic_size_t constructions = 0U;
    std::promise<FileDialogSystem::event_type> first_completion;
    std::promise<FileDialogSystem::event_type> second_completion;
    std::promise<FileDialogSystem::event_type> third_completion;
    std::atomic_size_t completions = 0U;
    FileDialogSystem dialog{[&] {
                                ++constructions;
                                return std::make_unique<FakeDialogRuntime>(gate, constructions.load() == 1U);
                            },
                            [&](FileDialogSystem::event_type event) {
                                switch (completions++) {
                                    case 0U: first_completion.set_value(std::move(event)); throw std::runtime_error("observer failure");
                                    case 1U: second_completion.set_value(std::move(event)); break;
                                    default: third_completion.set_value(std::move(event)); break;
                                }
                            }};
    const services::FileDialogOpen selector{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{services::file_dialog_catalog().entries().front().stable_id}}};
    const auto admission = dialog.Open(selector);
    CHECK(admission.generation == 1U);
    CHECK(admission.active);
    CHECK(admission.valid());
    CHECK(admission.target == selector.target);
    CHECK_FALSE(admission.selection);
    CHECK_THROWS_AS(dialog.Open(selector), contracts::BusyError);
    gate->Release();
    const auto failed_dialog = first_completion.get_future().get();
    REQUIRE(std::holds_alternative<FileDialogFailed>(failed_dialog));
    CHECK(std::get<FileDialogFailed>(failed_dialog).snapshot.generation == 1U);
    CHECK(std::get<FileDialogFailed>(failed_dialog).snapshot.target == selector.target);
    CHECK_FALSE(dialog.snapshot().active);
    CHECK(dialog.snapshot().valid());
    CHECK_FALSE(dialog.snapshot().selection);
    gate = std::make_shared<mmltk::testsupport::StopGate>();
    static_cast<void>(dialog.Open(selector));
    const auto stopping = dialog.Stop();
    CHECK(stopping.generation == 2U);
    CHECK(stopping.active);
    CHECK(stopping.cancellation_requested);
    CHECK(stopping.valid());
    CHECK(stopping.target == selector.target);
    CHECK_FALSE(stopping.selection);
    CHECK(dialog.Stop().generation == stopping.generation);
    CHECK(dialog.Stop().cancellation_requested);
    CHECK(std::holds_alternative<FileDialogCompleted>(second_completion.get_future().get()));
    REQUIRE(dialog.snapshot().selection);
    CHECK(dialog.snapshot().valid());
    CHECK(dialog.snapshot().selection->valid_for(selector.target));
    CHECK_FALSE(dialog.snapshot().selection->selected());
    gate->Release();
    // CLEANUP-IGNORE: The fake dialog service independently injects selected and cancelled outcomes; no native modal is
    // automated.
    static_cast<void>(dialog.Open(selector));
    CHECK(std::holds_alternative<FileDialogCompleted>(third_completion.get_future().get()));
    REQUIRE(dialog.snapshot().selection);
    CHECK(dialog.snapshot().valid());
    CHECK(dialog.snapshot().selection->valid_for(selector.target));
    REQUIRE(dialog.snapshot().selection->selected());
    CHECK(std::get<services::FileDialogSelected>(dialog.snapshot().selection->result).path == "/tmp/input");
    CHECK(constructions == 2U);
}
TEST_CASE("settings serializes competing durable updates", "[controller][systems][settings]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-concurrent");
    SettingsSystem settings;
    REQUIRE(settings.Load(install_settings(root)).applied());
    std::barrier start{3};
    std::atomic_int applied = 0;
    auto update = [&](contracts::SettingsValueUpdate value) {
        start.arrive_and_wait();
        try {
            contracts::SettingsUpdateRequest request;
            request.updates.emplace_back(std::move(value));
            static_cast<void>(settings.Update(std::move(request)));
            ++applied;
        } catch (...) {}
    };
    std::jthread first{update, contracts::SettingsValueUpdate{.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{true}}};
    std::jthread second{update, contracts::SettingsValueUpdate{.path = "ui.annotation_brush_radius",
                                                               .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{9}}}};
    start.arrive_and_wait();
    first.join();
    second.join();
    CHECK(applied == 2);
    const auto snapshot = settings.snapshot();
    CHECK(snapshot.revision == 3U);
    CHECK(snapshot.settings_state.ui.dark_mode);
    CHECK(snapshot.settings_state.ui.annotation_brush_radius == 9);
}
TEST_CASE("settings rejects empty updates without persisting and accepts relation clears", "[controller][systems][settings]") {
    using TrainRequest = mmltk::backend::models::rfdetr::TrainRequest;
    using TrainRecipeRelation = mmltk::frameworks::reflection::catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>;
    constexpr auto lr = mmltk::frameworks::reflection::member_path<&TrainRequest::lr>;
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-empty-update");
    const services::SettingsLocation location{(root / "gui.json").string()};
    std::size_t events = 0U;
    SettingsSystem settings{[&events](SettingsSystem::event_type) { ++events; }};
    REQUIRE(settings.Load(location).applied());
    const auto before = settings.snapshot();
    const auto events_before = events;
    CHECK_THROWS_AS(settings.Update({}), contracts::InvalidIntentError);
    CHECK(settings.snapshot() == before);
    CHECK(events == events_before);
    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    CHECK(reloaded.snapshot() == before);
    contracts::SettingsUpdateRequest pin;
    pin.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "workflows.train.request.lr",
        .value = mmltk::frameworks::serialization::wire::FlatValue{0.002},
    });
    const auto pinned = settings.Update(std::move(pin));
    CHECK(TrainRecipeRelation::template overridden<lr>(pinned.settings_state.workflows.train.request.recipe_overrides));
    contracts::SettingsUpdateRequest clear;
    clear.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "workflows.train.request.lr",
        .value = mmltk::frameworks::serialization::wire::FlatValue{std::monostate{}},
    });
    const auto cleared = settings.Update(std::move(clear));
    CHECK(cleared.revision == pinned.revision + 1U);
    CHECK_FALSE(TrainRecipeRelation::template overridden<lr>(cleared.settings_state.workflows.train.request.recipe_overrides));
    CHECK(cleared.settings_state.workflows.train.request.lr ==
          mmltk::backend::models::rfdetr::train_recipe(cleared.settings_state.workflows.train.request.optimizer).lr);
}
TEST_CASE("Explore catalog identity changes only through successful catalog persistence", "[controller][systems][settings][explore]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-explore-catalog");
    const auto location = install_settings(root);
    SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    const auto before = settings.snapshot();
    contracts::SettingsUpdateRequest request;
    request.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "workflows.explore.class_catalog_identity",
        .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{0x1234U}},
    });
    CHECK_THROWS_AS(settings.Update(std::move(request)), contracts::InvalidIntentError);
    CHECK(settings.snapshot() == before);
    constexpr ExploreClassCatalogIdentity identity = 0x9123'4567'89ab'cdefULL;
    const auto candidate = settings.explore_settings_candidate();
    static_cast<void>(settings.Update(candidate, {.preferences = candidate.preferences.policy, .class_catalog_identity = identity}));
    CHECK(settings.snapshot().settings_state.workflows.explore.class_catalog_identity == identity);
    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    CHECK(reloaded.snapshot().settings_state.workflows.explore.class_catalog_identity == identity);
}
TEST_CASE("successful Explore catalog persistence includes a pending Settings recovery", "[controller][systems][settings][explore]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-explore-catalog-pending");
    const auto location = install_settings(root);
    SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    queue_failed_dark_mode_update(settings, root);
    REQUIRE(std::filesystem::remove(root / "settings.json"));
    constexpr ExploreClassCatalogIdentity identity = 0x1234'5678U;
    const auto candidate = settings.explore_settings_candidate();
    static_cast<void>(settings.Update(candidate, {.preferences = candidate.preferences.policy, .class_catalog_identity = identity}));
    const auto committed = settings.snapshot();
    CHECK(committed.settings_state.ui.dark_mode);
    CHECK(committed.settings_state.workflows.explore.class_catalog_identity == identity);
    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    CHECK(reloaded.snapshot() == committed);
}
TEST_CASE("failed Explore catalog persistence preserves the exact pending Settings recovery", "[controller][systems][settings][explore]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-explore-catalog-retry");
    const auto location = install_settings(root);
    SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    queue_failed_dark_mode_update(settings, root);
    constexpr ExploreClassCatalogIdentity rejected_identity = 0x8765'4321U;
    const auto candidate = settings.explore_settings_candidate();
    CHECK_THROWS_AS(settings.Update(candidate, {.preferences = candidate.preferences.policy, .class_catalog_identity = rejected_identity}), contracts::FailedError);
    REQUIRE(std::filesystem::remove(root / "settings.json"));
    REQUIRE(settings.Retry().applied());
    const auto recovered = settings.snapshot();
    CHECK(recovered.settings_state.ui.dark_mode);
    CHECK(recovered.settings_state.workflows.explore.class_catalog_identity == 0U);
    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    CHECK(reloaded.snapshot() == recovered);
}
TEST_CASE("settings retry cannot overwrite a newer committed update", "[controller][systems][settings]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-retry");
    std::atomic_bool block_changed = false;
    std::promise<void> update_committed;
    std::promise<void> release_update;
    auto release = release_update.get_future().share();
    SettingsSystem settings{[&](SettingsSystem::event_type event) {
        if (block_changed && std::holds_alternative<SettingsChanged>(event)) {
            update_committed.set_value();
            release.wait();
        }
    }};
    REQUIRE(settings.Load(install_settings(root)).applied());
    queue_failed_dark_mode_update(settings, root);
    REQUIRE(std::filesystem::remove(root / "settings.json"));
    block_changed = true;
    std::jthread update{[&] {
        contracts::SettingsUpdateRequest request;
        request.updates.emplace_back(
            contracts::SettingsValueUpdate{.path = "ui.annotation_brush_radius", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{9}}});
        static_cast<void>(settings.Update(std::move(request)));
    }};
    update_committed.get_future().wait();
    std::promise<void> retry_started;
    std::jthread retry{[&] {
        retry_started.set_value();
        static_cast<void>(settings.Retry());
    }};
    retry_started.get_future().wait();
    release_update.set_value();
    update.join();
    retry.join();
    const auto snapshot = settings.snapshot();
    CHECK(snapshot.revision == 2U);
    CHECK_FALSE(snapshot.settings_state.ui.dark_mode);
    CHECK(snapshot.settings_state.ui.annotation_brush_radius == 9);
}
} // namespace
} // namespace mmltk::controller
