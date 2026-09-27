#pragma once  // backend.data private implementation boundary
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/compiler/dataset_compile_progress.h"
#include "src/backend/data/benchmark/detail/benchmark_catalog.h"
#include <string_view>
#include <stdexcept>
namespace mmltk::backend::data::benchmark_internal {
// Only remote/source unavailability or rejected response content. Local setup, allocation,
// invalid requests, callback failures, storage failures, and cancellation propagate
// separately and must never permit optional-source omission.
class BenchmarkDownloadUnavailable final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
inline constexpr std::uint32_t kMaximumAttempts = 5U;
enum class DownloadProgressPhase : std::uint8_t {
 kDownloading,
 kVerifyingCachedArtifact,
 kVerifyingDownloadedArtifact,
};
struct DownloadRequest {
 std::string artifact_id;
 std::string url;
 std::filesystem::path destination;
 std::filesystem::path lock_path;
 std::uint64_t expected_size = 0U;
 std::optional<std::string> expected_sha256;
 std::uint32_t maximum_attempts = kMaximumAttempts;
 // An owning acquisition invalidated an earlier artifact before this request.
 bool redownload = false;
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
 // Source-owned estimate for an artifact whose HTTP/catalog size is unknown.
 std::uint64_t storage_estimate = 0;
};
struct DownloadProgress {
 std::string artifact_id;
 BenchmarkTransferProgress transfer{};
 DownloadProgressPhase phase = DownloadProgressPhase::kDownloading;
 bool redownload = false;
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
};
struct DownloadResult {
 std::filesystem::path path;
 std::uint64_t size = 0U;
 std::string identity;
 std::string failure_sha256;
 std::string etag;
 std::string last_modified;
 std::uint32_t attempts = 0U;
 bool resumed = false;
 bool cache_hit = false;
};
[[nodiscard]] DownloadRequest make_download_request(const BenchmarkCacheLayout&, std::string_view, const CatalogArtifact&);
struct DownloadReady {
 std::size_t request_index = 0;
 DownloadResult artifact;
};
// Short enqueue-only consumer; called after releasing the completed artifact's
// lock. Ownership of the durable result crosses this boundary by value.
using DownloadReadySink = std::function<void(DownloadReady)>;
using DownloadProgressSink = std::function<void(const DownloadProgress&)>;
[[nodiscard]] std::vector<DownloadResult> download_artifacts(const std::vector<DownloadRequest>& requests, std::size_t maximum_concurrency,
 mmltk::common::concurrency::CancellationObservation cancel_requested, const DownloadProgressSink& progress = {}, const BenchmarkTraceSink& trace = {}, const DownloadReadySink& ready = {},
 BenchmarkCompilePipeline* execution = nullptr, const BenchmarkAllowance& parent = {}, StorageReservationPool* storage = nullptr);
void invalidate_download_artifact(const DownloadRequest& request, mmltk::common::concurrency::CancellationObservation cancel_requested = {}, const BenchmarkTraceSink& trace = {},
 BenchmarkCompilePipeline* execution = nullptr, const BenchmarkAllowance& parent = {});
}  // namespace mmltk::backend::data::benchmark_internal
