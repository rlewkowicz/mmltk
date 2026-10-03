#pragma once
#include "src/backend/data/tests/test_fixture.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <vector>
namespace mmltk::backend::data::testsupport::profile {
enum class ProfileMode : std::uint8_t { Loader, Compile, Pixels, Resample, Coconut, Pipeline };
MMLTK_REFLECT_ENUM(ProfileMode)
enum class ProfileInputStorage : std::uint8_t { Mapped, Pooled };
MMLTK_REFLECT_ENUM(ProfileInputStorage)
struct Options {
 std::string test_dir = "/tmp/mmltk_profile";
 bool keep_artifacts = false;
 int width = 384;
 int height = 384;
 int num_images = 256;
 int batch_size = 32;
 int num_epochs = 2;
 int compile_workers = -1;
 int shuffle_prefetch = 3;
 int repetitions = 10;
 int warmup_runs = 1;
 int task_records = 0, output_cap_mib = 2048;
 ProfileInputStorage input_storage = ProfileInputStorage::Mapped;
 bool completion_gaps = false;
 ProfileMode mode = ProfileMode::Loader;
 FixtureImageEncoding encoding = FixtureImageEncoding::Png;
 int source_width = 0, source_height = 0;
 bool perceptual = false, mixed_geometry = false, shuffle_geometry = false;
 std::string input_manifest, cache_dir, trace_file, cpu_affinity;
 std::vector<std::string> trace_prefix;
};
MMLTK_REFLECT_FIELDS(Options)

[[nodiscard]] Options parse_options(int, char**);
[[nodiscard]] std::vector<ProfileImageFixture> profile_inputs(const Options&);
[[nodiscard]] std::vector<std::size_t> profile_order(const Options&, std::size_t input_count);
[[nodiscard]] std::size_t profile_workers(const Options&);
[[nodiscard]] double seconds_since(std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point);
[[nodiscard]] std::string iteration_label(int);
void record_duration_metric(const char*, const char*, std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point);
void record_value_metric(const char*, const char*, std::uint64_t);
class ProfileTrace final {
 std::ofstream output_;
 std::mutex mutex_;
 std::uint64_t run_ = 0;
 CompileTraceSelection selection_;
 bool selected_ = false;

public:
 ProfileTrace(const std::string&, std::vector<std::string>);
 [[nodiscard]] benchmark_internal::BenchmarkTraceSink sink();
};
void run_pipeline_profile(const Options&, std::span<const ProfileImageFixture>, const benchmark_internal::BenchmarkTraceSink&);
}  // namespace mmltk::backend::data::testsupport::profile
