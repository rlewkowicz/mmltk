#pragma once
#include <filesystem>
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>
#include "training_continuation.h"
#include "training_data_state.h"
#include "training_artifact.h"
#include <span>
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/contract/training_artifacts.h"
namespace mmltk::backend::models::rfdetr {
[[nodiscard]] bool is_training_session_manifest(const std::filesystem::path&);
enum class TrainingPublicationStep { Created, PlanWritten, ModelWritten, ModelValidated, Serialized, Validated, Synced, Published, Retained };
// The bounded admission retains a physical directory lease through all archive
// readers. Publication retires only generations for which it obtains exclusivity.
class TrainingSessionAdmission final {
public:
 explicit TrainingSessionAdmission(const std::filesystem::path&, std::stop_token = {});
 ~TrainingSessionAdmission();
 TrainingSessionAdmission(TrainingSessionAdmission&&) noexcept;
 TrainingSessionAdmission& operator=(TrainingSessionAdmission&&) noexcept;
 TrainingSessionAdmission(const TrainingSessionAdmission&) = delete;
 TrainingSessionAdmission& operator=(const TrainingSessionAdmission&) = delete;
 [[nodiscard]] const TrainingSessionManifest& manifest() const noexcept { return manifest_; }
 [[nodiscard]] const TrainingPlanState& plan() const;
 [[nodiscard]] std::size_t model_count() const noexcept { return manifest_.models.size(); }
 [[nodiscard]] const DecodedNativeModelState& model(std::size_t index) const { return models_.at(index); }
 [[nodiscard]] const detail::TrainingContinuation& continuation(std::size_t index) const { return continuations_.at(index); }
 [[nodiscard]] std::shared_ptr<const TrainingArtifactAdmission> best_admission(std::size_t index) const;
 void require_unchanged() const;
 // Inspection retains exact file evidence and its lease, without tensor/plan allocations.
 void release_decoded_state();

private:
 struct Lease;
 // Declared first so all decoded readers retire before the generation lease.
 std::unique_ptr<Lease> lease_;
 TrainingSessionManifest manifest_;
 std::shared_ptr<const TrainingPlanState> plan_;
 std::vector<DecodedNativeModelState> models_;
 std::vector<detail::TrainingContinuation> continuations_;
};
class TrainingSessionCheckpoint final {
public:
 explicit TrainingSessionCheckpoint(std::filesystem::path directory);
 [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
 // All serialize callbacks run synchronously at the session's drained boundary.
 void publish(
  TrainingSessionManifest&, const TrainingPlanState&, std::function_ref<void(const std::filesystem::path&, std::size_t)> serialize,
  std::span<const std::shared_ptr<const TrainingArtifactAdmission>> candidates = {}, std::function_ref<void(TrainingPublicationStep)> observe = [](TrainingPublicationStep) {});

private:
 std::filesystem::path directory_, path_;
 std::vector<std::shared_ptr<const TrainingArtifactAdmission>> candidates_;
 void retire(const TrainingSessionManifest&);
};
}  // namespace mmltk::backend::models::rfdetr
