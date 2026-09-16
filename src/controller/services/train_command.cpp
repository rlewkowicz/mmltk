#include "src/controller/services/train_command.h"
#include <array>
#include <charconv>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>
#include "src/backend/models/rfdetr/contract/cli.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/common/system/runtime_paths.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
namespace mmltk::controller::services {
namespace {
void append_value(std::vector<std::string>& arguments, const std::string_view name, const std::string_view value) {
    if (value.empty()) return;
    arguments.emplace_back(name.data(), name.size());
    arguments.emplace_back(value.data(), value.size());
}
void append_path(std::vector<std::string>& arguments, const std::string_view name, const std::filesystem::path& value) {
    if (!value.empty()) append_value(arguments, name, value.string());
}
void append_number(std::vector<std::string>& arguments, const std::string_view name, const int value) { append_value(arguments, name, std::to_string(value)); }
void append_number(std::vector<std::string>& arguments, const std::string_view name, const std::size_t value) {
    append_value(arguments, name, std::to_string(value));
}
void append_number(std::vector<std::string>& arguments, const std::string_view name, const double value) {
    append_value(arguments, name, std::to_string(value));
}
void append_number(std::vector<std::string>& arguments, const std::string_view name, const float value) {
    std::array<char, 32U> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value, std::chars_format::general, std::numeric_limits<float>::max_digits10);
    if (converted.ec != std::errc{}) throw std::logic_error("RF-DETR supervision float cannot be represented for the CLI");
    append_value(arguments, name, std::string_view{text.data(), static_cast<std::size_t>(converted.ptr - text.data())});
}
void append_flag(std::vector<std::string>& arguments, const std::string_view enabled_name, const std::string_view disabled_name, const bool enabled) {
    const std::string_view name = enabled ? enabled_name : disabled_name;
    arguments.emplace_back(name.data(), name.size());
}
[[nodiscard]] std::string_view require_enum_spelling(const std::string_view spelling, const char* error) {
    if (spelling.empty()) throw std::invalid_argument(error);
    return spelling;
}
[[nodiscard]] std::string train_recipe_option_name(const std::string_view member_name) {
    std::string result{"--"};
    result.reserve(member_name.size() + 2U);
    for (const char character : member_name) result.push_back(character == '_' ? '-' : character);
    return result;
}
template <class Value>
void append_train_recipe_value(std::vector<std::string>& arguments, const std::string_view name, const Value& value) {
    if constexpr (std::is_enum_v<Value>) {
        append_value(arguments, name,
                     require_enum_spelling(mmltk::backend::models::rfdetr::cli_enum_spelling(value), "RF-DETR train request has an invalid recipe enum"));
    } else {
        append_number(arguments, name, value);
    }
}
void append_augmentation_group(std::vector<std::string>& arguments, const std::string_view group,
                               const mmltk::backend::models::rfdetr::AugmentationGroupConfig& value) {
    std::string prefix{"--aug-"};
    prefix.append(group.data(), group.size());
    append_number(arguments, prefix + "-prob", value.probability);
    append_number(arguments, prefix + "-min-strength", value.min_strength);
    append_number(arguments, prefix + "-max-strength", value.max_strength);
}
[[nodiscard]] std::string joined_integer_list(const std::vector<int>& devices) {
    std::string result;
    for (const int device : devices) {
        if (!result.empty()) result.push_back(',');
        result += std::to_string(device);
    }
    return result;
}
}  // namespace
std::vector<std::string> build_train_command_arguments(const mmltk::backend::models::rfdetr::TrainRequest& request,
                                                       const std::string_view fallback_preset_name) {
    const auto* command = mmltk::backend::models::rfdetr::rfdetr_command_descriptor(mmltk::backend::models::rfdetr::RfdetrCommand::Train);
    if (command == nullptr) throw std::logic_error("RF-DETR train command vocabulary is incomplete");
    auto effective = request;
    if (effective.preset_name.empty()) effective.preset_name = fallback_preset_name;
    mmltk::backend::models::rfdetr::validate_train_request(effective);
    std::vector<int> devices = effective.device_ids;
    if (devices.empty() && effective.device_id >= 0) devices.push_back(effective.device_id);
    if (devices.empty()) throw std::invalid_argument("RF-DETR train requires at least one selected CUDA device");
    if (devices.size() == 1U && !effective.numa_nodes.empty()) {
        effective.numa_node = effective.numa_nodes.front();
        effective.numa_nodes.clear();
    }
    std::vector<std::string> arguments;
    arguments.reserve(144U);
    arguments.emplace_back("rfdetr");
    arguments.emplace_back(command->name);
    append_path(arguments, "--train-compiled", effective.train_compiled_path);
    append_path(arguments, "--val-compiled", effective.val_compiled_path);
    append_path(arguments, "--test-compiled", effective.test_compiled_path);
    append_number(arguments, "--resolution", effective.resolution);
    append_path(arguments, "--output-dir", effective.output_dir);
    append_path(arguments, "--weights", effective.weights_path);
    append_path(arguments, "--resume", effective.resume_path);
    append_value(arguments, "--preset", effective.preset_name);
    append_number(arguments, "--num-queries", effective.num_queries);
    append_number(arguments, "--batch-size", effective.batch_size);
    append_number(arguments, "--val-batch-size", effective.val_batch_size);
    append_number(arguments, "--epochs", effective.epochs);
    append_number(arguments, "--grad-accum-steps", effective.grad_accum_steps);
    append_value(
        arguments, "--optimizer",
        require_enum_spelling(mmltk::backend::models::rfdetr::cli_enum_spelling(effective.optimizer), "RF-DETR train request has an invalid optimizer"));
    const auto& overrides = effective.recipe_overrides;
    using RecipeRelation = mmltk::frameworks::reflection::catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>;
    RecipeRelation::VisitMembers([&]<class Entry>() {
        if (!RecipeRelation::template overridden<Entry::destination>(overrides)) return;
        const auto option_name = train_recipe_option_name(
            mmltk::frameworks::reflection::materialized_member_name<std::remove_cvref_t<decltype(Entry::destination)>::terminal_member>());
        append_train_recipe_value(arguments, option_name,
                                  mmltk::frameworks::reflection::access<const mmltk::backend::models::rfdetr::TrainRequest, Entry::destination>(effective));
    });
    append_flag(arguments, "--freeze-encoder", "--no-freeze-encoder", effective.freeze_encoder);
    append_number(arguments, "--clip-max-norm", effective.clip_max_norm);
    append_flag(arguments, "--fused-optimizer", "--no-fused-optimizer", effective.fused_optimizer);
    append_flag(arguments, "--use-ema", "--no-ema", effective.use_ema);
    append_flag(arguments, "--validation-loss", "--no-validation-loss", effective.validation_loss);
    append_flag(arguments, "--validation-profile", "--no-validation-profile", effective.validation_profile);
    append_number(arguments, "--ema-decay", effective.ema_decay);
    append_number(arguments, "--ema-tau", effective.ema_tau);
    append_number(arguments, "--eval-max-dets", effective.eval_max_dets);
    const auto& supervision = effective.training_supervision;
    append_value(
        arguments, "--assignment",
        require_enum_spelling(mmltk::backend::models::rfdetr::cli_enum_spelling(supervision.assignment), "RF-DETR train request has an invalid assignment"));
    append_number(arguments, "--match-free-rho", supervision.match_free.rho);
    append_number(arguments, "--match-free-correspondence-weight", supervision.match_free.correspondence_weight);
    append_number(arguments, "--match-free-query-weight", supervision.match_free.query_weight);
    append_flag(arguments, "--dn", "--no-dn", supervision.denoising.enabled);
    append_number(arguments, "--dn-groups", static_cast<std::size_t>(supervision.denoising.groups));
    append_number(arguments, "--dn-label-noise-ratio", supervision.denoising.label_noise_ratio);
    append_number(arguments, "--dn-center-noise-scale", supervision.denoising.center_noise_scale);
    append_number(arguments, "--dn-size-noise-scale", supervision.denoising.size_noise_scale);
    append_flag(arguments, "--gpu-augment", "--no-gpu-augment", effective.gpu_augmentation.enabled);
    append_flag(arguments, "--aug-perceptual-downscale", "--no-aug-perceptual-downscale", effective.gpu_augmentation.perceptual_downscale);
    append_augmentation_group(arguments, "geometry", effective.gpu_augmentation.geometry);
    append_augmentation_group(arguments, "resize", effective.gpu_augmentation.resize);
    append_augmentation_group(arguments, "color", effective.gpu_augmentation.color);
    append_augmentation_group(arguments, "noise", effective.gpu_augmentation.noise);
    append_augmentation_group(arguments, "blur", effective.gpu_augmentation.blur);
    append_augmentation_group(arguments, "occlusion", effective.gpu_augmentation.occlusion);
    append_number(arguments, "--aug-copy-paste-prob", effective.gpu_augmentation.copy_paste_probability);
    if (devices.size() == 1U) {
        append_number(arguments, "--device-id", devices.front());
    } else {
        append_value(arguments, "--device-ids", joined_integer_list(devices));
    }
    append_number(arguments, "--workers", effective.workers);
    append_number(arguments, "--lanes", effective.lanes);
    namespace reflection = mmltk::frameworks::reflection;
    using Request = std::remove_cvref_t<decltype(effective)>;
    constexpr std::array transport_options{reflection::negative_flag<Request, &Request::h2d_dataloader>("--gdrcopy", "Use GDRCopy image loading", "Execution")};
    reflection::emit(arguments, effective, transport_options);
    append_number(arguments, "--numa-node", effective.numa_node);
    if (!effective.numa_nodes.empty()) append_value(arguments, "--numa-nodes", joined_integer_list(effective.numa_nodes));
    append_value(arguments, "--cpu-affinity", effective.cpu_affinity);
    append_number(arguments, "--prefetch-factor", effective.prefetch_factor);
    append_number(arguments, "--print-freq", effective.print_freq);
    append_number(arguments, "--seed", effective.seed);
    append_flag(arguments, "--amp", "--no-amp", effective.amp);
    append_flag(arguments, "--progress", "--no-progress", effective.progress_bar);
    append_value(arguments, "--compile-mode",
                 require_enum_spelling(mmltk::backend::models::rfdetr::cli_enum_spelling(effective.compilation_mode),
                                       "RF-DETR train request has an invalid compilation mode"));
    if (effective.distributed_worker) {
        arguments.emplace_back("--dist-worker");
        append_number(arguments, "--dist-rank", effective.distributed_rank);
        append_number(arguments, "--dist-world-size", effective.distributed_world_size);
        append_path(arguments, "--dist-store-file", effective.distributed_store_path);
    }
    return arguments;
}
std::filesystem::path current_executable_path() { return mmltk::common::system::runtime_paths::current_executable_path(); }
std::filesystem::path resolve_sibling_mmltk_cli(const std::filesystem::path& executable_path) {
    const std::filesystem::path cli_path = executable_path.parent_path() / "mmltk";
    if (!std::filesystem::exists(cli_path)) { throw std::runtime_error("failed to locate sibling mmltk next to mmltk-browser-host: " + cli_path.string()); }
    return cli_path;
}
}  // namespace mmltk::controller::services
