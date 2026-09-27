#include "detail/training_artifact.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/backend/models/rfdetr/contract/training_metrics.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/frameworks/serialization/reflected_json.h"
#include "src/pch_std.h"
namespace mmltk::backend::models::rfdetr {
namespace {
namespace io = mmltk::common::io;
namespace serial = mmltk::frameworks::serialization;
constexpr serial::wire::Limits manifest_limits{.max_bytes = kTrainingManifestBytes, .max_items = 65536, .max_depth = 32};
constexpr serial::wire::Limits selected_limits{.max_bytes = kTrainingSelectionBytes, .max_items = 4096, .max_depth = 32};
std::string read_file(const std::filesystem::path& path, std::size_t bound) {
 const auto file = io::FileHandle::open_readonly(path.string());
 if (file.size() > bound) throw std::runtime_error("training artifact exceeds byte limit");
 std::string value(file.size(), '\0');
 file.pread_all(value.data(), value.size(), 0);
 return value;
}
template <class T>
void write_json(const std::filesystem::path& path, const T& value, serial::wire::Limits limits) {
 std::vector<std::byte> scratch(limits.max_bytes);
 const auto text = serial::reflected_json(value, scratch, limits).dump();
 const auto staged = path.string() + ".staging";
 try {
  auto file = io::FileHandle::create_output(staged, text.size());
  file.pwrite_all(text.data(), text.size(), 0);
  file.sync_data();
  io::publish_staged_path_atomically(staged, path);
 } catch (...) {
  std::error_code ignored;
  std::filesystem::remove(staged, ignored);
  throw;
 }
}
void require_metric(const TrainingArtifact& artifact, const NativeCheckpointMetadata& metadata) {
 const auto* preset = find_preset_catalog_entry(metadata.preset_name);
 if (!artifact.selection_metric || !artifact.evaluation || !preset || training_selection_metric(*artifact.evaluation, preset->task == ModelTask::Segmentation) != *artifact.selection_metric)
  throw std::invalid_argument("native training artifact metric differs");
}
}  // namespace
std::string native_state_fingerprint(std::span<const NormalizedModelStateEntry> state) {
 io::Sha256Hasher hash;
 const auto bytes = [&](const void* data, std::size_t size) { hash.Update({static_cast<const std::uint8_t*>(data), size}); };
 for (const auto& entry : state) {
  const auto name_size = static_cast<std::uint64_t>(entry.name.size());
  bytes(&name_size, sizeof(name_size));
  bytes(entry.name.data(), entry.name.size());
  const auto dtype = entry.tensor.scalar_type();
  bytes(&dtype, sizeof(dtype));
  const auto dimensions = static_cast<std::uint64_t>(entry.tensor.dim());
  bytes(&dimensions, sizeof(dimensions));
  bytes(entry.tensor.sizes().data(), entry.tensor.dim() * sizeof(std::int64_t));
  const auto value = entry.tensor.detach().to(torch::kCPU).contiguous();
  bytes(value.const_data_ptr(), value.nbytes());
 }
 return io::sha256_hex(hash.Finish());
}
std::string training_artifact_identity() {
 std::random_device random;
 return std::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
}
TrainingArtifactAdmission::TrainingArtifactAdmission(const std::filesystem::path& path, std::stop_token stop) {
 auto state = std::make_shared<const DecodedNativeModelState>(decode_native_model_state(path, stop));
 if (state->entries().empty()) throw std::invalid_argument("native training artifact has no values");
 for (const auto& entry : state->entries()) {
  if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
  if (entry.tensor.is_floating_point() && !torch::isfinite(entry.tensor).all().item<bool>()) throw std::invalid_argument("native training artifact contains nonfinite values");
 }
 metadata_ = state->metadata;
 evidence_ = state->class_artifact;
 sha256_ = io::sha256_hex(evidence_->file()->sha256);
 content_ = native_state_fingerprint(state->entries());
 require_unchanged(stop);
 decoded_ = std::move(state);
}
TrainingArtifactAdmission::TrainingArtifactAdmission(const TrainingArtifact& artifact, std::stop_token stop) : TrainingArtifactAdmission(artifact.path, stop) { require_matches(artifact, stop); }
TrainingArtifact TrainingArtifactAdmission::describe(TrainingArtifact artifact) const {
 artifact.sha256 = sha256_;
 artifact.content = content_;
 require_matches(artifact);
 return artifact;
}
void TrainingArtifactAdmission::require_matches(const TrainingArtifact& artifact, std::stop_token stop) const {
 if (std::filesystem::absolute(artifact.path).lexically_normal() != evidence_->artifact_path() || artifact.sha256 != sha256_ || artifact.content != content_)
  throw std::invalid_argument("native training artifact identity differs");
 require_metric(artifact, metadata_);
 require_unchanged(stop);
}
void TrainingArtifactAdmission::require_unchanged(std::stop_token stop) const { evidence_->RequireUnchanged(stop); }
std::shared_ptr<const DecodedNativeModelState> TrainingArtifactAdmission::decode(std::stop_token stop) const {
 require_unchanged(stop);
 if (decoded_) return decoded_;
 // The retained proof supplies the existing digest; decoding is needed only
 // when a later consumer actually needs values after their release.
 return std::make_shared<const DecodedNativeModelState>(decode_model_state(evidence_->artifact_path(), evidence_, {}, stop));
}
void publish_training_selection(const std::filesystem::path& path, const TrainingSelection& selected, const TrainingArtifactAdmission& admission) {
 validate_training_selection(selected);
 admission.require_matches(selected.artifact);
 write_json(path, selected, selected_limits);
}
TrainingSelection read_training_selection(const std::filesystem::path& path) {
 const auto identity = io::FileSnapshot::Read(path);
 auto selected = serial::decode_reflected_json<TrainingSelection>(read_file(path, kTrainingSelectionBytes), selected_limits);
 validate_training_selection(selected);
 TrainingArtifactAdmission admission(selected.artifact);
 identity.RequireUnchanged(path);
 return selected;
}
void write_training_manifest(const std::filesystem::path& path, const TrainingSessionManifest& value) { write_json(path, value, manifest_limits); }
TrainingSessionManifest read_training_manifest(const std::filesystem::path& path) {
 auto value = serial::decode_reflected_json<TrainingSessionManifest>(read_file(path, kTrainingManifestBytes), manifest_limits);
 validate_training_session_manifest(value);
 return value;
}
}  // namespace mmltk::backend::models::rfdetr
