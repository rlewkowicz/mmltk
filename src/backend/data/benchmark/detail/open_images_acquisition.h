#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/common/concurrency/cancellation_observation.h"
#include "src/backend/data/benchmark/detail/benchmark_images.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_progress.h"
namespace mmltk::backend::data::benchmark_internal {
struct QuarantinedImage {
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
 std::uint64_t image_id = 0U;
 std::string reason;
 // Manifest order is the declaration order of these fields; the type owns it so no call site
 // spells the comparison out again.
 auto operator<=>(const QuarantinedImage&) const = default;
};
struct AcquiredOpenImages {
 CachedImageDirectory directory;
 std::vector<std::uint64_t> available_image_ids;
};
[[nodiscard]] AcquiredOpenImages acquire_open_images(const BenchmarkCacheLayout& cache, NormalizedAnnotationIndex& index, std::vector<QuarantinedImage>* quarantined,
 mmltk::common::concurrency::CancellationObservation cancel_requested, ProgressReporter* progress, const int num_workers, const std::size_t cache_workers, const BenchmarkTraceSink& trace,
 const std::optional<ImageDecodeProbe> decode_probe = std::nullopt, BenchmarkCompilePipeline* execution = nullptr);
}  // namespace mmltk::backend::data::benchmark_internal
