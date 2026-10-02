#include "src/backend/data/benchmark/coconut/detail/coconut_annotation_identity.h"
#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/file_work_queue.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include <system_error>
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include <tuple>
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotations.h"
#include <exception>
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory_io.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/pch_std.h"
#include <iterator>
namespace mmltk::backend::data::benchmark_internal {
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileExecution;
using mmltk::backend::data::detail::CompileTaskKind;
class CoconutComponentBacking final : public CoconutComponentMetadata {
public:
 NormalizedAnnotationReadView index;
 std::shared_ptr<const std::vector<CoconutInventoryImage>> inventory;
 std::vector<CoconutRecoveryImage> recovery;
 CoconutPhysicalDependencies dependencies;
 std::vector<std::string> image_dependencies;
 std::vector<std::uint64_t> rejected_images, compiled_ordinal_bases, incomplete_ordinal_images;
 bool source_extent_complete = false;
 std::uint64_t source_rows = 0, source_segments = 0;
 std::shared_ptr<CoconutInventorySeal> seal;
 mutable std::once_flag full_admission;
 mutable std::mutex completion_mutex;
 mutable std::shared_ptr<const NormalizedAnnotationCompletion> completion;
 bool metadata_only = false;
 static CoconutComponent finish(std::shared_ptr<CoconutComponentBacking>, NormalizedAnnotationIndex, bool, mmltk::common::concurrency::CancellationObservation, const std::filesystem::path&,
  detail::StorageReservationPool*, const CoconutComponent* reuse = nullptr, detail::FileWorkQueue* = nullptr, const CompileAllowance& = {}, const BenchmarkTraceSink& = {});
 static CoconutComponent loaded(std::shared_ptr<CoconutComponentBacking> backing) { return CoconutComponent(backing, backing->index, backing->seal, false); }
};
namespace {
using Json = nlohmann::json;
using Cancellation = mmltk::common::concurrency::CancellationObservation;
[[noreturn]] void invalid(std::string_view detail) { throw CoconutInputError("COCONut: " + std::string(detail)); }
std::string component_input_identity(std::string_view annotations, CoconutImageNamespace source, std::string_view physical, std::uint32_t policy, std::string_view original) {
 const auto material =
  nlohmann::json{{"domain", "coconut-component-input-v2"}, {"annotations", annotations}, {"source", source}, {"physical", physical}, {"recovery_policy", policy}, {"original", original}}.dump();
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size())));
}
struct PhysicalKey {
 CoconutImageNamespace source;
 std::uint64_t id;
 bool operator==(const PhysicalKey&) const = default;
};
struct PhysicalKeyHash {
 std::size_t operator()(PhysicalKey key) const noexcept { return std::hash<std::uint64_t>{}(key.id) ^ (static_cast<std::size_t>(key.source) * 0x9e3779b97f4a7c15ULL); }
};
PhysicalKey physical_key(CoconutImageNamespace source, std::uint64_t id) { return {source, id}; }
// Both import assembly and the final artifact join sort by physical ID within
// one namespace. Withdrawal joins and rejection callbacks stay on the controller;
// they retire exact claims before any later component admission.
template <class Row, class Inventory, class Dependency, class Reject>
void settle_annotation_identities(std::vector<Row>& rows, Inventory inventory, Dependency dependency, Reject reject, bool local_rejection, const CoconutPhysicalMembership& membership,
 BenchmarkCompilePipeline* execution, const CompileAllowance& allowance, Cancellation cancellation) {
 std::vector<std::pair<std::size_t, std::size_t>> ambiguous;
 const auto inspect = [&](std::size_t) {
  std::ranges::sort(rows, {}, [&](const Row& row) { return inventory(row).physical.image_id; });
  for (std::size_t first = 0; first < rows.size();) {
   throw_if_benchmark_cancelled(cancellation);
   const auto& image = inventory(rows[first]).physical;
   auto end = first + 1;
   while (end < rows.size() && inventory(rows[end]).physical.image_id == image.image_id) ++end;
   if (end != first + 1) {
    if (!local_rejection) invalid("duplicate annotation physical identity: " + image.member);
    ambiguous.emplace_back(first, end);
   }
   first = end;
  }
 };
 if (execution)
  execution->execution().run(CompileTaskKind::Metadata, {}, inspect, allowance);
 else
  inspect(0);
 for (const auto [first, end] : ambiguous)
  for (auto i = first; i < end; ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto& row = rows[i];
   const auto& physical = inventory(row).physical;
   const std::string_view input = dependency(row);
   if (execution && !input.empty()) {
    const auto root = membership.identity_root(physical.source);
    execution->retire_labels(root, physical.image_id, input);
    execution->retire_labels(root, physical.image_id, std::string(input) + ".provisional");
   }
   reject(row, "ambiguous annotation physical identity");
  }
 if (ambiguous.empty()) return;
 const auto compact = [&](std::size_t) {
  std::size_t kept = 0, first = 0;
  const auto retain = [&](std::size_t end) {
   while (first < end) {
    throw_if_benchmark_cancelled(cancellation);
    if (kept != first) rows[kept] = std::move(rows[first]);
    ++kept;
    ++first;
   }
  };
  for (const auto [begin, end] : ambiguous) {
   retain(begin);
   first = end;
  }
  retain(rows.size());
  rows.resize(kept);
 };
 if (execution)
  execution->execution().run(CompileTaskKind::Metadata, {}, compact, allowance);
 else
  compact(0);
}
}  // namespace
namespace {
InventoryHeader component_header(const CoconutComponentMetadata& component, std::size_t count) {
 InventoryHeader header;
 header.component = true;
 header.input_identity = component.input_identity;
 header.edition = component.edition;
 header.source = component.source;
 header.count = count;
 return header;
}
std::shared_ptr<CoconutInventorySeal> seal_inventory(const CoconutComponent& component, Cancellation cancellation, const std::filesystem::path& directory, detail::StorageReservationPool* storage) {
 auto seal = std::make_shared<CoconutInventorySeal>();
 seal->directory = directory.empty() ? std::filesystem::temp_directory_path() : directory;
 std::filesystem::create_directories(seal->directory);
 const auto destination = seal->directory / ".coconut-inventory";
 detail::StorageReservationPool reservations(destination, {}, storage);
 seal->staged = detail::StagedArtifact::create(reservations, destination, 0, "COCONut inventory staging");
 InventoryOutput output(&seal->staged, cancellation);
 const CoconutComponentMetadata metadata{component.edition(), component.source(), component.input_identity(), component.recovery_policy(), std::string(component.original_annotation_identity())};
 seal->identity = encode_inventory(output, component_header(metadata, component.index().image_count()), component.inventory(), cancellation, component.recovery_policy(),
  component.original_annotation_identity(), component.recovery());
 seal->staged.file().sync_data();
 seal->capture(seal->staged.file());
 seal->staged.close();
 return seal;
}
void admit_component_metadata(const CoconutComponentMetadata& component, const NormalizedAnnotationMetadata& index, std::size_t images, std::size_t inventory, std::size_t recovery) {
 (void)coconut_namespace_name(component.source);
 (void)coconut_release_component(component.edition);
 if (component.input_identity.empty() || images != inventory || index.source != coconut_index_source(component.source) || index.split != coconut_component_split(component.edition, component.source))
  invalid("component index/inventory admission mismatch");
 if (component.recovery_policy) {
  if (component.recovery_policy != kCoconutRecoveryPolicy || (component.source != CoconutImageNamespace::CocoTrain && component.source != CoconutImageNamespace::CocoValidation) ||
      recovery != inventory)
   invalid("invalid recovery source or image count");
  (void)mmltk::common::io::parse_sha256_hex(component.original_annotation_identity);
 } else if (!component.original_annotation_identity.empty() || recovery)
  invalid("unexpected recovery facts");
}
// Runs in the owning assembly/decode loop. The ordinal set is temporary join
// evidence, never a second inventory or a retained selection.
class ComponentInventoryAdmission final {
public:
 explicit ComponentInventoryAdmission(CoconutImageNamespace source) : source_(source) {}
 void image(const CoconutInventoryImage& row, const NormalizedImage& normalized, bool physical_admitted = false, bool ordinal_complete = true) {
  if (!physical_admitted) validate_coconut_physical_image(row.physical);
  if (row.physical.source != source_ || row.physical.image_id != normalized.source_image_id || row.physical.shard != normalized.source_shard ||
      (ordinal_complete && !ordinals_.insert(row.source_ordinal).second) || (previous_ && row.physical.image_id <= *previous_))
   invalid("invalid component image inventory");
  previous_ = row.physical.image_id;
 }

private:
 CoconutImageNamespace source_;
 std::optional<std::uint64_t> previous_;
 std::unordered_set<std::uint64_t> ordinals_;
};
void admit_recovery_image(const CoconutRecoveryImage& fact, const NormalizedImage& image) {
 if (fact.image_id != image.source_image_id || fact.unresolved > 65535U || fact.unresolved != fact.omissions.size() || fact.objects.size() > image.box_count ||
     fact.objects.size() > 65535U - fact.omissions.size())
  invalid("invalid image recovery facts");
}
class ComponentRecoveryAdmission final {
public:
 explicit ComponentRecoveryAdmission(const CoconutRecoveryImage* fact) : fact_(fact) {}
 void box(const NormalizedBox& box) {
  if (fact_ && (!fact_->objects.empty() || !fact_->omissions.empty())) by_annotation_.emplace(box.annotation_id, box);
 }
 void finish() const {
  if (!fact_) return;
  std::unordered_set<std::uint64_t> originals, annotations;
  for (const auto& object : fact_->omissions)
   if (object.original_annotation_id != 0 || object.source_category_id == 0 || !annotations.insert(object.annotation_id).second || by_annotation_.contains(object.annotation_id))
    invalid("invalid omitted object identity");
  for (const auto& object : fact_->objects) {
   const auto found = by_annotation_.find(object.annotation_id);
   if (!originals.insert(object.original_annotation_id).second || !annotations.insert(object.annotation_id).second || found == by_annotation_.end() ||
       found->second.source_ordinal != object.source_ordinal || found->second.source_category_id != object.source_category_id)
    invalid("invalid recovered object identity");
  }
 }

private:
 const CoconutRecoveryImage* fact_;
 std::unordered_map<std::uint64_t, NormalizedBox> by_annotation_;
};
}  // namespace
detail::CompileResources coconut_component_assembly_resources(std::uint64_t images, std::uint64_t recovery_join_boxes) {
 // Layout/slice/cursor capacity survives assembly and inventory I/O. Only one
 // image's recovery join is live; the same descriptor serves each physical close.
 const auto join = common::math::checked_multiply(recovery_join_boxes, std::uint64_t{256}, "COCONut recovery join workspace overflow");
 const auto bytes = common::math::checked_add(common::math::checked_multiply(images, std::uint64_t{6 * sizeof(CoconutComponentImage)}, "COCONut merge workspace overflow"),
  common::math::checked_add(join, std::uint64_t{65536}, "COCONut merge workspace overflow"), "COCONut merge workspace overflow");
 return {bytes, 1};
}
std::optional<CoconutComponent> assemble_coconut_component(CoconutComponentMetadata header, std::vector<CoconutComponentImage>& rows, AnnotationRejectCounts rejected, bool metadata_only,
 std::uint64_t source_rows, std::optional<std::uint64_t> source_extent, std::span<const std::size_t> source_ordinals, std::span<const CoconutComponent> reusable_components,
 const CoconutPhysicalMembership& physical, const CoconutMaskRecovery* recovery, const std::filesystem::path& directory, Cancellation cancellation, detail::StorageReservationPool* storage,
 BenchmarkCompilePipeline* execution, const CompileAllowance& merge_allowance, bool local_rejection, const CoconutImageRejection& reject, const BenchmarkTraceSink& trace) {
 if (rows.empty()) return std::nullopt;
 auto producer = execution ? execution->execution().open_class(detail::CompileWorkClass::Annotations) : CompileExecution::Producer{};
 const auto source = header.source;
 settle_annotation_identities(rows, [](const auto& row) -> const CoconutInventoryImage& { return *row.inventory; },
  [](const auto& row) -> std::string_view { return row.dependency ? std::string_view(*row.dependency) : std::string_view{}; }, [&](const auto& row, std::string_view reason) {
  if (reject) reject(*row.inventory, reason);
 }, local_rejection, physical, execution, merge_allowance, cancellation);
 if (rows.empty()) return std::nullopt;
 auto sealed = std::make_shared<CoconutComponentBacking>();
 auto inventory = std::make_shared<std::vector<CoconutInventoryImage>>();
 const CoconutComponent* reusable = nullptr;
 std::optional<NormalizedAnnotationAssembler> assembly;
 std::vector<NormalizedAnnotationAssembler::Slice> slices;
 NormalizedAnnotationIndex normalized;
 const auto plan = [&](std::size_t) {
  static_cast<CoconutComponentMetadata&>(*sealed) = header;
  sealed->source_rows = source_rows;
  sealed->source_extent_complete = source_extent.has_value();
  sealed->source_segments = source_extent.value_or(0);
  if (!sealed->recovery_policy && !reusable_components.empty())
   for (const auto& candidate : reusable_components)
    if (!candidate.index().selected() && !candidate.recovery_policy() && candidate.edition() == sealed->edition && candidate.source() == source &&
        candidate.matches_inputs(header.annotation_input_identity, physical, metadata_only ? nullptr : recovery) && candidate.index().image_count() == rows.size()) {
     reusable = &candidate;
     break;
    }
  inventory->reserve(rows.size());
  sealed->image_dependencies.reserve(rows.size());
  if (sealed->recovery_policy) sealed->recovery.reserve(rows.size());
  ComponentInventoryAdmission inventory_admission(source);
  NormalizedAnnotationMetadata metadata = *rows.front().metadata;
  metadata.rejected = rejected;
  if (!metadata_only) {
   assembly.emplace(metadata, cancellation, directory, storage);
   slices.reserve(rows.size());
  }
  NormalizedAnnotationBuilder membership;
  static_cast<NormalizedAnnotationMetadata&>(membership) = metadata;
  if (metadata_only) membership.images.reserve(rows.size());
  for (const auto& row : rows) {
   throw_if_benchmark_cancelled(cancellation);
   const auto& image = *row.image;
   inventory->push_back(*row.inventory);
   sealed->compiled_ordinal_bases.push_back(row.segment_base);
   if (!row.required_labels_valid) sealed->rejected_images.push_back(image.source_image_id);
   if (!row.ordinals_complete) sealed->incomplete_ordinal_images.push_back(image.source_image_id);
   auto& inventory_row = inventory->back();
   if (!metadata_only && !source_ordinals.empty()) {
    if (inventory_row.source_ordinal >= source_ordinals.size()) invalid("source ordinal exceeds canonical placement");
    inventory_row.source_ordinal = source_ordinals[inventory_row.source_ordinal];
   }
   if (reusable && inventory_row != reusable->inventory_image(inventory->size() - 1)) reusable = nullptr;
   inventory_admission.image(inventory_row, image, reusable != nullptr, row.ordinals_complete);
   sealed->dependencies.add(row.inventory->physical);
   sealed->image_dependencies.push_back(
    row.dependency ? *row.dependency : coconut_image_input_identity(sealed->annotation_input_identity, row.inventory->physical, sealed->original_annotation_identity));
   if (sealed->recovery_policy) {
    auto recovered_image = row.component->recovery_policy ? *row.recovery : CoconutRecoveryImage{image.source_image_id, 0, {}};
    for (auto* objects : {&recovered_image.objects, &recovered_image.omissions})
     for (auto& object : *objects) object.source_ordinal = mmltk::common::math::checked_add(object.source_ordinal, row.segment_base, "COCONut recovery ordinal overflow");
    admit_recovery_image(recovered_image, image);
    sealed->recovery.push_back(std::move(recovered_image));
   }
   ComponentRecoveryAdmission recovery_admission(sealed->recovery_policy ? &sealed->recovery.back() : nullptr);
   if (metadata_only)
    membership.images.push_back(image);
   else {
    for (auto box : row.boxes) {
     box.source_ordinal = common::math::checked_add(box.source_ordinal, row.segment_base, "COCONut segment ordinal overflow");
     if (!row.ordinals_complete) box.flags |= kAnnotationOrdinalIncomplete;
     recovery_admission.box(box);
    }
    recovery_admission.finish();
    auto slice = row.admission ? NormalizedAnnotationAssembler::Slice::admitted(*row.admission, 0, row.segment_base, !row.ordinals_complete)
                               : NormalizedAnnotationAssembler::Slice{image, row.boxes, row.runs, row.segment_base, !row.ordinals_complete};
    // Keep the supplied row authoritative. Only the exact sealed header and
    // payload ranges may reuse admission; a changed view validates normally.
    slice.image = image;
    slice.boxes = row.boxes;
    slice.runs = row.runs;
    slices.push_back(std::move(slice));
   }
  }
  if (metadata_only) normalized = seal_normalized_annotation_metadata(std::move(membership));
  // CLEANUP-IGNORE: Distinct component and shard planning bodies only share inventory publication and the direct metadata admission call.
  sealed->inventory = std::move(inventory);
 };
 if (execution)
  execution->execution().run(CompileTaskKind::Metadata, {}, plan, merge_allowance);
 else
  plan(0);
 if (assembly) {
  assembly->gather(slices, execution ? &execution->execution() : nullptr, execution ? &execution->files() : nullptr, merge_allowance, trace);
  normalized = assembly->finish(trace);
 }
 const auto identify = [&](std::size_t) {
  sealed->input_identity = coconut_component_input_identity(header.annotation_input_identity, source, metadata_only ? nullptr : recovery, &physical, header.edition, sealed->dependencies);
 };
 if (execution)
  execution->execution().run(CompileTaskKind::Metadata, {}, identify, merge_allowance);
 else
  identify(0);
 // The mapped normalized output closed its file before returning. Inventory
 // sealing uses the caller's same retained output descriptor grant.
 return CoconutComponentBacking::finish(
  std::move(sealed), std::move(normalized), metadata_only, cancellation, directory, storage, reusable, execution ? &execution->files() : nullptr, merge_allowance, trace);
}
CoconutComponent::CoconutComponent(std::shared_ptr<const CoconutComponentBacking> backing, NormalizedAnnotationReadView index, std::shared_ptr<CoconutInventorySeal> seal, bool membership)
    : backing_(std::move(backing)), index_(std::move(index)), seal_(std::move(seal)), membership_(membership), original_generation_(backing_->original_generation) {}
CoconutEdition CoconutComponent::edition() const noexcept { return backing_->edition; }
CoconutImageNamespace CoconutComponent::source() const noexcept { return backing_->source; }
const std::string& CoconutComponent::input_identity() const noexcept { return backing_->input_identity; }
BenchmarkLabelInput CoconutComponent::labels(std::size_t image) const {
 if (membership_ || backing_->metadata_only) invalid("membership has no normalized labels");
 return BenchmarkLabelInput(index_, image);
}
CoconutComponent CoconutComponent::with_original(const CoconutOriginalInput& original) const {
 if (membership_ || backing_->metadata_only || !original.terminal || !original.generation || (source() != CoconutImageNamespace::CocoTrain && source() != CoconutImageNamespace::CocoValidation))
  invalid("component has no admitted original generation");
 const auto identity = original.originals ? original.originals->identity(source()) : std::string_view{};
 if (identity != backing_->original_annotation_identity) invalid("component has a different original input");
 auto result = *this;
 result.original_generation_ = original.generation;
 return result;
}
bool CoconutComponent::source_extent_complete() const { return backing_->source_extent_complete; }
bool CoconutComponent::source_ordinals_complete(std::size_t image) const { return !std::ranges::binary_search(backing_->incomplete_ordinal_images, index_.image(image).source_image_id); }
std::pair<std::uint64_t, std::uint64_t> CoconutComponent::source_extent() const { return {backing_->source_rows, backing_->source_segments}; }
bool CoconutComponent::required_labels_valid(std::size_t image) const { return !std::ranges::binary_search(backing_->rejected_images, index_.image(image).source_image_id); }
std::uint64_t CoconutComponent::compiled_ordinal_base(std::size_t image) const {
 return backing_->compiled_ordinal_bases.empty() ? 0 : backing_->compiled_ordinal_bases.at(index_.source_position(image));
}
const std::string& CoconutComponent::image_input_identity(std::size_t image) const { return backing_->image_dependencies.at(index_.source_position(image)); }
bool CoconutComponent::matches_inputs(std::string_view annotations, const CoconutPhysicalMembership& physical, const CoconutMaskRecovery* recovery, bool current_physical) const {
 return input_identity() == coconut_component_input_identity(annotations, source(), recovery, &physical, edition(), backing_->dependencies, current_physical);
}
bool CoconutComponent::matches_physical(const CoconutPhysicalMembership& physical) const {
 return physical.dependency_identity(edition(), source(), backing_->dependencies) == physical.dependency_identity(edition(), source(), backing_->dependencies, false);
}
std::uint32_t CoconutComponent::recovery_policy() const noexcept { return membership_ ? 0 : backing_->recovery_policy; }
std::string_view CoconutComponent::original_annotation_identity() const noexcept { return membership_ ? std::string_view{} : backing_->original_annotation_identity; }
const CoconutInventoryImage& CoconutComponent::inventory_image(std::size_t position) const { return backing_->inventory->at(index_.source_position(position)); }
const CoconutRecoveryImage& CoconutComponent::recovery_image(std::size_t position) const {
 if (!recovery_policy()) throw std::out_of_range("COCONut membership has no recovery claims");
 return backing_->recovery.at(index_.source_position(position));
}
std::shared_ptr<const NormalizedAnnotationCompletion> CoconutComponent::completion() const {
 const std::lock_guard lock(backing_->completion_mutex);
 return backing_->completion;
}
CoconutComponent CoconutComponent::membership() const {
 auto result = *this;
 result.membership_ = true;
 return result;
}
CoconutComponent CoconutComponent::select_images(std::vector<std::size_t> positions, Cancellation cancellation) const {
 auto result = *this;
 result.index_ = index_.select_images(std::move(positions), std::nullopt, cancellation);
 if (result.index_.image_count() == index_.image_count()) return result;
 // Selection identities include the underlying recovery facts even when the
 // caller asks only for membership. Neither selection nor publication mutates
 // the full backing's completion or once-only admission.
 const bool membership = result.membership_;
 result.membership_ = false;
 result.seal_ = seal_inventory(result, cancellation, seal_->directory, nullptr);
 result.membership_ = membership;
 result.index_.annotation_sha256 = result.seal_->identity;
 return result;
}
CoconutComponent CoconutComponentBacking::finish(std::shared_ptr<CoconutComponentBacking> backing, NormalizedAnnotationIndex normalized, bool metadata_only, Cancellation cancellation,
 const std::filesystem::path& directory, detail::StorageReservationPool* storage, const CoconutComponent* reuse, detail::FileWorkQueue* files, const CompileAllowance& allowance,
 const BenchmarkTraceSink& trace) {
 admit_component_metadata(*backing, normalized, normalized.images.size(), backing->inventory->size(), backing->recovery.size());
 trace_benchmark_event(
  trace, "benchmark.annotations.inventory_started", [&] { return nlohmann::json{{"source", backing->source}, {"images", normalized.images.size()}, {"reused", reuse != nullptr}}; });
 const auto expected_identity = std::exchange(normalized.annotation_sha256, {});
 normalized.completion.reset();
 backing->index = NormalizedAnnotationReadView(normalized);
 backing->metadata_only = metadata_only;
 if (reuse) {
  backing->inventory = reuse->backing_->inventory;
  backing->seal = reuse->seal_;
 } else {
  const auto seal = [&, allowance] { backing->seal = seal_inventory(loaded(backing), cancellation, directory, storage); };
  if (files)
   files->submit({.work = seal}).wait();
  else
   seal();
 }
 if (!expected_identity.empty() && expected_identity != backing->seal->identity) invalid("component inventory/index identity mismatch");
 trace_benchmark_event(
  trace, "benchmark.annotations.inventory_sealed", [&] { return nlohmann::json{{"source", backing->source}, {"images", normalized.images.size()}, {"identity", backing->seal->identity}}; });
 normalized.annotation_sha256 = backing->seal->identity;
 backing->index = NormalizedAnnotationReadView(std::move(normalized));
 if (!metadata_only) std::call_once(backing->full_admission, [] {});
 auto result = loaded(std::move(backing));
 return metadata_only ? result.membership() : result;
}
CoconutComponent CoconutComponentBuilder::finish(Cancellation cancellation, const std::filesystem::path& directory, detail::StorageReservationPool* storage) && {
 admit_component_metadata(*this, index, index.images.size(), inventory.size(), recovery.size());
 auto backing = std::make_shared<CoconutComponentBacking>();
 static_cast<CoconutComponentMetadata&>(*backing) = std::move(static_cast<CoconutComponentMetadata&>(*this));
 NormalizedAnnotationAssembler assembly(index, index.images.size(), index.boxes.size(), index.mask_rle_pairs.size(), cancellation);
 ComponentInventoryAdmission admission(backing->source);
 std::size_t boxes = 0, runs = 0;
 for (std::size_t position = 0; position < index.images.size(); ++position) {
  throw_if_benchmark_cancelled(cancellation);
  const auto& image = index.images[position];
  admission.image(inventory[position], image);
  backing->dependencies.add(inventory[position].physical);
  backing->image_dependencies.push_back(coconut_image_input_identity(backing->annotation_input_identity, inventory[position].physical, backing->original_annotation_identity));
  if (image.first_box != boxes || image.box_count > index.boxes.size() - boxes) invalid("invalid component box extent");
  const auto* facts = backing->recovery_policy ? &recovery[position] : nullptr;
  if (facts) admit_recovery_image(*facts, image);
  ComponentRecoveryAdmission join(facts);
  assembly.begin_image(image);
  for (const auto& box : std::span(index.boxes).subspan(boxes, image.box_count)) {
   if (box.mask_rle_offset != runs || box.mask_rle_pairs > index.mask_rle_pairs.size() - runs) invalid("invalid component mask extent");
   join.box(box);
   assembly.append_box(box, std::span(index.mask_rle_pairs).subspan(runs, box.mask_rle_pairs));
   runs += box.mask_rle_pairs;
  }
  join.finish();
  boxes += image.box_count;
 }
 if (boxes != index.boxes.size() || runs != index.mask_rle_pairs.size()) invalid("component has unreferenced normalized records");
 backing->inventory = std::make_shared<const std::vector<CoconutInventoryImage>>(std::move(inventory));
 backing->recovery = std::move(recovery);
 return CoconutComponentBacking::finish(std::move(backing), assembly.finish(), false, cancellation, directory, storage);
}
std::string coconut_component_input_identity(std::string_view base, CoconutImageNamespace source, const CoconutMaskRecovery* recovery, const CoconutPhysicalMembership* physical,
 CoconutEdition edition, const CoconutPhysicalDependencies& dependencies, bool current) {
 const auto original = recovery ? recovery->original_identity(source) : std::string_view{};
 return component_input_identity(
  base, source, physical ? physical->dependency_identity(edition, source, dependencies, current) : std::string{}, original.empty() ? 0 : kCoconutRecoveryPolicy, original);
}
std::vector<CoconutComponent> merge_coconut_components(std::span<const CoconutComponentPart> parts, std::string_view identity, const CoconutPhysicalMembership& physical,
 const std::filesystem::path& directory, Cancellation cancellation, detail::StorageReservationPool* storage, BenchmarkCompilePipeline* execution, const CoconutImageRejection& rejected,
 const BenchmarkTraceSink& trace) {
 struct Row {
  const CoconutComponent* component;
  std::size_t position;
  std::optional<std::uint64_t> rows, segments;
 };
 std::uint64_t image_count = 0;
 for (const auto& part : parts)
  for (const auto& component : part.components) image_count = common::math::checked_add(image_count, std::uint64_t{component.index().image_count()}, "COCONut merge image count overflow");
 if (!image_count) return {};
 auto producer = execution ? execution->execution().open_class(detail::CompileWorkClass::Annotations) : CompileExecution::Producer{};
 // Source views remain owned by parts through planning, mapped preparation,
 // gather and sealing. One descriptor promise serves the sequential outputs.
 auto merge_allowance = execution ? execution->execution().reserve(coconut_component_assembly_resources(image_count)) : CompileAllowance{};
 std::map<CoconutImageNamespace, std::vector<Row>> sources;
 const auto collect = [&](std::size_t) {
  for (const auto& part : parts)
   for (const auto& component : part.components)
    for (std::size_t i = 0; i < component.index().image_count(); ++i) {
     throw_if_benchmark_cancelled(cancellation);
     sources[component.source()].push_back({&component, i, part.row_base, part.segment_base});
    }
 };
 if (execution)
  execution->execution().run(CompileTaskKind::Metadata, {}, collect, merge_allowance);
 else
  collect(0);
 std::vector<CoconutComponent> result;
 for (auto& [source, rows] : sources) {
  settle_annotation_identities(rows, [](const Row& row) -> const CoconutInventoryImage& { return row.component->inventory_image(row.position); },
   [](const Row& row) -> std::string_view { return row.component->image_input_identity(row.position); }, [&](const Row& row, std::string_view reason) {
   if (rejected) rejected(row.component->inventory_image(row.position), reason);
  }, true, physical, execution, merge_allowance, cancellation);
  if (rows.empty()) continue;
  const auto& first = *rows.front().component;
  auto backing = std::make_shared<CoconutComponentBacking>();
  std::optional<NormalizedAnnotationAssembler> assembled;
  std::vector<NormalizedAnnotationAssembler::Slice> slices;
  const auto plan = [&](std::size_t) {
   static_cast<CoconutComponentMetadata&>(*backing) = static_cast<const CoconutComponentMetadata&>(*first.backing_);
   backing->original_generation = first.original_generation();
   backing->annotation_input_identity = identity;
   backing->source_extent_complete = std::ranges::all_of(parts, [](const auto& part) {
    return part.row_base && part.segment_base && (part.source_extent || (!part.components.empty() && std::ranges::all_of(part.components, &CoconutComponent::source_extent_complete)));
   });
   if (backing->source_extent_complete)
    for (const auto& part : parts) {
     auto extent = part.source_extent.value_or(std::pair<std::uint64_t, std::uint64_t>{});
     if (!part.source_extent)
      for (const auto& component : part.components) {
       extent.first = std::max(extent.first, component.source_extent().first);
       extent.second = std::max(extent.second, component.source_extent().second);
      }
     const auto [count, segments] = extent;
     backing->source_rows = std::max(backing->source_rows, mmltk::common::math::checked_add(*part.row_base, count, "COCONut source row extent overflow"));
     backing->source_segments = std::max(backing->source_segments, mmltk::common::math::checked_add(*part.segment_base, segments, "COCONut source segment extent overflow"));
    }
   NormalizedAnnotationMetadata metadata = first.index();
   metadata.annotation_sha256.clear();
   metadata.completion.reset();
   metadata.rejected = {};
   for (const auto& part : parts)
    for (const auto& component : part.components)
     if (component.source() == source)
      mmltk::frameworks::reflection::visit_materialized_members<AnnotationRejectCounts>([&]<class Declaration>(const auto&) {
       metadata.rejected.*Declaration::pointer =
        mmltk::common::math::checked_add(metadata.rejected.*Declaration::pointer, component.index().rejected.*Declaration::pointer, "COCONut rejected count overflow");
      });
   assembled.emplace(metadata, cancellation, directory, storage);
   slices.reserve(rows.size());
   auto inventory = std::make_shared<std::vector<CoconutInventoryImage>>();
   inventory->reserve(rows.size());
   for (const auto& row : rows) {
    throw_if_benchmark_cancelled(cancellation);
    const auto& component = *row.component;
    if (component.recovery_policy() != first.recovery_policy() || component.original_annotation_identity() != first.original_annotation_identity() ||
        component.original_generation() != first.original_generation())
     throw CoconutOriginalChanged(source);
    auto physical_row = component.inventory_image(row.position);
    const auto segment_base = component.source_ordinals_complete(row.position) ? row.segments : std::nullopt;
    const bool complete = row.rows && segment_base;
    if (row.rows) physical_row.source_ordinal = mmltk::common::math::checked_add(physical_row.source_ordinal, *row.rows, "COCONut shard row ordinal overflow");
    if (!complete) backing->incomplete_ordinal_images.push_back(physical_row.physical.image_id);
    inventory->push_back(physical_row);
    backing->dependencies.add(physical_row.physical);
    backing->image_dependencies.push_back(component.image_input_identity(row.position));
    backing->compiled_ordinal_bases.push_back(mmltk::common::math::checked_add(component.compiled_ordinal_base(row.position), segment_base.value_or(0), "COCONut compiled ordinal overflow"));
    if (!component.required_labels_valid(row.position)) backing->rejected_images.push_back(physical_row.physical.image_id);
    slices.push_back(NormalizedAnnotationAssembler::Slice::admitted(component.index().storage(), component.index().source_position(row.position), segment_base.value_or(0), !complete));
    if (backing->recovery_policy) {
     auto recovery = component.recovery_image(row.position);
     for (auto* objects : {&recovery.objects, &recovery.omissions})
      for (auto& object : *objects)
       if (segment_base) object.source_ordinal = mmltk::common::math::checked_add(object.source_ordinal, *segment_base, "COCONut shard recovery ordinal overflow");
     backing->recovery.push_back(std::move(recovery));
    }
   }
   backing->inventory = std::move(inventory);
  };
  if (execution)
   execution->execution().run(CompileTaskKind::Metadata, {}, plan, merge_allowance);
  else
   plan(0);
  assembled->gather(slices, execution ? &execution->execution() : nullptr, execution ? &execution->files() : nullptr, merge_allowance, trace);
  const auto identify = [&](std::size_t) {
   backing->input_identity =
    component_input_identity(identity, source, physical.dependency_identity(first.edition(), source, backing->dependencies), backing->recovery_policy, backing->original_annotation_identity);
  };
  if (execution)
   execution->execution().run(CompileTaskKind::Metadata, {}, identify, merge_allowance);
  else
   identify(0);
  result.push_back(
   CoconutComponentBacking::finish(std::move(backing), assembled->finish(trace), false, cancellation, directory, storage, nullptr, execution ? &execution->files() : nullptr, merge_allowance, trace));
 }
 return result;
}
std::uint64_t reconcile_coconut_extensions(std::vector<CoconutComponent>& components, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 auto replacement = components;
 std::unordered_set<PhysicalKey, PhysicalKeyHash> large, xlarge;
 for (const auto& component : components)
  if (component.edition() == CoconutEdition::Large) {
   for (const auto& image : component.inventory()) {
    throw_if_benchmark_cancelled(cancellation);
    if (!large.insert(physical_key(image.physical.source, image.physical.image_id)).second) invalid("duplicate Large physical image");
   }
  }
 std::uint64_t removed = 0;
 for (auto& component : replacement)
  if (component.edition() == CoconutEdition::XLarge) {
   std::vector<std::size_t> retained;
   for (std::size_t i = 0; i < component.inventory().size(); ++i) {
    throw_if_benchmark_cancelled(cancellation);
    const auto& image = component.inventory()[i].physical;
    const auto key = physical_key(image.source, image.image_id);
    if (!xlarge.insert(key).second) invalid("duplicate XL physical image");
    if (large.contains(key))
     ++removed;
    else
     retained.push_back(i);
   }
   if (retained.size() != component.inventory().size()) component = component.select_images(std::move(retained), cancellation);
  }
 throw_if_benchmark_cancelled(cancellation);
 components = std::move(replacement);
 return removed;
}
void store_coconut_component(
 const std::filesystem::path& index_path, const CoconutComponent& component, Cancellation cancellation, detail::StorageReservationPool* storage, const BenchmarkTraceSink& trace) {
 if (component.membership_ || component.backing_->metadata_only) invalid("membership cannot publish a full component");
 admit_coconut_component(component, cancellation);
 // Reassembled compact provenance can describe an already published immutable
 // cache. Preserve its inode and completion rather than rewriting equal bytes.
 if (auto cached = load_coconut_component(index_path, component.edition(), component.source(), component.input_identity(), cancellation)) {
  if (!component.index().selected() && cached->index().annotation_sha256 == component.index().annotation_sha256 && cached->seal_->identity == component.seal_->identity &&
      cached->source_extent() == component.source_extent() && cached->source_extent_complete() == component.source_extent_complete() &&
      cached->backing_->incomplete_ordinal_images == component.backing_->incomplete_ordinal_images && cached->backing_->rejected_images == component.backing_->rejected_images) {
   const std::lock_guard lock(component.backing_->completion_mutex);
   component.backing_->completion = cached->completion();
   trace_benchmark_event(trace, "benchmark.annotations.cache_reused", [&] { return nlohmann::json{{"path", index_path.string()}, {"images", component.index().image_count()}}; });
   return;
  }
 }
 const auto& identity = component.seal_->identity;
 const auto inventory_path = std::filesystem::path(index_path.string() + ".inventory");
 auto& seal = *component.seal_;
 const std::lock_guard lock(seal.mutex);
 seal.publish(inventory_path, cancellation, storage);
 throw_if_benchmark_cancelled(cancellation);
 Json manifest = Json::object();
 std::vector<std::uint64_t> rejected_images, incomplete_images;
 for (std::size_t i = 0; i < component.index().image_count(); ++i) {
  const auto id = component.index().image(i).source_image_id;
  if (!component.required_labels_valid(i)) rejected_images.push_back(id);
  if (!component.source_ordinals_complete(i)) incomplete_images.push_back(id);
 }
 const CoconutCompletionFacts facts{
  component.edition(), component.source(), component.input_identity(), std::string(kCoconutNormalizationRevision), identity, component.inventory().size(), component.recovery_policy(),
  std::string(component.original_annotation_identity()), component.recovery().size(), std::move(rejected_images), component.backing_->source_rows, component.backing_->source_segments,
  component.backing_->source_extent_complete, std::move(incomplete_images)
 };
 auto extension = Json::object();
 mmltk::frameworks::reflection::visit_materialized_members<CoconutCompletionFacts>([&]<class Declaration>(const auto& field) {
  const auto& value = facts.*Declaration::pointer;
  if constexpr (mmltk::frameworks::reflection::OptionalValue<typename Declaration::member_type>::value) {
   if (value) extension[field.member_name] = *value;
  } else
   extension[field.member_name] = value;
 });
 manifest["coconut"] = std::move(extension);
 throw_if_benchmark_cancelled(cancellation);
 // Persistence is an explicit assembly boundary. The view and its original
 // backing keep their cache identity; the newly written product gets its own.
 NormalizedAnnotationIndex persisted;
 if (component.index().selected()) {
  NormalizedAnnotationAssembler assembly(
   component.index(), component.index().image_count(), component.index().box_count(), component.index().run_count(), cancellation, index_path.parent_path(), storage);
  const auto& source = component.index().storage();
  std::vector<NormalizedAnnotationAssembler::Slice> slices;
  slices.reserve(component.index().image_count());
  for (std::size_t i = 0; i < component.index().image_count(); ++i) slices.push_back(NormalizedAnnotationAssembler::Slice::admitted(source, component.index().source_position(i)));
  assembly.gather(slices, nullptr, nullptr, {}, trace);
  persisted = assembly.finish(trace);
 } else {
  persisted = component.index().storage();
  static_cast<NormalizedAnnotationMetadata&>(persisted) = component.index();
 }
 const auto completion = store_normalized_annotation_index(index_path, persisted, cancellation, trace, storage, manifest);
 if (!component.index().selected()) {
  const std::lock_guard completion_lock(component.backing_->completion_mutex);
  component.backing_->completion = completion;
 }
}
void admit_coconut_component(const CoconutComponent& component, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 const auto& backing = *component.backing_;
 if (backing.metadata_only) invalid("membership has no normalized labels");
 admit_normalized_annotations(backing.index.storage(), cancellation);
 std::call_once(backing.full_admission, [&] {
  for (std::size_t i = 0; i < backing.recovery.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto& fact = backing.recovery[i];
   if (fact.objects.empty() && fact.omissions.empty()) continue;
   ComponentRecoveryAdmission join(&fact);
   const auto& image = backing.index.image(i);
   for (const auto& box : backing.index.storage().boxes.subspan(static_cast<std::size_t>(image.first_box), image.box_count)) join.box(box);
   join.finish();
  }
 });
}
std::uint64_t coconut_component_storage_bytes(const CoconutComponent& component) {
 const auto completion = component.completion();
 if (!completion) invalid("component has no completed storage");
 return mmltk::common::math::checked_add(
  completion->size, mmltk::common::math::checked_add(completion->proof_bytes, component.backing_->seal->bytes, "COCONut index storage overflow"), "COCONut index storage overflow");
}
std::optional<CoconutComponent> load_coconut_component(const std::filesystem::path& index_path, CoconutEdition edition, CoconutImageNamespace source, std::string_view input_identity,
 Cancellation cancellation, bool metadata_only, const CoconutPhysicalMembership* physical, const CoconutMaskRecovery* recovery, bool allow_changed_physical, const CoconutCacheAdmission& publication) {
 throw_if_benchmark_cancelled(cancellation);
 if (bool(publication.ready) != bool(publication.withdrawn)) throw std::invalid_argument("incremental component admission requires paired publication and withdrawal");
 std::shared_ptr<CoconutComponentBacking> component;
 std::size_t published = 0;
 bool publication_failed = false, mapping_io = false;
 const auto withdraw = [&] {
  if (published && publication.withdrawn) publication.withdrawn(CoconutComponentBacking::loaded(component), published);
  published = 0;
 };
 try {
  if (!std::filesystem::is_regular_file(index_path.string() + ".complete.json")) return std::nullopt;
  std::uint64_t completion_bytes = 0;
  const auto manifest = read_json_file(index_path.string() + ".complete.json", &completion_bytes);
  const auto& extension = manifest.at("coconut");
  CoconutCompletionFacts facts;
  mmltk::frameworks::reflection::visit_materialized_members<CoconutCompletionFacts>([&]<class Declaration>(const auto& field) {
   if constexpr (mmltk::frameworks::reflection::OptionalValue<typename Declaration::member_type>::value) {
    if (const auto found = extension.find(field.member_name); found != extension.end())
     facts.*Declaration::pointer = found->template get<mmltk::frameworks::reflection::OptionalValueT<typename Declaration::member_type>>();
   } else
    extension.at(field.member_name).get_to(facts.*Declaration::pointer);
  });
  if (facts.edition != edition || facts.source != source || (!physical && facts.input_identity != input_identity) || facts.normalization != kCoconutNormalizationRevision) return std::nullopt;
  component = std::make_shared<CoconutComponentBacking>();
  component->edition = edition;
  component->source = source;
  component->input_identity = facts.input_identity;
  component->source_rows = facts.source_rows.value_or(0);
  component->source_segments = facts.source_segments.value_or(0);
  component->source_extent_complete = facts.source_extent_complete.value_or(false);
  component->incomplete_ordinal_images = facts.incomplete_ordinal_images.value_or(std::vector<std::uint64_t>{});
  if (!std::ranges::is_sorted(component->incomplete_ordinal_images) || std::ranges::adjacent_find(component->incomplete_ordinal_images) != component->incomplete_ordinal_images.end())
   return std::nullopt;
  component->rejected_images = facts.rejected_images.value_or(std::vector<std::uint64_t>{});
  if (!std::ranges::is_sorted(component->rejected_images) || std::ranges::adjacent_find(component->rejected_images) != component->rejected_images.end()) return std::nullopt;
  if (physical) component->annotation_input_identity = input_identity;
  component->recovery_policy = facts.recovery_policy.value_or(0);
  component->original_annotation_identity = facts.original_annotation_identity.value_or("");
  if (physical && component->original_annotation_identity != (recovery ? recovery->original_identity(source) : std::string_view{})) return std::nullopt;
  if (!std::filesystem::is_regular_file(index_path.string() + ".inventory")) return std::nullopt;
  mapping_io = true;
  InventoryInput input(index_path.string() + ".inventory", cancellation);
  mapping_io = false;
  InventoryHeader header;
  input.value(header);
  validate_inventory_header(header, component_header(*component, facts.inventory_count));
  if (header.count > input.maximum_records() || facts.inventory_count != header.count) invalid("invalid component inventory count");
  auto inventory = std::make_shared<std::vector<CoconutInventoryImage>>();
  inventory->reserve(mmltk::common::math::checked_cast<std::size_t>(header.count, "component inventory count overflow"));
  component->inventory = inventory;
  ComponentInventoryAdmission admission(source);
  component->image_dependencies.reserve(static_cast<std::size_t>(header.count));
  // The same row admission owns both ordinary full loads and incremental
  // production. Inventory/recovery checksum acceptance remains terminal.
  const auto row_ready = [&](const NormalizedAnnotationIndex& normalized_index, std::size_t i) {
   if (!i) {
    component->index = NormalizedAnnotationReadView(normalized_index);
    if (header.count != component->index.image_count()) invalid("invalid component inventory count");
    admit_component_metadata(*component, component->index, component->index.image_count(), header.count, facts.recovery_images.value_or(0));
   }
   throw_if_benchmark_cancelled(cancellation);
   CoconutInventoryImage image;
   input.value(image);
   const bool complete = !std::ranges::binary_search(component->incomplete_ordinal_images, image.physical.image_id);
   const auto& normalized = normalized_index.images[i];
   admission.image(image, normalized, false, complete);
   if (!metadata_only)
    for (const auto& box : normalized_index.boxes.subspan(normalized.first_box, normalized.box_count))
     if (((box.flags & kAnnotationOrdinalIncomplete) == 0U) != complete) invalid("inconsistent cached annotation provenance");
   component->dependencies.add(image.physical);
   component->image_dependencies.push_back(coconut_image_input_identity(component->annotation_input_identity, image.physical, component->original_annotation_identity));
   inventory->push_back(std::move(image));
   if (!metadata_only && publication.ready) {
    // Count before entry: a throwing receiver may already have installed work.
    published = i + 1;
    try {
     publication.ready(CoconutComponentBacking::loaded(component), i);
    } catch (...) {
     publication_failed = true;
     throw;
    }
   }
  };
  auto index = load_normalized_annotation_index(
   index_path, coconut_index_source(source), coconut_component_split(edition, source), facts.inventory_identity, cancellation, {}, &manifest, metadata_only, completion_bytes, row_ready);
  if (!index) {
   withdraw();
   return std::nullopt;
  }
  component->index = NormalizedAnnotationReadView(std::move(*index));
  if (header.count != component->index.image_count()) invalid("invalid component inventory count");
  if (physical && facts.input_identity != coconut_component_input_identity(input_identity, source, recovery, physical, edition, component->dependencies, !allow_changed_physical))
   invalid("component input dependency mismatch");
  for (const auto id : component->incomplete_ordinal_images)
   if (!std::ranges::binary_search(*inventory, id, {}, [](const auto& image) { return image.physical.image_id; })) invalid("unknown incomplete-provenance image");
  if (component->recovery_policy) {
   std::uint32_t policy = 0;
   input.value(policy);
   if (policy != component->recovery_policy || policy != kCoconutRecoveryPolicy) invalid("invalid recovery policy");
   input.value(component->original_annotation_identity);
   std::uint64_t count = 0;
   input.value(count);
   if (count != header.count || facts.recovery_images != count || facts.original_annotation_identity != component->original_annotation_identity) invalid("invalid recovery completion");
   component->recovery.resize(static_cast<std::size_t>(count));
   for (std::size_t i = 0; i < count; ++i) {
    throw_if_benchmark_cancelled(cancellation);
    auto& fact = component->recovery[i];
    const auto& image = component->index.image(i);
    input.value(fact);
    admit_recovery_image(fact, image);
    if (!metadata_only) {
     ComponentRecoveryAdmission join(&fact);
     if (!fact.objects.empty() || !fact.omissions.empty())
      for (const auto& box : component->index.storage().boxes.subspan(static_cast<std::size_t>(image.first_box), image.box_count)) join.box(box);
     join.finish();
    }
   }
  } else if (!facts.original_annotation_identity.value_or("").empty() || facts.recovery_images.value_or(0))
   invalid("unexpected recovery completion");
  admit_component_metadata(*component, component->index, component->index.image_count(), component->inventory->size(), component->recovery.size());
  const auto identity = input.finish();
  if (facts.inventory_identity != identity) invalid("component inventory completion mismatch");
  component->seal = input.seal();
  component->seal->identity = identity;
  component->completion = component->index.completion;
  if (!metadata_only) std::call_once(component->full_admission, [] {});
  throw_if_benchmark_cancelled(cancellation);
  return CoconutComponentBacking::loaded(std::move(component));
 } catch (const std::exception& error) {
  withdraw();
  if (publication_failed || (mapping_io && !dynamic_cast<const CoconutInputError*>(&error)) || is_benchmark_capacity_failure(error) || dynamic_cast<const std::system_error*>(&error)) throw;
  throw_if_benchmark_cancelled(cancellation);
  return std::nullopt;
 } catch (...) {
  withdraw();
  throw;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
