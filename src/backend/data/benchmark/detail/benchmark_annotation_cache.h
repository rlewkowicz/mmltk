#pragma once
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_catalog.h"
#include "src/backend/data/benchmark/detail/benchmark_download.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCompilePipeline;
class StorageReservationPool;
class ProgressReporter;
class ArtifactProgressTotals;
[[nodiscard]] std::vector<DownloadResult> repair_annotation_artifacts(std::vector<DownloadRequest>, BenchmarkDatasetSource, std::string_view, ProgressReporter&, ArtifactProgressTotals&, std::size_t,
 mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&, BenchmarkCompilePipeline* execution = nullptr, const BenchmarkAllowance& parent = {}, StorageReservationPool* storage = nullptr);
[[nodiscard]] std::string extract_archive_member(const std::filesystem::path&, std::string_view, const std::filesystem::path&, std::string_view, const std::filesystem::path&,
 mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&, StorageReservationPool* storage = nullptr, BenchmarkCompilePipeline* execution = nullptr, const BenchmarkAllowance& parent = {});
[[nodiscard]] std::optional<NormalizedAnnotationIndex> discover_cached_index(
 const std::filesystem::path&, BenchmarkDatasetSource, std::string_view, mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&);
[[nodiscard]] NormalizedAnnotationIndex load_or_build_index(const BenchmarkCacheLayout&, const std::filesystem::path&, BenchmarkDatasetSource, std::string_view, std::string_view,
 mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&, const std::function<NormalizedAnnotationIndex()>&, StorageReservationPool* storage = nullptr, BenchmarkCompilePipeline* execution = nullptr, const BenchmarkAllowance& parent = {});
// Three body attempts; cancellation and local capacity failures never repair source data.
void retry_annotation_indexing(mmltk::common::concurrency::CancellationObservation, const std::function<void()>&, const std::function<void(const std::exception&)>&);
enum class CocoSplitAdmission : std::uint8_t { Unselected, Optional, Required };
struct CocoAnnotationRequest {
 CocoSplitAdmission train = CocoSplitAdmission::Unselected;
 CocoSplitAdmission validation = CocoSplitAdmission::Unselected;
};
struct CocoAnnotationIndexes {
 std::filesystem::path train_path, validation_path;
 std::optional<NormalizedAnnotationIndex> train, validation;
 bool cache_hit = false;
 std::uint64_t retained_storage_bytes = 0;
};
// Holds the source lifecycle lease from independent discovery through settlement.
// Callers may acquire other source leases after construction, before discovery.
[[nodiscard]] BenchmarkResources coco_annotation_resources();
class CocoAnnotationCache final {
public:
 CocoAnnotationCache(const BenchmarkCacheLayout&, const CatalogArtifact&, CocoAnnotationRequest, std::uint32_t train_count, std::uint32_t validation_count, int parse_workers,
  mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&, BenchmarkCompilePipeline* execution = nullptr, StorageReservationPool* storage = nullptr, std::shared_ptr<ArtifactLease> custody = {});
 void discover(ProgressReporter&);
 [[nodiscard]] std::uint64_t completed_indexes() const noexcept;
 [[nodiscard]] const std::optional<DownloadRequest>& pending_download() const noexcept { return pending_; }
 void download_unavailable(const BenchmarkDownloadUnavailable&, ProgressReporter&);
 void settle(DownloadResult, ProgressReporter&, std::size_t workers, std::uint64_t& completed, std::uint64_t total);
 [[nodiscard]] CocoAnnotationIndexes take_indexes();
 [[nodiscard]] const BenchmarkAllowance& allowance() const noexcept { return lease_->allowance(); }

private:
 void build_split(bool training, const DownloadResult&, ProgressReporter&, std::uint64_t&, std::uint64_t);
 void invalidate_missing();
 void warn_unavailable(ProgressReporter&);
 [[nodiscard]] std::filesystem::path source_json(bool training) const;
 const BenchmarkCacheLayout& cache_;
 DownloadRequest request_;
 CocoAnnotationRequest selection_;
 bool warned_unavailable_ = false;
 std::uint32_t train_count_, validation_count_;
 int parse_workers_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 const BenchmarkTraceSink& trace_;
 std::unique_ptr<BenchmarkCurl::Channel> source_transport_;
 std::shared_ptr<ArtifactLease> lease_;
 BenchmarkCompilePipeline* execution_ = nullptr;
 StorageReservationPool* storage_ = nullptr;
 CocoAnnotationIndexes indexes_;
 std::optional<DownloadRequest> pending_;
};
}  // namespace mmltk::backend::data::benchmark_internal
