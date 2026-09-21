#pragma once  // backend.data private implementation boundary
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "benchmark_cache.h"
#include "benchmark_catalog.h"
#include <string_view>
namespace mmltk::backend::data::benchmark_internal {
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
};
struct DownloadProgress {
 std::string artifact_id;
 // Retained prefix plus accepted artifact writes; discarded HTTP bodies never count.
 std::uint64_t completed_bytes = 0U;
 // Zero remains unknown until an admitted response or successful settlement establishes size.
 std::uint64_t total_bytes = 0U;
 std::uint32_t attempt = 0U;
 bool resumed = false;
 bool cache_hit = false;
 DownloadProgressPhase phase = DownloadProgressPhase::kDownloading;
 // Durable bytes retained before the active attempt; excludes in-flight observations.
 std::uint64_t retained_bytes = 0U;
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
using DownloadProgressSink = std::function<void(const DownloadProgress&)>;
[[nodiscard]] std::vector<DownloadResult> download_artifacts(const std::vector<DownloadRequest>& requests, std::size_t maximum_concurrency,
                                                             mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                             const DownloadProgressSink& progress = {}, const BenchmarkTraceSink& trace = {});
void invalidate_download_artifact(const DownloadRequest& request, mmltk::common::concurrency::CancellationObservation cancel_requested = {},
                                  const BenchmarkTraceSink& trace = {});
}  // namespace mmltk::backend::data::benchmark_internal
