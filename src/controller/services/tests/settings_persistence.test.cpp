#include "src/controller/services/tests/support/settings_test_fixture.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/controller/services/file_dialog_system.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/contracts/model_selection.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "mmltk/frameworks/reflection/member_relation.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <array>
#include <barrier>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
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
TEST_CASE("prediction source saving choices persist independently", "[controller][systems][settings][prediction]") {
 const auto root = mmltk::testsupport::make_temp_root("prediction-saving-settings");
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root)).applied());
 const auto defaults = settings.snapshot().settings_state.workflows.predict.saving;
 CHECK(defaults.single_enabled);
 CHECK(defaults.compiled_enabled);
 CHECK(defaults.compiled_percent == 10U);
 CHECK(defaults.video_enabled);
 CHECK(defaults.video_mode == contracts::PredictionVideoSaving::Full);
 contracts::SettingsUpdateRequest edit;
 edit.updates.push_back({.path = "workflows.predict.saving.single_enabled", .value = mmltk::frameworks::serialization::wire::FlatValue{false}});
 edit.updates.push_back({.path = "workflows.predict.saving.compiled_percent", .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{37}}});
 edit.updates.push_back({.path = "workflows.predict.saving.compiled_total", .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{3}}});
 edit.updates.push_back({.path = "workflows.predict.saving.video_samples", .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{17}}});
 static_cast<void>(settings.Update(std::move(edit)));
 SettingsSystem restored;
 REQUIRE(restored.Load(services::SettingsLocation{(root / "settings.json").string()}).applied());
 const auto saved = restored.snapshot().settings_state.workflows.predict.saving;
 CHECK_FALSE(saved.single_enabled);
 CHECK(saved.compiled_enabled);
 CHECK(saved.compiled_percent == 37U);
 CHECK(saved.compiled_total == 3U);
 CHECK(saved.video_samples == 17U);
 CHECK(saved.video_mode == contracts::PredictionVideoSaving::Full);
}
TEST_CASE("Shared native inference compilation policies survive settings reconstruction", "[controller][settings][compilation]") {
 const auto root = mmltk::testsupport::make_temp_root("compilation-settings");
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root)).applied());
 contracts::SettingsUpdateRequest edit;
 edit.updates.push_back({.path = "workflows.predict.request.compilation_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{0}}});
 edit.updates.push_back({.path = "workflows.validate.request.compilation_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{0}}});
 static_cast<void>(settings.Update(std::move(edit)));
 SettingsSystem restored;
 REQUIRE(restored.Load(services::SettingsLocation{(root / "settings.json").string()}).applied());
 const auto state = restored.snapshot().settings_state;
 CHECK(state.workflows.predict.request.compilation_mode == mmltk::backend::models::rfdetr::CompilationMode::kNone);
 CHECK(state.workflows.validate.request.compilation_mode == mmltk::backend::models::rfdetr::CompilationMode::kNone);
}
TEST_CASE("accepted display confidence survives Settings reconstruction", "[controller][systems][settings]") {
 const auto root = mmltk::testsupport::make_temp_root("validation-display-settings");
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root)).applied());
 CHECK(settings.validation_display_settings().confidence_threshold == 0.4F);
 contracts::SettingsUpdateRequest edit;
 edit.updates.push_back({.path = "workflows.validate.display.confidence_threshold", .value = mmltk::frameworks::serialization::wire::FlatValue{0.437}});
 static_cast<void>(settings.Update(std::move(edit)));
 SettingsSystem restored;
 REQUIRE(restored.Load(services::SettingsLocation{(root / "settings.json").string()}).applied());
 CHECK(restored.validation_display_settings().confidence_threshold == 0.437F);
}
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
 const services::FileDialogOpen selector{.target = services::FileDialogTarget{services::SettingsFieldTarget{services::file_dialog_catalog().entries().front().stable_id}}};
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
 std::jthread second{update, contracts::SettingsValueUpdate{.path = "ui.annotation_brush_radius", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{9}}}};
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
 using TrainRecipeSettings = mmltk::backend::models::rfdetr::TrainRecipeSettings;
 using TrainRecipeRelation = mmltk::frameworks::reflection::catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>;
 constexpr auto lr = mmltk::frameworks::reflection::member_path<&TrainRecipeSettings::lr>;
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
  .path = "workflows.train.request.recipe.lr",
  .value = mmltk::frameworks::serialization::wire::FlatValue{0.002},
 });
 const auto pinned = settings.Update(std::move(pin));
 CHECK(TrainRecipeRelation::template overridden<lr>(pinned.settings_state.workflows.train.request.recipe.overrides));
 contracts::SettingsUpdateRequest clear;
 clear.updates.emplace_back(contracts::SettingsValueUpdate{
  .path = "workflows.train.request.recipe.lr",
  .value = mmltk::frameworks::serialization::wire::FlatValue{std::monostate{}},
 });
 const auto cleared = settings.Update(std::move(clear));
 CHECK(cleared.revision == pinned.revision + 1U);
 CHECK_FALSE(TrainRecipeRelation::template overridden<lr>(cleared.settings_state.workflows.train.request.recipe.overrides));
 CHECK(cleared.settings_state.workflows.train.request.recipe.lr == mmltk::backend::models::rfdetr::train_recipe(cleared.settings_state.workflows.train.request.recipe.optimizer).lr);
}
TEST_CASE("all recipe field clears resolve the selected catalog and preserve model recipes", "[controller][systems][settings]") {
 namespace r = mmltk::backend::models::rfdetr;
 namespace reflection = mmltk::frameworks::reflection;
 using Relation = reflection::catalog_provider_relation<r::TrainRecipeCatalog>;
 using Value = mmltk::frameworks::serialization::wire::FlatValue;
 for (const auto& row : r::kTrainRecipeCatalog) {
  const mmltk::testsupport::ScopedTempDir root("scoped-recipe-clears");
  SettingsSystem settings;
  REQUIRE(settings.Load(install_settings(root.path())).applied());
  contracts::SettingsUpdateRequest grow;
  grow.training_model_count = 1;
  auto lanes = settings.Update(grow).settings_state.workflows.train.request.lane_configuration;
  lanes.models.front().recipe.lr = .031;
  Relation::set_override<reflection::member_path<&r::TrainRecipeSettings::lr>>(lanes.models.front().recipe.overrides);
  contracts::SettingsUpdateRequest select;
  select.lane_configuration = lanes;
  select.updates.push_back({"workflows.train.request.recipe.optimizer", *Value::text(reflection::enum_name(row.optimizer), reflection::kMaximumNameBytes)});
  const auto selected = settings.Update(std::move(select));
  CHECK(static_cast<const r::TrainRecipeValues&>(selected.settings_state.workflows.train.request.recipe) == static_cast<const r::TrainRecipeValues&>(row));
  Relation::VisitMembers([&]<class Entry>() {
   constexpr auto relative = reflection::reflected_member_path<r::TrainRecipeSettings, Entry::destination>();
   const std::string path = "workflows.train.request.recipe." + std::string(relative.view());
   const auto field = reflection::access<const r::TrainRecipeCatalogEntry, Entry::source>(row);
   const auto wire = [&] {
    using Field = std::remove_cvref_t<decltype(field)>;
    if constexpr (std::is_enum_v<Field>)
     return *Value::text(reflection::enum_name(field), reflection::kMaximumNameBytes);
    else if constexpr (std::same_as<Field, int>)
     return Value{static_cast<std::int64_t>(field)};
    else
     return Value{field};
   }();
   contracts::SettingsUpdateRequest pin;
   pin.updates.push_back({path, wire});
   const auto pinned = settings.Update(std::move(pin));
   CHECK(Relation::overridden<Entry::destination>(pinned.settings_state.workflows.train.request.recipe.overrides));
   contracts::SettingsUpdateRequest clear;
   clear.updates.push_back({path, Value{std::monostate{}}});
   const auto cleared = settings.Update(std::move(clear));
   CHECK(cleared.revision == pinned.revision + 1U);
   CHECK_FALSE(Relation::overridden<Entry::destination>(cleared.settings_state.workflows.train.request.recipe.overrides));
   CHECK(static_cast<const r::TrainRecipeValues&>(cleared.settings_state.workflows.train.request.recipe) == static_cast<const r::TrainRecipeValues&>(row));
   CHECK(cleared.settings_state.workflows.train.request.lane_configuration == lanes);
  });
  SettingsSystem restored;
  REQUIRE(restored.Load(services::SettingsLocation{(root.path() / "settings.json").string()}).applied());
  CHECK(restored.snapshot().settings_state == settings.snapshot().settings_state);
 }
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
  request.updates.emplace_back(contracts::SettingsValueUpdate{.path = "ui.annotation_brush_radius", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{9}}});
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
TEST_CASE("Native model count resolves accompanying global recipe before atomic growth", "[controller][settings][training]") {
 namespace r = mmltk::backend::models::rfdetr;
 using Value = mmltk::frameworks::serialization::wire::FlatValue;
 const mmltk::testsupport::ScopedTempDir root("native-model-count");
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root.path())).applied());
 contracts::SettingsUpdateRequest grow;
 grow.training_model_count = 2;
 grow.updates.push_back({"workflows.train.request.recipe.optimizer", *Value::text("SGD", mmltk::frameworks::reflection::kMaximumNameBytes)});
 grow.updates.push_back({"workflows.train.request.recipe.lr", Value{.025}});
 const auto initial = settings.snapshot();
 const auto grown = settings.Update(grow);
 CHECK(grown.revision == initial.revision + 1);
 const auto& training = grown.settings_state.workflows.train.request;
 REQUIRE(training.lane_configuration.models.size() == 2);
 for (const auto& model : training.lane_configuration.models) {
  CHECK(model.recipe == training.recipe);
  CHECK(model.seed == r::training_stochastic_key(training.seed, model.model_id, 0, 0));
 }
 grow.training_model_count = 3;
 grow.updates.back().value = Value{.05};
 const auto appended = settings.Update(grow);
 CHECK(appended.settings_state.workflows.train.request.lane_configuration.models.front() == training.lane_configuration.models.front());
 CHECK(appended.settings_state.workflows.train.request.lane_configuration.models.back().recipe.lr == .05);
 contracts::SettingsUpdateRequest mode;
 mode.lane_configuration = appended.settings_state.workflows.train.request.lane_configuration;
 mode.lane_configuration->mode = r::TrainLaneMode::PeriodicAveraging;
 const auto changed = settings.Update(mode);
 CHECK(changed.settings_state.workflows.train.request.lane_configuration.models == appended.settings_state.workflows.train.request.lane_configuration.models);
 for (const auto count : {0U, 17U}) {
  contracts::SettingsUpdateRequest invalid;
  invalid.training_model_count = count;
  CHECK_THROWS(settings.Update(invalid));
  CHECK(settings.snapshot().revision == changed.revision);
 }
}
TEST_CASE("Admitted training rejects queued execution changes but preserves independent settings progress", "[controller][settings][training]") {
 const mmltk::testsupport::ScopedTempDir root("locked-model-count");
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root.path())).applied());
 const auto initial = settings.snapshot();
 settings.LockTrainingConfiguration(initial.revision);
 contracts::SettingsUpdateRequest grow;
 grow.training_model_count = 2;
 CHECK_THROWS_AS(settings.Update(grow), contracts::BusyError);
 using Value = mmltk::frameworks::serialization::wire::FlatValue;
 for (const auto& update : std::array{contracts::SettingsValueUpdate{"workflows.train.compiled_dataset_dir", *Value::text("/next/dataset", mmltk::frameworks::reflection::kMaximumPathBytes)},
       contracts::SettingsValueUpdate{"workflows.train.model_source", Value{std::int64_t{0}}}}) {
  contracts::SettingsUpdateRequest locked;
  locked.updates.push_back(update);
  CHECK_THROWS_AS(settings.Update(locked), contracts::BusyError);
  CHECK(settings.snapshot().revision == initial.revision);
  CHECK(settings.snapshot().settings_state.workflows.train.request == initial.settings_state.workflows.train.request);
 }
 CHECK_THROWS_AS(settings.Load(services::SettingsLocation{(root.path() / "settings.json").string()}, true), contracts::BusyError);
 contracts::SettingsUpdateRequest independent;
 independent.updates.push_back({"ui.dark_mode", mmltk::frameworks::serialization::wire::FlatValue{true}});
 independent.updates.push_back({"workflows.train.output.automatic", mmltk::frameworks::serialization::wire::FlatValue{false}});
 const auto changed = settings.Update(independent);
 CHECK(changed.revision == initial.revision + 1);
 CHECK_FALSE(changed.settings_state.workflows.train.output.automatic);
 CHECK(changed.settings_state.workflows.train.request == initial.settings_state.workflows.train.request);
 CHECK_THROWS_AS(settings.Reset({}), contracts::BusyError);
 settings.UnlockTrainingConfiguration();
 CHECK_NOTHROW(settings.Update(grow));
 CHECK_THROWS_AS(settings.LockTrainingConfiguration(initial.revision), contracts::BusyError);
}
}  // namespace
}  // namespace mmltk::controller
namespace mmltk::controller {
namespace {
TEST_CASE("export formats persist independently without changing weights selection", "[controller][systems][settings][export]") {
 mmltk::testsupport::ScopedTempDir root{"export-format-settings"};
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root.path())).applied());
 const auto before = contracts::model_settings_projection(settings.snapshot().settings_state, contracts::FeatureId::Export);
 REQUIRE(before);
 for (const bool onnx : {false, true}) {
  for (const bool engine : {false, true}) {
   contracts::SettingsUpdateRequest edit;
   edit.updates = {{.path = "workflows.export_state.export_onnx", .value = mmltk::frameworks::serialization::wire::FlatValue{onnx}},
    {.path = "workflows.export_state.build_tensorrt", .value = mmltk::frameworks::serialization::wire::FlatValue{engine}}};
   static_cast<void>(settings.Update(std::move(edit)));
   SettingsSystem restored;
   REQUIRE(restored.Load(services::SettingsLocation{(root.path() / "settings.json").string()}).applied());
   const auto state = restored.snapshot().settings_state;
   CHECK(state.workflows.export_state.export_onnx == onnx);
   CHECK(state.workflows.export_state.build_tensorrt == engine);
   CHECK(contracts::model_settings_projection(state, contracts::FeatureId::Export) == before);
   CHECK_FALSE(std::filesystem::exists(root.path() / "export"));
  }
 }
}
TEST_CASE("Typed lane replacement and scalar edits publish one checked revision and persist stable recipes", "[controller][settings][training]") {
 namespace r = mmltk::backend::models::rfdetr;
 const mmltk::testsupport::ScopedTempDir root("typed-training-lanes");
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root.path())).applied());
 contracts::SettingsUpdateRequest grow;
 grow.training_model_count = 2;
 const auto initial = settings.Update(grow);
 auto lanes = initial.settings_state.workflows.train.request.lane_configuration;
 lanes.mode = r::TrainLaneMode::Independent;
 lanes.models[1].recipe.optimizer = r::TrainOptimizerKind::SGD;
 contracts::SettingsUpdateRequest request;
 request.lane_configuration = lanes;
 using Value = mmltk::frameworks::serialization::wire::FlatValue;
 request.updates.push_back({"workflows.train.request.lanes", Value{int64_t{2}}});
 request.updates.push_back({"workflows.train.request.batch_size", Value{uint64_t{3}}});
 request.updates.push_back({"workflows.train.request.grad_accum_steps", Value{int64_t{4}}});
 request.updates.push_back({"workflows.train.request.validation_lanes", Value{int64_t{5}}});
 request.updates.push_back({"workflows.train.request.val_batch_size", Value{uint64_t{2}}});
 const auto saved = settings.Update(request);
 CHECK(saved.revision == initial.revision + 1);
 CHECK(saved.train_execution.settings_revision == saved.revision);
 CHECK(saved.train_execution.logical_models == 2);
 CHECK(saved.train_execution.microbatches_per_attempt == 4);
 CHECK(saved.train_execution.effective_batch_per_model == 12);
 CHECK(saved.train_execution.aggregate_round_images == 24);
 CHECK(saved.training_validation_execution.effective_batch_per_model == 10);
 CHECK(saved.training_validation_execution.settings_revision == saved.revision);
 CHECK(saved.prediction_execution.settings_revision == saved.revision);
 CHECK(saved.validation_execution.settings_revision == saved.revision);
 CHECK(saved.settings_state.workflows.train.request.lane_configuration.models[1].recipe.lr == .01);
 CHECK(saved.settings_state.workflows.train.request.recipe.optimizer == r::TrainOptimizerKind::AdamW);
 CHECK(r::effective_final_policy(lanes) == r::TrainFinalPolicy::ValidationGreedy);
 SettingsSystem restored;
 REQUIRE(restored.Load(services::SettingsLocation{(root.path() / "settings.json").string()}).applied());
 CHECK(restored.snapshot().settings_state == saved.settings_state);
 for (int fault = 0; fault < 6; ++fault) {
  auto invalid = request;
  switch (fault) {
   case 0: invalid.lane_configuration->models.pop_back(); break;
   case 1: invalid.lane_configuration->models[1].model_id = invalid.lane_configuration->models[0].model_id; break;
   case 2: invalid.lane_configuration->models[0].coefficient = std::numeric_limits<double>::infinity(); break;
   case 3: invalid.lane_configuration->next_model_id = 1; break;
   case 4:
    invalid.lane_configuration->final_policy = r::TrainFinalPolicy::Explicit;
    for (auto& model : invalid.lane_configuration->models) model.coefficient = 0;
    break;
   case 5: invalid.updates.push_back({"workflows.train.request.lane_configuration.models.0.recipe.lr", Value{.1}}); break;
  }
  CHECK_THROWS(settings.Update(std::move(invalid)));
  CHECK(settings.snapshot().settings_state == saved.settings_state);
  CHECK(settings.snapshot().revision == saved.revision);
 }
 lanes = saved.settings_state.workflows.train.request.lane_configuration;
 const auto retired = lanes.models[1];
 contracts::SettingsUpdateRequest shrink;
 shrink.training_model_count = 1;
 const auto shrunk = settings.Update(shrink);
 lanes = shrunk.settings_state.workflows.train.request.lane_configuration;
 lanes.models.push_back(retired);
 contracts::SettingsUpdateRequest reuse;
 reuse.lane_configuration = lanes;
 CHECK_THROWS(settings.Update(reuse));
 shrink.training_model_count = 2;
 const auto regrown = settings.Update(shrink);
 CHECK(regrown.settings_state.workflows.train.request.lane_configuration.models.back().model_id > retired.model_id);
 CHECK(regrown.settings_state.workflows.train.request.lane_configuration.models.front() == saved.settings_state.workflows.train.request.lane_configuration.models.front());
}
}  // namespace
}  // namespace mmltk::controller
