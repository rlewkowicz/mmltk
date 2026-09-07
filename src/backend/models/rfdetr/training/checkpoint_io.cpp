#include <array>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "archive_utils.h"
#include "model_technical.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include "torch_api.h"
#include "detail/checkpoint_private.h"

import mmltk.common.logging.profile_utils;

namespace mmltk::backend::models::rfdetr::detail {

namespace torch_api = mmltk::backend::ml::torch_api;
namespace serialization = mmltk::frameworks::serialization;

namespace {

constexpr char kTrainingSupervisionKey[] = "training_supervision_config_cbor";
constexpr std::string_view kTrainingSupervisionPrefix = "training_supervision.";
constexpr std::size_t kTrainingSupervisionCborCapacity = serialization::reflected_maximum_cbor_bytes<TrainingSupervisionConfig>();
constexpr serialization::wire::Limits kTrainingSupervisionCborLimits{
    .max_bytes = kTrainingSupervisionCborCapacity,
    .max_items = 64U,
    .max_depth = 8U,
};

void write_state_archive_impl(torch_api::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                              const bool include_training_supervision) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_checkpoint_save_write_state_archive{"rfdetr.checkpoint.save.write_state_archive"};
    torch_api::OutputArchive state_archive;
    std::size_t entry_count = 0;
    for (const auto& entry : entries) {
        if (include_training_supervision || !entry.name.starts_with(kTrainingSupervisionPrefix)) { ++entry_count; }
    }
    write_int(state_archive, "entry_count", static_cast<int64_t>(entry_count));
    std::size_t output_index = 0;
    for (const auto& entry : entries) {
        if (!include_training_supervision && entry.name.starts_with(kTrainingSupervisionPrefix)) { continue; }
        torch_api::OutputArchive entry_archive;
        write_string(entry_archive, "name", entry.name);
        entry_archive.write("tensor", prepare_tensor_for_checkpoint_write(entry.tensor));
        state_archive.write(archive_entry_name(output_index++), entry_archive);
    }
    archive.write(key, state_archive);
}

}  // namespace

void write_native_checkpoint_metadata(torch_api::OutputArchive& archive, const NativeCheckpointMetadata& metadata) {
    if (metadata.num_queries <= 0 || metadata.num_select <= 0 || metadata.num_select > metadata.num_queries) {
        throw std::runtime_error("RF-DETR native checkpoint requires positive num_queries and num_select <= num_queries");
    }
    write_string(archive, "format", kNativeCheckpointFormat);
    write_int(archive, "format_version", kNativeCheckpointFormatVersion);
    write_string(archive, "preset_name", metadata.preset_name);
    write_string(archive, "source_kind", metadata.source_kind);
    write_string(archive, "source_path", metadata.source_path);
    write_int(archive, "num_classes", metadata.num_classes);
    write_int(archive, "num_queries", metadata.num_queries);
    write_int(archive, "num_select", metadata.num_select);
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

torch_api::Tensor prepare_tensor_for_checkpoint_write(const torch_api::Tensor& tensor) {
    if (!tensor.defined()) { throw std::runtime_error("RF-DETR checkpoint tensor is undefined"); }

    torch_api::Tensor prepared = tensor.detach();
    if (!prepared.device().is_cpu()) { prepared = prepared.to(torch_api::Device(torch_api::kCPU)); }
    if (!prepared.is_contiguous()) { prepared = prepared.contiguous(); }
    return prepared;
}

void write_state_archive(torch_api::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries) {
    write_state_archive_impl(archive, key, entries, false);
}

void write_resume_state_archive(torch_api::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries) {
    write_state_archive_impl(archive, key, entries, true);
}

void write_training_supervision_config(torch_api::OutputArchive& archive, const TrainingSupervisionConfig& config) {
    if (config == TrainingSupervisionConfig{}) { return; }
    std::array<std::byte, kTrainingSupervisionCborCapacity> bytes{};
    const auto encoded = serialization::encode(config, std::span<std::byte>(bytes), kTrainingSupervisionCborLimits);
    if (!encoded.has_value()) { throw std::runtime_error("failed to encode bounded RF-DETR training supervision configuration"); }
    auto tensor =
        torch_api::empty({static_cast<int64_t>(*encoded)}, torch_api::TensorOptions().dtype(torch_api::kUInt8).device(torch_api::kCPU));
    std::memcpy(tensor.data_ptr<std::uint8_t>(), bytes.data(), *encoded);
    archive.write(kTrainingSupervisionKey, tensor);
}

TrainingSupervisionConfig read_training_supervision_config(torch_api::InputArchive& archive) {
    torch_api::Tensor tensor;
    if (!archive.try_read(kTrainingSupervisionKey, tensor)) { return {}; }
    if (!tensor.defined() || !tensor.device().is_cpu() || tensor.scalar_type() != torch_api::kUInt8 || tensor.dim() != 1 ||
        tensor.numel() <= 0 || static_cast<std::uint64_t>(tensor.numel()) > kTrainingSupervisionCborCapacity) {
        throw std::runtime_error("invalid RF-DETR training supervision CBOR blob");
    }
    tensor = tensor.contiguous();
    const auto bytes =
        std::span(reinterpret_cast<const std::byte*>(tensor.data_ptr<std::uint8_t>()), static_cast<std::size_t>(tensor.numel()));
    const auto decoded = serialization::decode<TrainingSupervisionConfig>({bytes, {}}, kTrainingSupervisionCborLimits);
    if (!decoded.has_value() || !training_supervision_config_valid(*decoded)) {
        throw std::runtime_error("invalid RF-DETR training supervision CBOR configuration");
    }
    return *decoded;
}

void require_resume_training_supervision_config(const std::filesystem::path& checkpoint_path, const TrainingSupervisionConfig& expected) {
    torch_api::InputArchive archive;
    archive.load_from(checkpoint_path.string());
    if (read_training_supervision_config(archive) != expected) {
        throw std::runtime_error("native RF-DETR resume checkpoint training supervision configuration does not match");
    }
}

}  // namespace mmltk::backend::models::rfdetr::detail
