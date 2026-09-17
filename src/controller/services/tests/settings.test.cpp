#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/controller/contracts/default_state.h"
#include "src/controller/contracts/explore_filter.h"
#include "src/controller/contracts/gui_settings.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/model_selection.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/controller/services/file_dialog_types.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/controller/services/settings_system.h"
#include "src/frameworks/reflection/field_policy.h"
#include "mmltk/frameworks/reflection/member_path.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/serialization/serialization.h"
namespace {
using namespace mmltk::controller::contracts;
using namespace mmltk::controller::services;
using mmltk::controller::ExploreOrder;
using SettingsViewStates = GuiSettingsState;
GuiSettingsState& make_snapshot(SettingsViewStates& states) {
    states.workflows.annotate.preset_name = "rf-detr-seg-medium";
    states.current_view = mmltk::controller::contracts::FeatureId::Annotate;
    return states;
}
bool load_settings(const std::filesystem::path& path, GuiSettingsState& snapshot, nlohmann::json* const normalized_document = nullptr,
                   bool* const repair_required = nullptr) {
    return load_gui_settings_file(path.string(), snapshot, normalized_document, repair_required);
}
[[nodiscard]] auto mutable_settings_views(SettingsViewStates& states) noexcept {
    return std::tie(states.ui, states.workflows.train, states.workflows.validate, states.workflows.predict, states.workflows.annotate,
                    states.workflows.export_state, states.workflows.explore);
}
using TrainOptimizerKind = mmltk::backend::models::rfdetr::TrainOptimizerKind;
using TrainLrSchedulerKind = mmltk::backend::models::rfdetr::TrainLrSchedulerKind;
using TrainAssignmentKind = mmltk::backend::models::rfdetr::TrainAssignmentKind;
using TrainRequest = mmltk::backend::models::rfdetr::TrainRequest;
using TrainRecipeRelation = mmltk::frameworks::reflection::catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>;
template <class Member, class Visitor>
[[nodiscard]] bool visit_recipe_member(Member TrainRequest::* member, Visitor&& visitor) {
    bool matched = false;
    TrainRecipeRelation::VisitMembers([&]<class Entry>() {
        constexpr auto destination = std::remove_cvref_t<decltype(Entry::destination)>::terminal_member;
        if constexpr (std::same_as<std::remove_cvref_t<decltype(destination)>, Member TrainRequest::*>) {
            if (destination == member) {
                matched = true;
                visitor.template operator()<Entry>();
            }
        }
    });
    return matched;
}
template <class Member>
void set_recipe_override(mmltk::backend::models::rfdetr::TrainRecipeOverrideState& state, Member TrainRequest::* member, const bool overridden) {
    const bool matched = visit_recipe_member(member, [&]<class Entry>() {
        if (overridden)
            TrainRecipeRelation::template set_override<Entry::destination>(state);
        else
            TrainRecipeRelation::template clear_override<Entry::destination>(state);
    });
    REQUIRE(matched);
}
template <class Member>
[[nodiscard]] bool recipe_overridden(const mmltk::backend::models::rfdetr::TrainRecipeOverrideState& state, Member TrainRequest::* member) {
    bool result = false;
    const bool matched = visit_recipe_member(member, [&]<class Entry>() { result = TrainRecipeRelation::template overridden<Entry::destination>(state); });
    REQUIRE(matched);
    return result;
}
template <auto Access>
[[nodiscard]] consteval bool settings_path_is(const std::string_view expected) {
    return mmltk::frameworks::reflection::reflected_member_path<GuiSettingsState, Access>().view() == expected;
}
using WorkflowSettingsState = mmltk::controller::contracts::WorkflowSettingsState;
using ModelArtifactRequest = mmltk::backend::models::rfdetr::ModelArtifactRequest;
static_assert(!mmltk::frameworks::reflection::accessor_is_applicable<
              GuiSettingsState, mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &ExploreViewState::show_boxes>>());
static_assert(mmltk::frameworks::reflection::accessor_is_applicable<ExploreViewState, &ExploreViewState::show_boxes>());
static_assert(mmltk::frameworks::reflection::accessor_is_applicable<
              const GuiSettingsState,
              mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::explore, &ExploreViewState::show_boxes>>());
static_assert(settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train,
                                                                          &TrainExecutionPaneState::execution_target>>("workflows.train.execution_target"));
static_assert(
    settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::explore, &ExploreViewState::show_boxes>>(
        "workflows.explore.show_boxes"));
static_assert(settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                                                          &TrainRequest::weights_path>>("workflows.train.request.weights_path"));
static_assert(
    settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                                                &ModelArtifactRequest::weights_path>>("workflows.validate.request.weights_path"));
static_assert(
    settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                                                &ModelArtifactRequest::onnx_path>>("workflows.validate.request.onnx_path"));
static_assert(
    settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                                                &ModelArtifactRequest::tensorrt_path>>("workflows.validate.request.tensorrt_path"));
static_assert(
    settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                                                &ModelArtifactRequest::weights_path>>("workflows.predict.request.weights_path"));
static_assert(
    settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                                                &ModelArtifactRequest::onnx_path>>("workflows.predict.request.onnx_path"));
static_assert(
    settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                                                &ModelArtifactRequest::tensorrt_path>>("workflows.predict.request.tensorrt_path"));
static_assert(settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state,
                                                                          &ExportViewState::weights_path>>("workflows.export_state.weights_path"));
static_assert(settings_path_is<mmltk::frameworks::reflection::member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::export_state,
                                                                          &ExportViewState::onnx_input_path>>("workflows.export_state.onnx_input_path"));
struct OptimizerFixture final {
    int persisted_id;
    TrainOptimizerKind native_value;
};
constexpr std::array kOptimizerFixtures{
    OptimizerFixture{.persisted_id = 0, .native_value = TrainOptimizerKind::AdamW},
    OptimizerFixture{.persisted_id = 1, .native_value = TrainOptimizerKind::Muon},
};
struct DoubleRecipeFixture final {
    std::string_view key;
    double TrainRequest::* value_member;
    double adamw_value;
    double muon_value;
    double corpus_value;
    double prior_value;
    double invalid_value;
    bool corpus_override;
};
// These literal keys, persisted values, and member bindings are the independent schema-v8 oracle.
// They intentionally do not consume or reproduce the production persistence visitor.
constexpr std::array kDoubleRecipeFixtures{
    DoubleRecipeFixture{"lr", &TrainRequest::lr, 1.0e-4, 2.0e-4, 1.23e-3, 2.34e-3, -1.0, true},
    DoubleRecipeFixture{"lr_encoder", &TrainRequest::lr_encoder, 1.5e-4, 3.0e-4, 3.45e-3, 4.56e-3, -1.0, false},
    DoubleRecipeFixture{"lr_component_decay", &TrainRequest::lr_component_decay, 0.7, 0.7, 0.23, 0.34, 1.01, true},
    DoubleRecipeFixture{"encoder_layer_decay", &TrainRequest::encoder_layer_decay, 0.8, 0.8, 0.35, 0.46, 1.01, false},
    DoubleRecipeFixture{"momentum", &TrainRequest::momentum, 0.95, 0.9, 0.57, 0.68, 1.01, true},
    DoubleRecipeFixture{"weight_decay", &TrainRequest::weight_decay, 1.0e-4, 5.0e-4, 5.67e-3, 6.78e-3, -1.0, false},
    DoubleRecipeFixture{"warmup_epochs", &TrainRequest::warmup_epochs, 0.0, 3.0, 7.89, 8.91, -1.0, true},
    DoubleRecipeFixture{"warmup_momentum", &TrainRequest::warmup_momentum, 0.0, 0.8, 0.71, 0.82, 1.01, false},
    DoubleRecipeFixture{"lr_min_factor", &TrainRequest::lr_min_factor, 0.0, 0.01, 0.93, 0.84, 1.01, true},
};
constexpr auto kGoldenRecipeKeys = [] {
    std::array<std::string_view, 11U> keys{};
    for (std::size_t index = 0U; index < kDoubleRecipeFixtures.size(); ++index) { keys[index] = kDoubleRecipeFixtures[index].key; }
    keys[9U] = "lr_drop";
    keys[10U] = "lr_scheduler";
    return keys;
}();
constexpr int kCorpusLrDrop = 53;
constexpr int kPriorLrDrop = 137;
constexpr bool kCorpusLrDropOverride = false;
constexpr TrainLrSchedulerKind kCorpusScheduler = TrainLrSchedulerKind::Cosine;
constexpr TrainLrSchedulerKind kPriorScheduler = TrainLrSchedulerKind::Step;
constexpr bool kCorpusSchedulerOverride = true;
[[nodiscard]] nlohmann::json uniform_recipe_overrides(const bool value) {
    nlohmann::json overrides = nlohmann::json::object();
    for (const std::string_view key : kGoldenRecipeKeys) { overrides[std::string{key}] = value; }
    return overrides;
}
[[nodiscard]] nlohmann::json golden_recipe_values(const int persisted_optimizer_id) {
    nlohmann::json values = nlohmann::json::object();
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) {
        values[std::string{field.key}] = persisted_optimizer_id == 0 ? field.adamw_value : field.muon_value;
    }
    values["lr_drop"] = 100;
    values["lr_scheduler"] = persisted_optimizer_id == 0 ? "step" : "cosine";
    return values;
}
[[nodiscard]] nlohmann::json nonuniform_recipe_values() {
    nlohmann::json values = nlohmann::json::object();
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) { values[std::string{field.key}] = field.corpus_value; }
    values["lr_drop"] = kCorpusLrDrop;
    values["lr_scheduler"] = "cosine";
    return values;
}
[[nodiscard]] nlohmann::json nonuniform_recipe_overrides() {
    nlohmann::json overrides = nlohmann::json::object();
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) { overrides[std::string{field.key}] = field.corpus_override; }
    overrides["lr_drop"] = kCorpusLrDropOverride;
    overrides["lr_scheduler"] = kCorpusSchedulerOverride;
    return overrides;
}
[[nodiscard]] nlohmann::json recipe_document(const int persisted_optimizer_id, const nlohmann::json& values, const nlohmann::json& overrides) {
    nlohmann::json document = default_gui_settings_document();
    nlohmann::json& training = document["workflows"]["train"]["training"];
    training.update(values);
    training["optimizer"] = persisted_optimizer_id;
    training["recipe_overrides"] = overrides;
    return document;
}
[[nodiscard]] std::size_t count_object_key(const nlohmann::json& value, const std::string_view key) {
    std::size_t count = 0U;
    if (value.is_object()) {
        for (const auto& [child_key, child] : value.items()) {
            count += child_key == key ? 1U : 0U;
            count += count_object_key(child, key);
        }
    } else if (value.is_array()) {
        for (const nlohmann::json& child : value) { count += count_object_key(child, key); }
    }
    return count;
}
void check_recipe_schema_v8_shape(const nlohmann::json& document, const int persisted_optimizer_id, const nlohmann::json& expected_values,
                                  const nlohmann::json& expected_overrides) {
    REQUIRE(document.is_object());
    REQUIRE(document.at("schema_version").type() == nlohmann::json::value_t::number_unsigned);
    CHECK(document.at("schema_version") == 8U);
    const nlohmann::json& workflows = document.at("workflows");
    const nlohmann::json& train = workflows.at("train");
    const nlohmann::json& training = train.at("training");
    REQUIRE(training.is_object());
    REQUIRE_FALSE(workflows.contains("schema_version"));
    REQUIRE_FALSE(train.contains("schema_version"));
    REQUIRE_FALSE(training.contains("schema_version"));
    REQUIRE_FALSE(document.contains("recipe_overrides"));
    REQUIRE_FALSE(workflows.contains("recipe_overrides"));
    REQUIRE_FALSE(train.contains("recipe_overrides"));
    for (const std::string_view key : kGoldenRecipeKeys) {
        const std::string persisted_key{key};
        REQUIRE(training.contains(persisted_key));
        CHECK(training.at(persisted_key) == expected_values.at(persisted_key));
        REQUIRE_FALSE(document.contains(persisted_key));
        REQUIRE_FALSE(train.contains(persisted_key));
        CHECK(count_object_key(document, key) == 2U);
    }
    for (const std::string_view key : std::array<std::string_view, 9U>{"lr", "lr_encoder", "lr_component_decay", "encoder_layer_decay", "momentum",
                                                                       "weight_decay", "warmup_epochs", "warmup_momentum", "lr_min_factor"}) {
        CHECK(training.at(std::string{key}).type() == nlohmann::json::value_t::number_float);
    }
    CHECK(training.at("lr_drop").type() == nlohmann::json::value_t::number_integer);
    CHECK(training.at("lr_scheduler").type() == nlohmann::json::value_t::string);
    CHECK(training.at("optimizer").type() == nlohmann::json::value_t::number_integer);
    CHECK(training.at("optimizer") == persisted_optimizer_id);
    const nlohmann::json& overrides = training.at("recipe_overrides");
    REQUIRE(overrides.is_object());
    REQUIRE(overrides.size() == kGoldenRecipeKeys.size());
    CHECK(count_object_key(document, "recipe_overrides") == 1U);
    for (const std::string_view key : kGoldenRecipeKeys) {
        const nlohmann::json& value = overrides.at(std::string{key});
        CHECK(value.type() == nlohmann::json::value_t::boolean);
        CHECK(value == expected_overrides.at(std::string{key}));
    }
}
[[nodiscard]] std::filesystem::path write_recipe_case(const mmltk::testsupport::ScopedTempDir& temporary, const std::string_view name,
                                                      const nlohmann::json& document) {
    const std::filesystem::path path = temporary.path() / std::string{name};
    mmltk::testsupport::write_text_file(path, document.dump(2) + "\n");
    return path;
}
void seed_distinct_prior_recipe(GuiSettingsState& state) {
    TrainRequest& request = state.workflows.train.request;
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) {
        request.*field.value_member = field.prior_value;
        set_recipe_override(request.recipe_overrides, field.value_member, !field.corpus_override);
    }
    request.lr_drop = kPriorLrDrop;
    set_recipe_override(request.recipe_overrides, &TrainRequest::lr_drop, !kCorpusLrDropOverride);
    request.lr_scheduler = kPriorScheduler;
    set_recipe_override(request.recipe_overrides, &TrainRequest::lr_scheduler, !kCorpusSchedulerOverride);
    request.optimizer = TrainOptimizerKind::Muon;
}
void check_materialized_nonuniform_recipe(const GuiSettingsState& state, const std::string_view missing_value = {},
                                          const std::string_view missing_override = {}) {
    const TrainRequest& request = state.workflows.train.request;
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) {
        CAPTURE(field.key);
        CHECK(request.*field.value_member == (field.key == missing_value ? field.prior_value : field.corpus_value));
        CHECK(recipe_overridden(request.recipe_overrides, field.value_member) ==
              (field.key == missing_override ? !field.corpus_override : field.corpus_override));
    }
    CHECK(request.lr_drop == (missing_value == "lr_drop" ? kPriorLrDrop : kCorpusLrDrop));
    CHECK(recipe_overridden(request.recipe_overrides, &TrainRequest::lr_drop) ==
          (missing_override == "lr_drop" ? !kCorpusLrDropOverride : kCorpusLrDropOverride));
    CHECK(request.lr_scheduler == (missing_value == "lr_scheduler" ? kPriorScheduler : kCorpusScheduler));
    CHECK(recipe_overridden(request.recipe_overrides, &TrainRequest::lr_scheduler) ==
          (missing_override == "lr_scheduler" ? !kCorpusSchedulerOverride : kCorpusSchedulerOverride));
    CHECK(request.optimizer == TrainOptimizerKind::Muon);
}
[[nodiscard]] nlohmann::json expected_nonuniform_values(const std::string_view missing_value = {}) {
    nlohmann::json values = nonuniform_recipe_values();
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) {
        if (field.key == missing_value) { values[std::string{field.key}] = field.prior_value; }
    }
    if (missing_value == "lr_drop") {
        values["lr_drop"] = kPriorLrDrop;
    } else if (missing_value == "lr_scheduler") {
        values["lr_scheduler"] = "step";
    }
    return values;
}
[[nodiscard]] nlohmann::json expected_nonuniform_overrides(const std::string_view missing_override = {}) {
    nlohmann::json overrides = nonuniform_recipe_overrides();
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) {
        if (field.key == missing_override) { overrides[std::string{field.key}] = !field.corpus_override; }
    }
    if (missing_override == "lr_drop") {
        overrides["lr_drop"] = !kCorpusLrDropOverride;
    } else if (missing_override == "lr_scheduler") {
        overrides["lr_scheduler"] = !kCorpusSchedulerOverride;
    }
    return overrides;
}
[[nodiscard]] nlohmann::json prior_recipe_overrides() {
    nlohmann::json overrides = nlohmann::json::object();
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) { overrides[std::string{field.key}] = !field.corpus_override; }
    overrides["lr_drop"] = !kCorpusLrDropOverride;
    overrides["lr_scheduler"] = !kCorpusSchedulerOverride;
    return overrides;
}
void test_schema_v8_recipe_golden_shape_and_round_trip() {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-schema-v8-recipe-golden"};
    for (const OptimizerFixture optimizer : kOptimizerFixtures) {
        for (const bool explicit_overrides : {false, true}) {
            const nlohmann::json expected_values = golden_recipe_values(optimizer.persisted_id);
            const nlohmann::json expected_overrides = uniform_recipe_overrides(explicit_overrides);
            const nlohmann::json input = recipe_document(optimizer.persisted_id, expected_values, expected_overrides);
            check_recipe_schema_v8_shape(input, optimizer.persisted_id, expected_values, expected_overrides);
            const std::string stem = std::to_string(optimizer.persisted_id) + (explicit_overrides ? "-explicit" : "-default");
            const std::filesystem::path input_path = write_recipe_case(temporary, stem + "-input.json", input);
            GuiSettingsState loaded = default_gui_settings_state();
            bool repaired = true;
            REQUIRE(load_settings(input_path, loaded, nullptr, &repaired));
            CHECK_FALSE(repaired);
            CHECK(loaded.workflows.train.request.optimizer == optimizer.native_value);
            const nlohmann::json saved = snapshot_gui_settings(loaded);
            check_recipe_schema_v8_shape(saved, optimizer.persisted_id, expected_values, expected_overrides);
            const std::filesystem::path saved_path = write_recipe_case(temporary, stem + "-saved.json", saved);
            GuiSettingsState reloaded = default_gui_settings_state();
            REQUIRE(load_settings(saved_path, reloaded));
            CHECK(reloaded == loaded);
            CHECK(snapshot_gui_settings(reloaded) == saved);
        }
    }
}
void test_schema_v8_nonuniform_recipe_and_every_missing_member_are_preserved() {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-schema-v8-recipe-members"};
    const nlohmann::json corpus_values = nonuniform_recipe_values();
    const nlohmann::json corpus_overrides = nonuniform_recipe_overrides();
    const nlohmann::json corpus = recipe_document(1, corpus_values, corpus_overrides);
    GuiSettingsState materialized = default_gui_settings_state();
    REQUIRE(load_settings(write_recipe_case(temporary, "nonuniform.json", corpus), materialized));
    check_materialized_nonuniform_recipe(materialized);
    check_recipe_schema_v8_shape(snapshot_gui_settings(materialized), 1, corpus_values, corpus_overrides);
    nlohmann::json absent_overrides_document = corpus;
    absent_overrides_document["workflows"]["train"]["training"].erase("recipe_overrides");
    GuiSettingsState absent_overrides_state = default_gui_settings_state();
    seed_distinct_prior_recipe(absent_overrides_state);
    bool absent_overrides_repaired = false;
    REQUIRE(load_settings(write_recipe_case(temporary, "absent-overrides.json", absent_overrides_document), absent_overrides_state, nullptr,
                          &absent_overrides_repaired));
    CHECK(absent_overrides_repaired);
    const TrainRequest& absent_request = absent_overrides_state.workflows.train.request;
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) {
        CAPTURE(field.key);
        CHECK(recipe_overridden(absent_request.recipe_overrides, field.value_member) == !field.corpus_override);
    }
    CHECK(recipe_overridden(absent_request.recipe_overrides, &TrainRequest::lr_drop) == !kCorpusLrDropOverride);
    CHECK(recipe_overridden(absent_request.recipe_overrides, &TrainRequest::lr_scheduler) == !kCorpusSchedulerOverride);
    check_recipe_schema_v8_shape(snapshot_gui_settings(absent_overrides_state), 1, corpus_values, prior_recipe_overrides());
    for (const std::string_view key : kGoldenRecipeKeys) {
        CAPTURE(key);
        nlohmann::json missing_value_document = corpus;
        missing_value_document["workflows"]["train"]["training"].erase(std::string{key});
        GuiSettingsState missing_value_state = default_gui_settings_state();
        seed_distinct_prior_recipe(missing_value_state);
        bool repaired = false;
        REQUIRE(load_settings(write_recipe_case(temporary, std::string{"missing-value-"} + std::string{key} + ".json", missing_value_document),
                              missing_value_state, nullptr, &repaired));
        CHECK(repaired);
        check_materialized_nonuniform_recipe(missing_value_state, key);
        check_recipe_schema_v8_shape(snapshot_gui_settings(missing_value_state), 1, expected_nonuniform_values(key), corpus_overrides);
        nlohmann::json missing_override_document = corpus;
        missing_override_document["workflows"]["train"]["training"]["recipe_overrides"].erase(std::string{key});
        GuiSettingsState missing_override_state = default_gui_settings_state();
        seed_distinct_prior_recipe(missing_override_state);
        repaired = false;
        REQUIRE(load_settings(write_recipe_case(temporary, std::string{"missing-override-"} + std::string{key} + ".json", missing_override_document),
                              missing_override_state, nullptr, &repaired));
        CHECK(repaired);
        check_materialized_nonuniform_recipe(missing_override_state, {}, key);
        check_recipe_schema_v8_shape(snapshot_gui_settings(missing_override_state), 1, corpus_values, expected_nonuniform_overrides(key));
    }
}
void test_schema_v8_recipe_placement_and_unknown_fields_repair_canonically() {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-schema-v8-recipe-placement"};
    const nlohmann::json corpus_values = nonuniform_recipe_values();
    const nlohmann::json corpus_overrides = nonuniform_recipe_overrides();
    nlohmann::json misplaced = recipe_document(1, corpus_values, corpus_overrides);
    for (const std::string_view key : kGoldenRecipeKeys) {
        const std::string field{key};
        misplaced[field] = "ignored-root-copy";
        misplaced["workflows"][field] = "ignored-workflows-copy";
        misplaced["workflows"]["train"][field] = "ignored-train-copy";
        misplaced["workflows"]["validate"]["validation"][field] = "ignored-sibling-copy";
    }
    misplaced["recipe_overrides"] = nlohmann::json::object({{"lr", true}});
    misplaced["workflows"]["recipe_overrides"] = nlohmann::json::object({{"lr", true}});
    misplaced["workflows"]["train"]["recipe_overrides"] = nlohmann::json::object({{"lr", true}});
    misplaced["workflows"]["validate"]["validation"]["recipe_overrides"] = nlohmann::json::object({{"lr", true}});
    misplaced["workflows"]["train"]["training"]["unknown_recipe_value"] = 123;
    misplaced["workflows"]["train"]["training"]["recipe_overrides"]["unknown_override"] = true;
    GuiSettingsState loaded = default_gui_settings_state();
    bool repaired = false;
    REQUIRE(load_settings(write_recipe_case(temporary, "misplaced.json", misplaced), loaded, nullptr, &repaired));
    CHECK(repaired);
    check_materialized_nonuniform_recipe(loaded);
    check_recipe_schema_v8_shape(snapshot_gui_settings(loaded), 1, corpus_values, corpus_overrides);
}
void test_schema_v8_recipe_scheduler_compatibility() {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-schema-v8-recipe-scheduler"};
    struct SchedulerCase final {
        std::string_view name;
        nlohmann::json external_value;
        TrainLrSchedulerKind expected;
        std::string_view canonical_spelling;
        bool repaired;
    };
    const std::array cases{
        SchedulerCase{"signed-step", nlohmann::json::number_integer_t{0}, TrainLrSchedulerKind::Step, "step", true},
        SchedulerCase{"unsigned-step", nlohmann::json::number_unsigned_t{0}, TrainLrSchedulerKind::Step, "step", true},
        SchedulerCase{"signed-cosine", nlohmann::json::number_integer_t{1}, TrainLrSchedulerKind::Cosine, "cosine", true},
        SchedulerCase{"unsigned-cosine", nlohmann::json::number_unsigned_t{1}, TrainLrSchedulerKind::Cosine, "cosine", true},
        SchedulerCase{"step-spelling", "step", TrainLrSchedulerKind::Step, "step", false},
        SchedulerCase{"cosine-spelling", "cosine", TrainLrSchedulerKind::Cosine, "cosine", false},
        SchedulerCase{"negative", nlohmann::json::number_integer_t{-1}, kPriorScheduler, "step", true},
        SchedulerCase{"signed-outside", nlohmann::json::number_integer_t{2}, kPriorScheduler, "step", true},
        SchedulerCase{"unsigned-outside", nlohmann::json::number_unsigned_t{2}, kPriorScheduler, "step", true},
        SchedulerCase{"floating", 1.0, kPriorScheduler, "step", true},
        SchedulerCase{"boolean", true, kPriorScheduler, "step", true},
        SchedulerCase{"null", nullptr, kPriorScheduler, "step", true},
        SchedulerCase{"wrong-case", "Cosine", kPriorScheduler, "step", true},
        SchedulerCase{"unknown-spelling", "future-scheduler", kPriorScheduler, "step", true},
    };
    for (const SchedulerCase& test : cases) {
        CAPTURE(test.name);
        nlohmann::json document = recipe_document(1, nonuniform_recipe_values(), nonuniform_recipe_overrides());
        document["workflows"]["train"]["training"]["lr_scheduler"] = test.external_value;
        GuiSettingsState loaded = default_gui_settings_state();
        seed_distinct_prior_recipe(loaded);
        bool repaired = false;
        REQUIRE(load_settings(write_recipe_case(temporary, std::string{test.name} + ".json", document), loaded, nullptr, &repaired));
        CHECK(repaired == test.repaired);
        CHECK(loaded.workflows.train.request.lr_scheduler == test.expected);
        const nlohmann::json saved = snapshot_gui_settings(loaded);
        CHECK(saved.at("workflows").at("train").at("training").at("lr_scheduler") == test.canonical_spelling);
        CHECK(saved.at("workflows").at("train").at("training").at("lr_scheduler").is_string());
    }
}
void test_schema_v8_recipe_optimizer_compatibility() {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-schema-v8-recipe-optimizer"};
    for (const OptimizerFixture optimizer : kOptimizerFixtures) {
        const std::array<nlohmann::json, 2U> raw_ids{
            nlohmann::json(nlohmann::json::number_integer_t{optimizer.persisted_id}),
            nlohmann::json(nlohmann::json::number_unsigned_t{static_cast<unsigned int>(optimizer.persisted_id)}),
        };
        for (std::size_t index = 0U; index < raw_ids.size(); ++index) {
            nlohmann::json document = recipe_document(optimizer.persisted_id, golden_recipe_values(optimizer.persisted_id), uniform_recipe_overrides(false));
            document["workflows"]["train"]["training"]["optimizer"] = raw_ids[index];
            GuiSettingsState loaded = default_gui_settings_state();
            bool repaired = true;
            REQUIRE(
                load_settings(write_recipe_case(temporary, "valid-" + std::to_string(optimizer.persisted_id) + "-" + std::to_string(index) + ".json", document),
                              loaded, nullptr, &repaired));
            CHECK_FALSE(repaired);
            CHECK(loaded.workflows.train.request.optimizer == optimizer.native_value);
            const nlohmann::json saved = snapshot_gui_settings(loaded);
            CHECK(saved.at("workflows").at("train").at("training").at("optimizer").type() == nlohmann::json::value_t::number_integer);
            CHECK(saved.at("workflows").at("train").at("training").at("optimizer") == optimizer.persisted_id);
        }
    }
    nlohmann::json missing = recipe_document(0, golden_recipe_values(0), uniform_recipe_overrides(false));
    missing["workflows"]["train"]["training"].erase("optimizer");
    GuiSettingsState retained = default_gui_settings_state();
    retained.workflows.train.request.optimizer = TrainOptimizerKind::Muon;
    bool repaired = false;
    REQUIRE(load_settings(write_recipe_case(temporary, "missing.json", missing), retained, nullptr, &repaired));
    CHECK(repaired);
    CHECK(retained.workflows.train.request.optimizer == TrainOptimizerKind::Muon);
    CHECK(snapshot_gui_settings(retained).at("workflows").at("train").at("training").at("optimizer") == 1);
}
void test_schema_v8_recipe_malformed_and_constraint_rejection_is_atomic() {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-schema-v8-recipe-rejection"};
    const GuiSettingsState prior = [] {
        GuiSettingsState state = default_gui_settings_state();
        seed_distinct_prior_recipe(state);
        return state;
    }();
    const auto check_rejected = [&](const std::string_view name, nlohmann::json document) {
        GuiSettingsState candidate = prior;
        CHECK_FALSE(load_settings(write_recipe_case(temporary, name, document), candidate));
        CHECK(candidate == prior);
    };
    nlohmann::json missing_schema = recipe_document(0, golden_recipe_values(0), uniform_recipe_overrides(false));
    missing_schema.erase("schema_version");
    check_rejected("missing-schema.json", std::move(missing_schema));
    nlohmann::json unsupported_schema = recipe_document(0, golden_recipe_values(0), uniform_recipe_overrides(false));
    unsupported_schema["schema_version"] = 9U;
    check_rejected("unsupported-schema.json", std::move(unsupported_schema));
    for (const DoubleRecipeFixture& field : kDoubleRecipeFixtures) {
        CAPTURE(field.key);
        nlohmann::json malformed = recipe_document(0, golden_recipe_values(0), uniform_recipe_overrides(false));
        malformed["workflows"]["train"]["training"][std::string{field.key}] = "not-a-number";
        check_rejected(std::string{"malformed-"} + std::string{field.key} + ".json", std::move(malformed));
        nlohmann::json invalid = recipe_document(0, golden_recipe_values(0), uniform_recipe_overrides(false));
        invalid["workflows"]["train"]["training"][std::string{field.key}] = field.invalid_value;
        check_rejected(std::string{"invalid-"} + std::string{field.key} + ".json", std::move(invalid));
    }
    nlohmann::json malformed_lr_drop = recipe_document(0, golden_recipe_values(0), uniform_recipe_overrides(false));
    malformed_lr_drop["workflows"]["train"]["training"]["lr_drop"] = "not-an-integer";
    check_rejected("malformed-lr-drop.json", std::move(malformed_lr_drop));
    nlohmann::json invalid_lr_drop = recipe_document(0, golden_recipe_values(0), uniform_recipe_overrides(false));
    invalid_lr_drop["workflows"]["train"]["training"]["lr_drop"] = -1;
    check_rejected("invalid-lr-drop.json", std::move(invalid_lr_drop));
    for (const std::string_view key : kGoldenRecipeKeys) {
        nlohmann::json malformed_override = recipe_document(0, golden_recipe_values(0), uniform_recipe_overrides(false));
        malformed_override["workflows"]["train"]["training"]["recipe_overrides"][std::string{key}] = 1;
        check_rejected(std::string{"malformed-override-"} + std::string{key} + ".json", std::move(malformed_override));
    }
    const std::array invalid_optimizers{
        std::pair<std::string_view, nlohmann::json>{"malformed-optimizer", "adamw"},
        std::pair<std::string_view, nlohmann::json>{"floating-optimizer", 1.0},
        std::pair<std::string_view, nlohmann::json>{"boolean-optimizer", true},
        std::pair<std::string_view, nlohmann::json>{"negative-optimizer", -1},
        std::pair<std::string_view, nlohmann::json>{"outside-optimizer", 2},
        std::pair<std::string_view, nlohmann::json>{"unsigned-outside-optimizer", nlohmann::json::number_unsigned_t{2}},
        std::pair<std::string_view, nlohmann::json>{"overflowing-optimizer",
                                                    nlohmann::json::number_unsigned_t{static_cast<unsigned int>(std::numeric_limits<int>::max()) + 1U}},
    };
    for (const auto& [name, invalid_optimizer] : invalid_optimizers) {
        nlohmann::json document = recipe_document(0, golden_recipe_values(0), uniform_recipe_overrides(false));
        document["workflows"]["train"]["training"]["optimizer"] = invalid_optimizer;
        check_rejected(std::string{name} + ".json", std::move(document));
    }
}
void test_ui_settings_round_trip() {
    SettingsViewStates states;
    auto [ui, train, validate, predict, annotate, export_state, explore] = mutable_settings_views(states);
    CHECK(train.request.h2d_dataloader);
    CHECK(train.request.numa_node == -1);
    train.request.h2d_dataloader = true;
    validate.request.h2d_dataloader = true;
    validate.request.numa_node = 3;
    predict.request.h2d_dataloader = true;
    predict.request.numa_node = 2;
    explore.h2d_dataloader = true;
    explore.numa_node = 1;
    ui.dark_mode = true;
    ui.ui_scale = 1.35f;
    ui.font_size = 18.0f;
    ui.secondary_font_size = 15.0f;
    ui.mono_font_size = 14.0f;
    ui.text_input_font_size = 17.0f;
    ui.crop_edge_hit_half_width = 11.0f;
    ui.crop_corner_hit_size = 24.0f;
    ui.crop_handle_radius = 7.5f;
    ui.workspace_aspect_ratio = WorkspaceAspectRatio::Photo;
    ui.annotation_brush_radius = 27;
    ui.mask_cleanup_radius = 6;
    ui.show_workspace_performance = true;
    train.dataset_source_dir = "/tmp/dataset";
    train.compiled_dataset_dir = "/tmp/compiled";
    train.request.train_compiled_path = "/tmp/train.bin";
    train.request.val_compiled_path = "/tmp/val.bin";
    train.request.test_compiled_path = "/tmp/test.bin";
    train.request.output_dir = "/tmp/train-output";
    train.request.weights_path = "/tmp/weights.pt";
    train.model_input = ModelArtifactInputKind::Weights;
    train.request.resolution = 512;
    train.overwrite_compiled_dataset = true;
    train.compile_dimensions = true;
    train.compile_perceptual_downscale = true;
    train.request.gpu_augmentation.perceptual_downscale = true;
    train.request.cpu_affinity = "0-3";
    train.request.progress_bar = true;
    train.request.device_ids = {0, 2};
    train.request.lanes = 2;
    train.request.num_queries = 111;
    train.request.eval_max_dets = 113;
    set_recipe_override(train.request.recipe_overrides, &TrainRequest::lr, true);
    train.request.lr_scheduler = mmltk::backend::models::rfdetr::TrainLrSchedulerKind::Cosine;
    set_recipe_override(train.request.recipe_overrides, &TrainRequest::lr_scheduler, true);
    validate.request.compiled_path = "/tmp/validate.bin";
    validate.request.source_dir = "/tmp/source";
    validate.request.onnx_path = "/tmp/models/validate.onnx";
    validate.request.tensorrt_path = "/tmp/models/validate.engine";
    validate.request.save_engine_path = "/tmp/models/save.engine";
    validate.request.num_queries = 211;
    validate.request.eval_max_dets = 213;
    validate.request.profile = true;
    predict.source.kind = SourceKind::SingleImage;
    predict.source.single_image_path = "/tmp/input.png";
    predict.request.weights_path = "/tmp/predict.pt";
    train.request.class_layout_path = "/tmp/train.classes.json";
    validate.request.class_layout_path = "/tmp/validate.classes.json";
    predict.request.class_layout_path = "/tmp/predict.classes.json";
    export_state.class_layout_path = "/tmp/export.classes.json";
    predict.model_input = ModelArtifactInputKind::Weights;
    predict.request.output_path = "/tmp/predictions.json";
    predict.request.progress_bar = true;
    annotate.source.kind = SourceKind::ImageFolder;
    annotate.source.image_directory = "/tmp/images";
    annotate.weights_path = "/tmp/annotate.pt";
    annotate.model_input = ModelArtifactInputKind::Weights;
    annotate.output_dir = "/tmp/annotated-scenes";
    annotate.full_frame = true;
    export_state.weights_path = "/tmp/export.pt";
    export_state.onnx_input_path = "/tmp/export-input.onnx";
    export_state.onnx_output_path = "/tmp/export-output.onnx";
    export_state.output_path = "/tmp/export.engine";
    export_state.allow_fp16 = false;
    train.visualize_augmentation_in_explore = true;
    explore.dataset_source = ExploreDatasetSource::Custom;
    explore.custom_compiled_path = "/tmp/explore.bin";
    explore.device_id = 2;
    explore.grid_width = 7;
    explore.order = ExploreOrder::Shuffled;
    explore.shuffle_seed = 0x12345678U;
    explore.require_boxes = true;
    explore.require_masks = true;
    explore.min_instances = 2U;
    explore.max_instances = 17U;
    explore.min_compiled_index = 11U;
    explore.max_compiled_index = 9'001U;
    explore.class_catalog_identity = 0x9123'4567'89ab'cdefULL;
    explore.sample_classes[7] = false;
    explore.overlay_classes[9] = false;
    explore.show_boxes = false;
    explore.show_masks = false;
    explore.show_original_dimensions = true;
    explore.detail_scale_mode = ExploreDetailScaleMode::Neural;
    GuiSettingsState& snapshot = make_snapshot(states);
    const nlohmann::json saved = snapshot_gui_settings(snapshot);
    REQUIRE((saved.at("schema_version") == kGuiSettingsSchemaVersion));
    REQUIRE((saved.at("ui").at("workspace_aspect_ratio") == 3));
    REQUIRE((saved.at("ui").at("annotation_brush_radius") == 27));
    REQUIRE((saved.at("ui").at("mask_cleanup_radius") == 6));
    REQUIRE((saved.at("ui").at("show_workspace_performance") == true));
    REQUIRE((saved.at("workflows").at("train").at("dataset_paths").at("train_compiled_path") == "/tmp/train.bin"));
    REQUIRE((saved.at("workflows").at("train").at("dataset_paths").at("source_dir") == "/tmp/dataset"));
    REQUIRE((saved.at("workflows").at("train").at("dataset_paths").at("compiled_directory") == "/tmp/compiled"));
    REQUIRE((saved.at("workflows").at("train").at("dataset_paths").at("overwrite") == true));
    REQUIRE((saved.at("workflows").at("train").at("dataset_paths").at("compile_dimensions") == true));
    REQUIRE((saved.at("workflows").at("train").at("model_artifacts").at("resolution") == 512));
    REQUIRE((saved.at("workflows").at("train").at("model_artifacts").at("weights_path") == "/tmp/weights.pt"));
    REQUIRE((saved.at("workflows").at("train").at("execution").at("progress_bar") == true));
    REQUIRE((saved.at("workflows").at("train").at("training").at("local_device_ids") == nlohmann::json::array({0, 2})));
    REQUIRE((saved.at("workflows").at("train").at("training").at("num_queries") == 111));
    REQUIRE((saved.at("workflows").at("train").at("training").at("eval_max_dets") == 113));
    REQUIRE((saved.at("workflows").at("train").at("training").at("recipe_overrides").at("lr") == true));
    REQUIRE((saved.at("workflows").at("train").at("training").at("lr_scheduler") == "cosine"));
    REQUIRE((saved.at("workflows").at("validate").at("dataset_paths").at("compiled_path") == "/tmp/validate.bin"));
    REQUIRE((saved.at("workflows").at("validate").at("validation").at("num_queries") == 211));
    REQUIRE((saved.at("workflows").at("validate").at("validation").at("eval_max_dets") == 213));
    REQUIRE((saved.at("workflows").at("predict").at("predict").at("output_path") == "/tmp/predictions.json"));
    REQUIRE((saved.at("workflows").at("annotate").at("annotate").at("output_dir") == "/tmp/annotated-scenes"));
    REQUIRE((saved.at("workflows").at("export").at("model_artifacts").at("onnx_input_path") == "/tmp/export-input.onnx"));
    REQUIRE((saved.at("workflows").at("export").at("export").at("onnx_output_path") == "/tmp/export-output.onnx"));
    REQUIRE((saved.at("workflows").at("export").at("export").at("output_path") == "/tmp/export.engine"));
    const nlohmann::json& saved_explore = saved.at("workflows").at("explore");
    REQUIRE((saved_explore.at("device_id") == 2));
    REQUIRE((saved_explore.at("shuffle_seed") == 0x12345678U));
    REQUIRE((saved_explore.at("require_boxes")));
    REQUIRE((saved_explore.at("require_masks")));
    REQUIRE((saved_explore.at("min_instances") == 2U));
    REQUIRE((saved_explore.at("max_instances") == 17U));
    REQUIRE((saved_explore.at("min_compiled_index") == 11U));
    REQUIRE((saved_explore.at("max_compiled_index") == 9'001U));
    REQUIRE((saved_explore.at("class_catalog_identity") == 0x9123'4567'89ab'cdefULL));
    REQUIRE((!saved_explore.at("sample_classes").at(7).get<bool>()));
    REQUIRE((!saved_explore.at("overlay_classes").at(9).get<bool>()));
    REQUIRE((!saved_explore.at("show_boxes").get<bool>()));
    REQUIRE((!saved_explore.at("show_masks").get<bool>()));
    REQUIRE((saved_explore.at("show_original_dimensions").get<bool>()));
    REQUIRE((saved_explore.at("detail_scale_mode") == static_cast<int>(ExploreDetailScaleMode::Neural)));
    SettingsViewStates loaded_states;
    auto& loaded_ui = loaded_states.ui;
    auto& loaded_train = loaded_states.workflows.train;
    auto& loaded_validate = loaded_states.workflows.validate;
    auto& loaded_predict = loaded_states.workflows.predict;
    auto& loaded_annotate = loaded_states.workflows.annotate;
    auto& loaded_export = loaded_states.workflows.export_state;
    auto& loaded_explore = loaded_states.workflows.explore;
    GuiSettingsState& loaded = loaded_states;
    apply_gui_settings(saved, loaded);
    REQUIRE((loaded.current_view == mmltk::controller::contracts::FeatureId::Annotate));
    REQUIRE((loaded_annotate.preset_name == "rf-detr-seg-medium"));
    CHECK(loaded_train.request.h2d_dataloader);
    CHECK(loaded_validate.request.h2d_dataloader);
    CHECK(loaded_validate.request.numa_node == 3);
    CHECK(loaded_predict.request.h2d_dataloader);
    CHECK(loaded_predict.request.numa_node == 2);
    CHECK(loaded_explore.h2d_dataloader);
    CHECK(loaded_explore.numa_node == 1);
    REQUIRE((loaded_train.request.train_compiled_path == "/tmp/train.bin"));
    REQUIRE((loaded_train.dataset_source_dir == "/tmp/dataset"));
    REQUIRE((loaded_train.compiled_dataset_dir == "/tmp/compiled"));
    REQUIRE((loaded_train.overwrite_compiled_dataset));
    REQUIRE((loaded_train.compile_dimensions));
    CHECK(loaded_train.compile_perceptual_downscale);
    CHECK(loaded_train.request.gpu_augmentation.perceptual_downscale);
    REQUIRE((loaded_train.request.resolution == 512));
    REQUIRE((loaded_train.request.weights_path == "/tmp/weights.pt"));
    CHECK(loaded_train.request.class_layout_path == "/tmp/train.classes.json");
    CHECK(loaded_validate.request.class_layout_path == "/tmp/validate.classes.json");
    CHECK(loaded_predict.request.class_layout_path == "/tmp/predict.classes.json");
    CHECK(loaded_export.class_layout_path == "/tmp/export.classes.json");
    REQUIRE((loaded_train.request.progress_bar));
    REQUIRE((loaded_train.request.device_ids == std::vector<int>({0, 2})));
    REQUIRE((loaded_train.request.num_queries == 111));
    REQUIRE((loaded_train.request.eval_max_dets == 113));
    REQUIRE((recipe_overridden(loaded_train.request.recipe_overrides, &TrainRequest::lr)));
    REQUIRE((loaded_train.request.lr_scheduler == mmltk::backend::models::rfdetr::TrainLrSchedulerKind::Cosine));
    auto step_document = saved;
    step_document["workflows"]["train"]["training"]["lr_scheduler"] = "step";
    apply_gui_settings(step_document, loaded);
    REQUIRE((loaded.workflows.train.request.lr_scheduler == mmltk::backend::models::rfdetr::TrainLrSchedulerKind::Step));
    REQUIRE((loaded_validate.request.compiled_path == "/tmp/validate.bin"));
    REQUIRE((loaded_validate.request.save_engine_path == "/tmp/models/save.engine"));
    REQUIRE((loaded_validate.request.num_queries == 211));
    REQUIRE((loaded_validate.request.eval_max_dets == 213));
    REQUIRE((loaded_predict.source.kind == SourceKind::SingleImage));
    REQUIRE((loaded_predict.source.single_image_path == "/tmp/input.png"));
    REQUIRE((loaded_predict.request.progress_bar));
    REQUIRE((loaded_predict.request.output_path == "/tmp/predictions.json"));
    REQUIRE((loaded_annotate.source.kind == SourceKind::ImageFolder));
    REQUIRE((loaded_annotate.source.image_directory == "/tmp/images"));
    REQUIRE((loaded_annotate.full_frame));
    REQUIRE((loaded_export.onnx_input_path == "/tmp/export-input.onnx"));
    REQUIRE((loaded_export.onnx_output_path == "/tmp/export-output.onnx"));
    REQUIRE((loaded_export.output_path == "/tmp/export.engine"));
    REQUIRE((!loaded_export.allow_fp16));
    REQUIRE((loaded_train.visualize_augmentation_in_explore));
    REQUIRE((loaded_explore.dataset_source == ExploreDatasetSource::Custom));
    REQUIRE((loaded_explore.custom_compiled_path == "/tmp/explore.bin"));
    REQUIRE((loaded_explore.device_id == 2));
    REQUIRE((loaded_explore.grid_width == 7));
    REQUIRE((loaded_explore.order == ExploreOrder::Shuffled));
    REQUIRE((loaded_explore.shuffle_seed == 0x12345678U));
    REQUIRE((loaded_explore.require_boxes));
    REQUIRE((loaded_explore.require_masks));
    REQUIRE((loaded_explore.min_instances == 2U));
    REQUIRE((loaded_explore.max_instances == 17U));
    REQUIRE((loaded_explore.min_compiled_index == 11U));
    REQUIRE((loaded_explore.max_compiled_index == 9'001U));
    REQUIRE((loaded_explore.class_catalog_identity == 0x9123'4567'89ab'cdefULL));
    REQUIRE((!loaded_explore.sample_classes[7]));
    REQUIRE((!loaded_explore.overlay_classes[9]));
    REQUIRE((!loaded_explore.show_boxes));
    REQUIRE((!loaded_explore.show_masks));
    REQUIRE((loaded_explore.show_original_dimensions));
    REQUIRE((loaded_explore.detail_scale_mode == ExploreDetailScaleMode::Neural));
    REQUIRE((loaded_ui.dark_mode));
    REQUIRE((loaded_ui.ui_scale == 1.35f));
    REQUIRE((loaded_ui.font_size == 18.0f));
    REQUIRE((loaded_ui.secondary_font_size == 15.0f));
    REQUIRE((loaded_ui.mono_font_size == 14.0f));
    REQUIRE((loaded_ui.text_input_font_size == 17.0f));
    REQUIRE((loaded_ui.crop_edge_hit_half_width == 11.0f));
    REQUIRE((loaded_ui.crop_corner_hit_size == 24.0f));
    REQUIRE((loaded_ui.crop_handle_radius == 7.5f));
    REQUIRE((loaded_ui.workspace_aspect_ratio == WorkspaceAspectRatio::Photo));
    REQUIRE((loaded_ui.annotation_brush_radius == 27));
    REQUIRE((loaded_ui.mask_cleanup_radius == 6));
    REQUIRE((loaded_ui.show_workspace_performance));
}
void test_fresh_defaults_use_capture_only_annotate() {
    SettingsViewStates states;
    REQUIRE((states == default_gui_settings_state()));
    auto [ui, train, validate, predict, annotate, export_state, explore] = mutable_settings_views(states);
    states.workflows.apply_defaults();
    REQUIRE((ui.workspace_aspect_ratio == WorkspaceAspectRatio::Widescreen));
    REQUIRE((ui.font_size == 14.0F));
    REQUIRE((ui.secondary_font_size == 12.0F));
    REQUIRE((ui.mono_font_size == 12.0F));
    REQUIRE((ui.text_input_font_size == 13.0F));
    REQUIRE((!ui.show_workspace_performance));
    REQUIRE((train.model_input == ModelArtifactInputKind::Weights));
    REQUIRE((train.dataset_source_dir == "./dataset"));
    REQUIRE((train.compiled_dataset_dir == "./compiled"));
    REQUIRE((train.request.train_compiled_path == "./compiled/train.bin"));
    REQUIRE((train.request.val_compiled_path == "./compiled/val.bin"));
    REQUIRE((!train.overwrite_compiled_dataset));
    REQUIRE((!train.compile_dimensions));
    CHECK_FALSE(train.compile_perceptual_downscale);
    CHECK_FALSE(train.request.gpu_augmentation.perceptual_downscale);
    REQUIRE((train.request.num_queries == 0));
    REQUIRE((train.request.eval_max_dets == 0));
    REQUIRE((validate.model_input == ModelArtifactInputKind::Weights));
    REQUIRE((validate.request.num_queries == 0));
    REQUIRE((validate.request.eval_max_dets == 0));
    REQUIRE((predict.model_input == ModelArtifactInputKind::Weights));
    REQUIRE((predict.request.weights_path.empty()));
    REQUIRE((annotate.model_input == ModelArtifactInputKind::None));
    REQUIRE((annotate.weights_path.empty()));
    REQUIRE((annotate.onnx_path.empty()));
    REQUIRE((annotate.tensorrt_path.empty()));
    REQUIRE((export_state.model_input == ModelArtifactInputKind::None));
}
TEST_CASE("explicit compiled selections override inferred directories and keep test input optional", "[gui][settings]") {
    auto state = default_gui_settings_state();
    const std::array updates{
        SettingsValueUpdate{
            .path = "workflows.train.request.train_compiled_path",
            .value = *mmltk::frameworks::serialization::wire::FlatValue::text("/selected/compiled.mmltk", mmltk::frameworks::reflection::kMaximumPathBytes)},
    };
    REQUIRE(apply_gui_settings_values(state, updates));
    CHECK_FALSE(state.workflows.train.use_compiled_directory_defaults);
    CHECK(state.workflows.train.request.train_compiled_path == "/selected/compiled.mmltk");
    CHECK(state.workflows.train.request.test_compiled_path.empty());
    auto inferred = default_gui_settings_state();
    const std::array unrelated{SettingsValueUpdate{.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{true}}};
    REQUIRE(apply_gui_settings_values(inferred, unrelated));
    CHECK(inferred.workflows.train.request.test_compiled_path.empty());
}
TEST_CASE("GUI prediction loads batch one while backend requests retain batching", "[gui][settings]") {
    auto state = default_gui_settings_state();
    REQUIRE(state.workflows.predict.request.batch_size == 1U);
    auto saved = snapshot_gui_settings(state);
    saved["workflows"]["predict"]["predict"]["batch_size"] = 32U;
    saved["workflows"]["predict"]["model_artifacts"]["input"] = static_cast<int>(ModelArtifactInputKind::None);
    apply_gui_settings(saved, state);
    CHECK(state.workflows.predict.request.batch_size == 1U);
    CHECK(state.workflows.predict.model_input == ModelArtifactInputKind::Weights);
    mmltk::backend::models::rfdetr::PredictRequest cli;
    cli.batch_size = 32U;
    CHECK(cli.batch_size == 32U);
}
void test_model_input_load_normalizes_invalid_values_by_workflow() {
    SettingsViewStates states;
    GuiSettingsState& snapshot = make_snapshot(states);
    nlohmann::json saved = snapshot_gui_settings(snapshot);
    saved["workflows"]["predict"]["model_artifacts"]["input"] = 99;
    saved["workflows"]["annotate"]["model_artifacts"]["input"] = 99;
    saved["workflows"]["annotate"]["model_artifacts"]["weights_path"] = "/explicit/annotate.pt";
    apply_gui_settings(saved, snapshot);
    REQUIRE((states.workflows.predict.model_input == ModelArtifactInputKind::Weights));
    REQUIRE((states.workflows.annotate.model_input == ModelArtifactInputKind::None));
    REQUIRE((states.workflows.annotate.weights_path.empty()));
}
// Seeds view states with sentinel values, then requires that loading `path` fails and leaves them
// untouched.
void assert_load_rejected_and_state_preserved(const std::filesystem::path& path, const mmltk::controller::contracts::FeatureId view, const float ui_scale,
                                              const char* output_dir) {
    SettingsViewStates states;
    states.ui.ui_scale = ui_scale;
    states.workflows.train.request.output_dir = output_dir;
    states.current_view = view;
    REQUIRE((!load_settings(path, states)));
    REQUIRE((states.current_view == view));
    REQUIRE((states.ui.ui_scale == ui_scale));
    REQUIRE((states.workflows.train.request.output_dir == output_dir));
}
void test_persistence_rejects_unsupported_schema_and_malformed_files() {
    const std::filesystem::path temp_root = mmltk::testsupport::make_temp_root("mmltk-gui-settings-schema-test");
    const std::filesystem::path unsupported_path = temp_root / "unsupported-gui.json";
    {
        nlohmann::json unsupported;
        unsupported["schema_version"] = kGuiSettingsSchemaVersion - 1U;
        unsupported["current_view"] = 4;
        mmltk::testsupport::write_text_file(unsupported_path, unsupported.dump(2) + "\n");
    }
    assert_load_rejected_and_state_preserved(unsupported_path, mmltk::controller::contracts::FeatureId::Train, 3.0f, "/keep");
    const std::filesystem::path overflowing_schema_path = temp_root / "overflowing-schema-gui.json";
    {
        nlohmann::json overflowing_schema = default_gui_settings_document();
        overflowing_schema["schema_version"] = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
        mmltk::testsupport::write_text_file(overflowing_schema_path, overflowing_schema.dump(2) + "\n");
    }
    assert_load_rejected_and_state_preserved(overflowing_schema_path, mmltk::controller::contracts::FeatureId::Validate, 2.5F, "/keep-overflowing-schema");
    const std::filesystem::path overflowing_view_path = temp_root / "overflowing-view-gui.json";
    {
        nlohmann::json overflowing_view = default_gui_settings_document();
        overflowing_view["current_view"] = static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1U;
        mmltk::testsupport::write_text_file(overflowing_view_path, overflowing_view.dump(2) + "\n");
    }
    assert_load_rejected_and_state_preserved(overflowing_view_path, mmltk::controller::contracts::FeatureId::Predict, 2.25F, "/keep-overflowing-view");
    const std::filesystem::path malformed_path = temp_root / "malformed-gui.json";
    mmltk::testsupport::write_text_file(malformed_path, "{ this is not valid json");
    assert_load_rejected_and_state_preserved(malformed_path, mmltk::controller::contracts::FeatureId::Export, 4.0f, "/keep-malformed");
    const std::filesystem::path invalid_typed_path = temp_root / "invalid-typed-gui.json";
    nlohmann::json invalid_typed = default_gui_settings_document();
    invalid_typed["workflows"]["train"]["training"]["batch_size"] = 0;
    mmltk::testsupport::write_text_file(invalid_typed_path, invalid_typed.dump(2));
    assert_load_rejected_and_state_preserved(invalid_typed_path, mmltk::controller::contracts::FeatureId::Explore, 1.25F, "/keep-invalid-typed");
    std::error_code cleanup_error;
    std::filesystem::remove(unsupported_path, cleanup_error);
    std::filesystem::remove(overflowing_schema_path, cleanup_error);
    std::filesystem::remove(overflowing_view_path, cleanup_error);
    std::filesystem::remove(malformed_path, cleanup_error);
    std::filesystem::remove(invalid_typed_path, cleanup_error);
    std::filesystem::remove(temp_root, cleanup_error);
}
void test_persistence_repairs_catalog_and_compiled_directory_defaults() {
    const std::filesystem::path temp_root = mmltk::testsupport::make_temp_root("mmltk-gui-settings-default-repair-test");
    const std::filesystem::path settings_path = temp_root / "gui.json";
    nlohmann::json document = default_gui_settings_document();
    document["workflows"]["predict"]["model_artifacts"]["preset_name"] = "removed-preset";
    document["workflows"]["predict"]["model_artifacts"]["input"] = static_cast<int>(ModelArtifactInputKind::TensorRt);
    document["workflows"]["predict"]["model_artifacts"]["tensorrt_path"] = "/old/model.engine";
    document["workflows"]["train"]["dataset_paths"]["use_compiled_directory_defaults"] = true;
    document["workflows"]["train"]["dataset_paths"]["train_compiled_path"] = "/custom/train.bin";
    mmltk::testsupport::write_text_file(settings_path, document.dump(2));
    GuiSettingsState state = default_gui_settings_state();
    bool repair_required = false;
    REQUIRE(load_settings(settings_path, state, nullptr, &repair_required));
    CHECK(repair_required);
    CHECK(state.workflows.predict.request.preset_name == kDefaultModelPresetName);
    CHECK(state.workflows.predict.model_source == ModelSelectionSource::Canonical);
    CHECK(state.workflows.predict.model_input == ModelArtifactInputKind::Weights);
    CHECK(state.workflows.predict.request.tensorrt_path.empty());
    CHECK_FALSE(state.workflows.train.use_compiled_directory_defaults);
    std::error_code cleanup_error;
    std::filesystem::remove(settings_path, cleanup_error);
    std::filesystem::remove(temp_root, cleanup_error);
}
void test_bounded_flat_settings_mutation_is_atomic() {
    GuiSettingsState state = default_gui_settings_state();
    state.workflows.predict.model_source = ModelSelectionSource::Custom;
    state.workflows.predict.model_input = ModelArtifactInputKind::TensorRt;
    state.workflows.predict.request.tensorrt_path = "/tmp/custom.engine";
    const auto& preset = mmltk::backend::models::rfdetr::kPresetCatalog.front();
    const auto preset_name = mmltk::frameworks::serialization::wire::FlatValue::text(preset.preset_name, mmltk::frameworks::reflection::kMaximumNameBytes);
    REQUIRE(preset_name.has_value());
    const std::vector<SettingsValueUpdate> canonical_selection{
        {.path = "workflows.predict.request.preset_name", .value = *preset_name},
        {.path = "workflows.predict.request.resolution", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{preset.resolution}}},
        {.path = "workflows.predict.model_source", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{0}}},
        {.path = "workflows.predict.model_input", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{0}}},
    };
    REQUIRE(apply_gui_settings_values(state, canonical_selection));
    CHECK(state.workflows.predict.request.preset_name == preset.preset_name);
    CHECK(state.workflows.predict.request.resolution == preset.resolution);
    CHECK(state.workflows.predict.model_source == ModelSelectionSource::Canonical);
    CHECK(state.workflows.predict.model_input == ModelArtifactInputKind::Weights);
    const std::array device_id_values{mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{2}},
                                      mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{4}}};
    const auto device_ids =
        mmltk::frameworks::serialization::wire::FlatValue::array(device_id_values, {.max_bytes = 16U, .max_items = device_id_values.size(), .max_depth = 1U});
    REQUIRE(device_ids.has_value());
    const std::array direct_flat_updates{
        SettingsValueUpdate{.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{true}},
        SettingsValueUpdate{.path = "workflows.train.request.device_ids", .value = *device_ids},
    };
    REQUIRE(apply_gui_settings_values(state, direct_flat_updates));
    CHECK(state.ui.dark_mode);
    CHECK(state.workflows.train.request.device_ids == std::vector<int>({2, 4}));
    constexpr double browser_threshold = 0.42;
    const std::array browser_float_update{
        SettingsValueUpdate{.path = "workflows.annotate.threshold", .value = mmltk::frameworks::serialization::wire::FlatValue{browser_threshold}},
    };
    REQUIRE(apply_gui_settings_values(state, browser_float_update));
    CHECK(state.workflows.annotate.threshold == static_cast<float>(browser_threshold));
    const auto square_aspect = mmltk::frameworks::serialization::wire::FlatValue::text("Square", sizeof("Square") - 1U);
    REQUIRE(square_aspect.has_value());
    const std::array workspace_aspect_update{
        SettingsValueUpdate{.path = "ui.workspace_aspect_ratio", .value = *square_aspect},
    };
    REQUIRE(apply_gui_settings_values(state, workspace_aspect_update));
    CHECK(state.ui.workspace_aspect_ratio == WorkspaceAspectRatio::Square);
    const GuiSettingsState before_invalid_aspect = state;
    const std::array invalid_workspace_aspect{
        SettingsValueUpdate{.path = "ui.workspace_aspect_ratio", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{-1}}},
    };
    CHECK_FALSE(apply_gui_settings_values(state, invalid_workspace_aspect));
    CHECK(state == before_invalid_aspect);
    const GuiSettingsState before_invalid_flat_array = state;
    const std::array<mmltk::frameworks::serialization::wire::FlatValue, 0U> no_device_ids{};
    const auto empty_device_ids = mmltk::frameworks::serialization::wire::FlatValue::array(no_device_ids, {.max_bytes = 1U, .max_items = 1U, .max_depth = 0U});
    REQUIRE(empty_device_ids.has_value());
    const std::array invalid_flat_array{
        SettingsValueUpdate{.path = "workflows.train.request.device_ids", .value = *empty_device_ids},
    };
    CHECK_FALSE(apply_gui_settings_values(state, invalid_flat_array));
    CHECK(state == before_invalid_flat_array);
    const GuiSettingsState before_invalid_batch = state;
    const std::array invalid_batch{
        SettingsValueUpdate{.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{false}},
        SettingsValueUpdate{.path = "workflows.predict.request.unknown", .value = mmltk::frameworks::serialization::wire::FlatValue{true}},
    };
    CHECK_FALSE(apply_gui_settings_values(state, invalid_batch));
    CHECK(state == before_invalid_batch);
    const std::array duplicate_batch{
        SettingsValueUpdate{.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{false}},
        SettingsValueUpdate{.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{true}},
    };
    CHECK_FALSE(apply_gui_settings_values(state, duplicate_batch));
    CHECK(state == before_invalid_batch);
    GuiSettingsState copy = state;
    CHECK(state == copy);
    copy.ui.dark_mode = !copy.ui.dark_mode;
    CHECK(state != copy);
}
void test_catalog_source_transition_normalizes_model_input_at_the_native_boundary() {
    GuiSettingsState state = default_gui_settings_state();
    state.workflows.validate.model_source = ModelSelectionSource::Custom;
    state.workflows.validate.model_input = ModelArtifactInputKind::Onnx;
    state.workflows.validate.request.onnx_path = "/tmp/validate.onnx";
    state.workflows.predict.model_source = ModelSelectionSource::Custom;
    state.workflows.predict.model_input = ModelArtifactInputKind::TensorRt;
    state.workflows.predict.request.tensorrt_path = "/tmp/predict.engine";
    REQUIRE(gui_settings_valid(state));
    const auto select_catalog_source = [&state](std::string path) {
        const std::array update{
            SettingsValueUpdate{.path = std::move(path), .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{0}}},
        };
        REQUIRE(apply_gui_settings_values(state, update));
    };
    select_catalog_source("workflows.validate.model_source");
    CHECK(state.workflows.validate.model_source == ModelSelectionSource::Canonical);
    CHECK(state.workflows.validate.model_input == ModelArtifactInputKind::Weights);
    CHECK(state.workflows.validate.request.onnx_path == "/tmp/validate.onnx");
    select_catalog_source("workflows.predict.model_source");
    CHECK(state.workflows.predict.model_source == ModelSelectionSource::Canonical);
    CHECK(state.workflows.predict.model_input == ModelArtifactInputKind::Weights);
    CHECK(state.workflows.predict.request.tensorrt_path == "/tmp/predict.engine");
    GuiSettingsState defaults = default_gui_settings_state();
    REQUIRE(defaults.workflows.validate.model_source == ModelSelectionSource::Canonical);
    REQUIRE(defaults.workflows.validate.model_input == ModelArtifactInputKind::Weights);
    const std::array unrelated{
        SettingsValueUpdate{.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{true}},
    };
    REQUIRE(apply_gui_settings_values(defaults, unrelated));
    CHECK(defaults.workflows.validate.model_input == ModelArtifactInputKind::Weights);
    const GuiSettingsState before_invalid = defaults;
    const std::array invalid_custom_onnx{
        SettingsValueUpdate{.path = "workflows.validate.model_source", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{1}}},
        SettingsValueUpdate{.path = "workflows.validate.model_input", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{1}}},
    };
    CHECK_FALSE(apply_gui_settings_values(defaults, invalid_custom_onnx));
    CHECK(defaults == before_invalid);
}
void test_model_selection_settings_validation_exhausts_canonical_compatibility() {
    const auto validate_case = [](const FeatureId workflow, const ModelSelectionSource source, const ModelArtifactInputKind input, const bool build_tensorrt) {
        GuiSettingsState state = default_gui_settings_state();
        const ModelArtifactSelectionState artifacts{
            .weights_path = "/tmp/model.pt",
            .onnx_path = "/tmp/model.onnx",
            .tensorrt_path = "/tmp/model.engine",
            .preset_name = std::string{kDefaultModelPresetName},
            .resolution = kDefaultModelResolution,
            .source = source,
            .input = input,
        };
        switch (workflow) {
            case FeatureId::Train: apply_model_artifacts(state.workflows.train, artifacts); break;
            case FeatureId::Validate: apply_model_artifacts(state.workflows.validate, artifacts); break;
            case FeatureId::Predict: apply_model_artifacts(state.workflows.predict, artifacts); break;
            case FeatureId::Export:
                state.workflows.export_state.build_tensorrt = build_tensorrt;
                apply_model_artifacts(state.workflows.export_state, artifacts);
                break;
            case FeatureId::Annotate:
            case FeatureId::Live:
            case FeatureId::Explore: FAIL("test case requires a ModelSystem workflow");
        }
        const bool expected =
            input == ModelArtifactInputKind::None || (workflow == FeatureId::Export ? model_selection_compatible(workflow, source, input, build_tensorrt)
                                                                                    : model_selection_compatible(workflow, source, input));
        CHECK(gui_settings_valid(state) == expected);
    };
    for (const auto workflow : {FeatureId::Train, FeatureId::Validate, FeatureId::Predict}) {
        for (const auto source : {ModelSelectionSource::Canonical, ModelSelectionSource::Custom}) {
            for (const auto input :
                 {ModelArtifactInputKind::Weights, ModelArtifactInputKind::Onnx, ModelArtifactInputKind::TensorRt, ModelArtifactInputKind::None}) {
                validate_case(workflow, source, input, false);
            }
        }
    }
    for (const bool build_tensorrt : {false, true}) {
        for (const auto source : {ModelSelectionSource::Canonical, ModelSelectionSource::Custom}) {
            for (const auto input :
                 {ModelArtifactInputKind::Weights, ModelArtifactInputKind::Onnx, ModelArtifactInputKind::TensorRt, ModelArtifactInputKind::None}) {
                validate_case(FeatureId::Export, source, input, build_tensorrt);
            }
        }
    }
    GuiSettingsState annotation = default_gui_settings_state();
    annotation.workflows.annotate.model_source = ModelSelectionSource::Custom;
    annotation.workflows.annotate.model_input = ModelArtifactInputKind::TensorRt;
    annotation.workflows.annotate.tensorrt_path = "/tmp/annotation.engine";
    CHECK(gui_settings_valid(annotation));
    CHECK_FALSE(model_selection_workflow_supported(FeatureId::Annotate));
    annotation.workflows.annotate.model_source = ModelSelectionSource::Canonical;
    CHECK_FALSE(gui_settings_valid(annotation));
    annotation.workflows.annotate.model_input = ModelArtifactInputKind::None;
    CHECK(gui_settings_valid(annotation));
}
void test_gui_json_persistence_enforces_reflected_field_policies() {
    const GuiSettingsState defaults = default_gui_settings_state();
    REQUIRE(gui_settings_valid(defaults));
    auto train = defaults.workflows.train.request;
    REQUIRE_FALSE(mmltk::frameworks::reflection::validate_reflected_fields(train).has_value());
    train.lr = std::numeric_limits<double>::quiet_NaN();
    CHECK(mmltk::frameworks::reflection::validate_reflected_fields(train).has_value());
    auto predict = defaults.workflows.predict.request;
    REQUIRE_FALSE(mmltk::frameworks::reflection::validate_reflected_fields(predict).has_value());
    predict.threshold = -0.01F;
    CHECK(mmltk::frameworks::reflection::validate_reflected_fields(predict).has_value());
    struct PersistenceCase {
        std::string_view name;
        void (*mutate)(nlohmann::json&);
        bool accepted;
        bool repaired;
    };
    const std::array persistence_cases{
        PersistenceCase{"signed endpoint", [](nlohmann::json& document) { document["ui"]["annotation_brush_radius"] = kMaxAnnotationBrushRadius; }, true,
                        false},
        PersistenceCase{"signed outside", [](nlohmann::json& document) { document["ui"]["annotation_brush_radius"] = kMaxAnnotationBrushRadius + 1; }, true,
                        true},
        PersistenceCase{"unsigned endpoint", [](nlohmann::json& document) { document["workflows"]["explore"]["min_instances"] = 10'000U; }, true, false},
        PersistenceCase{"unsigned outside", [](nlohmann::json& document) { document["workflows"]["explore"]["min_instances"] = 10'001U; }, true, true},
        PersistenceCase{"unsigned conversion overflow",
                        [](nlohmann::json& document) { document["workflows"]["explore"]["min_instances"] = std::numeric_limits<std::uint64_t>::max(); }, false,
                        false},
        PersistenceCase{"float endpoint", [](nlohmann::json& document) { document["ui"]["ui_scale"] = 1.75F; }, true, false},
        PersistenceCase{"float outside", [](nlohmann::json& document) { document["ui"]["ui_scale"] = 1.7501F; }, true, true},
        PersistenceCase{"nonfinite JSON representation", [](nlohmann::json& document) { document["ui"]["ui_scale"] = nullptr; }, false, false},
        PersistenceCase{"path endpoint",
                        [](nlohmann::json& document) {
                            document["workflows"]["train"]["training"]["output_dir"] = std::string(mmltk::frameworks::reflection::kMaximumPathBytes, 'x');
                        },
                        true, false},
        PersistenceCase{"path outside",
                        [](nlohmann::json& document) {
                            document["workflows"]["train"]["training"]["output_dir"] = std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1U, 'x');
                        },
                        false, false},
        PersistenceCase{"container endpoint",
                        [](nlohmann::json& document) {
                            auto& ids = document["workflows"]["train"]["training"]["local_device_ids"];
                            ids = nlohmann::json::array();
                            for (std::size_t index = 0U; index < mmltk::backend::models::rfdetr::kMaximumTrainingDevices; ++index) { ids.push_back(index); }
                        },
                        true, false},
        PersistenceCase{"container outside",
                        [](nlohmann::json& document) {
                            auto& ids = document["workflows"]["train"]["training"]["local_device_ids"];
                            ids = nlohmann::json::array();
                            for (std::size_t index = 0U; index <= mmltk::backend::models::rfdetr::kMaximumTrainingDevices; ++index) { ids.push_back(index); }
                        },
                        false, false},
        PersistenceCase{"container element conversion overflow",
                        [](nlohmann::json& document) {
                            document["workflows"]["train"]["training"]["local_device_ids"] =
                                nlohmann::json::array({static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1U});
                        },
                        false, false},
    };
    mmltk::testsupport::ScopedTempDir persistence_root("mmltk-field-policy-persistence");
    std::size_t persistence_index = 0U;
    for (const auto& test : persistence_cases) {
        nlohmann::json document = default_gui_settings_document();
        test.mutate(document);
        const std::filesystem::path path = persistence_root.path() / ("case-" + std::to_string(persistence_index++) + ".json");
        mmltk::testsupport::write_text_file(path, document.dump());
        GuiSettingsState materialized = defaults;
        bool repaired = false;
        CAPTURE(test.name);
        CHECK(load_settings(path, materialized, nullptr, &repaired) == test.accepted);
        if (test.accepted) {
            CHECK(gui_settings_valid(materialized));
            CHECK(repaired == test.repaired);
        } else
            CHECK(materialized == defaults);
    }
}
void test_explore_class_capacity_is_canonical_across_persistence_and_reflection() {
    constexpr auto filter_policy = mmltk::frameworks::reflection::policy_of_member<&mmltk::controller::ExploreClassSelection::classes>();
    constexpr std::size_t sample_capacity = std::tuple_size_v<decltype(std::declval<ExploreViewState>().sample_classes)>;
    constexpr std::size_t overlay_capacity = std::tuple_size_v<decltype(std::declval<ExploreViewState>().overlay_classes)>;
    CHECK(filter_policy.maximum_items == mmltk::controller::kExploreClassCapacity);
    CHECK(sample_capacity == filter_policy.maximum_items);
    CHECK(overlay_capacity == filter_policy.maximum_items);
}
void test_explore_settings_projection_covers_every_scalar_in_both_directions() {
    for (std::size_t selected = 0U; selected < 4U; ++selected) {
        ExploreViewState one_hot;
        one_hot.require_boxes = selected == 0U;
        one_hot.require_masks = selected == 1U;
        one_hot.show_boxes = selected == 2U;
        one_hot.show_masks = selected == 3U;
        mmltk::controller::ExploreFilterUpdate projected;
        ExploreSettingsProjection::Project(one_hot, projected);
        CHECK(projected.filter.require_boxes == (selected == 0U));
        CHECK(projected.filter.require_masks == (selected == 1U));
        CHECK(projected.overlay.show_boxes == (selected == 2U));
        CHECK(projected.overlay.show_masks == (selected == 3U));
        ExploreViewState restored;
        ExploreSettingsProjection::Visit([&]<auto Setting, auto Filter>() {
            mmltk::frameworks::reflection::access<ExploreViewState, Setting>(restored) =
                mmltk::frameworks::reflection::access<const mmltk::controller::ExploreFilterUpdate, Filter>(projected);
        });
        CHECK(restored.require_boxes == (selected == 0U));
        CHECK(restored.require_masks == (selected == 1U));
        CHECK(restored.show_boxes == (selected == 2U));
        CHECK(restored.show_masks == (selected == 3U));
    }
    ExploreViewState settings;
    settings.require_boxes = true;
    settings.require_masks = false;
    settings.min_instances = 3U;
    settings.max_instances = 29U;
    settings.min_compiled_index = 41U;
    settings.max_compiled_index = 4'091U;
    settings.order = ExploreOrder::Shuffled;
    settings.shuffle_seed = 0x1234'5678U;
    settings.show_boxes = false;
    settings.show_masks = true;
    mmltk::controller::ExploreFilterUpdate filter;
    ExploreSettingsProjection::Visit([&]<auto Setting, auto Filter>() {
        mmltk::frameworks::reflection::access<mmltk::controller::ExploreFilterUpdate, Filter>(filter) =
            mmltk::frameworks::reflection::access<const ExploreViewState, Setting>(settings);
    });
    CHECK(filter.filter.require_boxes);
    CHECK_FALSE(filter.filter.require_masks);
    CHECK(filter.filter.minimum_instances == 3U);
    CHECK(filter.filter.maximum_instances == 29U);
    CHECK(filter.filter.minimum_compiled_index == 41U);
    CHECK(filter.filter.maximum_compiled_index == 4'091U);
    CHECK(filter.filter.order == ExploreOrder::Shuffled);
    CHECK(filter.filter.shuffle_seed == 0x1234'5678U);
    CHECK_FALSE(filter.overlay.show_boxes);
    CHECK(filter.overlay.show_masks);
    filter.filter.require_boxes = false;
    filter.filter.require_masks = true;
    filter.filter.minimum_instances = 5U;
    filter.filter.maximum_instances = 31U;
    filter.filter.minimum_compiled_index = 43U;
    filter.filter.maximum_compiled_index = 4'093U;
    filter.filter.order = ExploreOrder::Sequential;
    filter.filter.shuffle_seed = 0x8765'4321U;
    filter.overlay.show_boxes = true;
    filter.overlay.show_masks = false;
    ExploreSettingsProjection::Visit([&]<auto Setting, auto Filter>() {
        mmltk::frameworks::reflection::access<ExploreViewState, Setting>(settings) =
            mmltk::frameworks::reflection::access<const mmltk::controller::ExploreFilterUpdate, Filter>(filter);
    });
    CHECK_FALSE(settings.require_boxes);
    CHECK(settings.require_masks);
    CHECK(settings.min_instances == 5U);
    CHECK(settings.max_instances == 31U);
    CHECK(settings.min_compiled_index == 43U);
    CHECK(settings.max_compiled_index == 4'093U);
    CHECK(settings.order == ExploreOrder::Sequential);
    CHECK(settings.shuffle_seed == 0x8765'4321U);
    CHECK(settings.show_boxes);
    CHECK_FALSE(settings.show_masks);
}
void test_explore_class_catalog_identity_is_ordered_and_deterministic() {
    const std::array first{
        mmltk::backend::data::catalog::ClassName{.value = "person"},
        mmltk::backend::data::catalog::ClassName{.value = "vehicle"},
    };
    const std::array same = first;
    const std::array reordered{
        mmltk::backend::data::catalog::ClassName{.value = "vehicle"},
        mmltk::backend::data::catalog::ClassName{.value = "person"},
    };
    const std::array repartitioned{
        mmltk::backend::data::catalog::ClassName{.value = "personvehicle"},
    };
    CHECK(mmltk::controller::explore_class_catalog_identity(first) == mmltk::controller::explore_class_catalog_identity(same));
    CHECK(mmltk::controller::explore_class_catalog_identity(first) != mmltk::controller::explore_class_catalog_identity(reordered));
    CHECK(mmltk::controller::explore_class_catalog_identity(first) != mmltk::controller::explore_class_catalog_identity(repartitioned));
}
void test_explore_class_catalog_identity_rejects_generic_settings_mutation() {
    GuiSettingsState state = default_gui_settings_state();
    state.workflows.explore.class_catalog_identity = 0x1234U;
    const std::array update{
        SettingsValueUpdate{
            .path = "workflows.explore.class_catalog_identity",
            .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{0x5678U}},
        },
    };
    CHECK_FALSE(apply_gui_settings_values(state, update));
    CHECK(state.workflows.explore.class_catalog_identity == 0x1234U);
}
void test_opaque_recipe_mask_rejects_generic_settings_mutation_atomically() {
    GuiSettingsState state = default_gui_settings_state();
    const GuiSettingsState before = state;
    const std::array update{
        SettingsValueUpdate{
            .path = "workflows.train.request.recipe_overrides.mask",
            .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{1U}},
        },
    };
    CHECK_FALSE(apply_gui_settings_values(state, update));
    CHECK(state == before);
}
void test_training_supervision_relation_is_enforced_by_generic_settings_validity() {
    const auto match_free = mmltk::frameworks::serialization::wire::FlatValue::text("MatchFree", sizeof("MatchFree") - 1U);
    REQUIRE(match_free.has_value());
    const auto zero = mmltk::frameworks::serialization::wire::FlatValue{0.0};
    const auto one = mmltk::frameworks::serialization::wire::FlatValue{1.0};
    GuiSettingsState valid_ablation = default_gui_settings_state();
    const std::array one_zero_updates{
        SettingsValueUpdate{.path = "workflows.train.request.training_supervision.assignment", .value = *match_free},
        SettingsValueUpdate{.path = "workflows.train.request.training_supervision.match_free.correspondence_weight", .value = zero},
        SettingsValueUpdate{.path = "workflows.train.request.training_supervision.match_free.query_weight", .value = one},
    };
    REQUIRE(apply_gui_settings_values(valid_ablation, one_zero_updates));
    CHECK(gui_settings_valid(valid_ablation));
    CHECK(valid_ablation.workflows.train.request.training_supervision.match_free.correspondence_weight == 0.0F);
    const GuiSettingsState before_rejected_edit = valid_ablation;
    const std::array second_zero{
        SettingsValueUpdate{.path = "workflows.train.request.training_supervision.match_free.query_weight", .value = zero},
    };
    CHECK_FALSE(apply_gui_settings_values(valid_ablation, second_zero));
    CHECK(valid_ablation == before_rejected_edit);
    GuiSettingsState rejected_batch = default_gui_settings_state();
    const GuiSettingsState before_rejected_batch = rejected_batch;
    const std::array both_zero_updates{
        SettingsValueUpdate{.path = "workflows.train.request.training_supervision.assignment", .value = *match_free},
        SettingsValueUpdate{.path = "workflows.train.request.training_supervision.match_free.correspondence_weight", .value = zero},
        SettingsValueUpdate{.path = "workflows.train.request.training_supervision.match_free.query_weight", .value = zero},
    };
    CHECK_FALSE(apply_gui_settings_values(rejected_batch, both_zero_updates));
    CHECK(rejected_batch == before_rejected_batch);
    GuiSettingsState invalid_persistence_candidate = default_gui_settings_state();
    invalid_persistence_candidate.workflows.train.request.training_supervision.assignment = TrainAssignmentKind::MatchFree;
    invalid_persistence_candidate.workflows.train.request.training_supervision.match_free.correspondence_weight = 0.0F;
    invalid_persistence_candidate.workflows.train.request.training_supervision.match_free.query_weight = 0.0F;
    CHECK_FALSE(gui_settings_valid(invalid_persistence_candidate));
    mmltk::testsupport::ScopedTempDir persistence_root{"mmltk-training-supervision-settings"};
    const SettingsLocation location{(persistence_root.path() / "gui.json").string()};
    mmltk::controller::SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    SettingsUpdateRequest persisted_ablation;
    persisted_ablation.updates.assign(one_zero_updates.begin(), one_zero_updates.end());
    const auto persisted = settings.Update(std::move(persisted_ablation));
    CHECK(persisted.settings_state == valid_ablation);
    mmltk::controller::SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    CHECK(reloaded.snapshot().settings_state == valid_ablation);
    SettingsUpdateRequest rejected_persistence;
    rejected_persistence.updates.assign(second_zero.begin(), second_zero.end());
    CHECK_THROWS_AS(settings.Update(std::move(rejected_persistence)), mmltk::controller::contracts::InvalidIntentError);
    CHECK(settings.snapshot() == persisted);
    mmltk::controller::SettingsSystem reloaded_after_rejection;
    REQUIRE(reloaded_after_rejection.Load(location).applied());
    CHECK(reloaded_after_rejection.snapshot() == persisted);
}
void test_schema_v8_training_supervision_round_trip_defaults_and_atomic_rejection() {
    GuiSettingsState state = default_gui_settings_state();
    auto& supervision = state.workflows.train.request.training_supervision;
    supervision.assignment = TrainAssignmentKind::MatchFree;
    supervision.match_free = {.rho = 0.625F, .correspondence_weight = 0.75F, .query_weight = 1.25F};
    supervision.denoising = {
        .enabled = true,
        .groups = 10U,
        .label_noise_ratio = 0.3F,
        .center_noise_scale = 0.45F,
        .size_noise_scale = 0.35F,
    };
    const nlohmann::json saved = snapshot_gui_settings(state);
    const auto& persisted = saved.at("workflows").at("train").at("training").at("training_supervision");
    CHECK(persisted.at("assignment") == "match-free");
    CHECK(persisted.at("match_free").at("rho") == 0.625F);
    CHECK(persisted.at("denoising").at("enabled") == true);
    CHECK(persisted.at("denoising").at("groups") == 10U);
    GuiSettingsState round_trip = default_gui_settings_state();
    apply_gui_settings(saved, round_trip);
    CHECK(round_trip.workflows.train.request.training_supervision == supervision);
    nlohmann::json missing = saved;
    missing["workflows"]["train"]["training"].erase("training_supervision");
    apply_gui_settings(missing, round_trip);
    CHECK(round_trip.workflows.train.request.training_supervision == mmltk::backend::models::rfdetr::TrainingSupervisionConfig{});
    for (const nlohmann::json& malformed :
         {nlohmann::json{"not-an-object"},
          nlohmann::json{
              {"assignment", "match-free"},
              {"match_free", {{"rho", 0.0}, {"correspondence_weight", 1.0}, {"query_weight", 1.0}}},
              {"denoising", {{"enabled", false}, {"groups", 5}, {"label_noise_ratio", 0.2}, {"center_noise_scale", 0.4}, {"size_noise_scale", 0.4}}}},
          nlohmann::json{
              {"assignment", "match-free"},
              {"match_free", {{"rho", 0.5}, {"correspondence_weight", 0.0}, {"query_weight", 0.0}}},
              {"denoising", {{"enabled", false}, {"groups", 5}, {"label_noise_ratio", 0.2}, {"center_noise_scale", 0.4}, {"size_noise_scale", 0.4}}}}}) {
        nlohmann::json invalid = saved;
        invalid["workflows"]["train"]["training"]["training_supervision"] = malformed;
        const GuiSettingsState before = round_trip;
        CHECK_THROWS(apply_gui_settings(invalid, round_trip));
        CHECK(round_trip == before);
    }
}
void test_startup_transport_override_is_session_local_in_both_directions() {
    for (const bool h2d : {false, true}) {
        mmltk::testsupport::ScopedTempDir persistence_root{"mmltk-startup-transport-settings"};
        const SettingsLocation location{(persistence_root.path() / "gui.json").string()};
        auto persisted_state = default_gui_settings_state();
        persisted_state.workflows.train.request.h2d_dataloader = !h2d;
        persisted_state.workflows.validate.request.h2d_dataloader = !h2d;
        persisted_state.workflows.predict.request.h2d_dataloader = !h2d;
        persisted_state.workflows.explore.h2d_dataloader = !h2d;
        std::ofstream file{std::string{location.value()}};
        file << snapshot_gui_settings(persisted_state).dump();
        file.close();
        mmltk::controller::SettingsSystem persisted;
        REQUIRE(persisted.Load(location).applied());
        persisted_state = persisted.snapshot().settings_state;
        mmltk::controller::SettingsSystem overridden;
        REQUIRE(overridden.Load(location, h2d).applied());
        const auto selected = overridden.snapshot().settings_state;
        CHECK(selected.workflows.train.request.h2d_dataloader == h2d);
        CHECK(selected.workflows.validate.request.h2d_dataloader == h2d);
        CHECK(selected.workflows.predict.request.h2d_dataloader == h2d);
        CHECK(selected.workflows.explore.h2d_dataloader == h2d);
        CHECK(overridden.explore_settings_candidate().loading.h2d_dataloader == h2d);
        const auto facts = overridden.materialization_facts();
        CHECK(facts.settings.workflows.train.request.h2d_dataloader == h2d);
        SettingsUpdateRequest edit;
        edit.updates.push_back({.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{false}});
        REQUIRE(apply_gui_settings_values(persisted_state, std::span{edit.updates}));
        static_cast<void>(overridden.Update(std::move(edit)));
        mmltk::controller::SettingsSystem reloaded;
        REQUIRE(reloaded.Load(location).applied());
        CHECK(reloaded.snapshot().settings_state == persisted_state);
        CHECK(overridden.snapshot().settings_state.workflows.explore.h2d_dataloader == h2d);
    }
}
void test_explore_preview_candidate_is_atomic_and_persists_native_modes() {
    mmltk::testsupport::ScopedTempDir persistence_root{"mmltk-explore-preview-settings"};
    const SettingsLocation location{(persistence_root.path() / "gui.json").string()};
    mmltk::controller::SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    const auto initial = settings.explore_settings_candidate();
    CHECK_FALSE(initial.augmentation_preview_enabled);
    CHECK_FALSE(initial.show_original_dimensions);
    CHECK(mmltk::backend::models::rfdetr::gpu_augmentation_config_valid(initial.augmentation));
    const auto preview = settings.Update(initial, {.augmentation_enabled = true});
    CHECK(preview.version > initial.version);
    CHECK(preview.augmentation_preview_enabled);
    CHECK(preview.augmentation.enabled == initial.augmentation.enabled);
    CHECK_THROWS_AS(settings.Update(initial, {.show_original_dimensions = true}), mmltk::controller::contracts::BusyError);
    const auto detail = settings.Update(preview, {.show_original_dimensions = true});
    CHECK(detail.version > preview.version);
    CHECK(detail.augmentation_preview_enabled);
    CHECK(detail.show_original_dimensions);
    CHECK_THROWS_AS(settings.Update(initial, {.preferences = mmltk::controller::ExploreFilterUpdate{}}), mmltk::controller::contracts::BusyError);
    const auto filtered =
        settings.Update(detail, {.preferences = mmltk::controller::ExploreFilterUpdate{.filter = {.minimum_instances = 1U}, .overlay = {.show_boxes = false}}});
    CHECK(filtered.version > detail.version);
    CHECK(filtered.preferences.policy.filter.minimum_instances == 1U);
    CHECK_FALSE(filtered.preferences.policy.overlay.show_boxes);
    mmltk::controller::SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    const auto restored = reloaded.explore_settings_candidate();
    CHECK(restored.augmentation_preview_enabled);
    CHECK(restored.show_original_dimensions);
}
void test_copy_paste_default_and_persisted_overrides() {
    mmltk::testsupport::ScopedTempDir root{"mmltk-copy-paste-settings"};
    const SettingsLocation location{(root.path() / "gui.json").string()};
    mmltk::controller::SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    CHECK(settings.explore_settings_candidate().augmentation.copy_paste_probability == .80F);
    for (const float probability : {0.F, .30F, .50F, 1.F}) {
        SettingsUpdateRequest request;
        request.updates.push_back({.path = "workflows.train.request.gpu_augmentation.copy_paste_probability",
                                   .value = mmltk::frameworks::serialization::wire::FlatValue{static_cast<double>(probability)}});
        (void)settings.Update(std::move(request));
        mmltk::controller::SettingsSystem reloaded;
        REQUIRE(reloaded.Load(location).applied());
        CHECK(reloaded.explore_settings_candidate().augmentation.copy_paste_probability == probability);
    }
    (void)settings.Reset({});
    CHECK(settings.explore_settings_candidate().augmentation.copy_paste_probability == .80F);
    mmltk::controller::SettingsSystem reset_reloaded;
    REQUIRE(reset_reloaded.Load(location).applied());
    CHECK(reset_reloaded.explore_settings_candidate().augmentation.copy_paste_probability == .80F);
}
void test_apply_current_copy_paste_preference() {
    const auto* gate = std::getenv("MMLTK_ACCEPT_APPLY_COPY_PASTE_DEFAULT");
    if (gate == nullptr || std::string_view{gate} != "1") SKIP("current saved preferences require explicit acceptance authorization");
    const auto path = std::filesystem::current_path() / ".mmltk-data" / "gui.json";
    REQUIRE(std::filesystem::is_regular_file(path));
    const SettingsLocation location{path.string()};
    mmltk::controller::SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    auto expected = settings.snapshot().settings_state;
    expected.workflows.train.request.gpu_augmentation.copy_paste_probability = .80F;
    SettingsUpdateRequest request;
    request.updates.push_back({.path = "workflows.train.request.gpu_augmentation.copy_paste_probability",
                               .value = mmltk::frameworks::serialization::wire::FlatValue{static_cast<double>(.80F)}});
    const bool updated_only_requested_preference = settings.Update(std::move(request)).settings_state == expected;
    REQUIRE(updated_only_requested_preference);
    mmltk::controller::SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    const bool persisted_only_requested_preference = reloaded.snapshot().settings_state == expected;
    CHECK(persisted_only_requested_preference);
}
}  // namespace
TEST_CASE("test_ui_settings_round_trip", "[gui][settings]") { test_ui_settings_round_trip(); }
TEST_CASE("test_schema_v8_recipe_golden_shape_and_round_trip", "[gui][settings]") { test_schema_v8_recipe_golden_shape_and_round_trip(); }
TEST_CASE("test_schema_v8_nonuniform_recipe_and_every_missing_member_are_preserved", "[gui][settings]") {
    test_schema_v8_nonuniform_recipe_and_every_missing_member_are_preserved();
}
TEST_CASE("test_schema_v8_recipe_placement_and_unknown_fields_repair_canonically", "[gui][settings]") {
    test_schema_v8_recipe_placement_and_unknown_fields_repair_canonically();
}
TEST_CASE("test_schema_v8_recipe_scheduler_compatibility", "[gui][settings]") { test_schema_v8_recipe_scheduler_compatibility(); }
TEST_CASE("test_schema_v8_recipe_optimizer_compatibility", "[gui][settings]") { test_schema_v8_recipe_optimizer_compatibility(); }
TEST_CASE("test_schema_v8_recipe_malformed_and_constraint_rejection_is_atomic", "[gui][settings]") {
    test_schema_v8_recipe_malformed_and_constraint_rejection_is_atomic();
}
TEST_CASE("test_fresh_defaults_use_capture_only_annotate", "[gui][settings]") { test_fresh_defaults_use_capture_only_annotate(); }
TEST_CASE("test_model_input_load_normalizes_invalid_values_by_workflow", "[gui][settings]") { test_model_input_load_normalizes_invalid_values_by_workflow(); }
TEST_CASE("test_persistence_rejects_unsupported_schema_and_malformed_files", "[gui][settings]") {
    test_persistence_rejects_unsupported_schema_and_malformed_files();
}
// CLEANUP-IGNORE: Each named settings behavior remains independently registered with the native test inventory.
TEST_CASE("test_persistence_repairs_catalog_and_compiled_directory_defaults", "[gui][settings]") {
    test_persistence_repairs_catalog_and_compiled_directory_defaults();
}
TEST_CASE("test_bounded_flat_settings_mutation_is_atomic", "[gui][settings]") { test_bounded_flat_settings_mutation_is_atomic(); }
TEST_CASE("test_catalog_source_transition_normalizes_model_input_at_the_native_boundary", "[gui][settings]") {
    test_catalog_source_transition_normalizes_model_input_at_the_native_boundary();
}
TEST_CASE("test_model_selection_settings_validation_exhausts_canonical_compatibility", "[gui][settings]") {
    test_model_selection_settings_validation_exhausts_canonical_compatibility();
}
TEST_CASE("test_gui_json_persistence_enforces_reflected_field_policies", "[gui][settings]") { test_gui_json_persistence_enforces_reflected_field_policies(); }
TEST_CASE("test_explore_class_capacity_is_canonical_across_persistence_and_reflection", "[gui][settings]") {
    test_explore_class_capacity_is_canonical_across_persistence_and_reflection();
}
TEST_CASE("test_explore_settings_projection_covers_every_scalar_in_both_directions", "[gui][settings]") {
    test_explore_settings_projection_covers_every_scalar_in_both_directions();
}
TEST_CASE("test_explore_class_catalog_identity_is_ordered_and_deterministic", "[gui][settings]") {
    test_explore_class_catalog_identity_is_ordered_and_deterministic();
}
TEST_CASE("test_explore_class_catalog_identity_rejects_generic_settings_mutation", "[gui][settings]") {
    test_explore_class_catalog_identity_rejects_generic_settings_mutation();
}
TEST_CASE("test_opaque_recipe_mask_rejects_generic_settings_mutation_atomically", "[gui][settings]") {
    test_opaque_recipe_mask_rejects_generic_settings_mutation_atomically();
}
TEST_CASE("test_training_supervision_relation_is_enforced_by_generic_settings_validity", "[gui][settings][training_supervision]") {
    test_training_supervision_relation_is_enforced_by_generic_settings_validity();
}
TEST_CASE("test_schema_v8_training_supervision_round_trip_defaults_and_atomic_rejection", "[gui][settings][training_supervision]") {
    test_schema_v8_training_supervision_round_trip_defaults_and_atomic_rejection();
}
TEST_CASE("test_startup_transport_override_is_session_local_in_both_directions", "[gui][settings]") {
    test_startup_transport_override_is_session_local_in_both_directions();
}
TEST_CASE("test_explore_preview_candidate_is_atomic_and_persists_native_modes", "[gui][settings][explore]") {
    test_explore_preview_candidate_is_atomic_and_persists_native_modes();
}
TEST_CASE("test_copy_paste_default_and_persisted_overrides", "[gui][settings][copy_paste]") { test_copy_paste_default_and_persisted_overrides(); }
TEST_CASE("test_apply_current_copy_paste_preference", "[.][acceptance][settings]") { test_apply_current_copy_paste_preference(); }
