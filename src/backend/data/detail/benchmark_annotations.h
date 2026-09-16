#pragma once  // backend.data private implementation boundary
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "benchmark_cache.h"
#include "benchmark_catalog.h"
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/compiled_format.h"
namespace mmltk::backend::data::benchmark_internal {
struct __attribute__((packed)) NormalizedBox {
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    std::uint64_t mask_rle_offset = 0U;
    std::uint32_t mask_rle_pairs = 0U;
    std::uint8_t class_id = 0U;
    std::uint8_t reserved[3]{};
};
static_assert(sizeof(NormalizedBox) == 32U);
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
struct NormalizedAnnotationIndex {
    BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
    std::string split;
    std::string annotation_sha256;
    std::vector<NormalizedImage> images;
    std::vector<NormalizedBox> boxes;
    std::vector<RLEPair> mask_rle_pairs;
    AnnotationRejectCounts rejected;
};
struct AnnotationParseOptions {
    BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
    std::string split;
    std::uint32_t expected_image_count = 0U;
    int num_workers = 1;
    bool keep_images_without_mapped_boxes = false;
    mmltk::common::concurrency::CancellationObservation cancel_requested = {};
    BenchmarkTraceSink trace;
};
[[nodiscard]] NormalizedAnnotationIndex parse_coco_style_annotations(const std::filesystem::path& json_path, std::string annotation_sha256,
                                                                     std::span<const NumericCategoryMapping> mappings, const AnnotationParseOptions& options);
[[nodiscard]] NormalizedAnnotationIndex parse_open_images_annotations(const std::filesystem::path& boxes_csv_path,
                                                                      const std::filesystem::path& classes_csv_path, std::string annotation_sha256,
                                                                      std::span<const StringCategoryMapping> mappings, const AnnotationParseOptions& options);
[[nodiscard]] std::optional<NormalizedAnnotationIndex> load_normalized_annotation_index(const std::filesystem::path& path,
                                                                                        BenchmarkDatasetSource expected_source, std::string_view expected_split,
                                                                                        std::string_view expected_annotation_sha256,
                                                                                        mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                                                        const BenchmarkTraceSink& trace = {});
void store_normalized_annotation_index(const std::filesystem::path& path, const NormalizedAnnotationIndex& index,
                                       mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace = {});
[[nodiscard]] std::vector<std::uint64_t> image_ids(const NormalizedAnnotationIndex&, std::optional<std::uint16_t> shard = std::nullopt);
}  // namespace mmltk::backend::data::benchmark_internal
