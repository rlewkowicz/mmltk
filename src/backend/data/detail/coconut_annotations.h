#pragma once  // backend.data private implementation boundary
#include "benchmark_annotations.h"
#include "coconut_catalog.h"
#include "coconut_inventory.h"
#include "src/backend/data/compiled_format.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>
#include <utility>
#include <unordered_map>
namespace mmltk::backend::data::benchmark_internal {
class CoconutPhysicalMembershipError final : public std::runtime_error {
public:
 CoconutPhysicalMembershipError(CoconutImageNamespace source, std::uint64_t image_id, std::string message) : std::runtime_error(std::move(message)), source_(source), image_id_(image_id) {}
 [[nodiscard]] CoconutImageNamespace source() const noexcept { return source_; }
 [[nodiscard]] std::uint64_t image_id() const noexcept { return image_id_; }

private:
 CoconutImageNamespace source_;
 std::uint64_t image_id_;
};
// Borrows one immutable admitted inventory generation. The caller must keep its
// rows alive and unchanged until this lookup and all import requests release it.
class CoconutPhysicalMembership final {
public:
 explicit CoconutPhysicalMembership(std::span<const CoconutPhysicalImage>, mmltk::common::concurrency::CancellationObservation = {});
 CoconutPhysicalMembership(const CoconutPhysicalMembership&) = delete;
 CoconutPhysicalMembership& operator=(const CoconutPhysicalMembership&) = delete;
 [[nodiscard]] const CoconutPhysicalImage* find(CoconutImageNamespace, std::uint64_t) const noexcept;

private:
 std::unordered_map<CoconutImageNamespace, std::unordered_map<std::uint64_t, const CoconutPhysicalImage*>> namespaces_;
};
class CoconutMaskRecovery;
struct CoconutComponent {
 CoconutEdition edition = CoconutEdition::Base;
 CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
 std::string input_identity;
 NormalizedAnnotationIndex index;
 std::uint32_t recovery_policy = 0;
 std::string original_annotation_identity;
 // Image-keyed recovery facts, independent of normalized storage offsets.
 std::vector<CoconutRecoveryImage> recovery;
 // Exactly one entry per normalized image, in the same order.
 std::vector<CoconutInventoryImage> inventory;
};
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
 std::uint32_t width = 0, height = 0;
 std::uint64_t source_ordinal = 0;
 std::uint64_t first_segment_ordinal = 0;
 std::vector<CoconutSegment> segments;
};
struct CoconutImportLimits {
 std::uint64_t max_png_bytes = 64U * 1024U * 1024U;
 std::uint64_t max_pixels = 64U * 1024U * 1024U;
 std::uint32_t max_segments = 65535U;
 std::uint32_t max_dimension = MAX_IMAGE_EXTENT;
};
using CoconutRecordConsumer = std::function<void(const CoconutRecord&, std::span<const std::uint8_t>)>;
// PNG is borrowed for this call only. Reader and record batch remain alive through consumer.
void read_coconut_parquet(
 std::span<const std::filesystem::path> shards, const CoconutImportLimits& limits, mmltk::common::concurrency::CancellationObservation cancellation, const CoconutRecordConsumer& consumer);
struct CoconutImportRequest {
 CoconutEdition edition = CoconutEdition::Base;
 std::string input_identity;
 std::vector<std::filesystem::path> parquet_shards;
 std::filesystem::path annotation_json;
 std::filesystem::path mask_archive;
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
// Canonical relative member spelling; rejects absolute paths, traversal, backslashes and NUL.
[[nodiscard]] std::string canonical_coconut_archive_member(std::string_view raw);
// Full archive inventory, independent of annotations/foreground selection. Cache is identity-bound.
[[nodiscard]] std::vector<CoconutPhysicalImage> coconut_image_archive_inventory(const std::filesystem::path& archive_path, const std::filesystem::path& cache_path, CoconutImageNamespace source,
 std::uint16_t shard, std::string archive_identity, mmltk::common::concurrency::CancellationObservation cancellation = {});
void store_coconut_component(const std::filesystem::path& index_path, const CoconutComponent& component, mmltk::common::concurrency::CancellationObservation cancellation = {});
[[nodiscard]] std::optional<CoconutComponent> load_coconut_component(
 const std::filesystem::path& index_path, CoconutEdition edition, CoconutImageNamespace source, std::string_view input_identity, mmltk::common::concurrency::CancellationObservation cancellation = {});
}  // namespace mmltk::backend::data::benchmark_internal
