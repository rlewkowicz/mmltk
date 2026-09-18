
#include "src/backend/models/rfdetr/core/class_layout.h"
#include <array>
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include "src/backend/ml/torch/archive.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "detail/checkpoint_private.h"
#include "checkpoint.h"
#include "detail/native_optimizer_private.h"
#include "detail/training_continuation.h"
#include "detail/model_ema.h"
#include <unordered_map>
#include <type_traits>
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr::detail {
void publish_native_checkpoint_archive(torch::serialize::OutputArchive& archive, const std::filesystem::path& destination,
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
void write_state_archive_impl(torch::serialize::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                              const bool include_training_supervision, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_checkpoint_save_write_state_archive{"rfdetr.checkpoint.save.write_state_archive"};
    torch::serialize::OutputArchive state_archive;
    std::size_t entry_count = 0;
    for (const auto& entry : entries) {
        if (include_training_supervision || !entry.name.starts_with(kTrainingSupervisionPrefix)) { ++entry_count; }
    }
    mmltk::backend::ml::serialization::write_int(state_archive, "entry_count", static_cast<int64_t>(entry_count));
    std::size_t output_index = 0;
    for (const auto& entry : entries) {
        const auto slot = first_slot++;
        if (!include_training_supervision && entry.name.starts_with(kTrainingSupervisionPrefix)) { continue; }
        torch::serialize::OutputArchive entry_archive;
        mmltk::backend::ml::serialization::write_string(entry_archive, "name", entry.name);
        entry_archive.write("tensor", readback.Stage(slot));
        state_archive.write(mmltk::backend::ml::serialization::archive_entry_name(output_index++), entry_archive);
    }
    archive.write(key, state_archive);
}
}  // namespace
void write_native_checkpoint_metadata(torch::serialize::OutputArchive& archive, const NativeCheckpointMetadata& metadata) {
    if (metadata.num_queries <= 0 || metadata.num_select <= 0 || metadata.num_select > metadata.num_queries) {
        throw std::runtime_error("RF-DETR native checkpoint requires positive num_queries and num_select <= num_queries");
    }
    const ResolvedClassLayout layout(metadata.class_layout);
    if (layout.output_width() != static_cast<std::size_t>(metadata.num_classes))
        throw std::runtime_error("native checkpoint class layout disagrees with tensor output width");
    mmltk::backend::ml::serialization::write_string(archive, "class_layout", encode_class_layout(metadata.class_layout));
    mmltk::backend::ml::serialization::write_string(archive, "format", kNativeCheckpointFormat);
    mmltk::backend::ml::serialization::write_int(archive, "format_version", kNativeCheckpointFormatVersion);
    mmltk::backend::ml::serialization::write_string(archive, "preset_name", metadata.preset_name);
    mmltk::backend::ml::serialization::write_string(archive, "source_kind", metadata.source_kind);
    mmltk::backend::ml::serialization::write_string(archive, "source_path", metadata.source_path);
    mmltk::backend::ml::serialization::write_int(archive, "num_classes", metadata.num_classes);
    mmltk::backend::ml::serialization::write_int(archive, "num_queries", metadata.num_queries);
    mmltk::backend::ml::serialization::write_int(archive, "num_select", metadata.num_select);
    metadata.for_each_detection_field([&]<class Optional>(const char* key, const Optional& value) {
        if constexpr (std::is_same_v<typename Optional::value_type, bool>)
            mmltk::backend::ml::serialization::write_optional_bool(archive, key, value);
        else if constexpr (std::is_same_v<typename Optional::value_type, int64_t>)
            mmltk::backend::ml::serialization::write_optional_int(archive, key, value);
        else
            mmltk::backend::ml::serialization::write_optional_double(archive, key, value);
    });
}
void reserve_state_archive(const std::vector<NormalizedModelStateEntry>& entries, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback,
                           std::size_t first_slot) {
    std::vector<torch::Tensor> sources;
    sources.reserve(entries.size());
    for (const auto& entry : entries) sources.push_back(entry.tensor);
    readback.Reserve(sources, first_slot);
}
void write_state_archive(torch::serialize::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                         mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) {
    write_state_archive_impl(archive, key, entries, false, readback, first_slot);
}
void write_resume_state_archive(torch::serialize::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                                mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot) {
    write_state_archive_impl(archive, key, entries, true, readback, first_slot);
}
void write_training_supervision_config(torch::serialize::OutputArchive& archive, const TrainingSupervisionConfig& config) {
    if (config == TrainingSupervisionConfig{}) { return; }
    std::array<std::byte, kTrainingSupervisionCborCapacity> bytes{};
    const auto encoded = serialization::encode(config, std::span<std::byte>(bytes), kTrainingSupervisionCborLimits);
    if (!encoded.has_value()) { throw std::runtime_error("failed to encode bounded RF-DETR training supervision configuration"); }
    auto tensor = torch::empty({static_cast<int64_t>(*encoded)}, torch::TensorOptions().dtype(torch::kUInt8).device(torch::kCPU));
    std::memcpy(tensor.data_ptr<std::uint8_t>(), bytes.data(), *encoded);
    archive.write(kTrainingSupervisionKey, tensor);
}
TrainingSupervisionConfig read_training_supervision_config(torch::serialize::InputArchive& archive) {
    torch::Tensor tensor;
    if (!archive.try_read(kTrainingSupervisionKey, tensor)) {
        c10::IValue value;
        if (archive.try_read(kTrainingSupervisionKey, value)) throw std::runtime_error("invalid RF-DETR training supervision CBOR blob");
        return {};
    }
    if (!tensor.defined() || !tensor.device().is_cpu() || tensor.scalar_type() != torch::kUInt8 || tensor.dim() != 1 || tensor.numel() <= 0 ||
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
void require_resume_training_supervision_config(torch::serialize::InputArchive& archive, const TrainingSupervisionConfig& expected) {
    if (read_training_supervision_config(archive) != expected) {
        throw std::runtime_error("native RF-DETR resume checkpoint training supervision configuration does not match");
    }
}
std::vector<torch::Tensor> read_ema_shadow_archive(torch::serialize::InputArchive& archive, std::span<const std::string> expected_names, std::stop_token stop) {
    const auto count = mmltk::backend::ml::serialization::require_int(archive, "entry_count");
    if (count < 0 || static_cast<std::uint64_t>(count) != expected_names.size())
        throw std::runtime_error("RF-DETR training checkpoint state count does not match the bounded active inventory");
    std::vector<torch::Tensor> shadow;
    shadow.reserve(expected_names.size());
    for (std::size_t index = 0; index < expected_names.size(); ++index) {
        if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
        torch::serialize::InputArchive entry;
        archive.read(mmltk::backend::ml::serialization::archive_entry_name(index), entry);
        if (mmltk::backend::ml::serialization::require_string(entry, "name") != expected_names[index])
            throw std::runtime_error("RF-DETR training checkpoint state names do not match the active inventory");
        shadow.push_back(mmltk::backend::ml::serialization::require_tensor(entry, "tensor"));
    }
    return shadow;
}
}  // namespace mmltk::backend::models::rfdetr::detail
namespace mmltk::backend::models::rfdetr {
TrainingCheckpoint inspect_training_checkpoint(const std::filesystem::path& path, std::stop_token stop) {
    if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
    const auto container = identify_model_state_container(path);
    if (container == ModelStateContainer::Unknown) throw std::invalid_argument("unsupported training weights container");
    if (container == ModelStateContainer::Python) {
        // Container capability only. ModelSystem still owns complete transfer admission.
        TrainingCheckpoint result;
        result.path = std::filesystem::canonical(path);
        return result;
    }
    auto decoded = decode_native_model_state(path, stop);
    if (!decoded.class_artifact) throw std::runtime_error("training checkpoint lacks exact native artifact admission");
    decoded.class_artifact->RequireUnchanged(stop);
    const auto& state = decoded;
    if (!state.admitted_archive()) throw std::runtime_error("training checkpoint lacks its native archive");
    auto& archive = *state.admitted_archive();
    TrainingCheckpoint result;
    result.path = std::filesystem::canonical(path);
    result.class_layout = decoded.metadata.class_layout;
    result.original_weights = decoded.metadata.source_path;
    const auto continuation = detail::read_training_continuation(archive);
    if (!continuation) {
        decoded.class_artifact->RequireUnchanged(stop);
        return result;
    }
    const auto& request = continuation->configuration;
    torch::serialize::InputArchive optimizer;
    archive.read("optimizer", optimizer);
    std::unordered_map<std::string, torch::Tensor> tensors;
    tensors.reserve(state.entries().size());
    for (const auto& entry : state.entries()) {
        if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
        tensors.emplace(entry.name, entry.tensor);
    }
    const auto names = request.optimizer == TrainOptimizerKind::AdamW ? NativeAdamW::InspectCheckpoint(optimizer, tensors, stop)
                                                                      : NativeMuonWithAuxAdam::InspectCheckpoint(optimizer, tensors, stop);
    if (request.use_ema) {
        torch::serialize::InputArchive ema;
        archive.read("ema_state", ema);
        const auto shadow = detail::read_ema_shadow_archive(ema, names, stop);
        std::vector<torch::Tensor> parameters;
        parameters.reserve(names.size());
        for (const auto& name : names) {
            if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
            parameters.push_back(tensors.at(name));
        }
        ModelEma::validate_cpu_shadow(parameters, shadow, stop);
    }
    decoded.class_artifact->RequireUnchanged(stop);
    result.original_class_descriptor = continuation->values.training_original_descriptor;
    result.attempt_id = continuation->values.training_attempt_id;
    result.epoch = static_cast<int>(continuation->values.epoch);
    result.configuration = request;
    result.evaluated_weights = request.use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary;
    result.resumable = true;
    return result;
}
}  // namespace mmltk::backend::models::rfdetr
