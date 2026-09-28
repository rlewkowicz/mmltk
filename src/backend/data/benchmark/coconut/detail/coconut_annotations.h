#pragma once  // backend.data private implementation boundary
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCompilePipeline;
class StorageReservationPool;
class CoconutPhysicalMembership;
class CoconutMaskRecovery;
struct CoconutCompletionFacts {
 CoconutEdition edition = CoconutEdition::Base;
 CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
 std::string input_identity, normalization, inventory_identity;
 std::uint64_t inventory_count = 0;
 std::optional<std::uint32_t> recovery_policy;
 std::optional<std::string> original_annotation_identity;
 std::optional<std::uint64_t> recovery_images;
};
MMLTK_REFLECT_FIELDS(CoconutCompletionFacts)
class CoconutInventorySeal;
template<class Index>
struct CoconutComponentProduct {
 CoconutEdition edition = CoconutEdition::Base;
 CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
 std::string input_identity;
 Index index;
 std::uint32_t recovery_policy = 0;
 std::string original_annotation_identity;
 // Image-keyed recovery facts, independent of normalized storage offsets.
 std::vector<CoconutRecoveryImage> recovery;
 // Exactly one entry per normalized image, in the same order.
 std::vector<CoconutInventoryImage> inventory;
 mutable std::shared_ptr<CoconutInventorySeal> inventory_seal;
};
using CoconutComponent = CoconutComponentProduct<NormalizedAnnotationReadView>;
using CoconutComponentBuilder = CoconutComponentProduct<NormalizedAnnotationBuilder>;
struct CoconutSegment {
 std::uint32_t id = 0;
 std::uint64_t category_id = 0;
 bool isthing = false;
 bool crowd = false;
 bool ignore = false;
 std::optional<double> area = std::nullopt;
 // External COCO bbox convention: x, y, width, height.
 std::optional<std::array<double, 4>> bbox = std::nullopt;
};
struct CoconutRecord {
 std::uint64_t image_id = 0;
 std::string file_name;
 std::string physical_stem;
 std::optional<CoconutImageNamespace> namespace_hint;
 std::uint32_t width = 0, height = 0;
 std::uint64_t source_ordinal = 0;
 std::uint64_t first_segment_ordinal = 0;
 std::vector<CoconutSegment> segments;
};
class BenchmarkArchive;
class CoconutParquetMetadata;
struct CoconutRecordGroup {
 std::uint64_t first_row = 0, rows = 0, first_segment = 0, segments = 0;
};
struct CoconutAnnotationRecords {
 void discard() noexcept;
 // Commit group-local segment ordinals in catalog order after every group settles.
 void rebase_parquet_segments();
 std::vector<CoconutRecord> records;
 std::shared_ptr<BenchmarkArchive> archive;
 std::shared_ptr<CoconutParquetMetadata> parquet;
 std::vector<CoconutRecordGroup> groups;
 // Sealed inventories from membership survive into the unchanged full import.
 std::vector<std::shared_ptr<CoconutInventorySeal>> inventories;
 std::string identity;
};
struct CoconutImportLimits {
 std::uint64_t max_png_bytes = 64U * 1024U * 1024U;
 std::uint64_t max_pixels = 64U * 1024U * 1024U;
 std::uint32_t max_segments = 65535U;
 std::uint32_t max_dimension = MAX_IMAGE_EXTENT;
};
struct CoconutAnnotationInput {
 std::span<const std::uint8_t> png;
 BenchmarkAllowance allowance;
 // Keeps the Arrow batch and its complete pool alive, including after the
 // forward reader closes. Production retains only the current bounded batch.
 std::shared_ptr<const void> backing;
 std::uint64_t live_bytes = 0;
 bool membership = false;
};
// Consumers return before the next allocation window. Group scratch retirement
// is separate from noexcept input retirement at sequence end or failed growth.
using CoconutRecordConsumer = std::function<void(std::size_t group, const CoconutRecord&, const CoconutAnnotationInput&)>;
void read_coconut_parquet(std::span<const std::filesystem::path> shards, const CoconutImportLimits& limits, mmltk::common::concurrency::CancellationObservation cancellation,
 const CoconutRecordConsumer& consumer, bool metadata_only = false, BenchmarkCompilePipeline* execution = nullptr, const std::function<void(std::size_t)>& retire_consumer_scratch = {}, const BenchmarkAllowance& parent = {}, BenchmarkResources consumer_resources = {}, CoconutAnnotationRecords* retained = nullptr,
 const std::function<std::uint64_t(const CoconutRecord&)>& consumer_workspace = {}, const std::function<void(const BenchmarkAllowance&)>& retire_consumer_input = {});
struct CoconutImportRequest {
 BenchmarkCompilePipeline* execution = nullptr;
 BenchmarkAllowance parent_allowance;
 CoconutEdition edition = CoconutEdition::Base;
 std::string input_identity;
 bool metadata_only = false;
 std::shared_ptr<CoconutAnnotationRecords> records;
 std::span<const CoconutImageNamespace> retained_sources;
 std::vector<std::filesystem::path> parquet_shards;
 std::filesystem::path annotation_json;
 std::filesystem::path mask_archive;
 std::filesystem::path inventory_directory;
 const CoconutPhysicalMembership* physical_membership = nullptr;
 // Synchronous borrow; the owner and its original indexes outlive import.
 CoconutMaskRecovery* recovery = nullptr;
 std::uint64_t expected_rows = 0;
 CoconutImportLimits limits;
 mmltk::common::concurrency::CancellationObservation cancellation;
 std::function<void(std::uint64_t)> progress;
 // Synchronous observation of discarded objects; report failures never reject an image.
 std::function<void(const CoconutPhysicalImage&, const CoconutRecord&, const CoconutSegment&, std::string_view)> rejected_object;
};
[[nodiscard]] std::string coconut_component_input_identity(std::string_view base, CoconutImageNamespace, const CoconutMaskRecovery*);
// Every offered record is required. Unknown expected_rows means derive, never sample.
[[nodiscard]] std::vector<CoconutComponent> import_coconut_annotations(const CoconutImportRequest& request);
// Removes only XL rows covered by Large, retaining B and all namespace distinctions.
[[nodiscard]] std::uint64_t reconcile_coconut_extensions(std::vector<CoconutComponent>& components, mmltk::common::concurrency::CancellationObservation cancellation = {});
// Full archive inventory, independent of annotations/foreground selection. Cache is identity-bound.
[[nodiscard]] std::vector<CoconutPhysicalImage> coconut_image_archive_inventory(const std::filesystem::path& archive_path, const std::filesystem::path& cache_path, CoconutImageNamespace source,
 std::uint16_t shard, std::string archive_identity, mmltk::common::concurrency::CancellationObservation cancellation = {}, StorageReservationPool* storage = nullptr, BenchmarkCompilePipeline* execution = nullptr, const BenchmarkAllowance& parent = {});
void admit_coconut_component(const CoconutComponent&, mmltk::common::concurrency::CancellationObservation = {});
[[nodiscard]] std::uint64_t coconut_component_storage_bytes(const CoconutComponent&);
void store_coconut_component(const std::filesystem::path& index_path, const CoconutComponent& component, mmltk::common::concurrency::CancellationObservation cancellation = {}, StorageReservationPool* storage = nullptr);
[[nodiscard]] std::optional<CoconutComponent> load_coconut_component(
 const std::filesystem::path& index_path, CoconutEdition edition, CoconutImageNamespace source, std::string_view input_identity, mmltk::common::concurrency::CancellationObservation cancellation = {}, bool metadata_only = false);
}  // namespace mmltk::backend::data::benchmark_internal
