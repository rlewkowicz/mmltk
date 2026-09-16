#include "src/backend/models/rfdetr/core/class_layout.h"
#include <array>
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
#include "detail/training_continuation.h"
#include "detail/model_ema.h"
#include "model_state_access.h"
#include <unordered_map>
#include <type_traits>
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
    metadata.for_each_detection_field([&]<class Optional>(const char* key, const Optional& value) {
        if constexpr (std::is_same_v<typename Optional::value_type, bool>)
            write_optional_bool(archive, key, value);
        else if constexpr (std::is_same_v<typename Optional::value_type, int64_t>)
            write_optional_int(archive, key, value);
        else
            write_optional_double(archive, key, value);
    });
}
void reserve_state_archive(const std::vector<NormalizedModelStateEntry>& entries, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback,
                           std::size_t first_slot) {
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
    auto tensor = torch_api::empty({static_cast<int64_t>(*encoded)}, torch_api::TensorOptions().dtype(torch_api::kUInt8).device(torch_api::kCPU));
    std::memcpy(tensor.data_ptr<std::uint8_t>(), bytes.data(), *encoded);
    archive.write(kTrainingSupervisionKey, tensor);
}
TrainingSupervisionConfig read_training_supervision_config(torch_api::InputArchive& archive) {
    torch_api::Tensor tensor;
    if (!archive.try_read(kTrainingSupervisionKey, tensor)) {
        torch_api::IValue value;
        if (archive.try_read(kTrainingSupervisionKey, value)) throw std::runtime_error("invalid RF-DETR training supervision CBOR blob");
        return {};
    }
    if (!tensor.defined() || !tensor.device().is_cpu() || tensor.scalar_type() != torch_api::kUInt8 || tensor.dim() != 1 || tensor.numel() <= 0 ||
        static_cast<std::uint64_t>(tensor.numel()) > kTrainingSupervisionCborCapacity) {
        throw std::runtime_error("invalid RF-DETR training supervision CBOR blob");
    }
    tensor = tensor.contiguous();
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(tensor.data_ptr<std::uint8_t>()), static_cast<std::size_t>(tensor.numel()));
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
std::vector<torch_api::Tensor> read_ema_shadow_archive(torch_api::InputArchive& archive, std::span<const std::string> expected_names) {
    const auto count = require_int(archive, "entry_count");
    if (count < 0 || static_cast<std::uint64_t>(count) != expected_names.size())
        throw std::runtime_error("RF-DETR training checkpoint state count does not match the bounded active inventory");
    std::vector<torch_api::Tensor> shadow;
    shadow.reserve(expected_names.size());
    for (std::size_t index = 0; index < expected_names.size(); ++index) {
        torch_api::InputArchive entry;
        archive.read(archive_entry_name(index), entry);
        if (require_string(entry, "name") != expected_names[index])
            throw std::runtime_error("RF-DETR training checkpoint state names do not match the active inventory");
        shadow.push_back(require_tensor(entry, "tensor"));
    }
    return shadow;
}
}  // namespace mmltk::backend::models::rfdetr::detail
namespace mmltk::backend::models::rfdetr {
TrainingCheckpoint inspect_training_checkpoint(const std::filesystem::path& path) {
    namespace torch_api = mmltk::backend::ml::torch_api;
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
    const auto continuation = detail::read_training_continuation(archive);
    if (!continuation) {
        decoded.class_artifact->RequireUnchanged();
        return result;
    }
    const auto& request = continuation->configuration;
    torch_api::InputArchive optimizer;
    archive.read("optimizer", optimizer);
    std::unordered_map<std::string, torch_api::Tensor> tensors;
    tensors.reserve(state.entries.size());
    for (const auto& entry : state.entries) tensors.emplace(entry.name, entry.tensor);
    const auto names = request.optimizer == TrainOptimizerKind::AdamW ? NativeAdamW::InspectCheckpoint(optimizer, tensors)
                                                                      : NativeMuonWithAuxAdam::InspectCheckpoint(optimizer, tensors);
    if (request.use_ema) {
        torch_api::InputArchive ema;
        archive.read("ema_state", ema);
        const auto shadow = detail::read_ema_shadow_archive(ema, names);
        std::vector<torch_api::Tensor> parameters;
        parameters.reserve(names.size());
        for (const auto& name : names) parameters.push_back(tensors.at(name));
        ModelEma::validate_cpu_shadow(parameters, shadow);
    }
    decoded.class_artifact->RequireUnchanged();
    result.original_class_descriptor = continuation->values.training_original_descriptor;
    result.attempt_id = continuation->values.training_attempt_id;
    result.epoch = static_cast<int>(continuation->values.epoch);
    result.configuration = request;
    result.evaluated_weights = request.use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary;
    result.resumable = true;
    return result;
}
}  // namespace mmltk::backend::models::rfdetr
