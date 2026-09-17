#include "src/backend/models/rfdetr/contract/weight_catalog.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include "src/backend/models/rfdetr/contract/cli.h"
#include "src/backend/models/rfdetr/contract/contract.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace {
constexpr std::array kRetiredWeightFilenames{
    "rf-detr-base.pth", "rf-detr-base-o365.pth", "rf-detr-base-2.pth", "rf-detr-large.pth", "rf-detr-seg-preview.pt",
};
constexpr std::array kRetiredPresetNames{
    "rf-detr-base", "rf-detr-base-o365", "rf-detr-base-2", "rf-detr-large-deprecated", "rf-detr-seg-preview",
};
TEST_CASE("RF-DETR weight assets derive from the preset authority", "[backend][models][rfdetr][contract]") {
    using namespace mmltk::backend::models::rfdetr;
    REQUIRE(weight_catalog().size() == 10U);
    const auto* seg_medium = find_weight_asset("rf-detr-seg-medium.pt");
    REQUIRE(seg_medium != nullptr);
    REQUIRE(seg_medium->filename == std::string_view{"rf-detr-seg-medium.pt"});
    REQUIRE(seg_medium->download_url == std::string_view{"https://storage.googleapis.com/rfdetr/rf-detr-seg-m-ft.pth"});
    REQUIRE(seg_medium->md5_hash == std::string_view{"a49af1562c3719227ad43d0ca53b4c7a"});
    const auto* nano = find_weight_asset("rf-detr-nano.pth");
    REQUIRE(nano != nullptr);
    REQUIRE(nano->download_url == std::string_view{"https://storage.googleapis.com/rfdetr/nano_coco/"
                                                   "checkpoint_best_regular.pth"});
    for (const auto* filename : kRetiredWeightFilenames) {
        REQUIRE(find_weight_asset(filename) == nullptr);
        REQUIRE_FALSE(is_registered_weight_asset(filename));
    }
}
TEST_CASE("RF-DETR weight path lookup preserves membership semantics", "[backend][models][rfdetr][contract]") {
    using namespace mmltk::backend::models::rfdetr;
    const auto resolved = resolve_weight_asset_for_path(std::string{"/tmp/models/rf-detr-seg-small.pt"});
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->filename == std::string_view{"rf-detr-seg-small.pt"});
    REQUIRE(is_registered_weight_asset("rf-detr-seg-small.pt"));
    REQUIRE_FALSE(resolve_weight_asset_for_path(std::string{"/tmp/models/rf-detr-large.pth"}).has_value());
    REQUIRE_FALSE(is_registered_weight_asset("missing.pt"));
    REQUIRE(find_weight_asset("missing.pt") == nullptr);
}
TEST_CASE("RF-DETR preset projection preserves every architecture field", "[backend][models][rfdetr][contract]") {
    using namespace mmltk::backend::models::rfdetr;
    REQUIRE(kRfdetrContract.model_id == std::string_view{"rfdetr"});
    REQUIRE(kRfdetrContract.presets.size() == 10U);
    REQUIRE(model_presets().size() == kRfdetrContract.presets.size());
    const auto* preset = find_preset_catalog_entry("rf-detr-seg-medium");
    REQUIRE(preset != nullptr);
    REQUIRE(preset == &kPresetCatalog[6U]);
    REQUIRE(preset->preset_name == "rf-detr-seg-medium");
    REQUIRE(preset->encoder == "dinov2_windowed_small");
    REQUIRE(preset->canonical_weight_filename == "rf-detr-seg-medium.pt");
    REQUIRE(preset->resolution == 432);
    REQUIRE(preset->patch_size == 12);
    REQUIRE(preset->window_count == 2);
    REQUIRE(preset->positional_encoding_size == 36);
    REQUIRE(preset->decoder_layer_count == 5);
    REQUIRE(preset->query_count == 200);
    REQUIRE(preset->selected_query_count == 200);
    REQUIRE(preset->class_count == 91);
    REQUIRE(preset->hidden_dimension == 256);
    REQUIRE(preset->group_count == 13);
    REQUIRE(preset->two_stage);
    REQUIRE(preset->task == ModelTask::Segmentation);
    REQUIRE(preset->classification_loss_coefficient == 5.0);
    REQUIRE(preset->bounding_box_loss_coefficient == 5.0);
    REQUIRE(preset->generalized_iou_loss_coefficient == 2.0);
    REQUIRE(preset->mask_cross_entropy_loss_coefficient == 5.0);
    REQUIRE(preset->mask_dice_loss_coefficient == 5.0);
    const auto* large = find_model_preset_by_weight_filename("rf-detr-large-2026.pth");
    REQUIRE(large != nullptr);
    REQUIRE(large->preset_name == std::string_view{"rf-detr-large"});
    REQUIRE(large->patch_size == 16);
    REQUIRE(large->window_count == 2);
    REQUIRE(large->positional_encoding_size == 44);
    REQUIRE(large->task == ModelTask::Detection);
    const auto* seg_nano = find_preset_catalog_entry("rf-detr-seg-nano");
    REQUIRE(seg_nano != nullptr);
    REQUIRE(seg_nano->patch_size == 12);
    REQUIRE(seg_nano->window_count == 1);
    REQUIRE(seg_nano->positional_encoding_size == 26);
}
TEST_CASE("RF-DETR preset lookup rejects retired ambiguous and unknown names", "[backend][models][rfdetr][contract]") {
    using namespace mmltk::backend::models::rfdetr;
    const auto* upstream = find_model_preset_by_weight_filename("rf-detr-seg-m-ft.pth");
    REQUIRE(upstream != nullptr);
    REQUIRE(upstream->preset_name == std::string_view{"rf-detr-seg-medium"});
    const auto* legacy = infer_model_preset_from_path(std::filesystem::path{"/tmp/engines/output-seg-med/1train/checkpoint.pt"});
    REQUIRE(legacy != nullptr);
    REQUIRE(legacy->preset_name == std::string_view{"rf-detr-seg-medium"});
    for (const auto* preset_name : kRetiredPresetNames) REQUIRE(find_preset_catalog_entry(preset_name) == nullptr);
    for (const auto* filename : kRetiredWeightFilenames) REQUIRE(find_model_preset_by_weight_filename(filename) == nullptr);
    REQUIRE(find_model_preset_by_weight_filename("checkpoint_best_regular.pth") == nullptr);
    REQUIRE(find_preset_catalog_entry("unknown-preset") == nullptr);
    REQUIRE(find_preset_catalog_entry("") == nullptr);
    REQUIRE(find_preset_catalog_entry("RF-DETR-NANO") == nullptr);
    for (const auto& entry : kPresetCatalog) REQUIRE(find_preset_catalog_entry(entry.preset_name) == &entry);
    REQUIRE(find_model_preset_by_weight_filename("unknown-file.pth") == nullptr);
    REQUIRE(infer_model_preset_from_path(std::filesystem::path{"/tmp/unrelated.bin"}) == nullptr);
}
TEST_CASE("RF-DETR command spellings resolve to one typed vocabulary", "[backend][models][rfdetr][contract]") {
    using namespace mmltk::backend::models::rfdetr;
    REQUIRE(parse_rfdetr_command("evaluate") == RfdetrCommand::Evaluate);
    REQUIRE(parse_rfdetr_command("eval") == RfdetrCommand::Evaluate);
    REQUIRE(parse_rfdetr_command("val") == RfdetrCommand::Evaluate);
    REQUIRE(parse_rfdetr_command("train") == RfdetrCommand::Train);
    REQUIRE_FALSE(parse_rfdetr_command("unknown").has_value());
    const auto* descriptor = rfdetr_command_descriptor(RfdetrCommand::NormalizeWeights);
    REQUIRE(descriptor != nullptr);
    REQUIRE(descriptor->name == std::string_view{"normalize-weights"});
    REQUIRE(rfdetr_command_descriptor(static_cast<RfdetrCommand>(std::numeric_limits<std::uint8_t>::max())) == nullptr);
}
TEST_CASE("RF-DETR enum projections make unknown values observable", "[backend][models][rfdetr][contract]") {
    using namespace mmltk::backend::models::rfdetr;
    REQUIRE(model_task_name(ModelTask::Detection) == std::string_view{"detection"});
    REQUIRE(model_task_name(ModelTask::Segmentation) == std::string_view{"segmentation"});
    REQUIRE_FALSE(model_task_name(static_cast<ModelTask>(std::numeric_limits<std::uint8_t>::max())).has_value());
}
TEST_CASE("RF-DETR reflected scalar policy precedes relationship admission", "[backend][models][rfdetr][contract]") {
    using namespace mmltk::backend::models::rfdetr;
    TrainRequest request;
    request.train_compiled_path = "/tmp/train.mmltk";
    request.val_compiled_path = "/tmp/val.mmltk";
    request.weights_path = "/tmp/weights.pt";
    request.output_dir = "/tmp/output";
    request.gpu_augmentation.geometry.probability = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(validate_train_request(request), std::runtime_error);
    request.gpu_augmentation.geometry.probability = 0.5F;
    request.gpu_augmentation.geometry.min_strength = 0.75F;
    request.gpu_augmentation.geometry.max_strength = 0.25F;
    REQUIRE_THROWS_AS(validate_train_request(request), std::runtime_error);
}
}  // namespace
