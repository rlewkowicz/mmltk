#include "detail/training_session_checkpoint.h"
#include "checkpoint.h"
#include "detail/model_merging.h"
#include "detail/training_ops_private.h"
#include "detail/checkpoint_private.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/backend/models/rfdetr/contract/training_metrics.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/frameworks/serialization/reflected_json.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <cerrno>
#include <format>
#include <random>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
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
  file.pwrite_all(text.data(), text.size(), 0); file.sync_data();
  io::publish_staged_path_atomically(staged, path);
 } catch (...) { std::error_code ignored; std::filesystem::remove(staged, ignored); throw; }
}
io::ScopedFd lock(const std::filesystem::path& path, int operation) {
 io::ScopedFd file(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
 if (file.get() < 0 || ::flock(file.get(), operation) != 0) throw std::runtime_error("training generation lease is unavailable");
 return file;
}
void cancel(std::stop_token stop) { if (stop.stop_requested()) throw ArtifactPublicationCancelled{}; }
detail::TrainingContinuation admit_model(DecodedNativeModelState& model, const std::filesystem::path& file,
 const TrainingSessionManifest& manifest, const TrainingPlanState& plan, std::size_t index, std::stop_token stop = {}) {
 const auto& entry = manifest.models[index];
 if (index >= plan.shards.size() || entry.model_id != plan.shards[index].model_id || !model.admitted_archive())
  throw std::runtime_error("missing or mixed training generation");
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
 detail::inspect_training_model_checkpoint(model, file, stop).RequireUnchanged(stop);
 return std::move(*continuation);
}
void admit_best_candidates(const TrainingSessionManifest& manifest, const NativeCheckpointMetadata& semantics,
 std::vector<std::pair<std::filesystem::path, io::FileSnapshot>>& files,
 std::vector<std::shared_ptr<const ClassArtifactAdmission>>& evidence, std::stop_token stop = {}) {
 std::unordered_map<std::filesystem::path, const TrainingArtifact*> admitted;
 for (const auto& model : manifest.models) {
  cancel(stop);
  if (!model.best) continue;
  const auto& best = *model.best;
  if (best.weights != (manifest.request.use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary)) throw std::runtime_error("session candidate weight kind differs");
  const auto [found, inserted] = admitted.emplace(best.path, &best);
  if (!inserted) {
   auto identified = best;
   identified.model_id = found->second->model_id; identified.attempt = found->second->attempt;
   if (identified != *found->second) throw std::runtime_error("shared session candidate identities differ");
   continue;
  }
  files.emplace_back(best.path, io::FileSnapshot::Read(best.path));
  auto candidate = admit_training_artifact(best);
  if (!same_native_model_semantics(candidate.metadata, semantics)) throw std::runtime_error("session candidate semantics differ");
  evidence.push_back(candidate.class_artifact);
 }
}
}
std::string training_artifact_identity() {
 std::random_device random;
 return std::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
}
bool is_training_session_manifest(const std::filesystem::path& path) { return path.extension() == ".json"; }
struct TrainingSessionAdmission::Lease final {
 explicit Lease(const std::filesystem::path& path) : selected(path), snapshot(io::FileSnapshot::Read(path)) {
  const auto directory = std::filesystem::canonical(path).parent_path();
  for (unsigned slot = 0; slot < 16; ++slot) {
   io::ScopedFd candidate(::open((directory / (".session.reader-" + std::to_string(slot))).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
   if (candidate.get() < 0) throw std::runtime_error("training session reader lease is unavailable");
   if (::flock(candidate.get(), LOCK_EX | LOCK_NB) == 0) { reader = std::move(candidate); break; }
   if (errno != EWOULDBLOCK && errno != EAGAIN) throw std::runtime_error("training session reader lease failed");
  }
  if (reader.get() < 0) throw std::runtime_error("training session reader capacity exhausted");
 }
 std::filesystem::path selected;
 io::FileSnapshot snapshot;
 io::ScopedFd reader, descriptor;
 std::vector<std::pair<std::filesystem::path, io::FileSnapshot>> files;
 std::vector<std::shared_ptr<const ClassArtifactAdmission>> evidence;
};
TrainingSessionAdmission::TrainingSessionAdmission(const std::filesystem::path& path, std::stop_token stop) : lease_(std::make_unique<Lease>(path)) {
 cancel(stop);
 const auto root = std::filesystem::canonical(path).parent_path();
 const auto admission = lock(root / ".session.lock", LOCK_SH);
 manifest = serial::decode_reflected_json<TrainingSessionManifest>(read_file(path, kTrainingManifestBytes), manifest_limits);
 validate_training_session_manifest(manifest);
 const auto generation = root / "generations" / manifest.generation;
 lease_->descriptor = lock(generation / ".lease", LOCK_SH);
 const auto plan_path = generation / "plan.cbor";
 lease_->files.emplace_back(plan_path, io::FileSnapshot::Read(plan_path));
 if (io::sha256_hex(io::sha256_file(plan_path)) != manifest.plan_sha256) throw std::runtime_error("session data plan checksum differs");
 const auto plan_file = io::MappedFile::open_readonly(plan_path.string());
 constexpr auto plan_bound = serial::reflected_maximum_cbor_bytes<TrainingPlanState>();
 if (plan_file.size() > plan_bound) throw std::runtime_error("session data plan exceeds schema");
 auto decoded = serial::decode<TrainingPlanState>({std::span(reinterpret_cast<const std::byte*>(plan_file.data()), plan_file.size()), {}}, {.max_bytes = plan_bound, .max_items = std::numeric_limits<std::uint64_t>::max(), .max_depth = 32});
 if (!decoded || !decoded->plan_hash || decoded->shards.size() != manifest.models.size()) throw std::runtime_error("invalid session data plan");
 plan = std::make_shared<const TrainingPlanState>(std::move(*decoded));
 for (std::size_t index = 0; index < manifest.models.size(); ++index) {
  cancel(stop);
  const auto& entry = manifest.models[index];
  const auto file = root / entry.path;
  lease_->files.emplace_back(file, io::FileSnapshot::Read(file));
  if (entry.model_id != plan->shards[index].model_id || io::sha256_hex(io::sha256_file(file)) != entry.sha256) throw std::runtime_error("missing or mixed training generation");
  auto model = decode_native_model_state(file, stop);
  auto continuation = admit_model(model, file, manifest, *plan, index, stop);
  if (index && !same_native_model_semantics(model.metadata, models.front().metadata)) throw std::runtime_error("session model semantics differ");
  lease_->evidence.push_back(model.class_artifact);
  models.push_back(std::move(model)); continuations.push_back(std::move(continuation));
 }
 admit_best_candidates(manifest, models.front().metadata, lease_->files, lease_->evidence, stop);
 require_unchanged();
}
TrainingSessionAdmission::~TrainingSessionAdmission() = default;
TrainingSessionAdmission::TrainingSessionAdmission(TrainingSessionAdmission&&) noexcept = default;
TrainingSessionAdmission& TrainingSessionAdmission::operator=(TrainingSessionAdmission&&) noexcept = default;
void TrainingSessionAdmission::require_unchanged() const {
 lease_->snapshot.RequireUnchanged(lease_->selected);
 for (const auto& [path, snapshot] : lease_->files) snapshot.RequireUnchanged(path);
 for (const auto& evidence : lease_->evidence) evidence->RequireUnchanged();
}
void TrainingSessionAdmission::release_decoded_state() {
 models.clear(); continuations.clear(); plan.reset();
}
TrainingSessionCheckpoint::TrainingSessionCheckpoint(std::filesystem::path directory)
 : directory_(std::filesystem::absolute(directory).lexically_normal()), path_(directory_ / "session.json") { std::filesystem::create_directories(directory_ / "generations"); }
void TrainingSessionCheckpoint::publish(TrainingSessionManifest& value, const TrainingPlanState& plan,
 std::function_ref<void(const std::filesystem::path&, std::size_t)> serialize, std::function_ref<void(TrainingPublicationStep)> observe) {
 const auto writer = lock(directory_ / ".session.writer", LOCK_EX | LOCK_NB);
 TrainingSessionManifest next = value;
 TrainingSessionManifest current;
 if (std::filesystem::exists(path_)) {
  current = serial::decode_reflected_json<TrainingSessionManifest>(read_file(path_, kTrainingManifestBytes), manifest_limits);
  validate_training_session_manifest(current);
  next.previous_generation = current.generation;
 }
 { const auto pointer_lock = lock(directory_ / ".session.lock", LOCK_EX); retire(current); }
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
  plan_file.pwrite_all(bytes.data(), bytes.size(), 0); plan_file.sync_data();
  next.plan_sha256 = io::sha256_hex(io::sha256_file(staging / "plan.cbor"));
  observe(TrainingPublicationStep::PlanWritten);
  if (!plan.plan_hash || plan.shards.size() != next.models.size()) throw std::runtime_error("invalid session data plan");
  std::optional<NativeCheckpointMetadata> semantics;
  for (std::size_t index = 0; index < next.models.size(); ++index) {
   auto& model = next.models[index];
   model.path = std::filesystem::path("generations") / next.generation / ("model-" + std::to_string(model.model_id) + ".pt");
   const auto path = directory_ / model.path;
   serialize(path, index);
   observe(TrainingPublicationStep::ModelWritten);
   const auto identity = io::FileSnapshot::Read(path);
   auto decoded = decode_native_model_state(path);
   (void)admit_model(decoded, path, next, plan, index);
   if (semantics && !same_native_model_semantics(decoded.metadata, *semantics)) throw std::runtime_error("session model semantics differ");
   if (!semantics) semantics = decoded.metadata;
   model.sha256 = io::sha256_hex(io::sha256_file(path));
   io::FileHandle::open_readonly(path.string()).sync_data();
   identity.RequireUnchanged(path);
   observe(TrainingPublicationStep::ModelValidated);
  }
  observe(TrainingPublicationStep::Serialized);
  validate_training_session_manifest(next);
  std::vector<std::pair<std::filesystem::path, io::FileSnapshot>> candidate_files;
  std::vector<std::shared_ptr<const ClassArtifactAdmission>> candidate_evidence;
  admit_best_candidates(next, *semantics, candidate_files, candidate_evidence);
  observe(TrainingPublicationStep::Validated);
  write_json(staging / "manifest.json", next, manifest_limits);
  io::sync_parent_directory(staging / "manifest.json");
  io::sync_parent_directory(staging);
  observe(TrainingPublicationStep::Synced);
  const auto pointer_lock = lock(directory_ / ".session.lock", LOCK_EX);
  for (const auto& [file, identity] : candidate_files) identity.RequireUnchanged(file);
  for (const auto& evidence : candidate_evidence) evidence->RequireUnchanged();
  write_json(path_, next, manifest_limits);
  committed = true;
  value = next;
  observe(TrainingPublicationStep::Published);
  staged_lease.reset();
  retire(next);
  observe(TrainingPublicationStep::Retained);
 } catch (...) {
  if (!committed) { std::error_code ignored; std::filesystem::remove_all(staging, ignored); }
  else { try { const auto pointer_lock = lock(directory_ / ".session.lock", LOCK_EX); retire(value); } catch (...) {} }
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
void publish_training_selection(const std::filesystem::path& path, const TrainingSelection& selected) {
 validate_training_selection(selected);
 auto admitted = admit_training_artifact(selected.artifact);
 admitted.class_artifact->RequireUnchanged();
 write_json(path, selected, selected_limits);
}
TrainingSelection read_training_selection(const std::filesystem::path& path) {
 auto selected = serial::decode_reflected_json<TrainingSelection>(read_file(path, kTrainingSelectionBytes), selected_limits);
 validate_training_selection(selected);
 (void)admit_training_artifact(selected.artifact);
 return selected;
}
}  // namespace mmltk::backend::models::rfdetr
