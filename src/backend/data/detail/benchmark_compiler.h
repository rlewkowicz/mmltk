#pragma once  // backend.data private implementation boundary
#include <string_view>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "benchmark_cache.h"
#include "benchmark_download.h"
#include "benchmark_catalog.h"
namespace mmltk::backend::data::benchmark_internal {
[[nodiscard]] DownloadRequest make_download_request(const BenchmarkCacheLayout&, std::string_view, const CatalogArtifact&);
[[nodiscard]] std::string extract_archive_member(const std::filesystem::path&, std::string_view, const std::filesystem::path&,
    std::string_view, const std::filesystem::path&, mmltk::common::concurrency::CancellationObservation, const BenchmarkTraceSink&);
[[nodiscard]] BenchmarkTraceSink make_trace_sink(const BenchmarkTraceCallback& callback) noexcept;
[[nodiscard]] bool archive_selection_allows_quarantine(BenchmarkDatasetSource source, std::string_view split) noexcept;
// Completes the benchmark manifest envelope from acquired source/split facts
// and atomically publishes it into the existing staging transaction.
void publish_benchmark_manifest(const BenchmarkCompilerConfig&, const std::filesystem::path& staging_dir, const std::filesystem::path& cache_root,
                                nlohmann::json facts, mmltk::common::concurrency::CancellationObservation);
}  // namespace mmltk::backend::data::benchmark_internal
