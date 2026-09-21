#pragma once
#include "benchmark_annotations.h"
#include "benchmark_cache.h"
#include "benchmark_catalog.h"
#include "benchmark_download.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
class ProgressReporter;
class ArtifactProgressTotals;
[[nodiscard]] std::vector<DownloadResult> repair_annotation_artifacts(std::vector<DownloadRequest>, BenchmarkDatasetSource, std::string_view, ProgressReporter&,
                                                                      ArtifactProgressTotals&, std::size_t, mmltk::common::concurrency::CancellationObservation,
                                                                      const BenchmarkTraceSink&);
[[nodiscard]] std::string extract_archive_member(const std::filesystem::path&, std::string_view, const std::filesystem::path&, std::string_view,
                                                 const std::filesystem::path&, mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&);
[[nodiscard]] std::optional<NormalizedAnnotationIndex> discover_cached_index(const std::filesystem::path&, BenchmarkDatasetSource, std::string_view,
                                                                             mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&);
[[nodiscard]] NormalizedAnnotationIndex load_or_build_index(const BenchmarkCacheLayout&, const std::filesystem::path&, BenchmarkDatasetSource, std::string_view,
                                                            std::string_view, mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&,
                                                            const std::function<NormalizedAnnotationIndex()>&);
// Three body attempts; cancellation and local capacity failures never repair source data.
void retry_annotation_indexing(mmltk::common::concurrency::CancellationObservation, const std::function<void()>&,
                               const std::function<void(const std::exception&)>&);
struct CocoAnnotationIndexes {
 std::filesystem::path train_path, validation_path;
 std::optional<NormalizedAnnotationIndex> train, validation;
 bool cache_hit = false;
 std::uint64_t retained_storage_bytes = 0;
};
// Holds the source lifecycle lease from independent discovery through settlement.
// Callers may acquire other source leases after construction, before discovery.
class CocoAnnotationCache final {
public:
 CocoAnnotationCache(const BenchmarkCacheLayout&, const CatalogArtifact&, bool training, std::uint32_t train_count, std::uint32_t validation_count,
                     int parse_workers, mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&);
 void discover(ProgressReporter&);
 [[nodiscard]] std::uint64_t completed_indexes() const noexcept;
 [[nodiscard]] const std::optional<DownloadRequest>& pending_download() const noexcept { return pending_; }
 void settle(DownloadResult, ProgressReporter&, std::size_t workers, std::uint64_t& completed, std::uint64_t total);
 [[nodiscard]] CocoAnnotationIndexes take_indexes();

private:
 void build_split(bool training, const DownloadResult&, ProgressReporter&, std::uint64_t&, std::uint64_t);
 void invalidate_missing();
 [[nodiscard]] std::filesystem::path source_json(bool training) const;
 const BenchmarkCacheLayout& cache_;
 DownloadRequest request_;
 bool training_;
 std::uint32_t train_count_, validation_count_;
 int parse_workers_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 const BenchmarkTraceSink& trace_;
 ArtifactLease lease_;
 CocoAnnotationIndexes indexes_;
 std::optional<DownloadRequest> pending_;
};
}  // namespace mmltk::backend::data::benchmark_internal
