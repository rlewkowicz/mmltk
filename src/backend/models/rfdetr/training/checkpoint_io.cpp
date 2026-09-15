#include "src/backend/models/rfdetr/core/class_layout.h"
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include "src/backend/models/rfdetr/core/artifact_publication.h"
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
#include "checkpoint.h"
#include "detail/native_optimizer_private.h"
#include "detail/training_ops_private.h"
#include "model_state_access.h"
#include <unordered_map>

import mmltk.common.logging.profile_utils;

namespace mmltk::backend::models::rfdetr::detail {

namespace torch_api = mmltk::backend::ml::torch_api;

void publish_native_checkpoint_archive(torch_api::OutputArchive& archive, const std::filesystem::path& destination,
    const std::filesystem::path& explicit_descriptor) {
    ClassArtifactPublication publication(destination, explicit_descriptor);
    archive.save_to(publication.staged_artifact().string());
    publication.Publish();
}

namespace serialization = mmltk::frameworks::serialization;

void write_training_configuration(torch_api::OutputArchive& archive, const TrainRequest& request) {
    constexpr auto capacity = serialization::reflected_maximum_cbor_bytes<TrainRequest>();
    std::vector<std::byte> bytes(capacity);
    const auto encoded = serialization::encode(request, std::span<std::byte>(bytes), {.max_bytes = capacity, .max_items = 4096, .max_depth = 32});
    if (!encoded) throw std::runtime_error("training configuration violates its checkpoint schema");
    auto tensor = torch_api::empty({static_cast<int64_t>(*encoded)}, torch_api::kUInt8);
    std::memcpy(tensor.data_ptr(), bytes.data(), *encoded);
    archive.write("training_configuration_cbor", tensor);
}

TrainRequest read_training_configuration(torch_api::InputArchive& archive) {
    torch_api::Tensor configuration;
    if (!archive.try_read("training_configuration_cbor", configuration))
        throw std::runtime_error("full checkpoint is missing current saved training configuration");
    constexpr auto capacity = serialization::reflected_maximum_cbor_bytes<TrainRequest>();
    if (!configuration.defined() || !configuration.is_cpu() || configuration.scalar_type() != torch_api::kUInt8 ||
        configuration.dim() != 1 || configuration.numel() <= 0 || static_cast<std::size_t>(configuration.numel()) > capacity)
        throw std::runtime_error("invalid checkpoint training configuration");
    configuration = configuration.contiguous();
    const auto request = serialization::decode<TrainRequest>(
        {std::span(reinterpret_cast<const std::byte*>(configuration.const_data_ptr()), static_cast<std::size_t>(configuration.numel())), {}},
        {.max_bytes = capacity, .max_items = 4096, .max_depth = 32});
    if (!request) throw std::runtime_error("invalid checkpoint training configuration values");
    validate_train_request(*request);
    return *request;
}

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
                              const bool include_training_supervision, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_checkpoint_save_write_state_archive{"rfdetr.checkpoint.save.write_state_archive"};
    torch_api::OutputArchive state_archive;
    std::size_t entry_count = 0;
    for (const auto& entry : entries) {
        if (include_training_supervision || !entry.name.starts_with(kTrainingSupervisionPrefix)) { ++entry_count; }
    }
    write_int(state_archive, "entry_count", static_cast<int64_t>(entry_count));
    std::size_t output_index = 0;
    for (const auto& entry : entries) {
        const auto slot = first_slot++;
        if (!include_training_supervision && entry.name.starts_with(kTrainingSupervisionPrefix)) { continue; }
        torch_api::OutputArchive entry_archive;
        write_string(entry_archive, "name", entry.name);
        entry_archive.write("tensor", readback.Stage(slot));
        state_archive.write(archive_entry_name(output_index++), entry_archive);
    }
    archive.write(key, state_archive);
}

}  // namespace

void write_native_checkpoint_metadata(torch_api::OutputArchive& archive, const NativeCheckpointMetadata& metadata) {
    if (metadata.num_queries <= 0 || metadata.num_select <= 0 || metadata.num_select > metadata.num_queries) {
        throw std::runtime_error("RF-DETR native checkpoint requires positive num_queries and num_select <= num_queries");
    }
    const ResolvedClassLayout layout(metadata.class_layout);
    if (layout.output_width() != static_cast<std::size_t>(metadata.num_classes))
        throw std::runtime_error("native checkpoint class layout disagrees with tensor output width");
    write_string(archive, "class_layout", encode_class_layout(metadata.class_layout));
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

void reserve_state_archive(const std::vector<NormalizedModelStateEntry>& entries,
                           mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) {
    std::vector<torch_api::Tensor> sources;
    sources.reserve(entries.size());
    for (const auto& entry : entries) sources.push_back(entry.tensor);
    readback.Reserve(sources, first_slot);
}
void write_state_archive(torch_api::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                         mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) {
    write_state_archive_impl(archive, key, entries, false, readback, first_slot);
}
void write_resume_state_archive(torch_api::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                                mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) {
    write_state_archive_impl(archive, key, entries, true, readback, first_slot);
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

void require_resume_training_supervision_config(torch_api::InputArchive& archive, const TrainingSupervisionConfig& expected) {
    if (read_training_supervision_config(archive) != expected) {
        throw std::runtime_error("native RF-DETR resume checkpoint training supervision configuration does not match");
    }
}

}  // namespace mmltk::backend::models::rfdetr::detail

namespace mmltk::backend::models::rfdetr {
TrainingCheckpoint inspect_training_checkpoint(const std::filesystem::path& path) {
    namespace torch_api = mmltk::backend::ml::torch_api;
    namespace serialization = mmltk::frameworks::serialization;
    auto decoded = decode_native_model_state(path);
    if (!decoded.class_artifact) throw std::runtime_error("training checkpoint lacks exact native artifact admission");
    decoded.class_artifact->RequireUnchanged();
    auto& state = detail::model_state_owner(decoded);
    if (!state.native_archive) throw std::runtime_error("training checkpoint lacks its native archive");
    auto& archive = *state.native_archive;
    TrainingCheckpoint result;
    result.path = std::filesystem::canonical(path);
    result.class_layout = decoded.metadata.class_layout;
    result.original_weights = decoded.metadata.source_path;
    torch_api::InputArchive optimizer;
    if (!archive.try_read("optimizer", optimizer)) {
        torch_api::Tensor continuation;
        if (archive.try_read("epoch", continuation) || archive.try_read("training_configuration_cbor", continuation))
            throw std::runtime_error("full training checkpoint is missing optimizer continuation");
        decoded.class_artifact->RequireUnchanged();
        return result;
    }
    const auto request = detail::read_training_configuration(archive);
    const auto epoch = require_int(archive, "epoch");
    if (epoch < 0 || epoch >= std::numeric_limits<int>::max()) throw std::runtime_error("invalid checkpoint epoch");
    std::unordered_map<std::string, torch_api::Tensor> tensors;
    for (const auto& entry : state.entries) tensors.emplace(entry.name, entry.tensor);
    const auto kind = require_string(archive, "optimizer_kind");
    if (kind != cli_enum_spelling(request.optimizer)) throw std::runtime_error("checkpoint optimizer configuration disagrees");
    const auto names = request.optimizer == TrainOptimizerKind::AdamW ? NativeAdamW::InspectCheckpoint(optimizer, tensors) :
                                                                      NativeMuonWithAuxAdam::InspectCheckpoint(optimizer, tensors);
    torch_api::InputArchive ema;
    if (archive.try_read("ema_state", ema) != request.use_ema) throw std::runtime_error("checkpoint EMA continuation is incomplete");
    if (request.use_ema) {
        if (require_int(ema, "entry_count") != static_cast<int64_t>(names.size()))
            throw std::runtime_error("checkpoint EMA inventory is incomplete");
        for (std::size_t index = 0; index < names.size(); ++index) {
            torch_api::InputArchive entry;
            ema.read(std::format("entry_{:06}", index), entry);
            auto tensor = require_tensor(entry, "tensor");
            const auto& parameter = tensors.at(names[index]);
            if (require_string(entry, "name") != names[index] || !tensor.is_cpu() || !tensor.is_floating_point() ||
                tensor.layout() != parameter.layout() || tensor.sizes() != parameter.sizes() ||
                tensor.scalar_type() != parameter.scalar_type() || !torch_api::isfinite(tensor).all().item<bool>())
                throw std::runtime_error("checkpoint EMA tensor is invalid");
        }
    }
    validate_resume_continuation_manifest({request.use_ema, request.use_ema,
        require_double(archive, "grad_scaler_scale"), require_int(archive, "grad_scaler_growth_tracker")});
    if (require_string(archive, "lr_scheduler") != cli_enum_spelling(request.lr_scheduler) ||
        require_int(archive, "lr_drop") != request.lr_drop || require_double(archive, "warmup_epochs") != request.warmup_epochs ||
        require_double(archive, "warmup_momentum") != request.warmup_momentum || require_double(archive, "lr_min_factor") != request.lr_min_factor)
        throw std::runtime_error("checkpoint scheduler configuration disagrees");
    if (std::isnan(require_double(archive, "best_regular_metric")) || std::isnan(require_double(archive, "best_ema_metric")))
        throw std::runtime_error("checkpoint best metric is invalid");
    detail::require_resume_training_supervision_config(archive, request.training_supervision);
    decoded.class_artifact->RequireUnchanged();
    result.original_class_descriptor = require_string(archive, "training_original_descriptor");
    if (result.original_class_descriptor.native().size() > mmltk::frameworks::reflection::kMaximumPathBytes)
        throw std::runtime_error("invalid original checkpoint descriptor provenance");
    result.attempt_id = require_string(archive, "training_attempt_id");
    if (result.attempt_id.empty() || result.attempt_id.size() > 64) throw std::runtime_error("invalid checkpoint attempt identity");
    result.epoch = static_cast<int>(epoch);
    result.configuration = request;
    result.evaluated_weights = request.use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary;
    result.resumable = true;
    return result;
}
}  // namespace mmltk::backend::models::rfdetr
