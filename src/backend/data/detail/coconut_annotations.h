#pragma once  // backend.data private implementation boundary
#include "benchmark_annotations.h"
#include "coconut_catalog.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
struct CoconutPhysicalImage {
    CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
    std::uint64_t image_id = 0;
    std::uint16_t shard = 0;
    std::string member;
    std::string archive_identity;
    bool operator==(const CoconutPhysicalImage&) const = default;
};
MMLTK_REFLECT_FIELDS(CoconutPhysicalImage)
struct CoconutInventoryImage {
    CoconutPhysicalImage physical;
    std::uint64_t release_image_id = 0;
    std::uint64_t source_ordinal = 0;
    bool operator==(const CoconutInventoryImage&) const = default;
};
MMLTK_REFLECT_FIELDS(CoconutInventoryImage)
struct CoconutComponent {
    CoconutEdition edition = CoconutEdition::Base;
    CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
    std::string input_identity;
    NormalizedAnnotationIndex index;
    // Exactly one entry per normalized image, in the same order.
    std::vector<CoconutInventoryImage> inventory;
};
struct CoconutSegment {
    std::uint32_t id = 0;
    std::uint64_t category_id = 0;
    bool isthing = false;
    bool crowd = false;
    bool ignore = false;
    std::optional<double> area;
    // External COCO bbox convention: x, y, width, height.
    std::optional<std::array<double, 4>> bbox;
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
void read_coconut_parquet(std::span<const std::filesystem::path> shards, const CoconutImportLimits& limits,
                          mmltk::common::concurrency::CancellationObservation cancellation, const CoconutRecordConsumer& consumer);
struct CoconutImportRequest {
    CoconutEdition edition = CoconutEdition::Base;
    std::string input_identity;
    std::vector<std::filesystem::path> parquet_shards;
    std::filesystem::path annotation_json;
    std::filesystem::path mask_archive;
    std::span<const CoconutPhysicalImage> physical_images;
    std::uint64_t expected_rows = 0;
    CoconutImportLimits limits;
    mmltk::common::concurrency::CancellationObservation cancellation;
    std::function<void(std::uint64_t)> progress;
};
// Every offered record is required. Unknown expected_rows means derive, never sample.
[[nodiscard]] std::vector<CoconutComponent> import_coconut_annotations(const CoconutImportRequest& request);
// Removes only XL rows covered by Large, retaining B and all namespace distinctions.
[[nodiscard]] std::uint64_t reconcile_coconut_extensions(std::vector<CoconutComponent>& components,
    mmltk::common::concurrency::CancellationObservation cancellation = {});
// Full archive inventory, independent of annotations/foreground selection. Cache is identity-bound.
[[nodiscard]] std::vector<CoconutPhysicalImage> coconut_image_archive_inventory(
    const std::filesystem::path& archive_path, const std::filesystem::path& cache_path,
    CoconutImageNamespace source, std::uint16_t shard, std::string archive_identity,
    mmltk::common::concurrency::CancellationObservation cancellation = {});
void store_coconut_component(const std::filesystem::path& index_path, const CoconutComponent& component,
                             mmltk::common::concurrency::CancellationObservation cancellation = {});
[[nodiscard]] std::optional<CoconutComponent> load_coconut_component(
    const std::filesystem::path& index_path, CoconutEdition edition, CoconutImageNamespace source,
    std::string_view input_identity, mmltk::common::concurrency::CancellationObservation cancellation = {});
}  // namespace mmltk::backend::data::benchmark_internal
