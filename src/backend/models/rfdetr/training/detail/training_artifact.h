#pragma once
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include "src/backend/models/rfdetr/contract/training_artifacts.h"
#include "src/backend/models/rfdetr/core/model_state.h"
namespace mmltk::backend::models::rfdetr {
// Exact named-file and companion evidence survives release of the heavy archive.
// Logical model identities remain with the candidate, even when files share values.
class TrainingArtifactAdmission final {
public:
 explicit TrainingArtifactAdmission(const std::filesystem::path&, std::stop_token = {});
 explicit TrainingArtifactAdmission(const TrainingArtifact&, std::stop_token = {});
 [[nodiscard]] const NativeCheckpointMetadata& metadata() const noexcept { return metadata_; }
 [[nodiscard]] const std::shared_ptr<const ClassArtifactAdmission>& evidence() const noexcept { return evidence_; }
 [[nodiscard]] const std::string& sha256() const noexcept { return sha256_; }
 [[nodiscard]] const std::string& content() const noexcept { return content_; }
 [[nodiscard]] TrainingArtifact describe(TrainingArtifact) const;
 void require_matches(const TrainingArtifact&, std::stop_token = {}) const;
 void require_unchanged(std::stop_token = {}) const;
 // A caller retains this shared read through all tensor/archive consumers.
 [[nodiscard]] std::shared_ptr<const DecodedNativeModelState> decode(std::stop_token = {}) const;
 void release_decoded_state() noexcept { decoded_.reset(); }
private:
 NativeCheckpointMetadata metadata_;
 std::shared_ptr<const ClassArtifactAdmission> evidence_;
 std::string sha256_, content_;
 std::shared_ptr<const DecodedNativeModelState> decoded_;
};
struct TrainingArtifactCandidate final {
 TrainingArtifact artifact;
 std::shared_ptr<const TrainingArtifactAdmission> admission;
};
[[nodiscard]] std::string training_artifact_identity();
[[nodiscard]] std::string native_state_fingerprint(std::span<const NormalizedModelStateEntry>);
void publish_training_selection(const std::filesystem::path&, const TrainingSelection&, const TrainingArtifactAdmission&);
[[nodiscard]] TrainingSelection read_training_selection(const std::filesystem::path&);
// Bounded reflected descriptor I/O; generation locking and retirement stay with
// TrainingSessionCheckpoint, which publishes the current pointer last.
void write_training_manifest(const std::filesystem::path&, const TrainingSessionManifest&);
[[nodiscard]] TrainingSessionManifest read_training_manifest(const std::filesystem::path&);
}  // namespace mmltk::backend::models::rfdetr
