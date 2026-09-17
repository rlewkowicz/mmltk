#include <cmath>
#include "src/controller/services/train_command.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "src/test_support/error_expectation_test_utils.hpp"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace {
bool train_recipe_value_matches(double lhs, double rhs, double eps = 1.0e-12) { return std::abs(lhs - rhs) <= eps; }
using namespace mmltk::controller::services;
using TrainRecipeRelation = mmltk::frameworks::reflection::catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>;
template <auto Member>
void pin_recipe_member(mmltk::backend::models::rfdetr::TrainRequest& request) {
    TrainRecipeRelation::template set_override<mmltk::frameworks::reflection::member_path<Member>>(request.recipe_overrides);
}
// Canonical train request with the shared dataset/output/weight paths used by every test here.
mmltk::backend::models::rfdetr::TrainRequest make_train_request(std::vector<int> device_ids = {}) {
    mmltk::backend::models::rfdetr::TrainRequest request;
    request.train_compiled_path = "/tmp/train.bin";
    request.val_compiled_path = "/tmp/val.bin";
    request.output_dir = "/tmp/output";
    request.weights_path = "/tmp/weights.pt";
    request.resolution = 384;
    request.device_ids = std::move(device_ids);
    return request;
}
void assert_flag_with_value(const std::vector<std::string>& args, const std::string_view flag, const std::string_view value) {
    const auto found = std::find(args.begin(), args.end(), flag);
    REQUIRE((found != args.end()));
    REQUIRE((found + 1 != args.end()));
    REQUIRE((*(found + 1) == value));
}
void assert_flag_present(const std::vector<std::string>& args, const std::string_view flag) {
    REQUIRE((std::find(args.begin(), args.end(), flag) != args.end()));
}
void assert_float_flag_round_trip(const std::vector<std::string>& args, const std::string_view flag, const float expected) {
    const auto found = std::find(args.begin(), args.end(), flag);
    REQUIRE(found != args.end());
    REQUIRE(found + 1 != args.end());
    float parsed = 0.0F;
    const std::string& text = *(found + 1);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, std::chars_format::general);
    REQUIRE(result.ec == std::errc{});
    REQUIRE(result.ptr == text.data() + text.size());
    CHECK(parsed == expected);
}
void assert_flag_absent(const std::vector<std::string>& args, const std::string_view flag) {
    REQUIRE((std::find(args.begin(), args.end(), flag) == args.end()));
}
void test_single_device_builds_device_id() {
    const std::vector<std::string> args = build_train_command_arguments(make_train_request({2}));
    assert_flag_with_value(args, "--device-id", "2");
    assert_flag_with_value(args, "--numa-node", "-1");
    assert_flag_absent(args, "--device-ids");
    assert_flag_absent(args, "--gdrcopy");
    auto request = make_train_request({2});
    request.numa_nodes = {3};
    request.h2d_dataloader = false;
    const auto placed = build_train_command_arguments(request);
    assert_flag_with_value(placed, "--numa-node", "3");
    assert_flag_present(placed, "--gdrcopy");
    assert_flag_absent(placed, "--numa-nodes");
}
void test_multi_device_builds_device_ids() {
    auto request = make_train_request({0, 2, 4});
    request.numa_nodes = {0, 3, -1};
    const std::vector<std::string> args = build_train_command_arguments(request);
    assert_flag_with_value(args, "--numa-node", "-1");
    assert_flag_with_value(args, "--numa-nodes", "0,3,-1");
    assert_flag_with_value(args, "--device-ids", "0,2,4");
    assert_flag_absent(args, "--device-id");
}
void test_zero_device_rejected() {
    auto request = make_train_request();
    request.device_id = -1;
    mmltk::testsupport::expect_runtime_error_contains([&request]() { (void)build_train_command_arguments(request); });
}
void test_perceptual_selection_is_independent_in_child_arguments() {
    auto request = make_train_request({0});
    for (const bool enabled : {false, true})
        for (const bool perceptual : {false, true}) {
            request.gpu_augmentation.enabled = enabled;
            request.gpu_augmentation.perceptual_downscale = perceptual;
            const auto arguments = build_train_command_arguments(request);
            CHECK(std::ranges::find(arguments, enabled ? "--gpu-augment" : "--no-gpu-augment") != arguments.end());
            CHECK(std::ranges::find(arguments, perceptual ? "--aug-perceptual-downscale" : "--no-aug-perceptual-downscale") != arguments.end());
        }
}
void test_optimizer_arguments_are_forwarded() {
    mmltk::backend::models::rfdetr::TrainRequest request = make_train_request({1});
    request.optimizer = mmltk::backend::models::rfdetr::TrainOptimizerKind::Muon;
    request.print_freq = 3;
    request.lr_encoder = 3.0e-4;
    request.lr_component_decay = 0.7;
    request.encoder_layer_decay = 0.8;
    request.momentum = 0.91;
    request.lr_scheduler = mmltk::backend::models::rfdetr::TrainLrSchedulerKind::Cosine;
    request.lr_min_factor = 0.01;
    request.warmup_epochs = 3.0;
    request.warmup_momentum = 0.8;
    pin_recipe_member<&mmltk::backend::models::rfdetr::TrainRequest::lr_encoder>(request);
    pin_recipe_member<&mmltk::backend::models::rfdetr::TrainRequest::lr_component_decay>(request);
    pin_recipe_member<&mmltk::backend::models::rfdetr::TrainRequest::encoder_layer_decay>(request);
    pin_recipe_member<&mmltk::backend::models::rfdetr::TrainRequest::momentum>(request);
    pin_recipe_member<&mmltk::backend::models::rfdetr::TrainRequest::lr_scheduler>(request);
    pin_recipe_member<&mmltk::backend::models::rfdetr::TrainRequest::lr_min_factor>(request);
    pin_recipe_member<&mmltk::backend::models::rfdetr::TrainRequest::warmup_epochs>(request);
    pin_recipe_member<&mmltk::backend::models::rfdetr::TrainRequest::warmup_momentum>(request);
    const std::vector<std::string> args = build_train_command_arguments(request);
    assert_flag_with_value(args, "--optimizer", "muon");
    assert_flag_with_value(args, "--momentum", "0.91");
    assert_flag_with_value(args, "--print-freq", "3");
    assert_flag_with_value(args, "--lr-encoder", "3e-04");
    assert_flag_with_value(args, "--warmup-momentum", "0.8");
    assert_flag_with_value(args, "--lr-scheduler", "cosine");
}
void test_scheduler_spelling_is_stable_for_cli_and_checkpoint_metadata() {
    using mmltk::backend::models::rfdetr::cli_enum_spelling;
    using mmltk::backend::models::rfdetr::train_lr_scheduler_from_spelling;
    using mmltk::backend::models::rfdetr::TrainLrSchedulerKind;
    REQUIRE((cli_enum_spelling(TrainLrSchedulerKind::Step) == "step"));
    REQUIRE((cli_enum_spelling(TrainLrSchedulerKind::Cosine) == "cosine"));
    REQUIRE((train_lr_scheduler_from_spelling("step") == TrainLrSchedulerKind::Step));
    REQUIRE((train_lr_scheduler_from_spelling("cosine") == TrainLrSchedulerKind::Cosine));
    REQUIRE((!train_lr_scheduler_from_spelling("Cosine")));
}
void test_recipe_defaults_are_not_serialized_as_overrides() {
    const std::vector<std::string> args = build_train_command_arguments(make_train_request({1}));
    assert_flag_absent(args, "--lr");
    assert_flag_absent(args, "--lr-encoder");
    assert_flag_absent(args, "--weight-decay");
}
void check_progress_flag_forwarding(const bool progress_bar, const std::string_view expected_present, const std::string_view expected_absent) {
    mmltk::backend::models::rfdetr::TrainRequest request = make_train_request({1});
    request.progress_bar = progress_bar;
    const std::vector<std::string> args = build_train_command_arguments(request);
    assert_flag_present(args, expected_present);
    assert_flag_absent(args, expected_absent);
}
void test_progress_flag_enabled_is_forwarded() { check_progress_flag_forwarding(true, "--progress", "--no-progress"); }
void test_progress_flag_disabled_is_forwarded() { check_progress_flag_forwarding(false, "--no-progress", "--progress"); }
void test_resume_input_is_serialized_without_weights() {
    mmltk::backend::models::rfdetr::TrainRequest request = make_train_request();
    request.weights_path.clear();
    request.resume_path = "/tmp/resume.pt";
    request.device_id = 3;
    const std::vector<std::string> args = build_train_command_arguments(request);
    assert_flag_with_value(args, "--resume", "/tmp/resume.pt");
    assert_flag_absent(args, "--weights");
}
void test_supervision_combinations_are_forwarded_with_exact_values() {
    using mmltk::backend::models::rfdetr::TrainAssignmentKind;
    for (const auto& [assignment, denoising, expected_assignment, expected_dn] :
         {std::tuple{TrainAssignmentKind::Hungarian, false, "hungarian", "--no-dn"}, std::tuple{TrainAssignmentKind::Hungarian, true, "hungarian", "--dn"},
          std::tuple{TrainAssignmentKind::MatchFree, false, "match-free", "--no-dn"}, std::tuple{TrainAssignmentKind::MatchFree, true, "match-free", "--dn"}}) {
        auto request = make_train_request({1});
        request.training_supervision.assignment = assignment;
        request.training_supervision.match_free = {
            .rho = 0.625F,
            .correspondence_weight = 0.75F,
            .query_weight = 1.25F,
        };
        request.training_supervision.denoising = {
            .enabled = denoising,
            .groups = 10U,
            .label_noise_ratio = 0.3F,
            .center_noise_scale = 0.45F,
            .size_noise_scale = 0.35F,
        };
        const auto args = build_train_command_arguments(request);
        assert_flag_with_value(args, "--assignment", expected_assignment);
        assert_float_flag_round_trip(args, "--match-free-rho", request.training_supervision.match_free.rho);
        assert_float_flag_round_trip(args, "--match-free-correspondence-weight", request.training_supervision.match_free.correspondence_weight);
        assert_float_flag_round_trip(args, "--match-free-query-weight", request.training_supervision.match_free.query_weight);
        assert_flag_present(args, expected_dn);
        assert_flag_with_value(args, "--dn-groups", "10");
        assert_float_flag_round_trip(args, "--dn-label-noise-ratio", request.training_supervision.denoising.label_noise_ratio);
        assert_float_flag_round_trip(args, "--dn-center-noise-scale", request.training_supervision.denoising.center_noise_scale);
        assert_float_flag_round_trip(args, "--dn-size-noise-scale", request.training_supervision.denoising.size_noise_scale);
    }
}
void test_supervision_float_arguments_round_trip_at_representable_boundaries() {
    auto request = make_train_request({1});
    request.training_supervision.assignment = mmltk::backend::models::rfdetr::TrainAssignmentKind::MatchFree;
    request.training_supervision.match_free.rho = mmltk::backend::models::rfdetr::kSupervisionOpenUnitMaximum;
    request.training_supervision.match_free.correspondence_weight = 0.12345679F;
    request.training_supervision.denoising.enabled = true;
    request.training_supervision.denoising.label_noise_ratio = 0.87654322F;
    request.training_supervision.denoising.center_noise_scale = mmltk::backend::models::rfdetr::kSupervisionOpenUnitMinimum;
    request.training_supervision.denoising.size_noise_scale = mmltk::backend::models::rfdetr::kSupervisionOpenUnitMaximum;
    const auto args = build_train_command_arguments(request);
    assert_float_flag_round_trip(args, "--match-free-rho", request.training_supervision.match_free.rho);
    assert_float_flag_round_trip(args, "--match-free-correspondence-weight", request.training_supervision.match_free.correspondence_weight);
    assert_float_flag_round_trip(args, "--dn-label-noise-ratio", request.training_supervision.denoising.label_noise_ratio);
    assert_float_flag_round_trip(args, "--dn-center-noise-scale", request.training_supervision.denoising.center_noise_scale);
    assert_float_flag_round_trip(args, "--dn-size-noise-scale", request.training_supervision.denoising.size_noise_scale);
}
void test_muon_recipe_defaults_are_resolved() {
    const auto recipe = mmltk::backend::models::rfdetr::train_recipe(mmltk::backend::models::rfdetr::TrainOptimizerKind::Muon);
    REQUIRE((train_recipe_value_matches(recipe.lr, 2.0e-4)));
    REQUIRE((train_recipe_value_matches(recipe.lr_encoder, 3.0e-4)));
    REQUIRE((train_recipe_value_matches(recipe.momentum, 0.9)));
    REQUIRE((train_recipe_value_matches(recipe.weight_decay, 5.0e-4)));
    REQUIRE((train_recipe_value_matches(recipe.warmup_epochs, 3.0)));
    REQUIRE((train_recipe_value_matches(recipe.warmup_momentum, 0.8)));
    REQUIRE((train_recipe_value_matches(recipe.lr_min_factor, 0.01)));
    REQUIRE((recipe.lr_scheduler == mmltk::backend::models::rfdetr::TrainLrSchedulerKind::Cosine));
}
void test_recipe_application_respects_overrides() {
    mmltk::backend::models::rfdetr::TrainRequest options;
    options.lr = 9.0e-4;
    mmltk::backend::models::rfdetr::TrainRecipeOverrideState overrides;
    TrainRecipeRelation::template set_override<mmltk::frameworks::reflection::member_path<&mmltk::backend::models::rfdetr::TrainRequest::lr>>(overrides);
    mmltk::backend::models::rfdetr::apply_train_recipe(
        options, mmltk::backend::models::rfdetr::train_recipe(mmltk::backend::models::rfdetr::TrainOptimizerKind::Muon), overrides);
    REQUIRE((train_recipe_value_matches(options.lr, 9.0e-4)));
    REQUIRE((train_recipe_value_matches(options.lr_encoder, 3.0e-4)));
    REQUIRE((options.lr_scheduler == mmltk::backend::models::rfdetr::TrainLrSchedulerKind::Cosine));
    REQUIRE((train_recipe_value_matches(options.warmup_momentum, 0.8)));
}
}  // namespace
TEST_CASE("test_single_device_builds_device_id", "[gui][train_command]") { test_single_device_builds_device_id(); }
TEST_CASE("test_multi_device_builds_device_ids", "[gui][train_command]") { test_multi_device_builds_device_ids(); }
TEST_CASE("test_zero_device_rejected", "[gui][train_command]") { test_zero_device_rejected(); }
TEST_CASE("test_optimizer_arguments_are_forwarded", "[gui][train_command]") { test_optimizer_arguments_are_forwarded(); }
TEST_CASE("test_scheduler_spelling_is_stable_for_cli_and_checkpoint_metadata", "[gui][train_command]") {
    test_scheduler_spelling_is_stable_for_cli_and_checkpoint_metadata();
}
TEST_CASE("test_recipe_defaults_are_not_serialized_as_overrides", "[gui][train_command]") { test_recipe_defaults_are_not_serialized_as_overrides(); }
TEST_CASE("test_progress_flag_enabled_is_forwarded", "[gui][train_command]") { test_progress_flag_enabled_is_forwarded(); }
TEST_CASE("test_progress_flag_disabled_is_forwarded", "[gui][train_command]") { test_progress_flag_disabled_is_forwarded(); }
TEST_CASE("test_resume_input_is_serialized_without_weights", "[gui][train_command]") { test_resume_input_is_serialized_without_weights(); }
TEST_CASE("test_supervision_combinations_are_forwarded_with_exact_values", "[gui][train_command][training_supervision]") {
    test_supervision_combinations_are_forwarded_with_exact_values();
}
TEST_CASE("test_supervision_float_arguments_round_trip_at_representable_boundaries", "[gui][train_command][training_supervision]") {
    test_supervision_float_arguments_round_trip_at_representable_boundaries();
}
TEST_CASE("test_muon_recipe_defaults_are_resolved", "[gui][train_command]") { test_muon_recipe_defaults_are_resolved(); }
TEST_CASE("test_recipe_application_respects_overrides", "[gui][train_command]") { test_recipe_application_respects_overrides(); }
TEST_CASE("test_perceptual_selection_is_independent_in_child_arguments", "[gui][train_command][perceptual]") {
    test_perceptual_selection_is_independent_in_child_arguments();
}

TEST_CASE("training command forwards only an explicitly selected test dataset", "[gui][train]") {
    auto request = make_train_request({0});
    assert_flag_absent(build_train_command_arguments(request), "--test-compiled");
    request.test_compiled_path = "/independent/test.bin";
    assert_flag_with_value(build_train_command_arguments(request), "--test-compiled", "/independent/test.bin");
    request.test_compiled_path.clear();
    assert_flag_absent(build_train_command_arguments(request), "--test-compiled");
}
