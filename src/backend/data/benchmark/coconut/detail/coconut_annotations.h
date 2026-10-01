#pragma once  // backend.data private implementation boundary
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotation_identity.h"
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/common/concurrency/cancellation_observation.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <array>
#include <cstddef>
#include <ranges>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <memory>
namespace mmltk::backend::data::benchmark_internal {
using mmltk::backend::data::detail::CompileAllowance;
class BenchmarkCompilePipeline;
class BenchmarkLabelInput;
class CoconutMaskRecovery;
class CoconutRecoveryOriginals;
struct CoconutCompletionFacts {
 CoconutEdition edition = CoconutEdition::Base;
 CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
 std::string input_identity, normalization, inventory_identity;
 std::uint64_t inventory_count = 0;
 std::optional<std::uint32_t> recovery_policy;
 std::optional<std::string> original_annotation_identity;
 std::optional<std::uint64_t> recovery_images;
 std::optional<std::vector<std::uint64_t>> rejected_images;
 std::optional<std::uint64_t> source_rows, source_segments;
 std::optional<bool> source_extent_complete;
 std::optional<std::vector<std::uint64_t>> incomplete_ordinal_images;
};
MMLTK_REFLECT_FIELDS(CoconutCompletionFacts)
struct CoconutOriginalInput;
class CoconutComponentBacking;
struct CoconutComponentPart;
class CoconutInventorySeal;
// Reports each rejected offered row after its exact label claims retire.
using CoconutImageRejection = std::function<void(const CoconutInventoryImage&, std::string_view reason)>;
class CoconutComponent final {
public:
 [[nodiscard]] CoconutEdition edition() const noexcept;
 [[nodiscard]] CoconutImageNamespace source() const noexcept;
 [[nodiscard]] const std::string& input_identity() const noexcept;
 [[nodiscard]] const std::string& image_input_identity(std::size_t) const;
 [[nodiscard]] BenchmarkLabelInput labels(std::size_t) const;
 [[nodiscard]] std::uint64_t original_generation() const noexcept { return original_generation_; }
 [[nodiscard]] bool required_labels_valid(std::size_t) const;
 [[nodiscard]] std::uint64_t compiled_ordinal_base(std::size_t) const;
 [[nodiscard]] bool source_ordinals_complete(std::size_t) const;
 [[nodiscard]] bool source_extent_complete() const;
 [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> source_extent() const;
 // Bind a cached view to the admitted original input used to validate it.
 [[nodiscard]] CoconutComponent with_original(const CoconutOriginalInput&) const;
 [[nodiscard]] bool matches_inputs(std::string_view, const CoconutPhysicalMembership&, const CoconutMaskRecovery*, bool current_physical = true) const;
 [[nodiscard]] bool matches_physical(const CoconutPhysicalMembership&) const;
 [[nodiscard]] std::uint32_t recovery_policy() const noexcept;
 [[nodiscard]] std::string_view original_annotation_identity() const noexcept;
 [[nodiscard]] const NormalizedAnnotationReadView& index() const noexcept { return index_; }
 [[nodiscard]] const CoconutInventoryImage& inventory_image(std::size_t) const;
 [[nodiscard]] const CoconutRecoveryImage& recovery_image(std::size_t) const;
 [[nodiscard]] auto inventory() const {
  return std::views::iota(std::size_t{0}, index_.image_count()) | std::views::transform([this](std::size_t i) -> const CoconutInventoryImage& { return inventory_image(i); });
 }
 [[nodiscard]] auto recovery() const {
  return std::views::iota(std::size_t{0}, recovery_policy() ? index_.image_count() : 0) | std::views::transform([this](std::size_t i) -> const CoconutRecoveryImage& { return recovery_image(i); });
 }
 // The full source completion remains bound to its backing even after selection.
 [[nodiscard]] std::shared_ptr<const NormalizedAnnotationCompletion> completion() const;
 [[nodiscard]] CoconutComponent membership() const;
 [[nodiscard]] CoconutComponent select_images(std::vector<std::size_t>, mmltk::common::concurrency::CancellationObservation = {}) const;

private:
 friend class CoconutComponentBacking;
 friend void admit_coconut_component(const CoconutComponent&, mmltk::common::concurrency::CancellationObservation);
 friend std::uint64_t coconut_component_storage_bytes(const CoconutComponent&);
 friend void store_coconut_component(
  const std::filesystem::path&, const CoconutComponent&, mmltk::common::concurrency::CancellationObservation, detail::StorageReservationPool*, const BenchmarkTraceSink&);
 friend std::vector<CoconutComponent> merge_coconut_components(std::span<const CoconutComponentPart>, std::string_view, const CoconutPhysicalMembership&, const std::filesystem::path&,
  mmltk::common::concurrency::CancellationObservation, detail::StorageReservationPool*, BenchmarkCompilePipeline*, const CoconutImageRejection&, const BenchmarkTraceSink&);
 CoconutComponent(std::shared_ptr<const CoconutComponentBacking>, NormalizedAnnotationReadView, std::shared_ptr<CoconutInventorySeal>, bool);
 std::shared_ptr<const CoconutComponentBacking> backing_;
 NormalizedAnnotationReadView index_;
 std::shared_ptr<CoconutInventorySeal> seal_;
 bool membership_ = false;
 std::uint64_t original_generation_ = 0;
};
struct CoconutComponentPart {
 std::vector<CoconutComponent> components;
 std::optional<std::uint64_t> row_base = 0, segment_base = 0;
 // Authoritative parser facts can complete an older immutable cache's absent
 // optional extent metadata without rewriting its records or completion.
 std::optional<std::pair<std::uint64_t, std::uint64_t>> source_extent{};
};
[[nodiscard]] std::vector<CoconutComponent> merge_coconut_components(std::span<const CoconutComponentPart>, std::string_view, const CoconutPhysicalMembership&, const std::filesystem::path&,
 mmltk::common::concurrency::CancellationObservation = {}, detail::StorageReservationPool* = nullptr, BenchmarkCompilePipeline* = nullptr, const CoconutImageRejection& = {},
 const BenchmarkTraceSink& = {});
struct CoconutComponentBuilder : CoconutComponentMetadata {
 std::uint64_t source_segment_begin = 0;
 NormalizedAnnotationBuilder index;
 std::vector<CoconutRecoveryImage> recovery;
 std::vector<CoconutInventoryImage> inventory;
 // Standalone construction performs real admission; no caller-supplied seal.
 [[nodiscard]] CoconutComponent finish(mmltk::common::concurrency::CancellationObservation = {}, const std::filesystem::path& directory = {}, detail::StorageReservationPool* = nullptr) &&;
};
class CoconutOriginalChanged final : public std::runtime_error {
public:
 explicit CoconutOriginalChanged(CoconutImageNamespace source) : std::runtime_error("COCONut original annotation generation changed"), source_(source) {}
 [[nodiscard]] CoconutImageNamespace source() const noexcept { return source_; }

private:
 CoconutImageNamespace source_;
};
struct CoconutOriginalInput {
 std::shared_ptr<const CoconutRecoveryOriginals> originals;
 bool terminal = false;
 std::uint64_t generation = 0;
};
struct CoconutComponentImage {
 const CoconutComponentMetadata* component;
 const NormalizedAnnotationMetadata* metadata;
 const NormalizedImage* image;
 std::span<const NormalizedBox> boxes;
 std::span<const RLEPair> runs;
 const CoconutInventoryImage* inventory;
 const CoconutRecoveryImage* recovery;
 std::uint64_t segment_base;
 const std::string* dependency = nullptr;
 bool required_labels_valid = true;
 bool ordinals_complete = true;
 const NormalizedAnnotationIndex* admission = nullptr;
};
[[nodiscard]] detail::CompileResources coconut_component_assembly_resources(std::uint64_t images, std::uint64_t recovery_join_boxes = 0);
[[nodiscard]] std::optional<CoconutComponent> assemble_coconut_component(CoconutComponentMetadata, std::vector<CoconutComponentImage>&, AnnotationRejectCounts, bool metadata_only,
 std::uint64_t source_rows, std::optional<std::uint64_t> source_segments, std::span<const std::size_t> source_ordinals, std::span<const CoconutComponent> reusable, const CoconutPhysicalMembership&,
 const CoconutMaskRecovery*, const std::filesystem::path&, common::concurrency::CancellationObservation, detail::StorageReservationPool*, BenchmarkCompilePipeline*, const CompileAllowance&,
 bool local_rejection, const CoconutImageRejection&, const BenchmarkTraceSink& = {});
[[nodiscard]] std::string coconut_component_input_identity(std::string_view base, CoconutImageNamespace, const CoconutMaskRecovery*, const CoconutPhysicalMembership* = nullptr,
 CoconutEdition = CoconutEdition::Base, const CoconutPhysicalDependencies& = {}, bool current = true);
// Removes only XL rows covered by Large, retaining B and all namespace distinctions.
[[nodiscard]] std::uint64_t reconcile_coconut_extensions(std::vector<CoconutComponent>& components, mmltk::common::concurrency::CancellationObservation cancellation = {});
void admit_coconut_component(const CoconutComponent&, mmltk::common::concurrency::CancellationObservation = {});
[[nodiscard]] std::uint64_t coconut_component_storage_bytes(const CoconutComponent&);
void store_coconut_component(const std::filesystem::path& index_path, const CoconutComponent& component, mmltk::common::concurrency::CancellationObservation cancellation = {},
 detail::StorageReservationPool* storage = nullptr, const BenchmarkTraceSink& trace = {});
// Rows retain their mapping while a later inventory/recovery proof is checked.
// Withdrawal addresses only rows exposed by this admission attempt.
struct CoconutCacheAdmission {
 std::function<void(const CoconutComponent&, std::size_t)> ready;
 std::function<void(const CoconutComponent&, std::size_t)> withdrawn;
};
[[nodiscard]] std::optional<CoconutComponent> load_coconut_component(const std::filesystem::path& index_path, CoconutEdition edition, CoconutImageNamespace source, std::string_view input_identity,
 mmltk::common::concurrency::CancellationObservation cancellation = {}, bool metadata_only = false, const CoconutPhysicalMembership* = nullptr, const CoconutMaskRecovery* = nullptr,
 bool allow_changed_physical = false, const CoconutCacheAdmission& = {});
}  // namespace mmltk::backend::data::benchmark_internal
