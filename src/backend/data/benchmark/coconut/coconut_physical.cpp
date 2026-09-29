#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory_io.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_images.h"
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
struct CoconutPhysicalMembership::Impl {
 struct Artifact {
  AdmittedRecipeArchive* archive;
  std::filesystem::path root;
  bool complete = false, admitted = false, cache_hit = false;
  std::string inspected_cache_identity;
  bool cache_generation_known = false;
  std::uint64_t completed_images = 0, bytes = 0;
 };
 struct Fact {
  CoconutPhysicalImage image;
  bool conflict = false, pixels = false, verified = true;
  std::uint64_t bytes = 0;
  const CoconutPhysicalImage* borrowed = nullptr;
  [[nodiscard]] const CoconutPhysicalImage& physical() const { return borrowed ? *borrowed : image; }
 };
 std::map<std::pair<CoconutImageNamespace, std::uint64_t>, Fact> images;
 std::map<CoconutEdition, std::vector<Artifact*>> releases;
 std::vector<Artifact> artifacts;
 std::map<CoconutImageNamespace, std::map<std::uint16_t, std::set<std::string>>> standalone_shards;
 const BenchmarkCacheLayout* cache = nullptr;
 StorageReservationPool* storage = nullptr;
 BenchmarkCompilePipeline* execution = nullptr;
 Cancellation cancellation;
 Progress progress;
 mutable std::mutex mutex;
 mutable std::condition_variable changed;
 Impl(std::span<AdmittedRecipeArchive> admitted, const CoconutRecipeCatalog& catalog, bool explicit_catalog, const BenchmarkCacheLayout& layout, StorageReservationPool& reservations,
  BenchmarkCompilePipeline& pipeline, Cancellation cancel, Progress observer)
     : cache(&layout), storage(&reservations), execution(&pipeline), cancellation(cancel), progress(std::move(observer)) {
  artifacts.reserve(admitted.size());
  for (auto& archive : admitted) artifacts.push_back({&archive, cache->source_images(benchmark_source_name(archive.origin.artifact.source)) / archive.origin.cache_shard});
  for (const auto& release : catalog.releases)
   for (auto& artifact : artifacts) {
    const auto& archive = artifact.archive->origin;
    if (std::ranges::find(coconut_release_sources(release.edition), archive.source) == coconut_release_sources(release.edition).end()) continue;
    if (!explicit_catalog && archive.source == CoconutImageNamespace::Objects365V2 &&
        std::ranges::find(coconut_objects_training_shards(release.edition), archive.shard) == coconut_objects_training_shards(release.edition).end()) continue;
    releases[release.edition].push_back(&artifact);
   }
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
 CoconutPhysicalImage resolve(CoconutEdition edition, const CoconutRecord& record, bool wait) const {
  const auto objects = admit_record(edition, record);
  const auto id = objects ? objects->id : record.image_id;
  std::unique_lock lock(mutex);
  const auto lookup = [&]() -> const CoconutPhysicalImage* {
   const CoconutPhysicalImage* match = nullptr;
   for (const auto source : coconut_release_sources(edition)) {
    if (objects && objects->source != source) continue;
    const auto found = images.find({source, id});
    if (found == images.end() || !found->second.verified) continue;
    if (found->second.conflict || match) throw CoconutPhysicalMembershipError(edition, id, "ambiguous physical image identity: " + record.file_name);
    const auto& physical = found->second.physical();
    if (execution) {
     const auto selected = releases.find(edition);
     if (selected == releases.end() || std::ranges::none_of(selected->second, [&](const auto* item) {
      return item->archive->origin.source == physical.source && item->archive->origin.shard == physical.shard;
     })) continue;
    }
    match = &physical;
   }
   return match;
  };
  for (;;) {
   if (const auto* image = lookup()) return *image;
   if (!execution || !wait) break;
   const auto selected = releases.find(edition);
   if (selected == releases.end() || std::ranges::all_of(selected->second, [](const auto* item) { return item->complete; })) break;
   throw_if_benchmark_cancelled(cancellation);
   changed.wait_for(lock, std::chrono::milliseconds(100));
  }
  if (execution && !wait) {
   const auto source = objects ? objects->source : edition == CoconutEdition::RelabeledValidation ? CoconutImageNamespace::CocoValidation : record.namespace_hint.value_or(CoconutImageNamespace::CocoTrain);
   const auto member = objects ? objects->stem + ".jpg" : (source == CoconutImageNamespace::CocoValidation ? "val2017/" : source == CoconutImageNamespace::CocoUnlabeled ? "unlabeled2017/" : "train2017/") + std::filesystem::path(record.file_name).filename().string();
   return {source, id, 0, member, "pending"};
  }
  throw CoconutPhysicalMembershipError(edition, id, "missing physical image: " + record.file_name);
 }
};
CoconutPhysicalMembership::CoconutPhysicalMembership(std::span<AdmittedRecipeArchive> admitted, const CoconutRecipeCatalog& catalog, bool explicit_catalog, const BenchmarkCacheLayout& cache,
 StorageReservationPool& storage, BenchmarkCompilePipeline& execution, Cancellation cancellation, Progress progress)
    : impl_(std::make_unique<Impl>(admitted, catalog, explicit_catalog, cache, storage, execution, cancellation, std::move(progress))) {}
CoconutPhysicalMembership::CoconutPhysicalMembership(std::span<const CoconutPhysicalImage> images, Cancellation cancellation) : impl_(std::make_unique<Impl>(images, cancellation)) {}
CoconutPhysicalMembership::~CoconutPhysicalMembership() = default;
const CoconutPhysicalImage* CoconutPhysicalMembership::find(CoconutImageNamespace source, std::uint64_t id) const noexcept {
 const std::lock_guard lock(impl_->mutex);
 return impl_->find(source, id);
}
CoconutPhysicalImage CoconutPhysicalMembership::identify(CoconutEdition edition, const CoconutRecord& record) const { return impl_->resolve(edition, record, false); }
CoconutPhysicalImage CoconutPhysicalMembership::resolve(CoconutEdition edition, const CoconutRecord& record) const {
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
  if (impl_->progress) impl_->progress(CoconutEdition::Base, archive, 0, 0);
 }
 const std::lock_guard lock(impl_->mutex);
 archive.download = std::move(download);
 impl_->artifact(archive).admitted = true;
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
 {
  const std::lock_guard lock(impl_->mutex);
  impl_->artifact(archive).complete = true;
 }
 impl_->changed.notify_all();
}
bool CoconutPhysicalMembership::consume(AdmittedRecipeArchive& archive, const std::function<void(const CoconutPhysicalImage&, std::string_view)>& rejected, bool cache_only, bool accept_partial) {
 auto& state = *impl_;
 auto& artifact = state.artifact(archive);
 const auto cancellation = state.cancellation;
 auto& execution = *state.execution;
 const auto lifecycle = state.cache->locks / (std::string(benchmark_source_name(archive.origin.artifact.source)) + "-" + archive.origin.cache_shard + ".images.lock");
 auto lease = ArtifactLease::acquire_charged(lifecycle, cancellation, &execution, archive_image_resources());
 const auto root = state.root(archive.origin.source);
 const auto inventory_path = state.cache->source_indexes(benchmark_source_name(archive.origin.artifact.source)) / (archive.origin.cache_shard + ".images.inventory");
 const auto proof_path = artifact.root / ".recipe-proofs/physical.inventory.complete.json";
 const auto& catalog = archive.origin.artifact;
 const nlohmann::json source_identity = nlohmann::json::array({catalog.artifact_id, catalog.url, catalog.filename, catalog.expected_size, catalog.expected_sha256});
 if (cache_only) {
  if (!std::filesystem::is_regular_file(inventory_path) || !std::filesystem::is_regular_file(proof_path)) return false;
  auto controls = execution.reserve(BenchmarkResources::handles(1, false), lease->allowance());
  std::ifstream input(proof_path);
  if (!input) throw std::runtime_error("cannot read COCONut image cache proof: " + proof_path.string());
  const auto proof = nlohmann::json::parse(input, nullptr, false);
  if (input.bad()) throw std::runtime_error("cannot read COCONut image cache proof: " + proof_path.string());
  if (proof.is_discarded()) return false;
  if (!proof.is_object() || !proof.contains("schema_version") || proof["schema_version"] != kBenchmarkCacheSchemaVersion || !proof.contains("source") || proof["source"] != source_identity ||
      !proof.contains("identity") || !proof["identity"].is_string() || !proof.contains("bytes") || !proof["bytes"].is_number_unsigned()) return false;
  const std::lock_guard lock(state.mutex);
  archive.download.identity = proof["identity"].get<std::string>();
  archive.download.size = proof["bytes"].get<std::uint64_t>();
  archive.download.cache_hit = true;
  artifact.admitted = true;
 }
 prepare_cached_image_directory(artifact.root);
 auto directory_allowance = execution.reserve(BenchmarkResources::handles(3, false), lease->allowance());
 const int descriptor = ::open(artifact.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
 if (descriptor < 0) throw common_io::errno_error("cannot open physical image cache", artifact.root.string());
 common_io::FileHandle directory(descriptor);
 BenchmarkImageDecoder decoder;
 StorageReservationPool destination(artifact.root, {}, state.storage);
 const CachedImageValidator validate = [&](std::uint64_t, std::span<const std::uint8_t> encoded) {
  if (!has_complete_image_markers(encoded)) throw BenchmarkImageError("incomplete COCONut JPEG");
  return decoder.read_header(encoded);
 };
 const auto fact = [&](const CoconutPhysicalImage& image, bool verified, bool duplicate = false) -> std::optional<bool> {
  bool conflict = duplicate, pixels = false, retire = false;
  {
   const std::lock_guard lock(state.mutex);
   const auto withdraw = [&](Impl::Fact& value) {
    if (!value.pixels) return;
    for (auto& owner : state.artifacts) if (owner.archive->origin.source == value.image.source && owner.archive->origin.shard == value.image.shard) {
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
   state.changed.notify_all();
  }
  if (conflict) {
   if (retire) execution.retire_image(root, image.image_id);
   if (rejected) rejected(image, "ambiguous physical image identity");
   return std::nullopt;
  }
  return pixels;
 };
 const auto publish = [&](const CoconutPhysicalImage& image, BenchmarkEncodedImage::Ptr input) {
  const auto bytes = input->size();
  execution.source_publication(root, lease, image.image_id)({image.image_id, std::pair{input->header().width, input->header().height}, true, std::move(input), artifact.root});
  if (execution.image_slot(root, image.image_id)) execution.wait_image(root, image.image_id);
  bool conflict = false;
  {
   const std::lock_guard lock(state.mutex);
   auto& fact = state.images.at({image.source, image.image_id});
   conflict = fact.conflict;
   if (!conflict) {
    if (!fact.pixels) { ++artifact.completed_images; artifact.bytes = mmltk::common::math::checked_add(artifact.bytes, bytes, "COCONut image cache byte overflow"); }
    fact.pixels = true;
    fact.bytes = bytes;
   }
  }
  // A peer can discover ambiguity while this decoder is active. Let that
  // borrower finish before retiring its slot; unrelated readers keep moving.
  if (conflict) execution.retire_image(root, image.image_id);
 };
 bool cache_generation_known = artifact.cache_generation_known && artifact.inspected_cache_identity == archive.download.identity;
 if (artifact.inspected_cache_identity != archive.download.identity && std::filesystem::is_regular_file(inventory_path)) {
  artifact.inspected_cache_identity = archive.download.identity;
  try {
   (void)load_coconut_physical_inventory(inventory_path, archive.origin.source, archive.origin.shard, archive.download.identity, cancellation, [&](const CoconutPhysicalImage& image) {
    cache_generation_known = artifact.cache_generation_known = true;
    const auto complete = fact(image, false);
    if (!complete) return;
    auto input = BenchmarkEncodedImage::open(directory.get(), image.image_id, validate, cancellation, &execution, lease->allowance(), {}, 32ULL << 20);
    if (!input) throw BenchmarkImageError("cached inventory image is missing");
    if (!*complete) publish(image, std::move(input));
    if (state.progress) state.progress(CoconutEdition::Base, archive, image.image_id, artifact.completed_images);
   });
   {
    const std::lock_guard lock(state.mutex);
    for (auto& [key, value] : state.images) if (key.first == archive.origin.source && value.image.shard == archive.origin.shard) value.verified = true;
   }
   { const std::lock_guard lock(state.mutex); artifact.cache_hit = true; }
   unavailable(archive);
   return true;
  } catch (const CoconutInputError&) { /* Repair the invalid inventory from its forward source. */ }
    catch (const BenchmarkImageReadError& error) { execution.retire_image(root, error.source_image_id()); }
    catch (const BenchmarkImageError&) { /* Only failed/missing cache rows need source bytes. */ }
    catch (const InvalidImageError&) { /* Invalid encoded cache input is locally repairable. */ }
 }
 if (cache_only) {
  const std::lock_guard lock(state.mutex);
  artifact.admitted = false;
  return false;
 }
 BenchmarkArchive reader(archive.download.path, &execution, 64ULL << 20, lease->allowance(), 1, true, {}, 1024, 2);
 std::vector<CoconutPhysicalImage> inventory;
 std::unordered_set<std::uint64_t> encountered;
 bool unreadable = false;
 const auto bad_image = [&](const CoconutPhysicalImage& image, std::string_view reason) {
  unreadable = true;
  if (accept_partial && rejected) rejected(image, reason);
 };
 while (reader.next(cancellation)) {
  if (!reader.regular() || !reader.member().ends_with(".jpg")) continue;
  const auto id = benchmark_archive_image_candidate(reader.member());
  if (!id) continue;
  CoconutPhysicalImage image{archive.origin.source, *id, archive.origin.shard, reader.member(), archive.download.identity};
  try { validate_coconut_physical_image(image); } catch (const CoconutInputError& error) {
   if (rejected) rejected(image, error.what());
   continue;
  }
  const auto complete = fact(image, true, !encountered.insert(*id).second);
  if (!complete) continue;
  inventory.push_back(image);
  if (*complete) continue;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
   try {
    BenchmarkEncodedImage::Ptr input;
    if (!attempt && cache_generation_known) input = BenchmarkEncodedImage::open(directory.get(), *id, validate, cancellation, &execution, reader.allowance(), {}, 32ULL << 20);
    if (!input) {
     const auto encoded = reader.read(32ULL << 20, cancellation);
     BenchmarkImageHeader header;
     reader.cpu([&] { header = validate(*id, encoded); });
     if (std::filesystem::exists(cached_image_path(artifact.root, *id))) invalidate_cached_image_proofs(artifact.root);
     input = BenchmarkEncodedImage::publish(directory.get(), *id, encoded, header, cancellation, destination, &execution, reader.allowance(), true);
    }
    publish(image, std::move(input));
    break;
   } catch (const BenchmarkImageReadError& error) {
    if (attempt) bad_image(image, error.what());
    execution.retire_image(root, *id);
    if (attempt) break;
   } catch (const BenchmarkImageError& error) {
    if (attempt) bad_image(image, error.what());
    if (attempt) break;
   } catch (const InvalidImageError& error) {
    if (attempt) bad_image(image, error.what());
    if (attempt) break;
   }
  }
  if (state.progress) state.progress(CoconutEdition::Base, archive, *id, artifact.completed_images);
 }
 if (unreadable && !accept_partial) throw BenchmarkArchiveError("COCONut archive contains unreadable JPEG payloads");
 // An omitted ambiguous ID stays absent from the completed source inventory.
 // It must never regain eligibility merely because one spelling had pixels.
 std::erase_if(inventory, [&](const auto& image) {
  const std::lock_guard lock(state.mutex);
  return !state.find(image.source, image.image_id);
 });
 std::ranges::sort(inventory, {}, &CoconutPhysicalImage::image_id);
 store_coconut_physical_inventory(inventory_path, inventory, archive.origin.source, archive.origin.shard, archive.download.identity, cancellation, state.storage);
 write_json_atomically(proof_path, {{"schema_version", kBenchmarkCacheSchemaVersion}, {"source", source_identity}, {"identity", archive.download.identity}, {"bytes", archive.download.size}}, cancellation, state.storage);
 unavailable(archive);
 return true;
}
CachedImageDirectory CoconutPhysicalMembership::summary(const AdmittedRecipeArchive& archive) const {
 const std::lock_guard lock(impl_->mutex);
 const auto& value = impl_->artifact(archive);
 return {std::string(benchmark_source_name(archive.origin.artifact.source)), archive.origin.cache_shard, value.root, archive.download.identity, {}, value.completed_images, value.bytes, value.cache_hit, {}};
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
   tuples.push_back({archive.origin.artifact.artifact_id, archive.origin.shard, current || used.empty() ? (item->admitted ? archive.download.identity : std::string{}) : used.at(archive.origin.shard)});
  }
 } else if (const auto found = impl_->standalone_shards.find(source); found != impl_->standalone_shards.end()) {
  for (const auto& [shard, identities] : found->second) {
   if (!used.empty() && !used.contains(shard)) continue;
   if (current || used.empty()) for (const auto& identity : identities) tuples.push_back({shard, identity});
   else tuples.push_back({shard, used.at(shard)});
  }
 }
 return nlohmann::json{{"domain", "coconut-physical-artifacts-v1"}, {"edition", edition}, {"source", source}, {"artifacts", std::move(tuples)}}.dump();
}
BenchmarkSourcePublication CoconutPhysicalMembership::label_publication(CoconutEdition, const CoconutPhysicalImage& image) const {
 return impl_->execution ? impl_->execution->source_publication(impl_->root(image.source), {}, image.image_id) : BenchmarkSourcePublication{};
}
void CoconutPhysicalMembership::withdraw(const AdmittedRecipeArchive& archive) {
 std::vector<std::uint64_t> withdrawn;
 {
  const std::lock_guard lock(impl_->mutex);
  impl_->artifact(archive).complete = false;
  impl_->artifact(archive).admitted = false;
  impl_->artifact(archive).completed_images = impl_->artifact(archive).bytes = 0;
  impl_->artifact(archive).cache_hit = false;
  impl_->artifact(archive).inspected_cache_identity.clear();
  impl_->artifact(archive).cache_generation_known = false;
  std::erase_if(impl_->images, [&](const auto& item) {
   const auto& image = item.second.image;
   if (image.source != archive.origin.source || image.shard != archive.origin.shard) return false;
   withdrawn.push_back(image.image_id);
   return true;
  });
 }
 for (const auto id : withdrawn) impl_->execution->retire_image(impl_->root(archive.origin.source), id);
}
}  // namespace mmltk::backend::data::benchmark_internal
