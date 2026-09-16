#pragma once  // backend.data private implementation boundary
#include <string_view>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "src/backend/data/benchmark_dataset_compiler.h"
namespace mmltk::backend::data::benchmark_internal {
[[nodiscard]] bool archive_selection_allows_quarantine(BenchmarkDatasetSource source, std::string_view split) noexcept;
// Completes the benchmark manifest envelope from acquired source/split facts
// and atomically publishes it into the existing staging transaction.
void publish_benchmark_manifest(const BenchmarkCompilerConfig&, const std::filesystem::path& staging_dir, const std::filesystem::path& cache_root,
                                nlohmann::json facts, mmltk::common::concurrency::CancellationObservation);
void compile_benchmark_dataset(BenchmarkCompilerConfig config);
}  // namespace mmltk::backend::data::benchmark_internal
