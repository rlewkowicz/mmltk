#include "mmltk/rfdetr/checkpoint.h"

#include "mmltk/rfdetr/model_config.h"
#include "profile_utils.h"
#include "rfdetr/archive_utils.h"
#include "rfdetr/checkpoint_internal.h"
#include "rfdetr/python_checkpoint_bridge.h"

#include <torch/torch.h>
#include <torch/serialize.h>

#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <string_view>

namespace mmltk::rfdetr {

namespace {

void write_metadata(torch::serialize::OutputArchive& archive, const NativeCheckpointMetadata& metadata) {
    write_string(archive, "format", kNativeCheckpointFormat);
    write_int(archive, "format_version", kNativeCheckpointFormatVersion);
    write_string(archive, "preset_name", metadata.preset_name);
    write_string(archive, "source_kind", metadata.source_kind);
    write_string(archive, "source_path", metadata.source_path);
    write_int(archive, "num_classes", metadata.num_classes);
    write_optional_bool(archive, "sum_group_losses", metadata.sum_group_losses);
    write_optional_bool(archive, "use_varifocal_loss", metadata.use_varifocal_loss);
    write_optional_bool(archive, "use_position_supervised_loss", metadata.use_position_supervised_loss);
    write_optional_bool(archive, "ia_bce_loss", metadata.ia_bce_loss);
    write_optional_bool(archive, "aux_loss", metadata.aux_loss);
    write_optional_int(archive, "mask_point_sample_ratio", metadata.mask_point_sample_ratio);
    write_optional_double(archive, "focal_alpha", metadata.focal_alpha);
    write_optional_double(archive, "cls_loss_coef", metadata.cls_loss_coef);
    write_optional_double(archive, "bbox_loss_coef", metadata.bbox_loss_coef);
    write_optional_double(archive, "giou_loss_coef", metadata.giou_loss_coef);
    write_optional_double(archive, "mask_ce_loss_coef", metadata.mask_ce_loss_coef);
    write_optional_double(archive, "mask_dice_loss_coef", metadata.mask_dice_loss_coef);
    write_optional_double(archive, "set_cost_class", metadata.set_cost_class);
    write_optional_double(archive, "set_cost_bbox", metadata.set_cost_bbox);
    write_optional_double(archive, "set_cost_giou", metadata.set_cost_giou);
}

NativeCheckpoint load_native_checkpoint(const std::filesystem::path& checkpoint_path) {
    const std::filesystem::path canonical_path = detail::canonical_checkpoint_path(checkpoint_path);
    const std::string canonical_path_string = canonical_path.string();

    torch::serialize::InputArchive archive;
    archive.load_from(canonical_path_string);
    if (!detail::is_supported_native_checkpoint_format(require_string(archive, "format"))) {
        throw std::runtime_error("RF-DETR checkpoint is not a native checkpoint: " + canonical_path_string);
    }

    const int64_t format_version = require_int(archive, "format_version");
    if (format_version != kNativeCheckpointFormatVersion) {
        throw std::runtime_error("unsupported RF-DETR native checkpoint format version " +
                                 std::to_string(format_version) + ": " + canonical_path_string);
    }

    NativeCheckpoint checkpoint;
    checkpoint.metadata.preset_name = read_optional_string(archive, "preset_name").value_or("");
    checkpoint.metadata.source_kind = read_optional_string(archive, "source_kind").value_or("native");
    checkpoint.metadata.source_path = read_optional_string(archive, "source_path").value_or(canonical_path_string);
    checkpoint.metadata.num_classes = read_optional_int(archive, "num_classes").value_or(0);
    checkpoint.metadata.sum_group_losses = read_optional_bool(archive, "sum_group_losses");
    checkpoint.metadata.use_varifocal_loss = read_optional_bool(archive, "use_varifocal_loss");
    checkpoint.metadata.use_position_supervised_loss = read_optional_bool(archive, "use_position_supervised_loss");
    checkpoint.metadata.ia_bce_loss = read_optional_bool(archive, "ia_bce_loss");
    checkpoint.metadata.aux_loss = read_optional_bool(archive, "aux_loss");
    checkpoint.metadata.mask_point_sample_ratio = read_optional_int(archive, "mask_point_sample_ratio");
    checkpoint.metadata.focal_alpha = read_optional_double(archive, "focal_alpha");
    checkpoint.metadata.cls_loss_coef = read_optional_double(archive, "cls_loss_coef");
    checkpoint.metadata.bbox_loss_coef = read_optional_double(archive, "bbox_loss_coef");
    checkpoint.metadata.giou_loss_coef = read_optional_double(archive, "giou_loss_coef");
    checkpoint.metadata.mask_ce_loss_coef = read_optional_double(archive, "mask_ce_loss_coef");
    checkpoint.metadata.mask_dice_loss_coef = read_optional_double(archive, "mask_dice_loss_coef");
    checkpoint.metadata.set_cost_class = read_optional_double(archive, "set_cost_class");
    checkpoint.metadata.set_cost_bbox = read_optional_double(archive, "set_cost_bbox");
    checkpoint.metadata.set_cost_giou = read_optional_double(archive, "set_cost_giou");

    torch::serialize::InputArchive state_archive;
    archive.read("state", state_archive);
    const int64_t entry_count = require_int(state_archive, "entry_count");
    if (entry_count < 0) {
        throw std::runtime_error("RF-DETR checkpoint entry_count is negative: " + canonical_path_string);
    }

    checkpoint.state_dict.reserve(static_cast<size_t>(entry_count));
    for (int64_t index = 0; index < entry_count; ++index) {
        torch::serialize::InputArchive entry_archive;
        state_archive.read(archive_entry_name(static_cast<size_t>(index)), entry_archive);

        StateDictEntry entry;
        entry.name = require_string(entry_archive, "name");
        entry_archive.read("tensor", entry.tensor);
        checkpoint.state_dict.push_back(std::move(entry));
    }

    return checkpoint;
}

#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
NativeCheckpoint load_python_checkpoint_file(const std::filesystem::path& checkpoint_path) {
    const std::filesystem::path canonical_path = detail::canonical_checkpoint_path(checkpoint_path);
    const std::string canonical_path_string = canonical_path.string();
    NativeCheckpoint checkpoint = load_upstream_python_checkpoint(canonical_path_string);
    checkpoint.metadata.source_kind = "upstream-python";
    checkpoint.metadata.source_path = canonical_path_string;

    const std::string filename = canonical_path.filename().string();
    if (const auto* filename_preset = find_model_preset_by_weight_filename(filename)) {
        checkpoint.metadata.preset_name = std::string(filename_preset->preset_name);
        checkpoint.metadata.num_classes = filename_preset->num_classes;
    } else if (const auto* path_preset = infer_model_preset_from_path(canonical_path)) {
        checkpoint.metadata.preset_name = std::string(path_preset->preset_name);
        checkpoint.metadata.num_classes = path_preset->num_classes;
    }
    for (const auto& entry : checkpoint.state_dict) {
        if ((entry.name == "class_embed.bias" || entry.name == "class_embed.weight") && entry.tensor.defined() &&
            entry.tensor.dim() >= 1) {
            checkpoint.metadata.num_classes = entry.tensor.size(0);
            break;
        }
    }
    return checkpoint;
}
#endif

}  // namespace

namespace detail {

torch::Tensor prepare_tensor_for_checkpoint_write(const torch::Tensor& tensor) {
    if (!tensor.defined()) {
        throw std::runtime_error("RF-DETR checkpoint tensor is undefined");
    }

    torch::Tensor prepared = tensor.detach();
    if (!prepared.device().is_cpu()) {
        prepared = prepared.to(torch::Device(torch::kCPU));
    }
    if (!prepared.is_contiguous()) {
        prepared = prepared.contiguous();
    }
    return prepared;
}

void write_state_archive(torch::serialize::OutputArchive& archive, const char* key,
                         const std::vector<StateDictEntry>& state_dict) {
    MMLTK_PROFILE_SCOPE("rfdetr.checkpoint.save.write_state_archive");
    torch::serialize::OutputArchive state_archive;
    write_int(state_archive, "entry_count", static_cast<int64_t>(state_dict.size()));
    for (size_t index = 0; index < state_dict.size(); ++index) {
        torch::serialize::OutputArchive entry_archive;
        write_string(entry_archive, "name", state_dict[index].name);
        entry_archive.write("tensor", prepare_tensor_for_checkpoint_write(state_dict[index].tensor));
        state_archive.write(archive_entry_name(index), entry_archive);
    }
    archive.write(key, state_archive);
}

}  // namespace detail

bool is_native_checkpoint_file(const std::filesystem::path& checkpoint_path) {
    try {
        const std::filesystem::path canonical_path = detail::canonical_checkpoint_path(checkpoint_path);
        torch::serialize::InputArchive archive;
        archive.load_from(canonical_path.string());
        return detail::is_supported_native_checkpoint_format(require_string(archive, "format"));
    } catch (const std::exception&) {
        return false;
    }
}

NativeCheckpoint load_checkpoint(const std::filesystem::path& checkpoint_path) {
    if (checkpoint_path.empty()) {
        throw std::runtime_error("RF-DETR checkpoint path must not be empty");
    }

    const std::filesystem::path canonical_path = detail::canonical_checkpoint_path(checkpoint_path);
    const std::string canonical_path_string = canonical_path.string();
    if (!std::filesystem::exists(canonical_path)) {
        throw std::runtime_error("missing RF-DETR checkpoint file: " + canonical_path_string);
    }

    if (is_native_checkpoint_file(canonical_path)) {
        return load_native_checkpoint(canonical_path);
    }
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
    return load_python_checkpoint_file(canonical_path);
#else
    throw std::runtime_error("RF-DETR upstream Python checkpoint loading is disabled at build time: " +
                             canonical_path_string);
#endif
}

StateDictLoadSummary apply_checkpoint_to_module(torch::nn::Module& module, const NativeCheckpoint& checkpoint,
                                                bool strict) {
    auto parameters = module.named_parameters(true);
    auto buffers = module.named_buffers(true);

    StateDictLoadSummary summary;
    summary.loaded_names.reserve(checkpoint.state_dict.size());

    std::unordered_set<std::string> expected_names;
    for (const auto& entry : parameters) {
        expected_names.insert(entry.key());
    }
    for (const auto& entry : buffers) {
        expected_names.insert(entry.key());
    }

    torch::NoGradGuard no_grad;
    for (const auto& entry : checkpoint.state_dict) {
        torch::Tensor* target = parameters.find(entry.name);
        if (target == nullptr) {
            target = buffers.find(entry.name);
        }
        if (target == nullptr) {
            summary.unexpected_names.push_back(entry.name);
            continue;
        }
        if (!target->defined()) {
            throw std::runtime_error("RF-DETR module target tensor is undefined: " + entry.name);
        }
        if (target->sizes() != entry.tensor.sizes()) {
            if (strict) {
                throw std::runtime_error("RF-DETR checkpoint shape mismatch for " + entry.name);
            }
            summary.incompatible_names.push_back(entry.name);
            continue;
        }
        if (target->scalar_type() != entry.tensor.scalar_type()) {
            if (strict) {
                throw std::runtime_error("RF-DETR checkpoint dtype mismatch for " + entry.name);
            }
            summary.incompatible_names.push_back(entry.name);
            continue;
        }
        target->copy_(entry.tensor.to(target->device(), target->scalar_type()));
        summary.loaded_names.push_back(entry.name);
        expected_names.erase(entry.name);
    }

    summary.missing_names.assign(expected_names.begin(), expected_names.end());
    std::ranges::sort(summary.missing_names);
    std::ranges::sort(summary.unexpected_names);
    std::ranges::sort(summary.incompatible_names);

    if (strict &&
        (!summary.missing_names.empty() || !summary.unexpected_names.empty() || !summary.incompatible_names.empty())) {
        std::ostringstream message;
        message << "RF-DETR checkpoint/module state_dict mismatch";
        if (!summary.missing_names.empty()) {
            message << " missing=" << summary.missing_names.front();
            if (summary.missing_names.size() > 1) {
                message << " +" << (summary.missing_names.size() - 1);
            }
        }
        if (!summary.unexpected_names.empty()) {
            message << " unexpected=" << summary.unexpected_names.front();
            if (summary.unexpected_names.size() > 1) {
                message << " +" << (summary.unexpected_names.size() - 1);
            }
        }
        if (!summary.incompatible_names.empty()) {
            message << " incompatible=" << summary.incompatible_names.front();
            if (summary.incompatible_names.size() > 1) {
                message << " +" << (summary.incompatible_names.size() - 1);
            }
        }
        throw std::runtime_error(message.str());
    }
    return summary;
}

StateDictLoadSummary apply_checkpoint_to_module(torch::nn::Module& module, const std::filesystem::path& checkpoint_path,
                                                bool strict) {
    return apply_checkpoint_to_module(module, load_checkpoint(checkpoint_path), strict);
}

void save_native_checkpoint(const std::filesystem::path& checkpoint_path, const NativeCheckpoint& checkpoint) {
    MMLTK_PROFILE_SCOPE("rfdetr.checkpoint.save.total");
    const std::filesystem::path canonical_path = detail::canonical_checkpoint_path(checkpoint_path);
    const std::string canonical_path_string = canonical_path.string();
    std::filesystem::create_directories(canonical_path.parent_path());

    torch::serialize::OutputArchive archive;
    write_metadata(archive, checkpoint.metadata);
    detail::write_state_archive(archive, "state", checkpoint.state_dict);
    {
        MMLTK_PROFILE_SCOPE("rfdetr.checkpoint.save.archive_save_to");
        archive.save_to(canonical_path_string);
    }
}

NativeCheckpoint normalize_checkpoint_to_native(const std::filesystem::path& input_path,
                                                const std::filesystem::path& output_path) {
    NativeCheckpoint checkpoint = load_checkpoint(input_path);
    save_native_checkpoint(output_path, checkpoint);
    return checkpoint;
}

}  // namespace mmltk::rfdetr
