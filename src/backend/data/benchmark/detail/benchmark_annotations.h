#pragma once  // backend.data private implementation boundary
#include <atomic>
#include <array>
#include <type_traits>
#include <nlohmann/json.hpp>
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <memory>
#include <span>
#include <ranges>
#include <utility>
#include <string>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "src/common/concurrency/cancellation_observation.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include "src/backend/data/benchmark/detail/benchmark_catalog.h"
#include "src/backend/data/benchmark/benchmark_dataset_compiler.h"
#include "src/backend/data/compiled/compiled_format.h"
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCompilePipeline;
class StorageReservationPool;
// Rejection of the annotation document itself, never a local execution failure.
class AnnotationDocumentRejected final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
inline constexpr std::uint32_t kNormalizedAnnotationIndexVersion = 3U;
struct __attribute__((packed)) NormalizedBox {
 float x1 = 0.0F;
 float y1 = 0.0F;
 float x2 = 0.0F;
 float y2 = 0.0F;
 std::uint64_t mask_rle_offset = 0U;
 std::uint32_t mask_rle_pairs = 0U;
 std::uint8_t class_id = 0U;
 std::uint8_t flags = 0U;
 std::uint8_t reserved[2]{};
 double original_area = 0.0;
 std::uint64_t annotation_id = 0U;
 std::uint64_t source_category_id = 0U;
 std::uint64_t source_ordinal = 0U;
};
static_assert(sizeof(NormalizedBox) == 64U);
struct __attribute__((packed)) NormalizedImage {
 std::uint64_t source_image_id = 0U;
 std::uint64_t first_box = 0U;
 std::uint32_t box_count = 0U;
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 std::uint16_t source_shard = 0U;
 std::uint16_t reserved = 0U;
};
static_assert(sizeof(NormalizedImage) == 32U);
struct AnnotationRejectCounts {
 std::uint64_t raw_records = 0U;
 std::uint64_t unmapped_categories = 0U;
 std::uint64_t unknown_images = 0U;
 std::uint64_t malformed_records = 0U;
 std::uint64_t degenerate_boxes = 0U;
 std::uint64_t duplicate_boxes = 0U;
};
MMLTK_REFLECT_FIELDS(AnnotationRejectCounts)
// The version-3 normalized index persists declaration order as six uint64 slots.
static_assert([] consteval {
 constexpr const auto& fields = mmltk::frameworks::reflection::field_declarations<AnnotationRejectCounts>();
 constexpr std::array<std::string_view, 6> names{"raw_records", "unmapped_categories", "unknown_images", "malformed_records", "degenerate_boxes", "duplicate_boxes"};
 static_assert(fields.size() == names.size());
 mmltk::frameworks::reflection::visit_materialized_members<AnnotationRejectCounts>(
  []<class Declaration>(const auto&) { static_assert(std::is_same_v<typename Declaration::member_type, std::uint64_t>); });
 for (std::size_t index = 0; index < names.size(); ++index) {
  if (fields[index].member_name != names[index]) return false;
 }
 return true;
}());
[[nodiscard]] nlohmann::json reject_json(const AnnotationRejectCounts& rejected);
struct NormalizedAnnotationCompletionFacts {
 std::uint32_t schema_version = 0, index_version = 0;
 bool complete = false;
 std::string source, split, mapping_revision, annotation_sha256;
 std::uint64_t size = 0;
 std::string identity, integrity_mode;
 std::uint64_t images = 0, mask_rle_pairs = 0, boxes = 0;
};
MMLTK_REFLECT_FIELDS(NormalizedAnnotationCompletionFacts)
struct NormalizedAnnotationCompletion : NormalizedAnnotationCompletionFacts {
 // The opened or published proof extent is custody metadata, never a wire field.
 std::uint64_t proof_bytes = 0;
};
struct NormalizedAnnotationMetadata {
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
 std::string split;
 std::string annotation_sha256;
 AnnotationRejectCounts rejected;
 // Identity always describes the full persisted product, including on a
 // selected read view. Unpersisted membership has no completion.
 std::shared_ptr<const NormalizedAnnotationCompletion> completion;
};
struct NormalizedAnnotationBuilder : NormalizedAnnotationMetadata {
 std::vector<NormalizedImage> images;
 std::vector<NormalizedBox> boxes;
 std::vector<RLEPair> mask_rle_pairs;
};
class NormalizedAnnotationBacking;
struct NormalizedAnnotationIndex : NormalizedAnnotationMetadata {
 std::span<const NormalizedImage> images;
 std::span<const NormalizedBox> boxes;
 std::span<const RLEPair> mask_rle_pairs;
 // The mapping or sealed builder outlives every derived read view.
 std::shared_ptr<NormalizedAnnotationBacking> backing;
};
// A read selection retains its complete immutable source, including its cache
// identity. Image offsets still address that source; only output assembly rebases.
class NormalizedAnnotationReadView final : public NormalizedAnnotationMetadata {
public:
 struct Counts {
  std::size_t boxes = 0, runs = 0;
 };
 NormalizedAnnotationReadView() = default;
 explicit NormalizedAnnotationReadView(NormalizedAnnotationIndex);
 [[nodiscard]] const NormalizedAnnotationIndex& storage() const noexcept { return storage_; }
 [[nodiscard]] std::size_t image_count() const noexcept { return positions_ ? positions_->size() : storage_.images.size(); }
 [[nodiscard]] std::size_t box_count() const noexcept { return counts_.boxes; }
 [[nodiscard]] std::size_t run_count() const noexcept { return counts_.runs; }
 [[nodiscard]] bool selected() const noexcept { return static_cast<bool>(positions_); }
 [[nodiscard]] std::size_t source_position(std::size_t position) const {
  if (position >= image_count()) throw std::out_of_range("normalized image position is invalid");
  return positions_ ? (*positions_)[position] : position;
 }
 [[nodiscard]] const NormalizedImage& image(std::size_t position) const { return storage_.images[source_position(position)]; }
 [[nodiscard]] auto images() const {
  return std::views::iota(std::size_t{0}, image_count()) | std::views::transform([this](std::size_t position) -> const NormalizedImage& { return image(position); });
 }
 // Positions address this view and must be a strictly increasing subset. Known
 // counts come from the sampler's existing selection pass; otherwise admitted
 // image/box endpoints supply the counts without reading mask payloads.
 [[nodiscard]] NormalizedAnnotationReadView select_images(std::vector<std::size_t>, std::optional<Counts> = std::nullopt, mmltk::common::concurrency::CancellationObservation = {}) const;

private:
 NormalizedAnnotationIndex storage_;
 std::shared_ptr<const std::vector<std::size_t>> positions_;
 Counts counts_;
};
// Construction and admission share each append. Only this owner can publish
// its admitted product without a later traversal over freshly produced masks.
class NormalizedAnnotationAssembler final {
public:
 explicit NormalizedAnnotationAssembler(NormalizedAnnotationMetadata, std::size_t images, std::size_t boxes, std::size_t runs, mmltk::common::concurrency::CancellationObservation = {});
 void begin_image(NormalizedImage);
 void append_box(NormalizedBox, std::span<const RLEPair>);
 [[nodiscard]] NormalizedAnnotationIndex finish(const BenchmarkTraceSink& = {});

private:
 NormalizedAnnotationBuilder builder_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 bool finished_ = false;
};
[[nodiscard]] NormalizedAnnotationIndex seal_normalized_annotations(NormalizedAnnotationBuilder&&, mmltk::common::concurrency::CancellationObservation = {});
[[nodiscard]] NormalizedAnnotationIndex seal_normalized_annotation_metadata(NormalizedAnnotationBuilder&&);
void admit_normalized_annotations(const NormalizedAnnotationIndex&, mmltk::common::concurrency::CancellationObservation = {});
// Source and destination must be distinct. Copies one complete slice, rebasing only
// storage offsets; source IDs need not be sorted. Metadata belongs to the caller.
void append_normalized_image_slice(
 NormalizedAnnotationBuilder& destination, const NormalizedAnnotationIndex& source, std::size_t image_position, mmltk::common::concurrency::CancellationObservation cancellation = {});
// Retains positions in the supplied order. Identity does not inspect mask payloads;
// increasing subsets retain allocation capacity. On cancellation during in-place
// compaction the owner must discard the index, never publish it.
void retain_normalized_image_slices(NormalizedAnnotationBuilder& index, std::span<const std::size_t> order, mmltk::common::concurrency::CancellationObservation cancellation = {});
struct AnnotationParseOptions {
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
 std::string split;
 std::uint32_t expected_image_count = 0U;
 int num_workers = 1;
 bool keep_images_without_mapped_boxes = false;
 mmltk::common::concurrency::CancellationObservation cancel_requested = {};
 BenchmarkTraceSink trace{};
 BenchmarkCompilePipeline* execution = nullptr;
 // Existing index transaction custody covers its one live input mapping.
 BenchmarkAllowance input_allowance{};
};
[[nodiscard]] NormalizedAnnotationIndex parse_coco_style_annotations(
 const std::filesystem::path& json_path, std::string annotation_sha256, std::span<const NumericCategoryMapping> mappings, const AnnotationParseOptions& options);
[[nodiscard]] NormalizedAnnotationIndex parse_open_images_annotations(const std::filesystem::path& boxes_csv_path, const std::filesystem::path& classes_csv_path, std::string annotation_sha256,
 std::span<const StringCategoryMapping> mappings, const AnnotationParseOptions& options);
[[nodiscard]] std::optional<NormalizedAnnotationIndex> load_normalized_annotation_index(const std::filesystem::path& path, BenchmarkDatasetSource expected_source, std::string_view expected_split,
 std::string_view expected_annotation_sha256, mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace = {}, const nlohmann::json* completion = nullptr,
 bool metadata_only = false, std::uint64_t completion_bytes = 0);
std::shared_ptr<const NormalizedAnnotationCompletion> store_normalized_annotation_index(const std::filesystem::path& path, const NormalizedAnnotationIndex& index,
 mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace = {}, StorageReservationPool* storage = nullptr, const nlohmann::json& extension = {});
void remove_normalized_annotation_index(const std::filesystem::path& path);
[[nodiscard]] std::vector<std::uint64_t> image_ids(const NormalizedAnnotationReadView&);
}  // namespace mmltk::backend::data::benchmark_internal
