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
};
struct DownloadProgress {
    std::string artifact_id;
    std::uint64_t completed_bytes = 0U;
    std::uint64_t total_bytes = 0U;
    std::uint32_t attempt = 0U;
    bool resumed = false;
    bool cache_hit = false;
    DownloadProgressPhase phase = DownloadProgressPhase::kDownloading;
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
using DownloadProgressSink = std::function<void(const DownloadProgress&)>;
[[nodiscard]] std::vector<DownloadResult> download_artifacts(const std::vector<DownloadRequest>& requests, std::size_t maximum_concurrency,
                                                             mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                             const DownloadProgressSink& progress = {}, const BenchmarkTraceSink& trace = {});
void invalidate_download_artifact(const DownloadRequest& request, mmltk::common::concurrency::CancellationObservation cancel_requested = {},
                                  const BenchmarkTraceSink& trace = {});
}  // namespace mmltk::backend::data::benchmark_internal
