#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotations.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_images.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_recipe.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/file_digest.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
namespace mmltk::backend::data::benchmark_internal {
namespace {
using Cancellation = mmltk::common::concurrency::CancellationObservation;
namespace common_io = mmltk::common::io;
[[noreturn]] void invalid(std::string_view detail) { throw std::runtime_error("COCONut: " + std::string(detail)); }
std::uint64_t decimal(std::string_view value) {
 std::uint64_t id = 0;
 const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
 if (value.empty() || error != std::errc{} || end != value.data() + value.size() || value.front() == '+' || value.front() == '-') invalid("invalid decimal image identity: " + std::string(value));
 return id;
}
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
CoconutPhysicalName parse_coconut_objects_member(std::string_view name) {
 const std::filesystem::path path(canonical_benchmark_archive_member(name));
 const auto extension = path.extension().string();
 if (!extension.empty() && extension != ".png" && extension != ".jpg" && extension != ".json") invalid("unsupported Objects365 member: " + std::string(name));
 const auto stem = path.stem().string();
 constexpr std::string_view v1 = "objects365_v1_", v2 = "objects365_v2_";
 const auto source = stem.starts_with(v1) ? CoconutImageNamespace::Objects365V1 : CoconutImageNamespace::Objects365V2;
 const auto prefix = source == CoconutImageNamespace::Objects365V1 ? v1 : v2;
 if (!stem.starts_with(prefix) || stem.size() != prefix.size() + 8U) invalid("invalid full Objects365 namespace/member: " + std::string(name));
 return {source, decimal(std::string_view(stem).substr(prefix.size())), stem};
}
std::uint64_t parse_coconut_coco_member(std::string_view name) {
 const std::filesystem::path path(canonical_benchmark_archive_member(name));
 if ((path.extension() != ".jpg" && path.extension() != ".png") || path.stem().string().size() != 12U) invalid("invalid COCO filename: " + std::string(name));
 return decimal(path.stem().string());
}
void validate_coconut_physical_image(const CoconutPhysicalImage& image) {
 if (image.archive_identity.empty() || image.member != canonical_benchmark_archive_member(image.member)) invalid("invalid physical inventory identity");
 if (std::filesystem::path(image.member).extension() != ".jpg") invalid("physical member is not JPEG: " + image.member);
 if (image.source == CoconutImageNamespace::Objects365V1 || image.source == CoconutImageNamespace::Objects365V2) {
  const auto name = parse_coconut_objects_member(image.member);
  if (name.source != image.source || name.id != image.image_id) invalid("contradictory Objects365 physical identity: " + image.member);
 } else {
  if (parse_coconut_coco_member(image.member) != image.image_id) invalid("contradictory COCO physical identity: " + image.member);
  const std::string_view directory = image.source == CoconutImageNamespace::CocoTrain ? "train2017/" : image.source == CoconutImageNamespace::CocoUnlabeled ? "unlabeled2017/" : "val2017/";
  if (!image.member.starts_with(directory)) invalid("COCO physical subset disagrees with archive member: " + image.member);
 }
}
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
CoconutPhysicalInputRequirement CoconutPhysicalInputRequirement::archive(std::uint64_t workspace_bytes, std::size_t reader_descriptors) {
 auto controls = BenchmarkResources::handles(1, false, reader_descriptors + 2);
 // This reader consumes an already admitted annotation producer. Its physical
 // input must be able to use the capacity reserved for dependent consumers.
 controls.producer = false;
 return {controls, workspace_bytes};
}
struct CoconutPhysicalMembership::Impl {
 struct PhysicalRoute {
  AdmittedRecipeArchive* archive;
  BenchmarkArchive::MemberPosition locator;
  std::uint64_t position;
  bool consumed = false, conflict = false;
 };
 struct PhysicalSource {
  std::vector<AdmittedRecipeArchive*> archives;
  struct Artifact {
   AdmittedRecipeArchive* archive;
   std::filesystem::path root;
   std::size_t order;
   bool cache_reusable = true;
  };
  std::unordered_map<std::uint16_t, Artifact> artifacts;
  std::size_t next = 0;
  std::unordered_map<std::uint64_t, PhysicalRoute> members;
  // One forward discovery cursor per shard, referencing the authoritative
  // encountered member location. Late reads must not rewind future discovery.
  std::unordered_map<const AdmittedRecipeArchive*, std::uint64_t> scanned;
 };
 struct PhysicalRelease {
  std::mutex mutex;
  std::string identity;
  std::map<std::uint64_t, CoconutPhysicalImage> resolved;
  std::optional<CoconutPhysicalInputRequirement> input;
  std::map<CoconutImageNamespace, PhysicalSource> sources;
  std::set<std::filesystem::path> prepared_roots;
  AdmittedRecipeArchive* active = nullptr;
  std::shared_ptr<ArtifactLease> lease;
  std::unique_ptr<BenchmarkArchive> reader;
  BenchmarkAllowance directory_allowance, reader_parent;
  common_io::FileHandle directory;
  BenchmarkImageDecoder decoder;
  void release_reader() {
   directory = {};
   directory_allowance = {};
   reader.reset();
   lease.reset();
   reader_parent = {};
   active = nullptr;
  }
 };
 std::map<CoconutEdition, PhysicalRelease> releases;
 std::unordered_map<CoconutImageNamespace, std::unordered_map<std::uint64_t, const CoconutPhysicalImage*>> namespaces;
 std::map<CoconutImageNamespace, std::map<std::uint16_t, std::set<std::string>>> standalone_shards;
 const BenchmarkCacheLayout* cache = nullptr;
 StorageReservationPool* storage = nullptr;
 BenchmarkCompilePipeline* execution = nullptr;
 Cancellation cancellation;
 Impl(std::span<AdmittedRecipeArchive> admitted, const CoconutRecipeCatalog& catalog, bool explicit_catalog, const BenchmarkCacheLayout& layout, StorageReservationPool& reservations,
  BenchmarkCompilePipeline& pipeline, Cancellation cancel)
     : cache(&layout), storage(&reservations), execution(&pipeline), cancellation(cancel) {
  // Routing is a one-time projection of the selected release catalog. The
  // explicit private catalog supplies its own bounded fixture source set.
  for (const auto& release : catalog.releases)
   for (const auto name : coconut_release_sources(release.edition)) {
    auto& state = releases[release.edition];
    auto& source = state.sources[name];
    for (auto& archive : admitted) {
     if (archive.origin.source != name) continue;
     if (!explicit_catalog && name == CoconutImageNamespace::Objects365V2) {
      const auto shards = coconut_objects_training_shards(release.edition);
      if (std::ranges::find(shards, archive.origin.shard) == shards.end()) continue;
     }
     source.archives.push_back(&archive);
     if (!source.artifacts
          .emplace(archive.origin.shard,
           PhysicalSource::Artifact{&archive, cache->source_images(benchmark_source_name(archive.origin.artifact.source)) / archive.origin.cache_shard, source.archives.size() - 1})
          .second)
      invalid("duplicate physical shard in release catalog");
    }
   }
  for (auto& [edition, state] : releases) (void)requirement(state);
 }
 explicit Impl(std::span<const CoconutPhysicalImage> images, Cancellation cancel) : cancellation(cancel) {
  for (const auto& image : images) {
   throw_if_benchmark_cancelled(cancellation);
   (void)coconut_namespace_name(image.source);
   validate_coconut_physical_image(image);
   if (!namespaces[image.source].emplace(image.image_id, &image).second) invalid("duplicate physical member: " + image.member);
   standalone_shards[image.source][image.shard].insert(image.archive_identity);
  }
 }
 const CoconutPhysicalImage* find(CoconutImageNamespace source, std::uint64_t id) const noexcept {
  const auto space = namespaces.find(source);
  if (space == namespaces.end()) return nullptr;
  const auto found = space->second.find(id);
  return found == space->second.end() ? nullptr : found->second;
 }
 static CoconutPhysicalInputRequirement requirement(PhysicalRelease& state) {
  if (!state.input) {
   std::uint64_t workspace = 0;
   std::size_t descriptors = 0;
   for (const auto& [name, source] : state.sources)
    for (const auto* archive : source.archives) {
     workspace = std::max(workspace, archive->resolution_input.workspace_bytes);
     descriptors = std::max(descriptors, archive->resolution_input.descriptors);
    }
   state.input = CoconutPhysicalInputRequirement::archive(workspace, descriptors);
  }
  return *state.input;
 }
 CoconutPhysicalImage resolve(CoconutEdition edition, std::string_view identity, const CoconutRecord& record, const BenchmarkAllowance& parent, bool canonical_metadata) {
  const auto objects = admit_record(edition, record);
  if (!execution) {
   const CoconutPhysicalImage* match = nullptr;
   if (objects)
    match = find(objects->source, objects->id);
   else
    for (const auto source : coconut_release_sources(edition)) {
     if (const auto* found = find(source, record.image_id)) {
      if (match) invalid("ambiguous COCO train/unlabeled membership: " + record.file_name);
      match = found;
     }
    }
   if (!match)
    throw CoconutPhysicalMembershipError(
     edition, record.image_id, objects ? "missing physical Objects365 archive member: " + record.physical_stem : "missing physical COCO archive member: " + record.file_name);
   return *match;
  }
  auto& state = releases.at(edition);
  // The join and cursor share one release lock. Concurrent requests must check
  // the cache here, before either can reopen or republish the same image.
  std::unique_lock lock(state.mutex);
  if (state.identity != identity) {
   state.resolved.clear();
   state.identity = identity;
  }
  const auto logical_key = edition == CoconutEdition::XLarge ? record.image_id : record.source_ordinal;
  if (const auto found = state.resolved.find(logical_key); found != state.resolved.end()) return found->second;
  const bool coco_record = !objects;
  const auto preferred = edition == CoconutEdition::RelabeledValidation ? std::optional(CoconutImageNamespace::CocoValidation) : record.namespace_hint;
  const auto requested_id = objects ? objects->id : record.image_id;
  const auto eligible = coconut_release_sources(edition);
  AdmittedRecipeArchive* repair_candidate = nullptr;
  for (const bool preferred_pass : {true, false})
   for (const auto source_name : eligible) {
    if ((!coco_record && source_name != objects->source) || (preferred && source_name == *preferred) != preferred_pass) continue;
    auto& source = state.sources.at(source_name);
    for (auto* archive : source.archives)
     if (!repair_candidate || archive->structural_attempts < repair_candidate->structural_attempts) repair_candidate = archive;
    for (;;) {
     auto found = source.members.find(requested_id);
     if (found == source.members.end() && source.next == source.archives.size()) break;
     const bool known_member = found != source.members.end();
     auto& archive = *(found != source.members.end() ? found->second.archive : source.archives[source.next]);
     const auto& root = source.artifacts.at(archive.origin.shard).root;
     if (state.active != &archive) {
      state.release_reader();
      state.active = &archive;
     }
     try {
      if (!state.lease) {
       if (canonical_metadata && parent && !execution->try_resize_workspace(parent, 0)) throw std::logic_error("physical metadata retains unrelated workspace");
       state.lease = ArtifactLease::acquire_charged(cache->locks / (std::string(benchmark_source_name(archive.origin.artifact.source)) + "-" + archive.origin.cache_shard + ".images.lock"),
        cancellation, execution, requirement(state).lease_controls(), parent);
       if (canonical_metadata && parent) {
        const auto workspace = requirement(state).workspace_bytes();
        if (!execution->try_resize_workspace(parent, workspace)) {
         // Another sequence may own the transient grant while approaching this
         // release. Never wait for its bytes while holding the shared cursor.
         state.release_reader();
         lock.unlock();
         for (;;) {
          const auto observed = execution->admission_generation();
          if (execution->try_resize_workspace(parent, workspace)) break;
          execution->wait_for_admission_change(observed);
         }
         return resolve(edition, identity, record, parent, false);
        }
       }
      }
      if (known_member && found->second.conflict) throw BenchmarkArchiveError("conflicting requested physical image identity");
      const bool new_reader = !state.reader;
      if (!state.reader) {
       state.reader_parent = parent;
       state.reader = std::make_unique<BenchmarkArchive>(
        archive.download.path, execution, 64ULL << 20, state.lease->allowance(), 1, false, parent.bytes() >= archive.resolution_input.workspace_bytes ? parent : BenchmarkAllowance{});
      }
      auto& reader = *state.reader;
      bool located = found != source.members.end() && reader.seek(found->second.locator, cancellation);
      if (!located) {
       if (const auto cursor = source.scanned.find(&archive); cursor != source.scanned.end()) {
        const auto& location = source.members.at(cursor->second);
        if (new_reader || reader.position() != location.position) (void)reader.seek(location.locator, cancellation);
       }
      }
      while (!located && reader.next(cancellation)) {
       if (!reader.member().ends_with(".jpg")) continue;
       const auto id = benchmark_archive_image_candidate(reader.member());
       if (!id) continue;
       const auto [entry, inserted] = source.members.emplace(*id, PhysicalRoute{&archive, reader.member_position(), reader.position()});
       if (!inserted && (entry->second.archive != &archive || entry->second.position != reader.position())) entry->second.conflict = true;
       if (entry->second.conflict && (entry->second.consumed || *id == requested_id)) throw BenchmarkArchiveError("conflicting consumed physical image identity");
       for (const auto& [other_name, other_source] : state.sources) {
        if (other_name == source_name) continue;
        const auto other = other_source.members.find(*id);
        if (other != other_source.members.end() && (other->second.consumed || *id == requested_id)) throw BenchmarkArchiveError("ambiguous encountered physical namespaces");
       }
       located = *id == requested_id;
      }
      if (!located && known_member) throw BenchmarkArchiveError("known requested member disappeared from its archive generation");
      if (!located) {
       ++source.next;
       state.release_reader();
       continue;
      }
      const CoconutPhysicalImage physical{archive.origin.source, requested_id, archive.origin.shard, reader.member(), archive.download.identity};
      try {
       validate_coconut_physical_image(physical);
      } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
       throw BenchmarkArchiveError(error.what());
      }
      if (state.prepared_roots.insert(root).second) prepare_cached_image_directory(root);
      if (state.directory.get() < 0) {
       state.directory_allowance = execution->reserve(BenchmarkResources::handles(2, true), state.lease->allowance());
       const int descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
       if (descriptor < 0) throw common_io::errno_error("cannot open physical image cache directory", root.string());
       state.directory = common_io::FileHandle(descriptor);
      }
      const CachedImageValidator validate = [&](std::uint64_t, std::span<const std::uint8_t> encoded) {
       if (!has_complete_image_markers(encoded)) throw BenchmarkImageError("incomplete requested COCONut image");
       return state.decoder.read_header(encoded);
      };
      BenchmarkEncodedImage::Ptr input;
      try {
       if (source.artifacts.at(archive.origin.shard).cache_reusable)
        input = BenchmarkEncodedImage::open(state.directory.get(), requested_id, validate, cancellation, execution, reader.allowance(), execution->image_input(root, requested_id), 32ULL << 20);
      } catch (const InvalidImageError&) { /* The consumed source replaces this invalid cache entry below. */ }
      if (input)
       input = input->header_only();
      else {
       const auto encoded = reader.read(32ULL << 20, cancellation);
       BenchmarkImageHeader header;
       reader.cpu([&] { header = validate(requested_id, encoded); });
       StorageReservationPool destination(root, {}, storage);
       input = BenchmarkEncodedImage::publish(state.directory.get(), requested_id, encoded, header, cancellation, destination, execution, {}, false);
      }
      const auto header = input->header();
      execution->source_publication(root, state.lease)({requested_id, std::pair{header.width, header.height}, true, std::move(input)});
      auto& route = source.members.at(requested_id);
      route.consumed = true;
      route.locator = reader.member_position();
      const auto cursor = source.scanned.find(&archive);
      if (cursor == source.scanned.end() || source.members.at(cursor->second).position < route.position) source.scanned[&archive] = requested_id;
      state.resolved.insert_or_assign(logical_key, physical);
      return physical;
     } catch (const PhysicalArchiveFailure&) { throw; } catch (const BenchmarkArchiveError& error) { throw PhysicalArchiveFailure(archive, error.what()); } catch (const BenchmarkImageError& error) {
      throw PhysicalArchiveFailure(archive, error.what());
     } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
      throw PhysicalArchiveFailure(archive, error.what(), std::current_exception());
     }
    }
   }
  // Resolve again after each bounded replacement. A valid alternative source
  // must not be replaced merely because a preferred archive omitted this image.
  const auto missing = "missing requested physical archive member: " + record.file_name;
  if (repair_candidate) throw PhysicalArchiveFailure(*repair_candidate, missing);
  throw CoconutPhysicalMembershipError(edition, record.image_id, missing);
 }
};
CoconutPhysicalMembership::CoconutPhysicalMembership(std::span<AdmittedRecipeArchive> admitted, const CoconutRecipeCatalog& catalog, bool explicit_catalog, const BenchmarkCacheLayout& cache,
 StorageReservationPool& storage, BenchmarkCompilePipeline& execution, Cancellation cancellation)
    : impl_(std::make_unique<Impl>(admitted, catalog, explicit_catalog, cache, storage, execution, cancellation)) {}
CoconutPhysicalMembership::CoconutPhysicalMembership(std::span<const CoconutPhysicalImage> images, Cancellation cancellation) : impl_(std::make_unique<Impl>(images, cancellation)) {}
CoconutPhysicalMembership::~CoconutPhysicalMembership() = default;
const CoconutPhysicalImage* CoconutPhysicalMembership::find(CoconutImageNamespace source, std::uint64_t id) const noexcept { return impl_->find(source, id); }
CoconutPhysicalImage CoconutPhysicalMembership::resolve(CoconutEdition edition, std::string_view identity, const CoconutRecord& record, const BenchmarkAllowance& parent, bool canonical_metadata) const {
 return impl_->resolve(edition, identity, record, parent, canonical_metadata);
}
// CLEANUP-IGNORE: Requirement and publication queries share only guard/lookup/lock setup; their returned facts and admission operations differ.
CoconutPhysicalInputRequirement CoconutPhysicalMembership::input_requirement(CoconutEdition edition) const {
 if (!impl_->execution) return {};
 auto& state = impl_->releases.at(edition);
 const std::lock_guard lock(state.mutex);
 return Impl::requirement(state);
}
void CoconutPhysicalMembership::release_readers(CoconutEdition edition, const BenchmarkAllowance& producer) const {
 if (!impl_->execution) return;
 auto& state = impl_->releases.at(edition);
 const std::lock_guard lock(state.mutex);
 if (!producer || state.reader_parent.aliases(producer)) state.release_reader();
}
bool CoconutPhysicalMembership::eligible(CoconutEdition edition, const AdmittedRecipeArchive& archive) const {
 const auto release = impl_->releases.find(edition);
 if (release == impl_->releases.end()) return false;
 const auto source = release->second.sources.find(archive.origin.source);
 if (source == release->second.sources.end()) return false;
 const auto found = source->second.artifacts.find(archive.origin.shard);
 return found != source->second.artifacts.end() && found->second.archive == &archive;
}
void CoconutPhysicalDependencies::add(const CoconutPhysicalImage& image) {
 const auto [entry, inserted] = shards_.try_emplace(image.shard, image.archive_identity);
 if (!inserted && entry->second != image.archive_identity) invalid("component mixes physical artifact generations");
}
std::string CoconutPhysicalMembership::dependency_identity(CoconutEdition edition, CoconutImageNamespace source, const CoconutPhysicalDependencies& dependencies, bool current) const {
 nlohmann::json tuples = nlohmann::json::array();
 const auto& used = dependencies.shards_;
 const auto release = impl_->releases.find(edition);
 if (release != impl_->releases.end()) {
  const auto found = release->second.sources.find(source);
  if (found != release->second.sources.end()) {
   const auto& routes = found->second;
   const auto append = [&](const AdmittedRecipeArchive& archive) {
    tuples.push_back(
     {archive.origin.artifact.artifact_id, archive.origin.shard, current || used.empty() ? std::string_view(archive.download.identity) : std::string_view(used.at(archive.origin.shard))});
   };
   if (used.empty())
    for (const auto* archive : routes.archives) append(*archive);
   else {
    // Restore catalog order without another inventory or whole-catalog scan.
    std::vector<const Impl::PhysicalSource::Artifact*> selected;
    selected.reserve(used.size());
    for (const auto& [shard, identity] : used) {
     (void)identity;
     if (const auto route = routes.artifacts.find(shard); route != routes.artifacts.end()) selected.push_back(&route->second);
    }
    std::ranges::sort(selected, {}, &Impl::PhysicalSource::Artifact::order);
    for (const auto* route : selected) append(*route->archive);
   }
  }
 } else if (const auto found = impl_->standalone_shards.find(source); found != impl_->standalone_shards.end()) {
  const auto append = [&](const auto& entry) {
   const auto& [shard, identities] = entry;
   if (current || used.empty())
    for (const auto& identity : identities) tuples.push_back({shard, identity});
   else
    tuples.push_back({shard, used.at(shard)});
  };
  if (used.empty())
   for (const auto& entry : found->second) append(entry);
  else
   for (const auto& [shard, identity] : used) {
    (void)identity;
    if (const auto entry = found->second.find(shard); entry != found->second.end()) append(*entry);
   }
 }
 return nlohmann::json{{"domain", "coconut-physical-artifacts-v1"}, {"edition", edition}, {"source", source}, {"artifacts", std::move(tuples)}}.dump();
}
BenchmarkSourcePublication CoconutPhysicalMembership::label_publication(CoconutEdition edition, const CoconutPhysicalImage& physical) const {
 if (!impl_->execution) return {};
 auto& release = impl_->releases.at(edition);
 const std::lock_guard lock(release.mutex);
 const auto& source = release.sources.at(physical.source);
 const auto found = source.artifacts.find(physical.shard);
 if (found == source.artifacts.end()) throw CoconutPhysicalMembershipError(edition, physical.image_id, "normalized image has no admitted physical dependency");
 const auto& archive = *found->second.archive;
 if (archive.origin.shard != physical.shard || archive.download.identity != physical.archive_identity)
  throw CoconutPhysicalMembershipError(edition, physical.image_id, "normalized image physical dependency retired");
 const auto& root = found->second.root;
 // Native support owns its bytes. Retain the existing generation/attempt ticket,
 // without extending the retired physical reader's descriptor or lock custody.
 return impl_->execution->source_publication(root, {}, physical.image_id);
}
void CoconutPhysicalMembership::withdraw(const AdmittedRecipeArchive& archive) {
 for (auto& [edition, state] : impl_->releases) {
  if (!eligible(edition, archive)) continue;
  const std::lock_guard lock(state.mutex);
  if (state.active == &archive) state.release_reader();
  auto& source = state.sources.at(archive.origin.source);
  source.artifacts.at(archive.origin.shard).cache_reusable = false;
  source.scanned.erase(&archive);
  std::erase_if(state.resolved, [&](const auto& entry) {
   const auto& physical = entry.second;
   return physical.source == archive.origin.source && physical.shard == archive.origin.shard && physical.archive_identity == archive.download.identity;
  });
  std::erase_if(source.members, [&](const auto& entry) { return entry.second.archive == &archive; });
  source.next = std::min(source.next, static_cast<std::size_t>(std::ranges::find(source.archives, &archive) - source.archives.begin()));
  state.input.reset();
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
