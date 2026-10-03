#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include <cuda_runtime.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <algorithm>
#include <fstream>
#include <numeric>
#include <random>
#include <mutex>
#include <vector>
#include <memory>
#include <span>
#include <stdexcept>
#include <cstdint>
#include <utility>
#include <nlohmann/json.hpp>
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/common/io/file_memory.h"
#include "src/backend/data/benchmark/benchmark_dataset_compiler.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/frameworks/reflection/cli_declarations.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiler/dataset_compiler.h"
#include "src/backend/data/loading/dataset_loader.h"
#include "src/backend/data/tests/test_fixture.h"
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/cuda/cuda_error.h"
using namespace mmltk::backend::data;
using namespace mmltk::common::logging;
using namespace mmltk::common::system;
using mmltk::frameworks::gpu::ensure_cuda_ok;
using namespace mmltk::backend::data::testsupport;
namespace fs = std::filesystem;
namespace {
enum class ProfileMode : std::uint8_t { Loader, Compile, Pixels, Resample, Coconut };
MMLTK_REFLECT_ENUM(ProfileMode)
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
 ProfileMode mode = ProfileMode::Loader;
 FixtureImageEncoding encoding = FixtureImageEncoding::Png;
 int source_width = 0, source_height = 0;
 bool perceptual = false, mixed_geometry = false, shuffle_geometry = false;
 std::string input_manifest, cache_dir, trace_file, cpu_affinity;
};
MMLTK_REFLECT_FIELDS(Options)
using ProfileOptions = mmltk::frameworks::reflection::CliScope<Options>;
inline constexpr std::array profile_options{
 MMLTK_CLI_OPTION(ProfileOptions, mode, "loader, compile (Directory STB), pixels (benchmark decode), resample (retained RGB), coconut (Stock + recovery)", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, encoding, "PNG or JPEG synthetic input", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, input_manifest, "Retained JPEG/PNG paths, one per line; pixels/resample only", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, cache_dir, "Retained production benchmark cache; required for coconut", "Profiling"),
 MMLTK_CLI_OPTION(ProfileOptions, trace_file, "Opt-in causal JSONL; omit for throughput", "Profiling"),
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
 if (!opts.input_manifest.empty() && opts.mode != ProfileMode::Pixels && opts.mode != ProfileMode::Resample) throw std::invalid_argument("retained input manifests require pixels or resample mode");
 return opts;
}
class ProfileTrace final {
 std::ofstream output_;
 std::mutex mutex_;
 std::uint64_t run_ = 0;

public:
 explicit ProfileTrace(const std::string& path) {
  if (path.empty()) return;
  if (const auto parent = fs::path(path).parent_path(); !parent.empty()) fs::create_directories(parent);
  output_.open(path, std::ios::app);
  run_ = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  if (!output_) throw std::runtime_error("cannot open profile trace");
 }
 [[nodiscard]] benchmark_internal::BenchmarkTraceSink sink() {
  if (!output_.is_open()) return {};
  return [this](std::string_view event, const nlohmann::json& fields) {
   const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
   const std::lock_guard lock(mutex_);
   output_ << nlohmann::json{{"event", event}, {"steady_ns", stamp}, {"run_id", run_}, {"fields", fields}}.dump() << '\n';
  };
 }
};
std::vector<ProfileImageFixture> profile_inputs(const Options& opts) {
 std::vector<ProfileImageFixture> inputs;
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
   inputs.push_back(profile_image_fixture(width, height, opts.encoding));
 } else
  inputs.push_back(profile_image_fixture(static_cast<unsigned>(opts.source_width), static_cast<unsigned>(opts.source_height), opts.encoding));
 if (inputs.empty()) throw std::invalid_argument("profile input manifest is empty");
 return inputs;
}
void run_pixel_profile(const Options& opts, const std::vector<ProfileImageFixture>& inputs, const benchmark_internal::BenchmarkTraceSink& trace) {
 using namespace detail;
 using namespace mmltk::backend::imaging::resample;
 const auto workers = opts.compile_workers < 0 ? std::max<std::size_t>(1, NumaTopology::Capture().permitted_cpus.size()) : static_cast<std::size_t>(opts.compile_workers);
 const auto setup_start = std::chrono::steady_clock::now();
 const CompilePixelPolicy policy{static_cast<unsigned>(opts.width), static_cast<unsigned>(opts.height), ImageResizeMode::Stretch, opts.perceptual};
 if (std::uint64_t(opts.width) * opts.height * 12 > (512ULL << 20) / workers) throw std::length_error("profile destinations exceed 512 MiB");
 RgbResizeWork layout_work;
 std::vector<CompilePixelWorkspacePlan> pixel_plans;
 std::vector<RgbResizeWorkspacePlan> resize_plans;
 std::vector<std::vector<std::uint8_t>> decoded;
 std::size_t maximum = 0;
 std::uint64_t retained_decoded = 0;
 CompileImageDecoder decoder;
 NumaMemory decode_memory(NumaTopology::Capture().permitted_nodes.front());
 for (const auto& input : inputs) {
  const auto header = decoder.read_header(input.encoded, CompileImagePolicy::Benchmark);
  pixel_plans.push_back(compile_pixel_workspace_plan(header, policy, &layout_work));
  resize_plans.push_back(pixel_plans.back().resize_plan());
  maximum = std::max(maximum, opts.mode == ProfileMode::Resample ? resize_plans.back().bytes() : pixel_plans.back().bytes());
  if (opts.mode == ProfileMode::Resample) {
   const auto plan = CompileImageDecoder::plan(header);
   retained_decoded += plan.output_bytes;
   if (retained_decoded > (512ULL << 20)) throw std::length_error("profile decoded inputs exceed 512 MiB");
   decode_memory.ensure_bytes(plan.bytes());
   const auto rgb = decoder.decode_rgb(input.encoded, header, {static_cast<std::byte*>(decode_memory.data()), decode_memory.capacity_bytes()});
   decoded.emplace_back(rgb.begin(), rgb.end());
  }
 }
 const auto backing = std::max<std::size_t>(host_page_size(), page_rounded_bytes(maximum));
 if (backing > CompileWorkspacePlan::ceiling / workers) throw std::length_error("profile workers exceed bounded workspace budget");
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(workers, {}, 0, 0, backing * workers));
 CompileExecution::Observer observer;
 if (trace) observer = [&](std::string_view event, const CompileExecutionSnapshot& value) { benchmark_internal::trace_compile_execution(trace, event, value); };
 CompileExecution execution(workers, {}, {.workspace_plan = plan}, {}, std::move(observer));
 std::vector<std::unique_ptr<CompilePixelWorkspace>> pixels;
 std::vector<std::unique_ptr<RgbImageResizer>> resizers;
 std::vector<std::vector<float>> output;
 std::vector<RgbResizeWork> resize_work(workers);
 for (std::size_t i = 0; i < workers; ++i) {
  pixels.push_back(std::make_unique<CompilePixelWorkspace>());
  resizers.push_back(std::make_unique<RgbImageResizer>(1, opts.perceptual));
  output.emplace_back(std::size_t(opts.width) * opts.height * 3);
 }
 std::vector<std::size_t> order(static_cast<std::size_t>(opts.num_images));
 for (std::size_t i = 0; i < order.size(); ++i) order[i] = i % inputs.size();
 if (opts.shuffle_geometry) {
  std::mt19937 random(1729);
  std::shuffle(order.begin(), order.end(), random);
 }
 std::uint64_t source_bytes = 0;
 for (const auto i : order) source_bytes += opts.mode == ProfileMode::Resample ? decoded[i].size() : inputs[i].encoded.size();
 const auto start = std::chrono::steady_clock::now();
 execution.for_each(CompileTaskKind::Pixels, order.size(), [&](std::size_t index) {
  if (opts.mode != ProfileMode::Resample) return pixel_plans[order[index]].resources();
  return CompileResources{.bytes = resize_plans[order[index]].bytes(), .supplied_workspace = true};
 }, [&](std::size_t index) {
  const auto i = order[index], lane = execution.current_lane();
  const auto memory = execution.current_allowance().workspace();
  RgbMutableImageView destination{output[lane].data(), pixel_plans[i].destination()};
  if (opts.mode == ProfileMode::Resample) {
   resizers[lane]->prepare(resize_plans[i], memory);
   resizers[lane]->resize_to_planar(
    {decoded[i].data(), {inputs[i].width, inputs[i].height, std::size_t(inputs[i].width) * 3, 0, decoded[i].size(), RgbPixelFormat::RGB8}}, destination, policy.mode, &resize_work[lane]);
  } else {
   const CompilePixelObserver observed = [&](const CompilePixelObservation& value) {
    resize_work[lane].preparations += value.resize_preparations;
    resize_work[lane].allocation_requests += value.resize_allocation_requests;
    resize_work[lane].allocation_bytes += value.resize_allocation_bytes;
    if (trace)
     benchmark_internal::trace_benchmark_event(trace, "compile.pixel", [&] {
      auto fields = benchmark_internal::benchmark_observation_fields(value);
      fields["image_id"] = index;
      fields["input_index"] = i;
      return fields;
     });
   };
   pixels[lane]->write(pixel_plans[i], inputs[i].encoded, pixel_plans[i].header(), memory, destination, +[] {}, observed);
  }
 });
 const auto end = std::chrono::steady_clock::now();
 record_duration_metric("pixels", "setup", setup_start, start);
 const auto seconds = seconds_since(start, end);
 std::printf("cpu_profile images=%zu workers=%zu sec=%.6f images_per_sec=%.3f source_bytes_per_sec=%.3f output_bytes_per_sec=%.3f retained_decoded_bytes=%llu plan_count=%zu setup_sec=%.6f\n",
  order.size(), workers, seconds, static_cast<double>(order.size()) / seconds, static_cast<double>(source_bytes) / seconds,
  static_cast<double>(order.size() * std::size_t(opts.width) * opts.height * 12) / seconds, static_cast<unsigned long long>(retained_decoded), inputs.size(), seconds_since(setup_start, start));
 std::printf("cpu_profile layout_preparations=%llu layout_allocation_requests=%llu layout_allocation_bytes=%llu\n", static_cast<unsigned long long>(layout_work.preparations),
  static_cast<unsigned long long>(layout_work.allocation_requests), static_cast<unsigned long long>(layout_work.allocation_bytes));
 RgbResizeWork work;
 for (const auto& lane : resize_work) {
  work.preparations += lane.preparations;
  work.allocation_requests += lane.allocation_requests;
  work.allocation_bytes += lane.allocation_bytes;
 }
 std::printf("cpu_profile resize_preparations=%llu resize_allocation_requests=%llu resize_allocation_bytes=%llu\n", static_cast<unsigned long long>(work.preparations),
  static_cast<unsigned long long>(work.allocation_requests), static_cast<unsigned long long>(work.allocation_bytes));
 record_duration_metric("pixels", "total", start, end);
}
void run_loader_case(const char* label, const DatasetLoader::Config& cfg, int num_epochs, bool overlap_consumer) {
 const auto init_start = std::chrono::steady_clock::now();
 DatasetLoader loader(cfg);
 const auto init_end = std::chrono::steady_clock::now();
 std::printf("%s init_sec=%.6f\n", label, seconds_since(init_start, init_end));
 record_duration_metric(label, "init", init_start, init_end);
 record_value_metric(label, "prefetch_factor", static_cast<std::uint64_t>(cfg.prefetch_factor));
 cudaStream_t consumer_stream = nullptr;
 if (overlap_consumer) {
  ensure_cuda_ok(cudaSetDevice(cfg.device_id), "cudaSetDevice");
  ensure_cuda_ok(cudaStreamCreateWithFlags(&consumer_stream, cudaStreamNonBlocking), "cudaStreamCreateWithFlags");
 }
 for (int epoch = 0; epoch < num_epochs; ++epoch) {
  const auto epoch_start = std::chrono::steady_clock::now();
  loader.begin_epoch();
  Batch batch{};
  size_t total = 0;
  while (loader.next_batch(batch)) {
   total += batch.num_images;
   if (overlap_consumer) {
    loader.handoff_batch(batch, consumer_stream);
    ensure_cuda_ok(cudaMemsetAsync(const_cast<float*>(batch.device_images), 0, batch.num_images * loader.image_stride(), consumer_stream), "cudaMemsetAsync");
    loader.release_batch(batch, consumer_stream);
   } else {
    loader.release_batch(batch);
   }
  }
  if (overlap_consumer) { ensure_cuda_ok(cudaStreamSynchronize(consumer_stream), "cudaStreamSynchronize"); }
  loader.synchronize();
  const auto epoch_end = std::chrono::steady_clock::now();
  const double secs = seconds_since(epoch_start, epoch_end);
  std::printf("%s epoch=%d images=%zu sec=%.6f img_per_sec=%.2f\n", label, epoch, total, secs, static_cast<double>(total) / secs);
  record_duration_metric(label, "epoch", epoch_start, epoch_end);
  record_value_metric(label, "epoch_images", total);
  record_value_metric(label, "epoch_img_per_sec_x100", static_cast<std::uint64_t>((static_cast<double>(total) / secs) * 100.0));
 }
 if (consumer_stream) { ensure_cuda_ok(cudaStreamDestroy(consumer_stream), "cudaStreamDestroy"); }
}
void run_profile_iteration(const FixtureSpec& fixture, const Options& opts, const benchmark_internal::BenchmarkTraceSink& trace) {
 auto ccfg = compiler_config(fixture);
 ccfg.num_workers = opts.compile_workers;
 ccfg.target_width = static_cast<unsigned>(opts.width);
 ccfg.target_height = static_cast<unsigned>(opts.height);
 ccfg.perceptual_downscale = opts.perceptual;
 DirectoryCompileObservation observation;
 if (trace) {
  observation.context = const_cast<benchmark_internal::BenchmarkTraceSink*>(&trace);
  observation.execution = +[](void* context, std::string_view event, const detail::CompileExecutionSnapshot& value) {
   benchmark_internal::trace_compile_execution(*static_cast<benchmark_internal::BenchmarkTraceSink*>(context), event, value);
  };
  observation.pixel = +[](void* context, std::uint32_t image, const detail::CompilePixelObservation& value) {
   const auto& sink = *static_cast<benchmark_internal::BenchmarkTraceSink*>(context);
   benchmark_internal::trace_benchmark_event(sink, "compile.pixel", [&] {
    auto fields = benchmark_internal::benchmark_observation_fields(value);
    fields["image_id"] = image;
    fields["source"] = "directory";
    return fields;
   });
  };
  ccfg.observation = &observation;
 }
 const auto compile_start = std::chrono::steady_clock::now();
 const DatasetCompilePlan compile_plan = DatasetCompiler::prepare(ccfg, {ccfg.split});
 DatasetCompiler::compile(compile_plan, 0U);
 const auto compile_end = std::chrono::steady_clock::now();
 const std::string bin_path = compiled_bin_path(fixture);
 std::printf("compile sec=%.6f file_bytes=%zu\n", seconds_since(compile_start, compile_end), static_cast<size_t>(fs::file_size(bin_path)));
 record_duration_metric("compile", "total", compile_start, compile_end);
 record_value_metric("compile", "file_bytes", static_cast<std::uint64_t>(fs::file_size(bin_path)));
 if (opts.mode == ProfileMode::Compile) return;
 DatasetLoader::Config sequential_cfg;
 sequential_cfg.compiled_path = bin_path;
 sequential_cfg.batch_size = static_cast<size_t>(opts.batch_size);
 sequential_cfg.shuffle = false;
 sequential_cfg.prefetch_factor = 6;
 run_loader_case("loader_seq", sequential_cfg, opts.num_epochs, false);
 DatasetLoader::Config shuffle_cfg = sequential_cfg;
 shuffle_cfg.shuffle = true;
 shuffle_cfg.seed = 7;
 shuffle_cfg.prefetch_factor = opts.shuffle_prefetch;
 run_loader_case("loader_shuffle", shuffle_cfg, opts.num_epochs, false);
 run_loader_case("loader_shuffle_overlap", shuffle_cfg, opts.num_epochs, true);
}
}  // namespace
int main(int argc, char** argv) {
 profile_enable();
 profile_set_process_label("profile.backend.data");
 profile_set_run_label("backend.data");
 try {
  const ExecutionPolicySnapshot execution_snapshot = apply_process_execution_policy();
  trace([&](auto& logger) {
   logger.trace(
    "event=profile.execution_policy executable=mmltk_backend_data_profile_runner online_cpu_count={} "
    "nice_value={} scheduler_policy={} scheduler_priority={} io_class={} io_priority_data={}",
    execution_snapshot.online_cpu_count, execution_snapshot.nice_value, execution_snapshot.scheduler_policy, execution_snapshot.scheduler_priority, execution_snapshot.io_class,
    execution_snapshot.io_priority_data);
  });
  const Options opts = parse_options(argc, argv);
  if (!opts.cpu_affinity.empty()) set_thread_affinity(resolve_cpu_affinity(opts.cpu_affinity));
  const FixtureSpec fixture{
   opts.test_dir,
   "train",
   opts.source_width,
   opts.source_height,
   opts.num_images,
  };
  auto inputs = opts.mode == ProfileMode::Coconut ? std::vector<ProfileImageFixture>{} : profile_inputs(opts);
  if (opts.shuffle_geometry && opts.mode != ProfileMode::Pixels && opts.mode != ProfileMode::Resample) {
   std::mt19937 random(1729);
   std::shuffle(inputs.begin(), inputs.end(), random);
  }
  if (opts.mode == ProfileMode::Compile || opts.mode == ProfileMode::Loader) {
   create_synthetic_dataset(fixture);
   for (int i = 1; i <= fixture.num_images; ++i) {
    const auto& input = inputs[static_cast<std::size_t>(i - 1) % inputs.size()];
    if (fixture.width != static_cast<int>(input.width) || fixture.height != static_cast<int>(input.height))
     replace_synthetic_image(fixture, i, static_cast<int>(input.width), static_cast<int>(input.height));
    std::array<char, 32> name{};
    std::snprintf(name.data(), name.size(), "%06d.png", i);
    // Directory's numbered input contract is extension-independent; preserve
    // its STB policy even for retained JPEG content.
    auto file = mmltk::common::io::FileHandle::create_output((fs::path(dataset_dir(fixture)) / fixture.split / name.data()).string(), input.encoded.size());
    file.pwrite_all(input.encoded.data(), input.encoded.size(), 0);
   }
  }
  ProfileTrace trace_file(opts.trace_file);
  const auto trace_sink = trace_file.sink();
  std::printf("profile mode=%s encoding=%s target=%dx%d perceptual=%d mixed_geometry=%d shuffled=%d retained_inputs=%zu trace=%d\n",
   std::string(mmltk::frameworks::reflection::enum_name(opts.mode)).c_str(), std::string(mmltk::frameworks::reflection::enum_name(opts.encoding)).c_str(), opts.width, opts.height, opts.perceptual,
   opts.mixed_geometry, opts.shuffle_geometry, inputs.size(), bool(trace_sink));
  std::printf("profile cpu_affinity=%s\n", format_cpu_list(allowed_cpu_set()).c_str());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
   const auto& input = inputs[i];
   std::printf("profile_input index=%zu width=%u height=%u encoded_bytes=%zu sha256=%s\n", i, input.width, input.height, input.encoded.size(),
    mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(input.encoded)).c_str());
  }
  const auto run = [&] {
   if (opts.mode == ProfileMode::Pixels || opts.mode == ProfileMode::Resample)
    run_pixel_profile(opts, inputs, trace_sink);
   else if (opts.mode == ProfileMode::Coconut) {
    BenchmarkCompilerConfig config;
    config.selection = {BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock, true};
    config.output_dir = compiled_dir(fixture);
    config.cache_dir = opts.cache_dir;
    config.resolution = static_cast<unsigned>(opts.width);
    config.num_workers = opts.compile_workers;
    config.perceptual_downscale = opts.perceptual;
    config.resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
    config.overwrite = true;
    if (trace_sink) config.trace = [&](std::string_view event, std::string_view fields) { trace_sink(event, nlohmann::json::parse(fields)); };
    const auto started = std::chrono::steady_clock::now();
    compile_benchmark_dataset(std::move(config));
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::ifstream manifest_file(fs::path(compiled_dir(fixture)) / "benchmark_manifest.json");
    const auto manifest = nlohmann::json::parse(manifest_file);
    const auto images = manifest.at("train").at("images").get<std::uint64_t>() + manifest.at("val").at("images").get<std::uint64_t>();
    const auto bytes = manifest.at("compiled_bytes").get<std::uint64_t>();
    std::printf("coconut_profile images=%llu output_bytes=%llu sec=%.6f images_per_sec=%.3f output_bytes_per_sec=%.3f\n", static_cast<unsigned long long>(images),
     static_cast<unsigned long long>(bytes), seconds, seconds > 0 ? static_cast<double>(images) / seconds : 0, seconds > 0 ? static_cast<double>(bytes) / seconds : 0);
   } else
    run_profile_iteration(fixture, opts, trace_sink);
  };
  for (int warmup = 0; warmup < opts.warmup_runs; ++warmup) {
   profile_reset_iteration();
   std::printf("warmup=%d/%d\n", warmup + 1, opts.warmup_runs);
   run();
  }
  for (int repetition = 0; repetition < opts.repetitions; ++repetition) {
   profile_reset_iteration();
   std::printf("repetition=%d/%d\n", repetition + 1, opts.repetitions);
   run();
   profile_capture_iteration(iteration_label(repetition + 1).c_str());
  }
  profile_flush();
  if (!opts.keep_artifacts && (opts.mode == ProfileMode::Loader || opts.mode == ProfileMode::Compile)) { fs::remove_all(fixture.root_dir); }
  return 0;
 } catch (const std::exception& error) {
  std::fprintf(stderr, "mmltk_backend_data_profile_runner error: %s\n", error.what());
  return 1;
 }
}
