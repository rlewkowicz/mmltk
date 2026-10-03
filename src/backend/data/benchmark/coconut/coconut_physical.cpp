#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory_io.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_images.h"
#include "src/backend/data/benchmark/detail/benchmark_image_publication.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/file_digest.h"
#include "src/common/math/checked_arithmetic.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <set>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fcntl.h>
namespace mmltk::backend::data::benchmark_internal {
using mmltk::backend::data::detail::CompileResources;
namespace {
using Cancellation = mmltk::common::concurrency::CancellationObservation;
namespace common_io = mmltk::common::io;
[[noreturn]] void invalid(std::string_view detail) { throw CoconutInputError("COCONut: " + std::string(detail)); }
std::optional<CoconutPhysicalName> admit_record(CoconutEdition edition, const CoconutRecord& record) {
 if (edition == CoconutEdition::Base || edition == CoconutEdition::RelabeledValidation) {
  if (parse_coconut_coco_member(record.file_name) != record.image_id) invalid("COCO row filename/image_id mismatch: " + record.file_name);
  return std::nullopt;
 }
 auto physical = parse_coconut_objects_member(record.physical_stem);
 if (edition != CoconutEdition::ObjectsValidation && physical.source != CoconutImageNamespace::Objects365V2) invalid("training extension requires Objects365 v2");
 return physical;
}
}  // namespace
std::span<const CoconutImageNamespace> coconut_release_sources(CoconutEdition edition) {
 static constexpr CoconutImageNamespace base[]{CoconutImageNamespace::CocoTrain, CoconutImageNamespace::CocoUnlabeled};
 static constexpr CoconutImageNamespace validation[]{CoconutImageNamespace::CocoValidation};
 static constexpr CoconutImageNamespace objects[]{CoconutImageNamespace::Objects365V2};
 static constexpr CoconutImageNamespace objects_validation[]{CoconutImageNamespace::Objects365V1};
 switch (edition) {
  case CoconutEdition::Base: return base;
  case CoconutEdition::RelabeledValidation: return validation;
  case CoconutEdition::Large:
  case CoconutEdition::XLarge: return objects;
  case CoconutEdition::ObjectsValidation: return objects_validation;
 }
 throw std::invalid_argument("invalid COCONut edition");
}
struct CoconutPhysicalMembership::Registration::State {
 std::weak_ptr<Impl> owner;
 std::pair<CoconutEdition, std::uint64_t> key;
 detail::CompileExecution::CompletionSource completion;
 bool registered = false;
 State(const std::shared_ptr<Impl>& membership, std::pair<CoconutEdition, std::uint64_t> identity, detail::CompileExecution* execution) : owner(membership), key(identity), completion(execution) {}
 ~State();
};
struct CoconutPhysicalMembership::Impl {
 struct Artifact {
  AdmittedRecipeArchive* archive;
  std::filesystem::path root;
  bool complete = false, admitted = false, cache_hit = false;
  std::string inspected_cache_identity{};
  bool cache_generation_known = false;
  std::uint64_t completed_images = 0, bytes = 0;
  unsigned admissions = 0;
  std::vector<std::uint64_t> unreadable{};
 };
 struct Fact {
  CoconutPhysicalImage image{};
  bool conflict = false, pixels = false, verified = true, header = false;
  std::uint64_t bytes = 0;
  const CoconutPhysicalImage* borrowed = nullptr;
  [[nodiscard]] const CoconutPhysicalImage& physical() const { return borrowed ? *borrowed : image; }
 };
 std::map<std::pair<CoconutImageNamespace, std::uint64_t>, Fact> images;
 std::map<CoconutEdition, std::vector<Artifact*>> releases;
 std::vector<Artifact> artifacts;
 std::map<CoconutImageNamespace, std::map<std::uint16_t, std::set<std::string>>> standalone_shards;
 const BenchmarkCacheLayout* cache = nullptr;
 detail::StorageReservationPool* storage = nullptr;
 BenchmarkCompilePipeline* execution = nullptr;
 Cancellation cancellation;
 Progress progress;
 Repair repair;
 BenchmarkTraceSink trace;
 mutable std::mutex mutex;
 mutable std::condition_variable changed;
 // Only registrations own their completion/callback. This routing table
 // retains weak entries, removed by the last observer even without an event.
 using Registrations = std::map<Registration::State*, std::weak_ptr<Registration::State>>;
 using Change = std::map<std::pair<CoconutEdition, std::uint64_t>, Registrations>;
 mutable Change readiness_changes;
 std::uint64_t readiness_generation = 0;
 [[nodiscard]] Change advance_locked(CoconutImageNamespace source, std::optional<std::uint64_t> image = {}) noexcept {
  ++readiness_generation;
  Change notifications;
  // Only a changed identity wakes its subscribers. Source settlement/repair
  // also wakes missing identities in that namespace to prove terminal absence.
  for (const auto& [edition, selected] : releases) {
   (void)selected;
   if (std::ranges::find(coconut_release_sources(edition), source) == coconut_release_sources(edition).end()) continue;
   if (image) {
    auto node = readiness_changes.extract({edition, *image});
    if (!node.empty()) notifications.insert(std::move(node));
   } else {
    auto next = readiness_changes.lower_bound({edition, 0});
    while (next != readiness_changes.end() && next->first.first == edition) notifications.insert(readiness_changes.extract(next++));
   }
  }
  return notifications;
 }
 void notify(Change current, std::exception_ptr failure = {}) noexcept {
  changed.notify_all();
  for (auto& [key, registrations] : current) {
   (void)key;
   for (auto& [identity, weak] : registrations) {
    (void)identity;
    if (const auto registration = weak.lock()) registration->completion.settle(failure);
   }
  }
 }
 Impl(std::span<AdmittedRecipeArchive> admitted, const CoconutRecipeCatalog& catalog, bool explicit_catalog, const BenchmarkCacheLayout& layout, detail::StorageReservationPool& reservations,
  BenchmarkCompilePipeline& pipeline, Cancellation cancel, Progress observer, Repair recover, BenchmarkTraceSink diagnostics)
     : cache(&layout), storage(&reservations), execution(&pipeline), cancellation(cancel), progress(std::move(observer)), repair(std::move(recover)), trace(std::move(diagnostics)) {
  artifacts.reserve(admitted.size());
  for (auto& archive : admitted) artifacts.push_back({&archive, cache->source_images(benchmark_source_name(archive.origin.artifact.source)) / archive.origin.cache_shard});
  for (const auto& release : catalog.releases)
   for (auto& artifact : artifacts) {
    const auto& archive = artifact.archive->origin;
    if (std::ranges::find(coconut_release_sources(release.edition), archive.source) == coconut_release_sources(release.edition).end()) continue;
    if (!explicit_catalog && archive.source == CoconutImageNamespace::Objects365V2 &&
        std::ranges::find(coconut_objects_training_shards(release.edition), archive.shard) == coconut_objects_training_shards(release.edition).end())
     continue;
    releases[release.edition].push_back(&artifact);
   }
  // Stock validation uses the same physical COCO domain even when its
  // annotations come from the original catalog instead of a COCONut release.
  if (!releases.contains(CoconutEdition::RelabeledValidation))
   for (auto& artifact : artifacts)
    if (artifact.archive->origin.source == CoconutImageNamespace::CocoValidation) releases[CoconutEdition::RelabeledValidation].push_back(&artifact);
 }
 explicit Impl(std::span<const CoconutPhysicalImage> values, Cancellation cancel) : cancellation(cancel) {
  for (const auto& image : values) {
   throw_if_benchmark_cancelled(cancellation);
   validate_coconut_physical_image(image);
   if (!images.emplace(std::pair{image.source, image.image_id}, Fact{.borrowed = &image}).second) invalid("duplicate physical member: " + image.member);
   standalone_shards[image.source][image.shard].insert(image.archive_identity);
  }
 }
 std::filesystem::path root(CoconutImageNamespace source) const {
  // Train and unlabeled share COCO's physical ID domain. The join is compile
  // local; JPEG files retain their established source/shard cache locations.
  if (source == CoconutImageNamespace::CocoUnlabeled) source = CoconutImageNamespace::CocoTrain;
  return cache ? cache->source_indexes("coconut") / ("identity-" + std::to_string(static_cast<unsigned>(source))) : std::filesystem::path{};
 }
 const CoconutPhysicalImage* find(CoconutImageNamespace source, std::uint64_t id) const noexcept {
  const auto found = images.find({source, id});
  return found == images.end() || found->second.conflict || !found->second.verified ? nullptr : &found->second.physical();
 }
 Artifact& artifact(const AdmittedRecipeArchive& archive) {
  const auto found = std::ranges::find_if(artifacts, [&](const auto& item) { return item.archive == &archive; });
  if (found == artifacts.end()) throw std::logic_error("physical source is outside its controller");
  return *found;
 }
 void complete(Artifact& item) {
  Change notification;
  {
   const std::lock_guard lock(mutex);
   item.complete = true;
   item.admissions = std::max(item.admissions, 1U);
   notification = advance_locked(item.archive->origin.source);
  }
  notify(std::move(notification));
 }
 struct Inspection {
  const Fact* image = nullptr;
  bool conflict = false, terminal = false;
  Artifact* repair = nullptr;
 };
 // The pointers in this inspection are borrowed only while mutex is held.
 Inspection inspect_locked(CoconutEdition edition, const CoconutRecord& record, const std::optional<CoconutPhysicalName>& objects) const {
  Inspection result;
  bool excluded = false;
  const auto id = objects ? objects->id : record.image_id;
  const auto selected = releases.find(edition);
  const auto sources = objects ? std::span<const CoconutImageNamespace>(&objects->source, 1) : coconut_release_sources(edition);
  for (const auto source : sources) {
   const auto found = images.find({source, id});
   if (found == images.end() || !found->second.verified) continue;
   // A verified ambiguous identity is rejected before release eligibility.
   // Filtering the conflicted spelling first could admit its selected peer.
   if (found->second.conflict || result.image) {
    result.image = nullptr;
    result.conflict = result.terminal = true;
    return result;
   }
   const auto& physical = found->second.physical();
   if (execution && (selected == releases.end() ||
                     std::ranges::none_of(selected->second, [&](const auto* item) { return item->archive->origin.source == physical.source && item->archive->origin.shard == physical.shard; }))) {
    excluded = true;
    continue;
   }
   result.image = &found->second;
  }
  result.terminal = !execution || (result.image && result.image->pixels) || selected == releases.end() || std::ranges::all_of(selected->second, [](const auto* item) { return item->complete; });
  // Discovery still settles before absence is terminal, preserving later
  // ambiguity. A verified location outside this release is already known;
  // replacing its eligible archives cannot make that location eligible.
  if (!result.image && excluded) return result;
  if (execution && result.terminal && (!result.image || !result.image->pixels) && repair && selected != releases.end())
   for (auto* item : selected->second) {
    const auto& origin = item->archive->origin;
    if (item->admissions >= 3) continue;
    if (result.image) {
     const auto& image = result.image->physical();
     if (origin.source != image.source || origin.shard != image.shard) continue;
    } else if ((objects && origin.source != objects->source) || (!objects && record.namespace_hint && origin.source != *record.namespace_hint)) {
     continue;
    }
    if (!result.repair || item->admissions < result.repair->admissions) result.repair = item;
   }
  return result;
 }
 struct RepairClaim {
  AdmittedRecipeArchive* archive;
  std::string reason;
  Change notification;
 };
 std::optional<RepairClaim> claim_repair_locked(const Inspection& inspected, const CoconutRecord& record) {
  if (!inspected.repair) return {};
  // Prepare the fallible request before withdrawing the terminal fact. Every
  // other join then sees this source pending until its owner settles it.
  RepairClaim claim{inspected.repair->archive, std::string(inspected.image ? "unreadable required image: " : "missing required image: ") + record.file_name, {}};
  inspected.repair->complete = false;
  claim.notification = advance_locked(inspected.repair->archive->origin.source);
  return claim;
 }
 CoconutPhysicalImage resolve(CoconutEdition edition, const CoconutRecord& record, bool wait) {
  const auto objects = admit_record(edition, record);
  const auto id = objects ? objects->id : record.image_id;
  std::unique_lock lock(mutex);
  for (;;) {
   const auto inspected = inspect_locked(edition, record, objects);
   if (inspected.conflict) throw CoconutPhysicalMembershipError(edition, id, "ambiguous physical image identity: " + record.file_name);
   if (inspected.image && (!execution || !wait || inspected.image->pixels || (inspected.terminal && !inspected.repair)))
    return inspected.image->physical();  // Final selection excludes unreadable pixels.
   if (!execution || !wait) break;
   if (auto claim = claim_repair_locked(inspected, record)) {
    lock.unlock();
    notify(std::move(claim->notification));
    // The owning I/O controller repairs its source; this join never traverses
    // an archive or retains a CPU lane while waiting for its new facts.
    repair(*claim->archive, std::move(claim->reason));
    lock.lock();
    continue;
   }
   if (inspected.terminal) break;
   throw_if_benchmark_cancelled(cancellation);
   changed.wait_for(lock, std::chrono::milliseconds(100));
  }
  if (execution && !wait) {
   const auto source = objects                                          ? objects->source
                       : edition == CoconutEdition::RelabeledValidation ? CoconutImageNamespace::CocoValidation
                                                                        : record.namespace_hint.value_or(CoconutImageNamespace::CocoTrain);
   const auto member = objects ? objects->stem + ".jpg"
                               : (source == CoconutImageNamespace::CocoValidation  ? "val2017/"
                                  : source == CoconutImageNamespace::CocoUnlabeled ? "unlabeled2017/"
                                                                                   : "train2017/") +
                                  std::filesystem::path(record.file_name).filename().string();
   return {source, id, 0, member, "pending"};
  }
  throw CoconutPhysicalMembershipError(edition, id, "missing physical image: " + record.file_name);
 }
};
CoconutPhysicalMembership::Registration::State::~State() {
 if (!registered) return;
 if (const auto membership = owner.lock()) {
  const std::lock_guard lock(membership->mutex);
  const auto found = membership->readiness_changes.find(key);
  if (found == membership->readiness_changes.end()) return;
  found->second.erase(this);
  if (found->second.empty()) membership->readiness_changes.erase(found);
 }
}
bool CoconutPhysicalMembership::Registration::ready() const { return !state_ || state_->completion.completion().ready(); }
void CoconutPhysicalMembership::Registration::subscribe(std::function<void(std::exception_ptr)> callback) const {
 if (state_) state_->completion.completion().subscribe(std::move(callback));
}
CoconutPhysicalMembership::CoconutPhysicalMembership(std::span<AdmittedRecipeArchive> admitted, const CoconutRecipeCatalog& catalog, bool explicit_catalog, const BenchmarkCacheLayout& cache,
 detail::StorageReservationPool& storage, BenchmarkCompilePipeline& execution, Cancellation cancellation, Progress progress, Repair repair, BenchmarkTraceSink trace)
    : impl_(std::make_shared<Impl>(admitted, catalog, explicit_catalog, cache, storage, execution, cancellation, std::move(progress), std::move(repair), std::move(trace))) {}
CoconutPhysicalMembership::CoconutPhysicalMembership(std::span<const CoconutPhysicalImage> images, Cancellation cancellation) : impl_(std::make_shared<Impl>(images, cancellation)) {}
CoconutPhysicalMembership::~CoconutPhysicalMembership() = default;
const CoconutPhysicalImage* CoconutPhysicalMembership::find(CoconutImageNamespace source, std::uint64_t id) const noexcept {
 const std::lock_guard lock(impl_->mutex);
 return impl_->find(source, id);
}
CoconutPhysicalMembership::Readiness CoconutPhysicalMembership::readiness(CoconutEdition edition, const CoconutRecord& record) const {
 const auto objects = admit_record(edition, record);
 std::optional<Impl::RepairClaim> claim;
 Readiness result;
 {
  const std::lock_guard lock(impl_->mutex);
  const auto inspected = impl_->inspect_locked(edition, record, objects);
  // Verified identity and its admitted encoded header authorize recovery;
  // successful resized pixels remain a separate final eligibility fact.
  if (inspected.image && !inspected.repair && (!impl_->execution || inspected.image->header || inspected.terminal)) result.image = inspected.image->physical();
  claim = impl_->claim_repair_locked(inspected, record);
  result.terminal = inspected.terminal && !claim;
  result.generation = impl_->readiness_generation;
  const auto id = objects ? objects->id : record.image_id;
  const auto key = std::pair{edition, id};
  auto registration = std::make_shared<Registration::State>(impl_, key, impl_->execution ? &impl_->execution->execution() : nullptr);
  auto [next, inserted] = impl_->readiness_changes.try_emplace(key);
  try {
   next->second.emplace(registration.get(), registration);
  } catch (...) {
   if (inserted) impl_->readiness_changes.erase(next);
   throw;
  }
  registration->registered = true;
  result.changed.state_ = std::move(registration);
 }
 if (claim) {
  impl_->notify(std::move(claim->notification));
  impl_->repair(*claim->archive, std::move(claim->reason));
 }
 return result;
}
CoconutPhysicalImage CoconutPhysicalMembership::identify(CoconutEdition edition, const CoconutRecord& record) const { return impl_->resolve(edition, record, false); }
CoconutPhysicalImage CoconutPhysicalMembership::resolve(CoconutEdition edition, const CoconutRecord& record) const { return impl_->resolve(edition, record, true); }
CoconutPhysicalImage CoconutPhysicalMembership::resolve(CoconutEdition edition, const CoconutPhysicalImage& image) const {
 CoconutRecord record;
 record.image_id = image.image_id;
 record.file_name = image.member;
 record.physical_stem = std::filesystem::path(image.member).stem().string();
 record.namespace_hint = image.source;
 return impl_->resolve(edition, record, true);
}
std::filesystem::path CoconutPhysicalMembership::identity_root(CoconutImageNamespace source) const { return impl_->root(source); }
bool CoconutPhysicalMembership::eligible(CoconutEdition edition, const AdmittedRecipeArchive& archive) const {
 const auto found = impl_->releases.find(edition);
 return found != impl_->releases.end() && std::ranges::any_of(found->second, [&](const auto* item) { return item->archive == &archive; });
}
void CoconutPhysicalMembership::admitted(AdmittedRecipeArchive& archive, DownloadResult download, bool replacement) {
 {
  const std::lock_guard lock(impl_->mutex);
  replacement = replacement || std::ranges::any_of(impl_->images, [&](const auto& row) {
   return row.first.first == archive.origin.source && row.second.image.shard == archive.origin.shard && row.second.image.archive_identity != download.identity;
  });
 }
 if (replacement) {
  withdraw(archive);
  invalidate_cached_image_proofs(impl_->artifact(archive).root);
  if (impl_->progress) impl_->progress(CoconutEdition::Base, archive, 0, 0);
 }
 Impl::Change notification;
 {
  const std::lock_guard lock(impl_->mutex);
  archive.download = std::move(download);
  if (replacement) impl_->artifact(archive).inspected_cache_identity = archive.download.identity;
  impl_->artifact(archive).admitted = true;
  ++impl_->artifact(archive).admissions;
  notification = impl_->advance_locked(archive.origin.source);
 }
 impl_->notify(std::move(notification));
}
bool CoconutPhysicalMembership::can_retry(const AdmittedRecipeArchive& archive) const {
 const std::lock_guard lock(impl_->mutex);
 return impl_->artifact(archive).admissions < 3;
}
bool CoconutPhysicalMembership::discovery_complete() const {
 const std::lock_guard lock(impl_->mutex);
 return std::ranges::all_of(impl_->artifacts, [](const auto& artifact) { return artifact.complete; });
}
void CoconutPhysicalMembership::wait_sources(std::optional<CoconutEdition> edition) const {
 std::unique_lock lock(impl_->mutex);
 const auto complete = [&] {
  if (!edition) return std::ranges::all_of(impl_->artifacts, [](const auto& artifact) { return artifact.complete; });
  const auto found = impl_->releases.find(*edition);
  return found == impl_->releases.end() || std::ranges::all_of(found->second, [](const auto* artifact) { return artifact->complete; });
 };
 while (!complete()) {
  throw_if_benchmark_cancelled(impl_->cancellation);
  impl_->changed.wait_for(lock, std::chrono::milliseconds(100));
 }
}
void CoconutPhysicalMembership::unavailable(AdmittedRecipeArchive& archive) {
 Impl::Change notification;
 {
  const std::lock_guard lock(impl_->mutex);
  impl_->artifact(archive).complete = true;
  impl_->artifact(archive).admissions = 3;
  notification = impl_->advance_locked(archive.origin.source);
 }
 impl_->notify(std::move(notification));
}
bool CoconutPhysicalMembership::consume(AdmittedRecipeArchive& archive, const std::function<void(const CoconutPhysicalImage&, std::string_view)>& rejected, bool cache_only) {
 auto& state = *impl_;
 auto& artifact = state.artifact(archive);
 const auto cancellation = state.cancellation;
 auto& execution = *state.execution;
 const auto lifecycle = state.cache->locks / (std::string(benchmark_source_name(archive.origin.artifact.source)) + "-" + archive.origin.cache_shard + ".images.lock");
 auto lease = ArtifactLease::acquire_charged(lifecycle, cancellation, &execution.execution(), archive_image_resources());
 const auto root = state.root(archive.origin.source);
 const auto inventory_path = state.cache->source_indexes(benchmark_source_name(archive.origin.artifact.source)) / (archive.origin.cache_shard + ".images.inventory");
 const auto proof_path = artifact.root / ".recipe-proofs/physical.inventory.complete.json";
 const auto& catalog = archive.origin.artifact;
 const nlohmann::json source_identity = nlohmann::json::array({catalog.artifact_id, catalog.url, catalog.filename, catalog.expected_size, catalog.expected_sha256});
 const auto seal_proof = [&] {
  write_json_atomically(proof_path,
   {{"schema_version", kBenchmarkCacheSchemaVersion}, {"source", source_identity}, {"identity", archive.download.identity}, {"bytes", archive.download.size},
    {"unreadable_images", artifact.unreadable}},
   cancellation, state.storage);
 };
 if (cache_only) {
  if (!std::filesystem::is_regular_file(inventory_path) || !std::filesystem::is_regular_file(proof_path)) return false;
  auto controls = execution.execution().reserve(CompileResources::handles(1, false), lease->allowance());
  std::ifstream input(proof_path);
  if (!input) throw std::runtime_error("cannot read COCONut image cache proof: " + proof_path.string());
  const auto proof = nlohmann::json::parse(input, nullptr, false);
  if (input.bad()) throw std::runtime_error("cannot read COCONut image cache proof: " + proof_path.string());
  if (proof.is_discarded()) return false;
  if (!proof.is_object() || !proof.contains("schema_version") || proof["schema_version"] != kBenchmarkCacheSchemaVersion || !proof.contains("source") || proof["source"] != source_identity ||
      !proof.contains("identity") || !proof["identity"].is_string() || !proof.contains("bytes") || !proof["bytes"].is_number_unsigned())
   return false;
  artifact.unreadable.clear();
  if (const auto rejected_images = proof.find("unreadable_images"); rejected_images != proof.end()) {
   if (!rejected_images->is_array()) return false;
   for (const auto& id : *rejected_images) {
    if (!id.is_number_unsigned()) return false;
    const auto value = id.get<std::uint64_t>();
    if (!artifact.unreadable.empty() && value <= artifact.unreadable.back()) return false;
    artifact.unreadable.push_back(value);
   }
  }
  const std::lock_guard lock(state.mutex);
  archive.download.identity = proof["identity"].get<std::string>();
  archive.download.path = state.cache->source_downloads(benchmark_source_name(catalog.source)) / catalog.filename;
  archive.download.size = proof["bytes"].get<std::uint64_t>();
  archive.download.cache_hit = true;
  artifact.admitted = true;
 }
 prepare_cached_image_directory(artifact.root);
 auto directory_allowance = execution.execution().reserve(CompileResources::handles(1), lease->allowance());
 const int descriptor = ::open(artifact.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
 if (descriptor < 0) throw common_io::errno_error("cannot open physical image cache", artifact.root.string());
 common_io::FileHandle directory(descriptor);
 BenchmarkImageDecoder decoder;
 const CachedImageValidator validate = [&](std::uint64_t, std::span<const std::uint8_t> encoded) {
  if (!has_complete_image_markers(encoded)) throw BenchmarkImageError("incomplete COCONut JPEG");
  return decoder.read_header(encoded);
 };
 const auto fact = [&](const CoconutPhysicalImage& image, bool verified, bool duplicate = false) -> std::optional<bool> {
  bool conflict = duplicate, pixels = false, retire = false;
  Impl::Change notification;
  {
   const std::lock_guard lock(state.mutex);
   const auto withdraw = [&](Impl::Fact& value) {
    if (!value.pixels) return;
    for (auto& owner : state.artifacts)
     if (owner.archive->origin.source == value.image.source && owner.archive->origin.shard == value.image.shard) {
      --owner.completed_images;
      owner.bytes -= value.bytes;
      break;
     }
    value.pixels = false;
    value.bytes = 0;
    retire = true;
   };
   if (image.source == CoconutImageNamespace::CocoTrain || image.source == CoconutImageNamespace::CocoUnlabeled) {
    const auto other = image.source == CoconutImageNamespace::CocoTrain ? CoconutImageNamespace::CocoUnlabeled : CoconutImageNamespace::CocoTrain;
    if (const auto peer = state.images.find({other, image.image_id}); peer != state.images.end()) {
     peer->second.conflict = true;
     withdraw(peer->second);
     conflict = true;
    }
   }
   auto [found, inserted] = state.images.emplace(std::pair{image.source, image.image_id}, Impl::Fact{image, conflict, false, verified});
   found->second.conflict = found->second.conflict || conflict || (!inserted && found->second.image != image);
   if (verified) found->second.verified = true;
   conflict = found->second.conflict;
   pixels = found->second.pixels;
   if (conflict) withdraw(found->second);
   notification = state.advance_locked(image.source, image.image_id);
  }
  try {
   if (retire) execution.retire_image(root, image.image_id);
  } catch (...) {
   state.notify(std::move(notification), std::current_exception());
   throw;
  }
  state.notify(std::move(notification));
  if (conflict) {
   if (rejected) rejected(image, "ambiguous physical image identity");
   return std::nullopt;
  }
  return pixels;
 };
 const auto window_size = std::clamp<std::size_t>(execution.execution().workers() * 2, 2, 64);
 std::vector<CoconutPhysicalImage> repairs;
 repairs.reserve(window_size * 2);
 bool cache_incomplete = false, replaying_inventory = true;
 const auto bad_image = [&](const CoconutPhysicalImage& image, std::string_view reason) {
  artifact.unreadable.push_back(image.image_id);
  if (rejected) rejected(image, reason);
 };
 const auto image_failure = [&](const CoconutPhysicalImage& image, bool source_read, std::exception_ptr failure) {
  std::string reason;
  try {
   std::rethrow_exception(failure);
  } catch (const BenchmarkImageReadError& error) { reason = error.what(); } catch (const BenchmarkImageError& error) {
   reason = error.what();
  } catch (const InvalidImageError& error) { reason = error.what(); }
  execution.retire_image(root, image.image_id);
  Impl::Change notification;
  {
   const std::lock_guard lock(state.mutex);
   const auto found = state.images.find({image.source, image.image_id});
   if (found != state.images.end() && found->second.image == image && found->second.pixels) {
    --artifact.completed_images;
    artifact.bytes -= found->second.bytes;
    found->second.pixels = false;
    found->second.bytes = 0;
   }
   notification = state.advance_locked(image.source, image.image_id);
  }
  state.notify(std::move(notification));
  if (source_read)
   bad_image(image, reason);
  else if (replaying_inventory)
   cache_incomplete = true;
  else
   repairs.push_back(image);
 };
 std::unique_ptr<BenchmarkArchive> archive_reader;
 BenchmarkTraceSink publication_trace;
 if (state.trace) publication_trace = {state.trace.selection(), [&](std::string_view event, const nlohmann::json& value) {
  trace_benchmark_event(state.trace, event, [&] {
   if (value.is_null()) return value;
   auto fields = value;
   fields["artifact"] = archive.origin.artifact.artifact_id;
   return fields;
  });
 }};
 BenchmarkCompletionWindow publications(window_size, &execution.execution(), [&] {
  if (archive_reader && archive_reader->reclamation_useful()) {
   trace_benchmark_event(publication_trace, "benchmark.images.pixel_wait", [&] {
    return nlohmann::json{{"member", archive_reader->member()}, {"decoder_bytes", archive_reader->retained_workspace_bytes()}};
   });
   if (publication_trace.accepts("benchmark.images.pressure") && archive_reader->retained_workspace_bytes()) trace_compile_execution(publication_trace, "benchmark.images.pressure", execution.execution().snapshot());
   archive_reader->suspend();
  }
 });
 // Capture source/image generations and mutation custody before any file work
 // is accepted. Only header facts and the ticket cross into file completion;
 // publication windows, repairs and unreadable rows remain controller-owned.
 const auto publication = [&](const CoconutPhysicalImage& image, bool source_read) {
  const auto generation = execution.image_generation(root, image.image_id);
  auto ticket = execution.source_publication(root, lease, image.image_id);
  CachedImageReadySink ready = [&, image, generation, ticket](const CachedImageReady& value) {
   if (execution.image_generation(root, image.image_id) != generation) return;
   Impl::Change header_notification;
   {
    const std::lock_guard lock(state.mutex);
    const auto found = state.images.find({image.source, image.image_id});
    if (found != state.images.end() && found->second.image == image && !found->second.header) {
     found->second.header = true;
     header_notification = state.advance_locked(image.source, image.image_id);
    }
   }
   state.notify(std::move(header_notification));
   ticket({value.image_id, value.dimensions, true, value.payload, artifact.root});
  };
  BenchmarkImagePublication::Settled settled = [&, image, generation, source_read, ticket](const CachedImageReady& value, std::exception_ptr failure) {
   if (execution.image_generation(root, image.image_id) != generation) return;
   if (failure) {
    image_failure(image, source_read, failure);
    return;
   }
   publications.make_room();
   const auto bytes = value.payload->size();
   publications.push(execution.image_completion(root, image.image_id), [&, image, bytes, generation, source_read, ticket](std::exception_ptr pixel_failure) {
    if (execution.image_generation(root, image.image_id) != generation) return;
    if (pixel_failure) {
     image_failure(image, source_read, pixel_failure);
     return;
    }
    bool conflict = false;
    std::size_t completed = 0;
    Impl::Change notification;
    {
     const std::lock_guard lock(state.mutex);
     const auto found = state.images.find({image.source, image.image_id});
     if (found == state.images.end() || found->second.image != image) return;
     auto& image_fact = found->second;
     conflict = image_fact.conflict;
     if (!conflict && !image_fact.pixels) {
      ++artifact.completed_images;
      artifact.bytes = mmltk::common::math::checked_add(artifact.bytes, bytes, "COCONut image cache byte overflow");
      image_fact.pixels = true;
      image_fact.bytes = bytes;
     }
     completed = artifact.completed_images;
     notification = state.advance_locked(image.source, image.image_id);
    }
    try {
     if (conflict) execution.retire_image(root, image.image_id);
    } catch (...) {
     state.notify(std::move(notification), std::current_exception());
     throw;
    }
    state.notify(std::move(notification));
    if (state.progress) state.progress(CoconutEdition::Base, archive, image.image_id, completed);
   });
  };
  return std::pair{std::move(ready), std::move(settled)};
 };
 const auto publish = [&](const CoconutPhysicalImage& image, BenchmarkEncodedImage::Ptr input, bool source_read = false) {
  auto [ready, settled] = publication(image, source_read);
  const CachedImageReady value{image.image_id, std::pair{input->header().width, input->header().height}, true, std::move(input)};
  ready(value);
  settled(value, {});
 };
 bool cache_generation_known = artifact.cache_generation_known && artifact.inspected_cache_identity == archive.download.identity;
 if (artifact.inspected_cache_identity != archive.download.identity && std::filesystem::is_regular_file(inventory_path)) {
  artifact.inspected_cache_identity = archive.download.identity;
  try {
   (void)load_coconut_physical_inventory(inventory_path, archive.origin.source, archive.origin.shard, archive.download.identity, cancellation, [&](const CoconutPhysicalImage& image) {
    cache_generation_known = artifact.cache_generation_known = true;
    const auto complete = fact(image, false);
    if (!complete) return;
    if (std::ranges::binary_search(artifact.unreadable, image.image_id)) {
     if (rejected) rejected(image, "unreadable JPEG in retained source generation");
     return;
    }
    publications.make_room();
    try {
     auto input = BenchmarkEncodedImage::open(directory.get(), image.image_id, validate, cancellation, &execution.execution(), lease->allowance(), {}, BenchmarkEncodedImage::normal_window_bytes);
     if (!input) throw BenchmarkImageError("cached inventory image is missing");
     if (!*complete) publish(image, std::move(input));
    } catch (...) { image_failure(image, false, std::current_exception()); }
   });
   Impl::Change notification;
   {
    const std::lock_guard lock(state.mutex);
    for (auto& [key, value] : state.images)
     if (key.first == archive.origin.source && value.image.shard == archive.origin.shard) value.verified = true;
    notification = state.advance_locked(archive.origin.source);
   }
   state.notify(std::move(notification));
   publications.drain();
   if (cache_incomplete) throw BenchmarkImageError("cached inventory has unreadable images");
   {
    const std::lock_guard lock(state.mutex);
    artifact.cache_hit = true;
   }
   if (!cache_only) seal_proof();
   state.complete(artifact);
   return true;
  } catch (const CoconutInputError&) { /* Repair the invalid inventory from its forward source. */
  } catch (const BenchmarkImageReadError& error) { execution.retire_image(root, error.source_image_id()); } catch (const BenchmarkImageError&) { /* Only failed/missing cache rows need source bytes. */
  } catch (const InvalidImageError&) { /* Invalid encoded cache input is locally repairable. */
  }
  publications.drain();
  invalidate_cached_image_proofs(artifact.root);
 }
 if (cache_only) {
  const std::lock_guard lock(state.mutex);
  artifact.admitted = false;
  return false;
 }
 replaying_inventory = false;
 archive_reader = std::make_unique<BenchmarkArchive>(archive.download.path, &execution.execution(), 0, lease->allowance(), 1, true, CompileAllowance{}, 1024, 0, 32768, publication_trace);
 auto& reader = *archive_reader;
 std::vector<CoconutPhysicalImage> inventory;
 std::unordered_set<std::uint64_t> encountered;
 artifact.unreadable.clear();
 BenchmarkImagePublication write_pool(artifact.root, cancellation, state.storage, lease->allowance(), &execution, directory.get(), true, reader, publication_trace);
 const auto read_source = [&](const CoconutPhysicalImage& image) {
  try {
   reader.require_regular(BenchmarkEncodedImage::normal_window_bytes);
   auto admitted = write_pool.acquire(mmltk::common::math::checked_cast<std::size_t>(reader.size(), "physical archive image size overflow"));
   write_pool.read(admitted);
   BenchmarkImageHeader header;
   execution.execution().run(detail::CompileTaskKind::Header, {}, [&](std::size_t) { header = validate(image.image_id, admitted->encoded); }, admitted->allowance);
   BenchmarkEncodedImage::Ptr input;
   // Unknown generations reuse healthy cache allocation only after exact byte
   // comparison. The output descriptor grant also covers this temporary read;
   // no archive-owned span or decoder storage escapes the forward copy.
   try {
    input = BenchmarkEncodedImage::open(directory.get(), image.image_id, validate, cancellation, &execution.execution(), admitted->allowance, {}, BenchmarkEncodedImage::normal_window_bytes);
    if (input && !std::ranges::equal(input->encoded(), admitted->encoded)) input.reset();
   } catch (const BenchmarkImageError&) { input.reset(); } catch (const InvalidImageError&) {
    input.reset();
   }
   if (input) {
    input = input->file_input();
    admitted.reset();
    publish(image, std::move(input), true);
   } else {
    if (std::filesystem::exists(cached_image_path(artifact.root, image.image_id))) invalidate_cached_image_proofs(artifact.root);
    auto [ready, settled] = publication(image, true);
    write_pool.submit(image.image_id, std::move(admitted), header, std::move(ready), std::move(settled));
   }
  } catch (...) { image_failure(image, true, std::current_exception()); }
 };
 const auto repair_cached = [&](bool restore_cursor) {
  write_pool.reap();
  publications.reap();
  if (repairs.empty()) return;
  const auto cursor = reader.member_position();
  auto batch = std::exchange(repairs, {});
  repairs.reserve(window_size * 2);
  std::vector<std::string> members;
  members.reserve(batch.size());
  for (const auto& image : batch) members.push_back(image.member);
  reader.visit_known(members, [&](std::size_t index) {
   publications.make_room();
   read_source(batch[index]);
  }, cancellation);
  // Every accepted writer owns its bytes. Cursor restoration needs no pixel
  // or file drain; the synchronous source copy/replay has already settled.
  if (restore_cursor && !reader.seek(cursor, cancellation)) throw BenchmarkArchiveError("physical archive cursor disappeared during repair");
 };
 while (reader.next(cancellation)) {
  write_pool.reap();
  publications.reap();
  if (repairs.size() >= window_size) repair_cached(true);
  if (!reader.regular() || !reader.member().ends_with(".jpg")) continue;
  const auto id = benchmark_archive_image_candidate(reader.member());
  if (!id) continue;
  CoconutPhysicalImage image{archive.origin.source, *id, archive.origin.shard, reader.member(), archive.download.identity};
  try {
   validate_coconut_physical_image(image);
  } catch (const CoconutInputError& error) {
   if (rejected) rejected(image, error.what());
   continue;
  }
  const auto complete = fact(image, true, !encountered.insert(*id).second);
  if (!complete) continue;
  inventory.push_back(image);
  if (*complete) continue;
  publications.make_room();
  BenchmarkEncodedImage::Ptr input;
  if (cache_generation_known) {
   try {
    auto controls = execution.execution().reserve(CompileResources::handles(1, true), reader.allowance());
    input = BenchmarkEncodedImage::open(directory.get(), *id, validate, cancellation, &execution.execution(), controls, {}, BenchmarkEncodedImage::normal_window_bytes);
    controls.retire_descriptors();
   } catch (const BenchmarkImageError&) {
   } catch (const InvalidImageError&) {}
  }
  if (input)
   publish(image, std::move(input));
  else
   read_source(image);
 }
 // Source completion must join every accepted publication before reconciling
 // repairs and sealing this exact inventory generation.
 write_pool.finish();
 publications.drain();
 repair_cached(false);
 write_pool.finish();
 publications.drain();
 // An omitted ambiguous ID stays absent from the completed source inventory.
 // It must never regain eligibility merely because one spelling had pixels.
 std::erase_if(inventory, [&](const auto& image) {
  const std::lock_guard lock(state.mutex);
  return !state.find(image.source, image.image_id);
 });
 std::ranges::sort(inventory, {}, &CoconutPhysicalImage::image_id);
 std::ranges::sort(artifact.unreadable);
 artifact.unreadable.erase(std::unique(artifact.unreadable.begin(), artifact.unreadable.end()), artifact.unreadable.end());
 store_coconut_physical_inventory(inventory_path, inventory, archive.origin.source, archive.origin.shard, archive.download.identity, cancellation, state.storage);
 seal_proof();
 state.complete(artifact);
 return true;
}
CachedImageDirectory CoconutPhysicalMembership::summary(const AdmittedRecipeArchive& archive) const {
 const std::lock_guard lock(impl_->mutex);
 const auto& value = impl_->artifact(archive);
 return {
  std::string(benchmark_source_name(archive.origin.artifact.source)), archive.origin.cache_shard, value.root, archive.download.identity, {}, value.completed_images, value.bytes, value.cache_hit, {}
 };
}
void CoconutPhysicalDependencies::add(const CoconutPhysicalImage& image) {
 const auto [entry, inserted] = shards_.try_emplace(image.shard, image.archive_identity);
 if (!inserted && entry->second != image.archive_identity) invalid("component mixes physical artifact generations");
}
std::string CoconutPhysicalMembership::dependency_identity(CoconutEdition edition, CoconutImageNamespace source, const CoconutPhysicalDependencies& dependencies, bool current) const {
 const std::lock_guard lock(impl_->mutex);
 nlohmann::json tuples = nlohmann::json::array();
 const auto& used = dependencies.shards_;
 if (const auto release = impl_->releases.find(edition); release != impl_->releases.end()) {
  for (const auto* item : release->second) {
   const auto& archive = *item->archive;
   if (archive.origin.source != source || (!used.empty() && !used.contains(archive.origin.shard))) continue;
   tuples.push_back(
    {archive.origin.artifact.artifact_id, archive.origin.shard, current || used.empty() ? (item->admitted ? archive.download.identity : std::string{}) : used.at(archive.origin.shard)});
  }
 } else if (const auto found = impl_->standalone_shards.find(source); found != impl_->standalone_shards.end()) {
  for (const auto& [shard, identities] : found->second) {
   if (!used.empty() && !used.contains(shard)) continue;
   if (current || used.empty())
    for (const auto& identity : identities) tuples.push_back({shard, identity});
   else
    tuples.push_back({shard, used.at(shard)});
  }
 }
 return nlohmann::json{{"domain", "coconut-physical-artifacts-v1"}, {"edition", edition}, {"source", source}, {"artifacts", std::move(tuples)}}.dump();
}
BenchmarkSourcePublication CoconutPhysicalMembership::label_publication(CoconutEdition, const CoconutPhysicalImage& image) const {
 return impl_->execution ? impl_->execution->source_publication(impl_->root(image.source), {}, image.image_id) : BenchmarkSourcePublication{};
}
void CoconutPhysicalMembership::withdraw(const AdmittedRecipeArchive& archive) {
 std::vector<std::uint64_t> withdrawn;
 Impl::Change notification;
 {
  const std::lock_guard lock(impl_->mutex);
  impl_->artifact(archive).complete = false;
  impl_->artifact(archive).admitted = false;
  impl_->artifact(archive).completed_images = impl_->artifact(archive).bytes = 0;
  impl_->artifact(archive).cache_hit = false;
  impl_->artifact(archive).inspected_cache_identity.clear();
  impl_->artifact(archive).cache_generation_known = false;
  impl_->artifact(archive).unreadable.clear();
  std::erase_if(impl_->images, [&](const auto& item) {
   const auto& image = item.second.image;
   if (image.source != archive.origin.source || image.shard != archive.origin.shard) return false;
   withdrawn.push_back(image.image_id);
   return true;
  });
  notification = impl_->advance_locked(archive.origin.source);
 }
 try {
  for (const auto id : withdrawn) impl_->execution->retire_image(impl_->root(archive.origin.source), id);
 } catch (...) {
  impl_->notify(std::move(notification), std::current_exception());
  throw;
 }
 impl_->notify(std::move(notification));
}
}  // namespace mmltk::backend::data::benchmark_internal
