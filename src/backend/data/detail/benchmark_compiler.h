#pragma once  // backend.data private implementation boundary

#include <string_view>

#include "src/backend/data/benchmark_dataset_compiler.h"

namespace mmltk::backend::data::benchmark_internal {

[[nodiscard]] bool archive_selection_allows_quarantine(BenchmarkDatasetSource source, std::string_view split) noexcept;

void compile_benchmark_dataset(BenchmarkCompilerConfig config);

}  // namespace mmltk::backend::data::benchmark_internal
