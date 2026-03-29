#include "gui/train_command.h"
#include "rfdetr/train_recipe.h"

#include "fastloader/rfdetr/train.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace fastloader::gui;

void test_single_device_builds_device_id() {
    TrainCommandConfig config;
    config.train_compiled_path = "/tmp/train.bin";
    config.val_compiled_path = "/tmp/val.bin";
    config.output_dir = "/tmp/output";
    config.weights_path = "/tmp/weights.pt";
    config.device_ids = {2};
    const std::vector<std::string> args = build_train_command_arguments(config);
    const auto found = std::find(args.begin(), args.end(), "--device-id");
    assert(found != args.end());
    assert(found + 1 != args.end());
    assert(*(found + 1) == "2");
    assert(std::find(args.begin(), args.end(), "--device-ids") == args.end());
}

void test_multi_device_builds_device_ids() {
    TrainCommandConfig config;
    config.train_compiled_path = "/tmp/train.bin";
    config.val_compiled_path = "/tmp/val.bin";
    config.output_dir = "/tmp/output";
    config.weights_path = "/tmp/weights.pt";
    config.device_ids = {0, 2, 4};
    const std::vector<std::string> args = build_train_command_arguments(config);
    const auto found = std::find(args.begin(), args.end(), "--device-ids");
    assert(found != args.end());
    assert(found + 1 != args.end());
    assert(*(found + 1) == "0,2,4");
    assert(std::find(args.begin(), args.end(), "--device-id") == args.end());
}

void test_zero_device_rejected() {
    TrainCommandConfig config;
    config.train_compiled_path = "/tmp/train.bin";
    config.val_compiled_path = "/tmp/val.bin";
    config.output_dir = "/tmp/output";
    config.weights_path = "/tmp/weights.pt";
    bool threw = false;
    try {
        (void)build_train_command_arguments(config);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

void test_optimizer_arguments_are_forwarded() {
    TrainCommandConfig config;
    config.train_compiled_path = "/tmp/train.bin";
    config.val_compiled_path = "/tmp/val.bin";
    config.output_dir = "/tmp/output";
    config.weights_path = "/tmp/weights.pt";
    config.device_ids = {1};
    config.optimizer = "muon";
    config.lr_encoder = 3.0e-4;
    config.lr_component_decay = 0.7;
    config.encoder_layer_decay = 0.8;
    config.momentum = 0.91;
    config.lr_scheduler = "cosine";
    config.lr_min_factor = 0.01;
    config.warmup_epochs = 3.0;
    config.warmup_momentum = 0.8;
    const std::vector<std::string> args = build_train_command_arguments(config);

    const auto optimizer = std::find(args.begin(), args.end(), "--optimizer");
    assert(optimizer != args.end());
    assert(optimizer + 1 != args.end());
    assert(*(optimizer + 1) == "muon");

    const auto momentum = std::find(args.begin(), args.end(), "--momentum");
    assert(momentum != args.end());
    assert(momentum + 1 != args.end());
    assert(*(momentum + 1) == "0.910000");

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

void test_muon_recipe_defaults_are_resolved() {
    const auto recipe = fastloader::rfdetr::resolve_train_recipe(
        "rf-detr-seg-medium",
        fastloader::rfdetr::TrainOptimizerKind::Muon);
    assert(fastloader::rfdetr::train_recipe_value_matches(recipe.lr, 2.0e-4));
    assert(fastloader::rfdetr::train_recipe_value_matches(recipe.lr_encoder, 3.0e-4));
    assert(fastloader::rfdetr::train_recipe_value_matches(recipe.momentum, 0.9));
    assert(fastloader::rfdetr::train_recipe_value_matches(recipe.weight_decay, 5.0e-4));
    assert(fastloader::rfdetr::train_recipe_value_matches(recipe.warmup_epochs, 3.0));
    assert(fastloader::rfdetr::train_recipe_value_matches(recipe.warmup_momentum, 0.8));
    assert(fastloader::rfdetr::train_recipe_value_matches(recipe.lr_min_factor, 0.01));
    assert(recipe.lr_scheduler == "cosine");
}

void test_recipe_application_respects_overrides() {
    fastloader::rfdetr::TrainOptions options;
    options.lr = 9.0e-4;
    fastloader::rfdetr::TrainRecipeFieldOverrides overrides;
    overrides.lr = true;
    fastloader::rfdetr::apply_train_recipe(
        options,
        fastloader::rfdetr::resolve_train_recipe("rf-detr-medium", fastloader::rfdetr::TrainOptimizerKind::Muon),
        overrides);
    assert(fastloader::rfdetr::train_recipe_value_matches(options.lr, 9.0e-4));
    assert(fastloader::rfdetr::train_recipe_value_matches(options.lr_encoder, 3.0e-4));
    assert(options.lr_scheduler == "cosine");
    assert(fastloader::rfdetr::train_recipe_value_matches(options.warmup_momentum, 0.8));
}

} // namespace

int main() {
    test_single_device_builds_device_id();
    test_multi_device_builds_device_ids();
    test_zero_device_rejected();
    test_optimizer_arguments_are_forwarded();
    test_muon_recipe_defaults_are_resolved();
    test_recipe_application_respects_overrides();
    return 0;
}
