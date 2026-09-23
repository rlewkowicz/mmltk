#pragma once
#include "src/backend/data/benchmark_dataset_options.h"
#include "src/backend/imaging/resample/image_resize.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/common/concurrency/cancellation_observation.h"
#include "src/backend/data/dataset_compile_phase.h"
#include "src/backend/data/dataset_compile_progress.h"
namespace mmltk::backend::data {
struct BenchmarkCompileProgress {
 DatasetCompilePhase phase = DatasetCompilePhase::Planning;
 std::string activity;
 std::uint64_t activity_elapsed_seconds = 0;
 std::uint64_t completed = 0;
 std::uint64_t total = 0;
 DatasetCompileTracks tracks{};
 std::uint64_t projected_output_bytes = 0;
 std::uint64_t dropped_instances = 0;
 std::uint64_t quarantined_images = 0;
 std::optional<BenchmarkDatasetSource> current_source = std::nullopt;
 std::vector<BenchmarkSourceProgress> sources;
};
using BenchmarkProgressCallback = std::function<void(const BenchmarkCompileProgress&)>;
// Synchronous borrowed views. Empty json_fields reports diagnostic construction or
// encoding failure; setup failure also has an empty event. Consumers must not
// publish it as a successful record. Callback failures never affect compilation.
using BenchmarkTraceCallback = std::function<void(std::string_view event, std::string_view json_fields)>;
[[nodiscard]] std::string format_benchmark_source_status(const BenchmarkSourceProgress& progress, std::string_view default_status);
struct BenchmarkCompilerConfig {
 BenchmarkDatasetSelection selection;
 std::filesystem::path output_dir{"./compiled"};
 // Optional final destination for callers that publish output_dir elsewhere.
 // Used only for cache overlap admission; empty means output_dir.
 std::filesystem::path publication_dir;
 std::filesystem::path cache_dir;
 std::uint32_t resolution = 432;
 int num_workers = -1;
 bool overwrite = false;
 bool perceptual_downscale = false;
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
 mmltk::common::concurrency::CancellationObservation cancel_requested;
 BenchmarkProgressCallback progress;
 BenchmarkTraceCallback trace;
};
void compile_benchmark_dataset(BenchmarkCompilerConfig config);
}  // namespace mmltk::backend::data
