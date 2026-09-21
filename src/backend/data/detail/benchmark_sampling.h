#pragma once  // backend.data private implementation boundary
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include "benchmark_annotations.h"
#include "src/common/concurrency/cancellation_observation.h"
namespace mmltk::backend::data::benchmark_internal {
inline constexpr std::string_view kSupplementalSamplingRevision = "coco80-adaptive-objects-heavy-v2";
inline constexpr std::uint32_t kSupplementalBudgetDenominator = 6U;
inline constexpr std::uint32_t kOpenImagesDiversityDenominator = 24U;
inline constexpr std::uint32_t kOpenImagesMaximumDenominator = 6U;
struct SupplementalSamplingStats {
 std::uint64_t full_images = 0U;
 std::uint64_t full_boxes = 0U;
 std::uint64_t selected_images = 0U;
 std::uint64_t selected_boxes = 0U;
 std::array<std::uint64_t, 80> available_class_images{};
 std::array<std::uint64_t, 80> selected_class_images{};
};
struct SupplementalSamplingResult {
 NormalizedAnnotationIndex index;
 SupplementalSamplingStats stats;
};
struct CombinedSupplementalSamplingResult {
 SupplementalSamplingResult objects365;
 SupplementalSamplingResult open_images;
 std::uint64_t target_images = 0U;
 std::uint64_t open_images_floor = 0U;
 std::uint64_t open_images_ceiling = 0U;
 std::uint64_t objects365_archive_bytes = 0U;
 std::vector<std::uint16_t> objects365_shards;
};
[[nodiscard]] CombinedSupplementalSamplingResult sample_combined_supplemental_indices(
 const NormalizedAnnotationIndex& coco_train, const NormalizedAnnotationIndex& objects365, const NormalizedAnnotationIndex& open_images,
 std::span<const std::uint64_t> objects365_shard_bytes, mmltk::common::concurrency::CancellationObservation cancel_requested = {});
}  // namespace mmltk::backend::data::benchmark_internal
