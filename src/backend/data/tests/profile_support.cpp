#include "src/backend/data/tests/profile_support.h"
#include "src/backend/data/detail/compile_image_decoder.h"
#include "src/backend/data/compiled/compiled_file_layout.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/frameworks/reflection/cli_declarations.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/common/system/cpu_affinity.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <utility>
import mmltk.common.logging.profile_utils;
namespace fs = std::filesystem;
namespace mmltk::backend::data::testsupport::profile {
using namespace mmltk::common::logging;
double seconds_since(const std::chrono::steady_clock::time_point start, const std::chrono::steady_clock::time_point end) { return std::chrono::duration<double>(end - start).count(); }
std::string metric_name(const char* label, const char* suffix) { return std::string("benchmark.") + label + "." + suffix; }
void record_duration_metric(const char* label, const char* suffix, const std::chrono::steady_clock::time_point start, const std::chrono::steady_clock::time_point end) {
 profile_record_duration_ns(metric_name(label, suffix).c_str(), static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
}
void record_value_metric(const char* label, const char* suffix, const std::uint64_t value) { profile_add_value(metric_name(label, suffix).c_str(), value); }
std::string iteration_label(const int repetition) {
 std::array<char, 64> buffer{};
 std::snprintf(buffer.data(), buffer.size(), "backend.data[%02d]", repetition);
 return buffer.data();
}
using ProfileOptions = mmltk::frameworks::reflection::CliScope<Options>;
inline constexpr std::array profile_options{
 MMLTK_CLI_OPTION(ProfileOptions, input_storage, "Pipeline input custody: mapped or pooled", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, task_records, "Pipeline task records, at most 1024; zero uses production sizing", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, output_cap_mib, "Staging/output cap: Pipeline <=2048 MiB, Compaction <=16384 MiB", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, completion_gaps, "Collect bounded completion spacing with profile.pipeline.completed selected", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, compaction_case, "Compaction fixture: Dense, Sparse, MixedLabels", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, compaction_cold, "Sync and advise only the staged fixture out of cache before compaction", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, mode, "loader, compile (Directory STB), pixels (benchmark decode), resample (retained RGB), coconut (Stock + recovery), pipeline (bounded local benchmark), compaction (bounded staged file)", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, encoding, "PNG or JPEG synthetic input", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, input_manifest, "Retained JPEG/PNG paths, one per line; pixels/resample only", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, cache_dir, "Retained production benchmark cache; required for coconut", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, trace_file, "Opt-in causal JSONL; omit for throughput", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, trace_prefix, "Collect matching event prefixes only (repeatable)", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, test_dir, "Synthetic working directory or isolated Coconut output root", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, keep_artifacts, "Retain generated fixtures", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, perceptual, "Enable perceptual shrinking", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, mixed_geometry, "Cycle the seven acceptance geometries", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, shuffle_geometry, "Shuffle input order with seed 1729", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, source_width, "Synthetic source width; zero uses target width", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, source_height, "Synthetic source height; zero uses target height", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, width, "Target width", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, height, "Target height", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, num_images, "Images per measured iteration", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, batch_size, "Loader batch size", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, num_epochs, "Loader epochs", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, compile_workers, "CPU workers, -1 automatic", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, shuffle_prefetch, "Loader shuffle prefetch", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, repetitions, "Measured iterations", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, warmup_runs, "Unmeasured iterations", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, cpu_affinity, "Linux CPU list, inherited by all production workers", "Profiling"),
};
Options parse_options(int argc, char** argv) {
 std::vector<std::string_view> arguments;
 for (int i = 1; i < argc; ++i) {
  if (std::string_view(argv[i]) == "--help") {
   std::puts(mmltk::frameworks::reflection::help(argv[0], "Opt-in production CPU and loader profiles", profile_options).c_str());
   std::exit(0);
  }
  arguments.emplace_back(argv[i]);
 }
 auto parsed = mmltk::frameworks::reflection::parse<Options>(arguments, profile_options);
 if (!parsed) throw parsed.error();
 Options opts = std::move(parsed->request);
 if (!opts.source_width) opts.source_width = opts.width;
 if (!opts.source_height) opts.source_height = opts.height;
 const auto valid_extent = [](int extent) { return extent > 0 && std::cmp_less_equal(extent, MAX_IMAGE_EXTENT); };
 if (!valid_extent(opts.width) || !valid_extent(opts.height) || !valid_extent(opts.source_width) || !valid_extent(opts.source_height) || opts.num_images <= 0 || opts.batch_size <= 0 ||
     opts.num_epochs <= 0 || opts.shuffle_prefetch <= 0 || opts.repetitions <= 0 || opts.warmup_runs < 0 || opts.compile_workers == 0 || opts.compile_workers < -1)
  throw std::invalid_argument("invalid profile dimensions or counts");
 if (opts.mode == ProfileMode::Coconut && (opts.cache_dir.empty() || opts.width != opts.height)) throw std::invalid_argument("coconut mode requires --cache-dir and square target dimensions");
 if (!opts.trace_prefix.empty() && opts.trace_file.empty()) throw std::invalid_argument("trace-prefix requires trace-file");
 if (!opts.input_manifest.empty() && opts.mode != ProfileMode::Pixels && opts.mode != ProfileMode::Resample) throw std::invalid_argument("retained input manifests require pixels or resample mode");
 if (opts.mode == ProfileMode::Pipeline && (opts.num_images < 256 || opts.num_images > 1024 || opts.width != opts.height || opts.output_cap_mib <= 0 || opts.output_cap_mib > 2048 || opts.task_records < 0 || opts.task_records > 1024))
  throw std::invalid_argument("pipeline requires 256-1024 images, square output, 0-1024 records, and 1-2048 MiB output cap");
 if (opts.mode == ProfileMode::Pipeline) {
  namespace math = mmltk::common::math;
  const auto pixels = math::checked_multiply<std::size_t>(opts.width, opts.height, "pipeline output extent overflow");
  const auto stride = math::checked_multiply(pixels, 3 * sizeof(float), "pipeline output stride overflow");
  const auto layout = compute_file_layout(static_cast<std::uint32_t>(opts.num_images), stride);
  if (layout.total_size > (static_cast<std::uint64_t>(opts.output_cap_mib) << 20)) throw std::length_error("pipeline staging/output exceeds configured cap before fixture creation");
 }
 if (opts.mode == ProfileMode::Compaction) (void)profile_compaction_layout(opts);
 return opts;
}
FileLayout profile_compaction_layout(const Options& opts) {
 if (opts.num_images <= 0 || opts.num_images > 4096 || opts.width <= 0 || opts.height <= 0 || std::cmp_greater(opts.width, MAX_IMAGE_EXTENT) || std::cmp_greater(opts.height, MAX_IMAGE_EXTENT) ||
     opts.output_cap_mib <= 0 || opts.output_cap_mib > 16384)
  throw std::invalid_argument("compaction requires 1-4096 slots, valid dimensions, and 1-16384 MiB output cap");
 namespace math = mmltk::common::math;
 const auto pixels = math::checked_multiply<std::size_t>(opts.width, opts.height, "compaction pixel extent overflow");
 const auto layout = compute_file_layout(static_cast<std::uint32_t>(opts.num_images), math::checked_multiply(pixels, 3 * sizeof(float), "compaction stride overflow"));
 if (layout.total_size > (static_cast<std::uint64_t>(opts.output_cap_mib) << 20)) throw std::length_error("compaction staging/output exceeds configured cap before fixture creation");
 return layout;
}
ProfileTrace::ProfileTrace(const std::string& path, std::vector<std::string> prefixes)
    : selection_(prefixes), selected_(!prefixes.empty()) {
 if (path.empty()) return;
 if (const auto parent = fs::path(path).parent_path(); !parent.empty()) fs::create_directories(parent);
 output_.open(path, std::ios::app);
 run_ = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
 if (!output_) throw std::runtime_error("cannot open profile trace");
}
benchmark_internal::BenchmarkTraceSink ProfileTrace::sink() {
 if (!output_.is_open()) return {};
 return {selection_, [this](std::string_view event, const nlohmann::json& fields) {
  const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const std::lock_guard lock(mutex_);
  output_ << nlohmann::json{{"event", event}, {"steady_ns", stamp}, {"run_id", run_}, {"fields", fields}}.dump() << '\n';
  if (selected_) output_.flush();
 }};
}
std::vector<std::size_t> profile_order(const Options& opts, std::size_t input_count) {
 if (!input_count) throw std::invalid_argument("profile inputs are empty");
 std::vector<std::size_t> order(static_cast<std::size_t>(opts.num_images));
 for (std::size_t i = 0; i < order.size(); ++i) order[i] = i % input_count;
 if (opts.shuffle_geometry) {
  std::mt19937 random(1729);
  std::shuffle(order.begin(), order.end(), random);
 }
 return order;
}
std::size_t profile_workers(const Options& opts) {
 const auto available = std::max<std::size_t>(1, mmltk::common::system::allowed_cpu_set().size());
 return opts.compile_workers < 0 ? available : std::min(available, static_cast<std::size_t>(opts.compile_workers));
}
std::vector<ProfileImageFixture> profile_inputs(const Options& opts) {
 std::vector<ProfileImageFixture> inputs;
 std::size_t synthetic_remaining = opts.mode == ProfileMode::Pipeline ? 64U << 20 : 256U << 20;
 const auto synthetic = [&](unsigned width, unsigned height) {
  auto input = profile_image_fixture(width, height, opts.encoding, 7, synthetic_remaining);
  if (opts.mode == ProfileMode::Pipeline) synthetic_remaining -= input.encoded.size();
  inputs.push_back(std::move(input));
 };
 if (!opts.input_manifest.empty()) {
  std::ifstream manifest(opts.input_manifest);
  if (!manifest) throw std::runtime_error("cannot open profile input manifest");
  std::string line;
  std::uint64_t retained_bytes = 0;
  while (std::getline(manifest, line)) {
   if (line.empty()) continue;
   if (inputs.size() == 64) throw std::length_error("profile manifest exceeds 64 retained images");
   auto path = fs::path(line);
   if (path.is_relative()) path = fs::path(opts.input_manifest).parent_path() / path;
   const auto input = mmltk::common::io::MappedFile::open_readonly(path.string());
   retained_bytes += input.size();
   if (retained_bytes > (256ULL << 20)) throw std::length_error("profile encoded inputs exceed 256 MiB");
   detail::CompileImageDecoder decoder;
   const auto header = decoder.read_header({input.data(), input.size()}, detail::CompileImagePolicy::Benchmark);
   inputs.push_back({header.width, header.height, {input.data(), input.data() + input.size()}});
  }
 } else if (opts.mixed_geometry) {
  for (const auto [width, height] : std::array<std::pair<unsigned, unsigned>, 7>{{{384, 384}, {640, 480}, {768, 768}, {1024, 768}, {2048, 2048}, {640, 360}, {320, 240}}})
   synthetic(width, height);
 } else
  synthetic(static_cast<unsigned>(opts.source_width), static_cast<unsigned>(opts.source_height));
 if (inputs.empty()) throw std::invalid_argument("profile input manifest is empty");
 return inputs;
}
}  // namespace mmltk::backend::data::testsupport::profile
