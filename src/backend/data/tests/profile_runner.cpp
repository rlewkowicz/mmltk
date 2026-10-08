#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/tests/profile_support.h"
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
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/frameworks/reflection/cli_declarations.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiler/dataset_compiler.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
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
using namespace mmltk::backend::data::testsupport::profile;
namespace fs = std::filesystem;
namespace {
void run_pixel_profile(const Options& opts, const std::vector<ProfileImageFixture>& inputs, const benchmark_internal::BenchmarkTraceSink& trace) {
 using namespace detail;
 using namespace mmltk::backend::imaging::resample;
 const auto workers = profile_workers(opts);
 const auto setup_start = std::chrono::steady_clock::now();
 const CompilePixelPolicy policy{static_cast<unsigned>(opts.width), static_cast<unsigned>(opts.height), ImageResizeMode::Stretch, opts.perceptual};
 if (std::uint64_t(opts.width) * opts.height * 12 > (512ULL << 20) / workers) throw std::length_error("profile destinations exceed 512 MiB");
 const bool observe_resize = trace.accepts("compile.pixel") || trace.accepts("profile.resize.completed");
 RgbResizeWork layout_work;
 std::vector<CompilePixelWorkspacePlan> pixel_plans;
 std::vector<RgbResizeWorkspacePlan> resize_plans;
 std::vector<std::vector<std::uint8_t>> decoded;
 std::size_t maximum = 0;
 std::uint64_t retained_decoded = 0;
 CompileImageDecoder decoder;
 for (const auto& input : inputs) {
  const auto header = decoder.read_header(input.encoded, CompileImagePolicy::Benchmark);
  pixel_plans.push_back(compile_pixel_workspace_plan(header, policy, observe_resize ? &layout_work : nullptr));
  resize_plans.push_back(pixel_plans.back().resize_plan());
  maximum = std::max(maximum, opts.mode == ProfileMode::Resample ? resize_plans.back().bytes() : pixel_plans.back().bytes());
 }
 const auto backing = std::max<std::size_t>(host_page_size(), page_rounded_bytes(maximum));
 if (backing > CompileWorkspacePlan::ceiling) throw std::length_error("profile image exceeds bounded workspace budget");
 const auto budget = backing > CompileWorkspacePlan::ceiling / workers ? CompileWorkspacePlan::ceiling : backing * workers;
 const std::array profiles{CompileWorkspaceProfile{backing, 0}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(workers, profiles, 0, 0, budget));
 if (opts.mode == ProfileMode::Resample) {
  decoded = retained_profile_rgb(inputs, 512U << 20, CompileWorkspacePlan::ceiling);
  for (const auto& rgb : decoded) retained_decoded += rgb.size();
 }
 auto observer = benchmark_internal::observe_compile_execution(trace, "compile.resource.wait");
 CompileExecution execution(workers, {}, {.workspace_plan = plan}, {}, std::move(observer));
 std::vector<std::unique_ptr<CompilePixelWorkspace>> pixels;
 std::vector<std::unique_ptr<RgbImageResizer>> resizers;
 std::vector<std::vector<float>> output;
 std::vector<RgbResizeWork> resize_work(observe_resize ? workers : 0);
 for (std::size_t i = 0; i < workers; ++i) {
  pixels.push_back(std::make_unique<CompilePixelWorkspace>());
  resizers.push_back(std::make_unique<RgbImageResizer>(1, opts.perceptual));
  output.emplace_back(std::size_t(opts.width) * opts.height * 3);
 }
 const auto order = profile_order(opts, inputs.size());
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
   resizers[lane]->resize_to_planar({decoded[i].data(), {inputs[i].width, inputs[i].height, std::size_t(inputs[i].width) * 3, 0, decoded[i].size(), RgbPixelFormat::RGB8}}, destination, policy.mode,
    observe_resize ? &resize_work[lane] : nullptr);
  } else {
   CompilePixelObserver observed;
   if (trace.accepts("compile.pixel"))
    observed = [&](const CompilePixelObservation& value) {
     benchmark_internal::trace_benchmark_event(trace, "compile.pixel", [&] {
      auto fields = benchmark_internal::benchmark_observation_fields(value);
      fields["image_id"] = index;
      fields["input_index"] = i;
      return fields;
     });
    };
   pixels[lane]->write(pixel_plans[i], inputs[i].encoded, pixel_plans[i].header(), memory, destination, +[] {}, observed, observe_resize ? &resize_work[lane] : nullptr);
  }
 });
 const auto end = std::chrono::steady_clock::now();
 record_duration_metric("pixels", "setup", setup_start, start);
 const auto seconds = seconds_since(start, end);
 std::printf("cpu_profile images=%zu workers=%zu sec=%.6f images_per_sec=%.3f source_bytes_per_sec=%.3f output_bytes_per_sec=%.3f retained_decoded_bytes=%llu plan_count=%zu setup_sec=%.6f\n",
  order.size(), workers, seconds, static_cast<double>(order.size()) / seconds, static_cast<double>(source_bytes) / seconds,
  static_cast<double>(order.size() * std::size_t(opts.width) * opts.height * 12) / seconds, static_cast<unsigned long long>(retained_decoded), inputs.size(), seconds_since(setup_start, start));
 if (observe_resize) {
  std::printf("cpu_profile layout_preparations=%llu layout_allocation_requests=%llu layout_allocation_bytes=%llu\n", static_cast<unsigned long long>(layout_work.preparations),
   static_cast<unsigned long long>(layout_work.allocation_requests), static_cast<unsigned long long>(layout_work.allocation_bytes));
  RgbResizeWork work;
  for (const auto& lane : resize_work)
   mmltk::frameworks::reflection::visit_materialized_members<RgbResizeWork>([&]<class Declaration>(const auto&) { work.*Declaration::pointer += lane.*Declaration::pointer; });
  benchmark_internal::trace_benchmark_event(trace, "profile.resize.completed",
   [&] { return nlohmann::json{{"layout", benchmark_internal::benchmark_observation_fields(layout_work)}, {"resize", benchmark_internal::benchmark_observation_fields(work)}}; });
  std::printf("cpu_profile resize_preparations=%llu resize_allocation_requests=%llu resize_allocation_bytes=%llu\n", static_cast<unsigned long long>(work.preparations),
   static_cast<unsigned long long>(work.allocation_requests), static_cast<unsigned long long>(work.allocation_bytes));
 }
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
  observation.execution_demand = benchmark_internal::observe_compile_execution(trace, "compile.resource.wait").demand();
  observation.execution = +[](void* context, std::string_view event, const detail::CompileExecutionSnapshot& value) {
   benchmark_internal::trace_compile_execution(*static_cast<benchmark_internal::BenchmarkTraceSink*>(context), event, value);
  };
  if (trace.accepts("compile.pixel"))
   observation.pixel = +[](void* context, std::uint32_t image, const detail::CompilePixelObservation& value) {
    const auto& sink = *static_cast<benchmark_internal::BenchmarkTraceSink*>(context);
    benchmark_internal::trace_benchmark_event(sink, "compile.pixel", [&] {
     auto fields = benchmark_internal::benchmark_observation_fields(value);
     fields["image_id"] = image;
     fields["source"] = "directory";
     return fields;
    });
   };
  if (trace.accepts("compile.compaction.summary"))
   observation.compaction = +[](void* context, const CompiledCompactionObservation& value) {
    const auto& sink = *static_cast<benchmark_internal::BenchmarkTraceSink*>(context);
    benchmark_internal::trace_benchmark_event(sink, "compile.compaction.summary", [&] { return benchmark_internal::benchmark_observation_fields(value); });
   };
  ccfg.observation = &observation;
 }
 const auto compile_start = std::chrono::steady_clock::now();
 // CLEANUP-IGNORE: Independent profilers invoke the public prepare/compile boundary before measuring different products; compilation is already shared.
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
  auto inputs = (opts.mode == ProfileMode::Coconut || opts.mode == ProfileMode::Compaction) ? std::vector<ProfileImageFixture>{} : profile_inputs(opts);
  if (opts.shuffle_geometry && opts.mode != ProfileMode::Pixels && opts.mode != ProfileMode::Resample && opts.mode != ProfileMode::Pipeline) {
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
  ProfileTrace trace_file(opts.trace_file, opts.trace_prefix);
  const auto trace_sink = trace_file.sink();
  std::printf("profile mode=%s encoding=%s target=%dx%d perceptual=%d mixed_geometry=%d shuffled=%d retained_inputs=%zu trace=%d\n",
   std::string(mmltk::frameworks::reflection::enum_name(opts.mode)).c_str(), std::string(mmltk::frameworks::reflection::enum_name(opts.encoding)).c_str(), opts.width, opts.height, opts.perceptual,
   opts.mixed_geometry, opts.shuffle_geometry, inputs.size(), bool(trace_sink));
  std::printf("profile cpu_affinity=%s\n", format_cpu_list(allowed_cpu_set()).c_str());
  for (const auto& prefix : opts.trace_prefix) std::printf("profile_trace_prefix=%s\n", prefix.c_str());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
   const auto& input = inputs[i];
   std::printf("profile_input index=%zu width=%u height=%u encoded_bytes=%zu sha256=%s\n", i, input.width, input.height, input.encoded.size(),
    mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(input.encoded)).c_str());
  }
  const auto run = [&] {
   if (opts.mode == ProfileMode::Pixels || opts.mode == ProfileMode::Resample)
    run_pixel_profile(opts, inputs, trace_sink);
   else if (opts.mode == ProfileMode::Pipeline)
    run_pipeline_profile(opts, inputs, trace_sink);
   else if (opts.mode == ProfileMode::Compaction)
    run_compaction_profile(opts, trace_sink);
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
    if (trace_sink)
     config.trace = {trace_sink.selection(), [&](std::string_view event, std::string_view fields) { trace_sink(event, fields.empty() ? nlohmann::json{} : nlohmann::json::parse(fields)); }};
    const auto started = std::chrono::steady_clock::now();
    config.progress = [started, next = started, phase = DatasetCompilePhase::Planning](const BenchmarkCompileProgress& value) mutable {
     const auto now = std::chrono::steady_clock::now();
     if (now < next && value.phase == phase) return;
     next = now + std::chrono::seconds(5);
     phase = value.phase;
     std::printf("coconut_progress elapsed_sec=%.3f phase=%s phase_completed=%llu phase_total=%llu %s activity=%s\n", std::chrono::duration<double>(now - started).count(),
      std::string(mmltk::frameworks::reflection::enum_name(phase)).c_str(), static_cast<unsigned long long>(value.completed), static_cast<unsigned long long>(value.total),
      format_dataset_compile_tracks(value.tracks).c_str(), nlohmann::json(value.activity).dump().c_str());
     std::fflush(stdout);
    };
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
