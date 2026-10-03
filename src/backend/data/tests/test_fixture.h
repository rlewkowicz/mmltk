#pragma once
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/compiler/dataset_compiler.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/io/file_memory.h"
#include <filesystem>
#include <memory>
#include <span>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::data {
struct ImageEntry;
struct FileHeader;
namespace benchmark_internal {
class BenchmarkCompilePipeline;
class BenchmarkSplitWriter;
class ProgressReporter;
class IndexingProgressTotals;
struct PreparedBenchmarkSplit;
struct EncodedImageRecord;
struct BenchmarkWriteRequest;
struct BenchmarkLabelChunk;
class NormalizedAnnotationReadView;
}  // namespace benchmark_internal
}  // namespace mmltk::backend::data
namespace mmltk::backend::data::testsupport {
enum class FixtureImageEncoding : std::uint8_t { Png, Jpeg };
MMLTK_REFLECT_ENUM(FixtureImageEncoding)
struct ProfileImageFixture {
 std::uint32_t width = 0, height = 0;
 std::vector<std::uint8_t> encoded;
};
[[nodiscard]] ProfileImageFixture profile_image_fixture(std::uint32_t width, std::uint32_t height, FixtureImageEncoding, std::uint32_t seed = 7, std::size_t byte_limit = 256U << 20);
// Bounded PAX/gzip source with deterministic member names, including a long
// first pathname. Preflight logical bytes and tar overhead before writing.
[[nodiscard]] std::vector<std::string> profile_archive_fixture(const std::filesystem::path&, std::span<const ProfileImageFixture>, std::span<const std::size_t> order, std::uint64_t byte_limit = 16U << 20);
// Preflight every actual RGB extent and peak decoder workspace before retaining
// any pixels. Limits are explicit so standard cases use small real fixtures.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> retained_profile_rgb(std::span<const ProfileImageFixture>, std::size_t retained_limit, std::size_t workspace_limit);
// Deterministic source-ordered rows: every fourth image is a background.
[[nodiscard]] benchmark_internal::NormalizedAnnotationReadView pipeline_annotation_fixture(std::span<const ProfileImageFixture>, std::span<const std::size_t> order);
// Enabled only by MMLTK_TEST_BENCHMARK_TRACE for focused failure diagnosis.
[[nodiscard]] benchmark_internal::BenchmarkTraceSink benchmark_trace();
// Seed the mixed known/unknown Coconut extents at the reported progress boundary.
[[nodiscard]] benchmark_internal::IndexingProgressTotals& mixed_coconut_row_progress(benchmark_internal::ProgressReporter&);
[[nodiscard]] mmltk::common::io::FileHandle create_compiled_fixture_file(const std::filesystem::path&, const FileHeader&);
[[nodiscard]] std::string benchmark_semantics(const std::filesystem::path&);
void bind_benchmark_slots(benchmark_internal::BenchmarkCompilePipeline&, benchmark_internal::BenchmarkSplitWriter&, const benchmark_internal::PreparedBenchmarkSplit&);
// Materialize input and file extents before holding a CPU in cooperation cases.
// The returned task runs the writer's actual decode/resize/store kernel.
[[nodiscard]] detail::CompileExecution::Task prepare_benchmark_pixel(benchmark_internal::BenchmarkCompilePipeline&, benchmark_internal::BenchmarkSplitWriter&, std::uint32_t slot = 0);
[[nodiscard]] ImageEntry benchmark_entry(const benchmark_internal::EncodedImageRecord&);
void write_benchmark_fixture(const benchmark_internal::BenchmarkWriteRequest&, std::span<const std::shared_ptr<benchmark_internal::BenchmarkLabelChunk>>);
struct FixtureSpec {
 std::string root_dir;
 std::string split = "train";
 int width = 65;
 int height = 65;
 int num_images = 20;
 int first_class_id = 1;
 int background_images = 10;
 bool pixel_evidence = false;
};
std::string dataset_dir(const FixtureSpec& spec);
std::string compiled_dir(const FixtureSpec& spec);
std::string compiled_bin_path(const FixtureSpec& spec);
void create_synthetic_dataset(const FixtureSpec& spec);
[[nodiscard]] CompilerConfig compiler_config(const FixtureSpec& spec);
void compile_existing_fixture(const FixtureSpec& spec);
void replace_synthetic_image(const FixtureSpec& spec, int image_index, int width, int height);
std::vector<float> expected_nchw_stub(const std::string& path, int width, int height);
// Retained compiler algorithm: RGB8 resize, then the public planar converter.
std::vector<float> expected_resized_rgb(std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height, std::uint32_t target_width, std::uint32_t target_height,
 mmltk::backend::imaging::resample::ImageResizeMode mode, bool perceptual);
void assert_image_matches(const float* actual, const std::vector<float>& expected);
}  // namespace mmltk::backend::data::testsupport
