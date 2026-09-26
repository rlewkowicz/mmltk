#include "detail/training_session_checkpoint.h"
#include "detail/training_ops_private.h"
#include "detail/checkpoint_private.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/backend/models/rfdetr/contract/training_metrics.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>
#include <stdexcept>
#include <system_error>
#include <cerrno>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
namespace mmltk::backend::models::rfdetr {
namespace {
namespace io = mmltk::common::io;
namespace serial = mmltk::frameworks::serialization;
io::ScopedFd lock(const std::filesystem::path& path, int operation) {
 io::ScopedFd file(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
 if (file.get() < 0 || ::flock(file.get(), operation) != 0) throw std::runtime_error("training generation lease is unavailable");
 return file;
}
void cancel(std::stop_token stop) {
 if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
}
detail::TrainingContinuation admit_model(
 const DecodedNativeModelState& model, const std::filesystem::path& file, const TrainingSessionManifest& manifest, const TrainingPlanState& plan, std::size_t index, std::stop_token stop = {}) {
 const auto& entry = manifest.models[index];
 if (index >= plan.shards.size() || entry.model_id != plan.shards[index].model_id || !model.admitted_archive()) throw std::runtime_error("missing or mixed training generation");
 auto continuation = detail::read_training_continuation(*model.admitted_archive());
 if (!continuation || continuation->values.training_attempt_id != manifest.attempt_id || continuation->values.data.model_id != entry.model_id ||
     continuation->values.data.plan_hash != plan.plan_hash || continuation->values.data.epoch != manifest.epoch)
  throw std::runtime_error("session continuation identity differs");
 const auto& values = continuation->values;
 if (static_cast<std::uint64_t>(values.grad_scaler_growth_tracker) > values.schedule.consumed_attempts || values.grad_scaler_growth_tracker >= GradScaler::kGrowthInterval ||
     (manifest.precision != TrainingPrecisionKind::Float16 && (values.grad_scaler_scale != GradScaler::kInitialScale || values.grad_scaler_growth_tracker != 0)))
  throw std::runtime_error("session gradient scaler state differs from admitted precision or age");
 if (manifest.request.use_ema && static_cast<std::uint64_t>(values.ema_completed_updates) != values.schedule.consumed_attempts)
  throw std::runtime_error("session EMA age differs from model attempt count");
 auto expected = manifest.request;
 if (expected.lane_configuration.mode != TrainLaneMode::SharedGradients) {
  const auto recipe = std::ranges::find(expected.lane_configuration.models, entry.model_id, &TrainModelSettings::model_id);
  if (recipe == expected.lane_configuration.models.end()) throw std::runtime_error("session model lacks an admitted recipe");
  expected.recipe = recipe->recipe;
 }
 detail::require_active_training_continuation(*continuation, expected);
 (void)detail::inspect_training_model_checkpoint(model, file, stop);
 return std::move(*continuation);
}
std::vector<std::shared_ptr<const TrainingArtifactAdmission>> admit_best_candidates(const TrainingSessionManifest& manifest, const NativeCheckpointMetadata& semantics,
 std::span<const std::shared_ptr<const TrainingArtifactAdmission>> retained = {}, std::span<const std::shared_ptr<const TrainingArtifactAdmission>> supplied = {}, std::stop_token stop = {}) {
 if (manifest.models.size() > kMaximumTrainingModels || supplied.size() > kMaximumTrainingModels) throw std::invalid_argument("session candidate capacity exceeded");
 std::vector<std::shared_ptr<const TrainingArtifactAdmission>> result;
 result.reserve(manifest.models.size());
 std::unordered_map<std::filesystem::path, const TrainingArtifact*> logical;
 for (const auto& model : manifest.models) {
  cancel(stop);
  if (!model.best) continue;
  const auto& best = *model.best;
  if (best.weights != (manifest.request.use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary)) throw std::runtime_error("session candidate weight kind differs");
  const auto path = std::filesystem::absolute(best.path).lexically_normal();
  const auto [found, inserted] = logical.emplace(path, &best);
  if (!inserted) {
   auto identified = best;
   identified.model_id = found->second->model_id;
   identified.attempt = found->second->attempt;
   identified.path = found->second->path;
   if (identified != *found->second) throw std::runtime_error("shared session candidate identities differ");
   continue;
  }
  const auto matches_path = [&](const auto& admission) { return admission && admission->evidence()->artifact_path() == path; };
  std::shared_ptr<const TrainingArtifactAdmission> admission;
  const auto previous = std::ranges::find_if(retained, matches_path);
  const auto provided = std::ranges::find_if(supplied, matches_path);
  if (previous != retained.end())
   admission = *previous;
  else if (provided != supplied.end())
   admission = *provided;
  else {
   auto fresh = std::make_shared<TrainingArtifactAdmission>(best, stop);
   fresh->release_decoded_state();
   admission = std::move(fresh);
  }
  admission->require_matches(best, stop);
  if (!same_native_model_semantics(admission->metadata(), semantics)) throw std::runtime_error("session candidate semantics differ");
  result.push_back(std::move(admission));
 }
 return result;
}
}  // namespace
bool is_training_session_manifest(const std::filesystem::path& path) { return path.extension() == ".json"; }
struct TrainingSessionAdmission::Lease final {
 explicit Lease(const std::filesystem::path& path) : selected(path), snapshot(io::FileSnapshot::Read(path)) {
  const auto directory = std::filesystem::canonical(path).parent_path();
  for (unsigned slot = 0; slot < 16; ++slot) {
   io::ScopedFd candidate(::open((directory / (".session.reader-" + std::to_string(slot))).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
   if (candidate.get() < 0) throw std::runtime_error("training session reader lease is unavailable");
   if (::flock(candidate.get(), LOCK_EX | LOCK_NB) == 0) {
    reader = std::move(candidate);
    break;
   }
   if (errno != EWOULDBLOCK && errno != EAGAIN) throw std::runtime_error("training session reader lease failed");
  }
  if (reader.get() < 0) throw std::runtime_error("training session reader capacity exhausted");
 }
 std::filesystem::path selected;
 io::FileSnapshot snapshot;
 io::ScopedFd reader, descriptor;
 std::vector<std::pair<std::filesystem::path, io::FileSnapshot>> files;
 std::vector<std::shared_ptr<const ClassArtifactAdmission>> evidence;
 std::vector<std::shared_ptr<const TrainingArtifactAdmission>> candidates;
};
TrainingSessionAdmission::TrainingSessionAdmission(const std::filesystem::path& path, std::stop_token stop) : lease_(std::make_unique<Lease>(path)) {
 cancel(stop);
 const auto root = std::filesystem::canonical(path).parent_path();
 const auto admission = lock(root / ".session.lock", LOCK_SH);
 manifest_ = read_training_manifest(path);
 const auto generation = root / "generations" / manifest_.generation;
 lease_->descriptor = lock(generation / ".lease", LOCK_SH);
 const auto plan_path = generation / "plan.cbor";
 lease_->files.emplace_back(plan_path, io::FileSnapshot::Read(plan_path));
 if (io::sha256_hex(io::sha256_file(plan_path)) != manifest_.plan_sha256) throw std::runtime_error("session data plan checksum differs");
 const auto plan_file = io::MappedFile::open_readonly(plan_path.string());
 constexpr auto plan_bound = serial::reflected_maximum_cbor_bytes<TrainingPlanState>();
 if (plan_file.size() > plan_bound) throw std::runtime_error("session data plan exceeds schema");
 auto decoded = serial::decode<TrainingPlanState>(
  {std::span(reinterpret_cast<const std::byte*>(plan_file.data()), plan_file.size()), {}}, {.max_bytes = plan_bound, .max_items = std::numeric_limits<std::uint64_t>::max(), .max_depth = 32});
 if (!decoded || !decoded->plan_hash || decoded->shards.size() != manifest_.models.size()) throw std::runtime_error("invalid session data plan");
 plan_ = std::make_shared<const TrainingPlanState>(std::move(*decoded));
 for (std::size_t index = 0; index < manifest_.models.size(); ++index) {
  cancel(stop);
  const auto& entry = manifest_.models[index];
  const auto file = root / entry.path;
  auto model = decode_native_model_state(file, stop);
  if (entry.model_id != plan_->shards[index].model_id || io::sha256_hex(model.class_artifact->file()->sha256) != entry.sha256) throw std::runtime_error("missing or mixed training generation");
  auto continuation = admit_model(model, file, manifest_, *plan_, index, stop);
  if (index && !same_native_model_semantics(model.metadata, models_.front().metadata)) throw std::runtime_error("session model semantics differ");
  lease_->evidence.push_back(model.class_artifact);
  models_.push_back(std::move(model));
  continuations_.push_back(std::move(continuation));
 }
 lease_->candidates = admit_best_candidates(manifest_, models_.front().metadata, {}, {}, stop);
 require_unchanged();
}
TrainingSessionAdmission::~TrainingSessionAdmission() = default;
TrainingSessionAdmission::TrainingSessionAdmission(TrainingSessionAdmission&&) noexcept = default;
TrainingSessionAdmission& TrainingSessionAdmission::operator=(TrainingSessionAdmission&& other) noexcept {
 if (this == &other) return *this;
 release_decoded_state();
 lease_ = std::move(other.lease_);
 manifest_ = std::move(other.manifest_);
 plan_ = std::move(other.plan_);
 models_ = std::move(other.models_);
 continuations_ = std::move(other.continuations_);
 return *this;
}
void TrainingSessionAdmission::require_unchanged() const {
 lease_->snapshot.RequireUnchanged(lease_->selected);
 for (const auto& [path, snapshot] : lease_->files) snapshot.RequireUnchanged(path);
 for (const auto& evidence : lease_->evidence) evidence->RequireUnchanged();
 for (const auto& candidate : lease_->candidates) candidate->require_unchanged();
}
void TrainingSessionAdmission::release_decoded_state() {
 models_.clear();
 continuations_.clear();
 plan_.reset();
}
const TrainingPlanState& TrainingSessionAdmission::plan() const {
 if (!plan_) throw std::logic_error("session decoded plan has been released");
 return *plan_;
}
std::shared_ptr<const TrainingArtifactAdmission> TrainingSessionAdmission::best_admission(std::size_t index) const {
 const auto& best = manifest_.models.at(index).best;
 if (!best) return {};
 const auto path = std::filesystem::absolute(best->path).lexically_normal();
 const auto found = std::ranges::find_if(lease_->candidates, [&](const auto& admission) { return admission->evidence()->artifact_path() == path; });
 if (found == lease_->candidates.end()) throw std::logic_error("session candidate lacks retained evidence");
 return *found;
}
TrainingSessionCheckpoint::TrainingSessionCheckpoint(std::filesystem::path directory) : directory_(std::filesystem::absolute(directory).lexically_normal()), path_(directory_ / "session.json") {
 std::filesystem::create_directories(directory_ / "generations");
}
void TrainingSessionCheckpoint::publish(TrainingSessionManifest& value, const TrainingPlanState& plan, std::function_ref<void(const std::filesystem::path&, std::size_t)> serialize,
 std::span<const std::shared_ptr<const TrainingArtifactAdmission>> candidates, std::function_ref<void(TrainingPublicationStep)> observe) {
 if (value.models.empty() || value.models.size() > kMaximumTrainingModels) throw std::invalid_argument("invalid session model count");
 const auto writer = lock(directory_ / ".session.writer", LOCK_EX | LOCK_NB);
 TrainingSessionManifest next = value;
 TrainingSessionManifest current;
 if (std::filesystem::exists(path_)) {
  current = read_training_manifest(path_);
  next.previous_generation = current.generation;
 }
 {
  const auto pointer_lock = lock(directory_ / ".session.lock", LOCK_EX);
  retire(current);
 }
 next.generation = training_artifact_identity();
 const auto staging = directory_ / "generations" / next.generation;
 if (!std::filesystem::create_directory(staging)) throw std::runtime_error("training generation identity already exists");
 bool committed = false;
 try {
  auto staged_lease = lock(staging / ".lease", LOCK_EX);
  observe(TrainingPublicationStep::Created);
  serial::wire::ByteBuffer bytes;
  constexpr auto bound = serial::reflected_maximum_cbor_bytes<TrainingPlanState>();
  if (!serial::encode(plan, bytes, {.max_bytes = bound, .max_items = std::numeric_limits<std::uint64_t>::max(), .max_depth = 32})) throw std::runtime_error("invalid immutable training plan");
  auto plan_file = io::FileHandle::create_output((staging / "plan.cbor").string(), bytes.size());
  plan_file.pwrite_all(bytes.data(), bytes.size(), 0);
  plan_file.sync_data();
  const auto plan_identity = io::FileSnapshot::Read(staging / "plan.cbor");
  next.plan_sha256 = io::sha256_hex(io::sha256_file(staging / "plan.cbor"));
  observe(TrainingPublicationStep::PlanWritten);
  if (!plan.plan_hash || plan.shards.size() != next.models.size()) throw std::runtime_error("invalid session data plan");
  std::optional<NativeCheckpointMetadata> semantics;
  std::vector<std::shared_ptr<const ClassArtifactAdmission>> model_evidence;
  model_evidence.reserve(next.models.size());
  for (std::size_t index = 0; index < next.models.size(); ++index) {
   auto& model = next.models[index];
   model.path = std::filesystem::path("generations") / next.generation / ("model-" + std::to_string(model.model_id) + ".pt");
   const auto path = directory_ / model.path;
   serialize(path, index);
   observe(TrainingPublicationStep::ModelWritten);
   auto decoded = decode_native_model_state(path);
   (void)admit_model(decoded, path, next, plan, index);
   if (semantics && !same_native_model_semantics(decoded.metadata, *semantics)) throw std::runtime_error("session model semantics differ");
   if (!semantics) semantics = decoded.metadata;
   model.sha256 = io::sha256_hex(decoded.class_artifact->file()->sha256);
   io::FileHandle::open_readonly(path.string()).sync_data();
   decoded.class_artifact->RequireUnchanged();
   model_evidence.push_back(decoded.class_artifact);
   observe(TrainingPublicationStep::ModelValidated);
  }
  observe(TrainingPublicationStep::Serialized);
  validate_training_session_manifest(next);
  auto admitted_candidates = admit_best_candidates(next, *semantics, candidates_, candidates);
  observe(TrainingPublicationStep::Validated);
  write_training_manifest(staging / "manifest.json", next);
  io::sync_parent_directory(staging / "manifest.json");
  io::sync_parent_directory(staging);
  observe(TrainingPublicationStep::Synced);
  const auto pointer_lock = lock(directory_ / ".session.lock", LOCK_EX);
  plan_identity.RequireUnchanged(staging / "plan.cbor");
  for (const auto& evidence : model_evidence) evidence->RequireUnchanged();
  for (const auto& evidence : admitted_candidates) evidence->require_unchanged();
  write_training_manifest(path_, next);
  committed = true;
  candidates_ = std::move(admitted_candidates);
  value = next;
  observe(TrainingPublicationStep::Published);
  staged_lease.reset();
  retire(next);
  observe(TrainingPublicationStep::Retained);
 } catch (...) {
  if (!committed) {
   std::error_code ignored;
   std::filesystem::remove_all(staging, ignored);
  } else {
   try {
    const auto pointer_lock = lock(directory_ / ".session.lock", LOCK_EX);
    retire(value);
   } catch (...) {}
  }
  throw;
 }
}
void TrainingSessionCheckpoint::retire(const TrainingSessionManifest& current) {
 for (const auto& entry : std::filesystem::directory_iterator(directory_ / "generations")) {
  if (!entry.is_directory() || entry.path().filename() == current.generation || entry.path().filename() == current.previous_generation) continue;
  io::ScopedFd lease(::open((entry.path() / ".lease").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
  if (lease.get() < 0 || ::flock(lease.get(), LOCK_EX | LOCK_NB) != 0) continue;
  std::filesystem::remove_all(entry.path());
 }
}
}  // namespace mmltk::backend::models::rfdetr
