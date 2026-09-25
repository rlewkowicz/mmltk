#include "checkpoint.h"
#include "detail/checkpoint_private.h"
#include "detail/training_session_checkpoint.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include <stdexcept>
#include <utility>
namespace mmltk::backend::models::rfdetr {
TrainingCheckpointAdmission::TrainingCheckpointAdmission(TrainingCheckpoint checkpoint, std::shared_ptr<const ClassArtifactAdmission> evidence)
    : checkpoint_(std::move(checkpoint)), evidence_(std::move(evidence)) {
 if (!std::get<0>(evidence_)) throw std::invalid_argument("missing checkpoint artifact evidence");
}
TrainingCheckpointAdmission::TrainingCheckpointAdmission(TrainingCheckpoint checkpoint, mmltk::common::io::FileSnapshot evidence) : checkpoint_(std::move(checkpoint)), evidence_(evidence) {}
TrainingCheckpointAdmission::TrainingCheckpointAdmission(TrainingCheckpoint checkpoint, std::shared_ptr<const TrainingSessionAdmission> evidence) : checkpoint_(std::move(checkpoint)), evidence_(std::move(evidence)) {}
void TrainingCheckpointAdmission::RequireUnchanged(std::stop_token stop) const {
 if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
 if (const auto* native = std::get_if<0>(&evidence_)) {
  (*native)->RequireUnchanged(stop);
  // Restoration uses the canonical path; also retain its identity when
  // the selected artifact was reached through a symlink or another name.
  (*native)->file()->snapshot.RequireUnchanged(checkpoint_.path);
 } else if (const auto* file = std::get_if<1>(&evidence_)) {
  file->RequireUnchanged(checkpoint_.path);
 } else {
  std::get<2>(evidence_)->require_unchanged();
 }
 if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
}
TrainingCheckpointAdmission inspect_training_checkpoint(const std::filesystem::path& path, std::stop_token stop) {
 if (is_training_session_manifest(path)) {
  auto admission = std::make_shared<TrainingSessionAdmission>(path, stop);
  TrainingCheckpoint result;
  result.path = std::filesystem::canonical(path);
  result.session_id = admission->manifest().session_id;
  result.attempt_id = admission->manifest().attempt_id;
  result.configuration = admission->manifest().request;
  result.class_layout = admission->model(0).metadata.class_layout;
  result.original_weights = admission->model(0).metadata.source_path;
  result.original_class_descriptor = admission->continuation(0).values.training_original_descriptor;
  result.epoch = admission->continuation(0).values.epoch;
  result.evaluated_weights = result.configuration->use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary;
  result.resumable = true;
  admission->require_unchanged();
  if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
  admission->release_decoded_state();
  return {std::move(result), std::move(admission)};
 }
 if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
 const auto identity = mmltk::common::io::FileSnapshot::Read(path);
 const auto container = identify_model_state_container(path);
 if (container == ModelStateContainer::Unknown) throw std::invalid_argument("unsupported training weights container");
 if (container == ModelStateContainer::Python) {
  // External import remains a container capability; ModelSystem owns transfer admission.
  TrainingCheckpoint result;
  result.path = std::filesystem::canonical(path);
  identity.RequireUnchanged(path);
  if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
  return {std::move(result), identity};
 }
 auto decoded = decode_native_model_state(path, stop);
 auto inspected = detail::inspect_training_model_checkpoint(decoded, path, stop);
 // A native deployment/individual archive supports Transfer; optimizer Resume
 // requires the complete immutable session set even when it has only one model.
 auto result = std::move(inspected);
 result.resumable = false;
 result.configuration.reset();
 decoded.class_artifact->RequireUnchanged(stop);
 identity.RequireUnchanged(path);
 return {std::move(result), decoded.class_artifact};
}
}  // namespace mmltk::backend::models::rfdetr
