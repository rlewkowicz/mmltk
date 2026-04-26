#include "gui/train_command.h"
#include "rfdetr/train_recipe.h"

#include "mmltk/rfdetr/workflow_requests.h"

#include <algorithm>
#include "support/catch2_compat.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace mmltk::gui;

void test_single_device_builds_device_id() {
    mmltk::rfdetr::TrainRequest request;
    request.train_compiled_path = "/tmp/train.bin";
    request.val_compiled_path = "/tmp/val.bin";
    request.output_dir = "/tmp/output";
    request.weights_path = "/tmp/weights.pt";
    request.device_ids = {2};
    const std::vector<std::string> args = build_train_command_arguments(request);
    const auto found = std::find(args.begin(), args.end(), "--device-id");
    assert(found != args.end());
    assert(found + 1 != args.end());
    assert(*(found + 1) == "2");
    assert(std::find(args.begin(), args.end(), "--device-ids") == args.end());
}

void test_multi_device_builds_device_ids() {
    mmltk::rfdetr::TrainRequest request;
    request.train_compiled_path = "/tmp/train.bin";
    request.val_compiled_path = "/tmp/val.bin";
    request.output_dir = "/tmp/output";
    request.weights_path = "/tmp/weights.pt";
    request.device_ids = {0, 2, 4};
    const std::vector<std::string> args = build_train_command_arguments(request);
    const auto found = std::find(args.begin(), args.end(), "--device-ids");
    assert(found != args.end());
    assert(found + 1 != args.end());
    assert(*(found + 1) == "0,2,4");
    assert(std::find(args.begin(), args.end(), "--device-id") == args.end());
}

void test_zero_device_rejected() {
    mmltk::rfdetr::TrainRequest request;
    request.train_compiled_path = "/tmp/train.bin";
    request.val_compiled_path = "/tmp/val.bin";
    request.output_dir = "/tmp/output";
    request.weights_path = "/tmp/weights.pt";
    request.device_id = -1;
    bool threw = false;
    try {
        (void)build_train_command_arguments(request);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

void test_optimizer_arguments_are_forwarded() {
    mmltk::rfdetr::TrainRequest request;
    request.train_compiled_path = "/tmp/train.bin";
    request.val_compiled_path = "/tmp/val.bin";
    request.output_dir = "/tmp/output";
    request.weights_path = "/tmp/weights.pt";
    request.device_ids = {1};
    request.optimizer = mmltk::rfdetr::TrainOptimizerKind::Muon;
    request.print_freq = 3;
    request.lr_encoder = 3.0e-4;
    request.lr_component_decay = 0.7;
    request.encoder_layer_decay = 0.8;
    request.momentum = 0.91;
    request.lr_scheduler = "cosine";
    request.lr_min_factor = 0.01;
    request.warmup_epochs = 3.0;
    request.warmup_momentum = 0.8;
    const std::vector<std::string> args = build_train_command_arguments(request);

    const auto optimizer = std::find(args.begin(), args.end(), "--optimizer");
    assert(optimizer != args.end());
    assert(optimizer + 1 != args.end());
    assert(*(optimizer + 1) == "muon");

    const auto momentum = std::find(args.begin(), args.end(), "--momentum");
    assert(momentum != args.end());
    assert(momentum + 1 != args.end());
    assert(*(momentum + 1) == "0.910000");

    const auto print_freq = std::find(args.begin(), args.end(), "--print-freq");
    assert(print_freq != args.end());
    assert(print_freq + 1 != args.end());
    assert(*(print_freq + 1) == "3");

    const auto lr_encoder = std::find(args.begin(), args.end(), "--lr-encoder");
    assert(lr_encoder != args.end());
    assert(lr_encoder + 1 != args.end());
    assert(*(lr_encoder + 1) == "0.000300");

    const auto warmup_momentum = std::find(args.begin(), args.end(), "--warmup-momentum");
    assert(warmup_momentum != args.end());
    assert(warmup_momentum + 1 != args.end());
    assert(*(warmup_momentum + 1) == "0.800000");

    const auto scheduler = std::find(args.begin(), args.end(), "--lr-scheduler");
    assert(scheduler != args.end());
    assert(scheduler + 1 != args.end());
    assert(*(scheduler + 1) == "cosine");
}

void test_progress_flag_enabled_is_forwarded() {
    mmltk::rfdetr::TrainRequest request;
    request.train_compiled_path = "/tmp/train.bin";
    request.val_compiled_path = "/tmp/val.bin";
    request.output_dir = "/tmp/output";
    request.weights_path = "/tmp/weights.pt";
    request.device_ids = {1};
    request.progress_bar = true;
    const std::vector<std::string> args = build_train_command_arguments(request);
    assert(std::find(args.begin(), args.end(), "--progress") != args.end());
    assert(std::find(args.begin(), args.end(), "--no-progress") == args.end());
}

void test_progress_flag_disabled_is_forwarded() {
    mmltk::rfdetr::TrainRequest request;
    request.train_compiled_path = "/tmp/train.bin";
    request.val_compiled_path = "/tmp/val.bin";
    request.output_dir = "/tmp/output";
    request.weights_path = "/tmp/weights.pt";
    request.device_ids = {1};
    request.progress_bar = false;
    const std::vector<std::string> args = build_train_command_arguments(request);
    assert(std::find(args.begin(), args.end(), "--no-progress") != args.end());
    assert(std::find(args.begin(), args.end(), "--progress") == args.end());
}

void test_resume_input_is_serialized_without_weights() {
    mmltk::rfdetr::TrainRequest request;
    request.train_compiled_path = "/tmp/train.bin";
    request.val_compiled_path = "/tmp/val.bin";
    request.output_dir = "/tmp/output";
    request.resume_path = "/tmp/resume.pt";
    request.device_id = 3;
    const std::vector<std::string> args = build_train_command_arguments(request);

    const auto resume = std::find(args.begin(), args.end(), "--resume");
    assert(resume != args.end());
    assert(resume + 1 != args.end());
    assert(*(resume + 1) == "/tmp/resume.pt");
    assert(std::find(args.begin(), args.end(), "--weights") == args.end());
}

void test_muon_recipe_defaults_are_resolved() {
    const auto recipe =
        mmltk::rfdetr::resolve_train_recipe("rf-detr-seg-medium", mmltk::rfdetr::TrainOptimizerKind::Muon);
    assert(mmltk::rfdetr::train_recipe_value_matches(recipe.lr, 2.0e-4));
    assert(mmltk::rfdetr::train_recipe_value_matches(recipe.lr_encoder, 3.0e-4));
    assert(mmltk::rfdetr::train_recipe_value_matches(recipe.momentum, 0.9));
    assert(mmltk::rfdetr::train_recipe_value_matches(recipe.weight_decay, 5.0e-4));
    assert(mmltk::rfdetr::train_recipe_value_matches(recipe.warmup_epochs, 3.0));
    assert(mmltk::rfdetr::train_recipe_value_matches(recipe.warmup_momentum, 0.8));
    assert(mmltk::rfdetr::train_recipe_value_matches(recipe.lr_min_factor, 0.01));
    assert(recipe.lr_scheduler == "cosine");
}

void test_recipe_application_respects_overrides() {
    mmltk::rfdetr::TrainOptions options;
    options.lr = 9.0e-4;
    mmltk::rfdetr::TrainRecipeFieldOverrides overrides;
    overrides.lr = true;
    mmltk::rfdetr::apply_train_recipe(
        options, mmltk::rfdetr::resolve_train_recipe("rf-detr-medium", mmltk::rfdetr::TrainOptimizerKind::Muon),
        overrides);
    assert(mmltk::rfdetr::train_recipe_value_matches(options.lr, 9.0e-4));
    assert(mmltk::rfdetr::train_recipe_value_matches(options.lr_encoder, 3.0e-4));
    assert(options.lr_scheduler == "cosine");
    assert(mmltk::rfdetr::train_recipe_value_matches(options.warmup_momentum, 0.8));
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][train_command]", test_single_device_builds_device_id);
MMLTK_REGISTER_TEST_CASE("[gui][train_command]", test_multi_device_builds_device_ids);
MMLTK_REGISTER_TEST_CASE("[gui][train_command]", test_zero_device_rejected);
MMLTK_REGISTER_TEST_CASE("[gui][train_command]", test_optimizer_arguments_are_forwarded);
MMLTK_REGISTER_TEST_CASE("[gui][train_command]", test_progress_flag_enabled_is_forwarded);
MMLTK_REGISTER_TEST_CASE("[gui][train_command]", test_progress_flag_disabled_is_forwarded);
MMLTK_REGISTER_TEST_CASE("[gui][train_command]", test_resume_input_is_serialized_without_weights);
MMLTK_REGISTER_TEST_CASE("[gui][train_command]", test_muon_recipe_defaults_are_resolved);
MMLTK_REGISTER_TEST_CASE("[gui][train_command]", test_recipe_application_respects_overrides);
