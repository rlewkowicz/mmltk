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
#include <span>
#include <string>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "src/common/concurrency/cancellation_observation.h"
#include "benchmark_cache.h"
#include "benchmark_catalog.h"
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/compiled_format.h"
namespace mmltk::backend::data::benchmark_internal {
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
struct NormalizedAnnotationIndex {
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
 std::string split;
 std::string annotation_sha256;
 std::vector<NormalizedImage> images;
 std::vector<NormalizedBox> boxes;
 std::vector<RLEPair> mask_rle_pairs;
 AnnotationRejectCounts rejected;
};
// Source and destination must be distinct. Copies one complete slice, rebasing only
// storage offsets; source IDs need not be sorted. Metadata belongs to the caller.
void append_normalized_image_slice(
 NormalizedAnnotationIndex& destination, const NormalizedAnnotationIndex& source, std::size_t image_position, mmltk::common::concurrency::CancellationObservation cancellation = {});
// Retains positions in the supplied order. Identity does not inspect mask payloads;
// increasing subsets retain allocation capacity. On cancellation during in-place
// compaction the owner must discard the index, never publish it.
void retain_normalized_image_slices(NormalizedAnnotationIndex& index, std::span<const std::size_t> order, mmltk::common::concurrency::CancellationObservation cancellation = {});
struct AnnotationParseOptions {
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
 std::string split;
 std::uint32_t expected_image_count = 0U;
 int num_workers = 1;
 bool keep_images_without_mapped_boxes = false;
 mmltk::common::concurrency::CancellationObservation cancel_requested = {};
 BenchmarkTraceSink trace;
};
[[nodiscard]] NormalizedAnnotationIndex parse_coco_style_annotations(
 const std::filesystem::path& json_path, std::string annotation_sha256, std::span<const NumericCategoryMapping> mappings, const AnnotationParseOptions& options);
[[nodiscard]] NormalizedAnnotationIndex parse_open_images_annotations(const std::filesystem::path& boxes_csv_path, const std::filesystem::path& classes_csv_path, std::string annotation_sha256,
 std::span<const StringCategoryMapping> mappings, const AnnotationParseOptions& options);
[[nodiscard]] std::optional<NormalizedAnnotationIndex> load_normalized_annotation_index(const std::filesystem::path& path, BenchmarkDatasetSource expected_source, std::string_view expected_split,
 std::string_view expected_annotation_sha256, mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace = {});
void store_normalized_annotation_index(
 const std::filesystem::path& path, const NormalizedAnnotationIndex& index, mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace = {});
[[nodiscard]] std::vector<std::uint64_t> image_ids(const NormalizedAnnotationIndex&, std::optional<std::uint16_t> shard = std::nullopt);
}  // namespace mmltk::backend::data::benchmark_internal
