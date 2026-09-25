#pragma once
#include <filesystem>
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>
#include "training_continuation.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/contract/training_artifacts.h"
namespace mmltk::backend::models::rfdetr {
[[nodiscard]] std::string training_artifact_identity();
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
 TrainingSessionManifest manifest;
 std::shared_ptr<const TrainingPlanState> plan;
 std::vector<DecodedNativeModelState> models;
 std::vector<detail::TrainingContinuation> continuations;
 void require_unchanged() const;
 // Inspection retains exact file evidence and its lease, without tensor/plan allocations.
 void release_decoded_state();
private:
 struct Lease;
 std::unique_ptr<Lease> lease_;
};
class TrainingSessionCheckpoint final {
public:
 explicit TrainingSessionCheckpoint(std::filesystem::path directory);
 [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
 // All serialize callbacks run synchronously at the session's drained boundary.
 void publish(TrainingSessionManifest&, const TrainingPlanState&,
  std::function_ref<void(const std::filesystem::path&, std::size_t)> serialize,
  std::function_ref<void(TrainingPublicationStep)> observe = [](TrainingPublicationStep) {});
private:
 std::filesystem::path directory_, path_;
 void retire(const TrainingSessionManifest&);
};
void publish_training_selection(const std::filesystem::path&, const TrainingSelection&);
[[nodiscard]] TrainingSelection read_training_selection(const std::filesystem::path&);
}  // namespace mmltk::backend::models::rfdetr
