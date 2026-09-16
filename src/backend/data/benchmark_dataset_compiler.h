#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "src/common/concurrency/cancellation_observation.h"
#include "src/backend/data/dataset_compile_phase.h"
namespace mmltk::backend::data {
enum class BenchmarkDatasetSource : std::uint8_t {
    kCoco2017,
    kObjects365V2,
    kOpenImagesV7,
};
struct BenchmarkSourceProgress {
    BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
    std::string activity;
    std::uint64_t completed_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t completed_images = 0;
    std::uint64_t total_images = 0;
    std::uint32_t retry_count = 0;
    bool cache_hit = false;
    bool resumed = false;
    bool complete = false;
};
struct BenchmarkCompileProgress {
    DatasetCompilePhase phase = DatasetCompilePhase::Planning;
    std::string activity;
    std::uint64_t activity_elapsed_seconds = 0;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::uint64_t pixel_attempt = 0;
    std::uint64_t pixel_attempt_offset = 0;
    std::uint64_t projected_output_bytes = 0;
    std::uint64_t dropped_instances = 0;
    std::uint64_t quarantined_images = 0;
    std::vector<BenchmarkSourceProgress> sources;
};
using BenchmarkProgressCallback = std::function<void(const BenchmarkCompileProgress&)>;
using BenchmarkTraceCallback = std::function<void(std::string_view event, std::string_view json_fields)>;
[[nodiscard]] std::string format_benchmark_source_status(const BenchmarkSourceProgress& progress, std::string_view default_status);
struct BenchmarkCompilerConfig {
    std::filesystem::path output_dir{"./compiled"};
    std::filesystem::path cache_dir;
    std::uint32_t resolution = 432;
    int num_workers = -1;
    bool overwrite = false;
    bool perceptual_downscale = false;
    mmltk::common::concurrency::CancellationObservation cancel_requested;
    BenchmarkProgressCallback progress;
    BenchmarkTraceCallback trace;
};
class BenchmarkDatasetCompiler {
   public:
    static void compile(BenchmarkCompilerConfig config);
};
}  // namespace mmltk::backend::data
