#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <meta>
#include <span>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "cli_output.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "spdmon/spdmon.hpp"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/dataset_loader.h"
import mmltk.common.logging.mmltk_logging;
#include "src/common/system/execution_policy.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace data = mmltk::backend::data;
namespace logging = mmltk::common::logging;
namespace reflection = mmltk::frameworks::reflection;
namespace common_system = mmltk::common::system;
#if MMLTK_BUILD_RFDETR_NATIVE
namespace mmltk::entrypoints::cli {
int handle_rfdetr_cli(std::span<const std::string_view> arguments, const logging::CliOverrides& logging_options);
}
#endif
namespace {
struct BenchCommandRequest final : data::DataLoadingOptions {
 [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::string compiled_path;
 [[= reflection::Minimum<std::size_t>{1U}]] std::size_t batch_size = 32U;
 [[= reflection::Minimum<int>{1}]] int epochs = 1;
};
struct InfoCommandRequest final {
 [[= reflection::MaxBytes{reflection::kMaximumPathBytes}]] std::string compiled_path;
};
MMLTK_REFLECT_FIELDS(BenchCommandRequest)
MMLTK_REFLECT_FIELDS(InfoCommandRequest)
inline constexpr std::array kCompileOptions{reflection::option<data::CompilerConfig, &data::CompilerConfig::resize_mode>("--resize-mode", "Image geometry: Stretch or Letterbox", "Dataset"),
 reflection::option<data::CompilerConfig, &data::CompilerConfig::perceptual_downscale>("--perceptual-downscale", "Perceptual shrinking", "Dataset"),
 reflection::option<data::CompilerConfig, &data::CompilerConfig::source_dir>("--source-dir", "Source dataset directory", "Dataset", "source_dir", {}, true),
 reflection::option<data::CompilerConfig, &data::CompilerConfig::output_dir>("--output-dir", "Compiled binary output directory", "Dataset", "output_dir", {}, true),
 reflection::option<data::CompilerConfig, &data::CompilerConfig::split>("--split", "Dataset split", "Dataset", "split", {}, true),
 reflection::option<data::CompilerConfig, &data::CompilerConfig::target_width>("--width", "Target image width", "Dataset"),
 reflection::option<data::CompilerConfig, &data::CompilerConfig::target_height>("--height", "Target image height", "Dataset"),
 reflection::option<data::CompilerConfig, &data::CompilerConfig::cuda_mask_batch_size>("--cuda-mask-batch-size", "CUDA mask batch size", "Execution"),
 reflection::option<data::CompilerConfig, &data::CompilerConfig::cuda_device_id>("--cuda-device-id", "CUDA device id", "Execution"),
 reflection::option<data::CompilerConfig, &data::CompilerConfig::num_workers>("--workers", "CPU worker budget", "Execution")};
inline constexpr std::array kBenchOptions{reflection::negative_flag<BenchCommandRequest, &BenchCommandRequest::h2d_dataloader>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 reflection::option<BenchCommandRequest, &BenchCommandRequest::numa_node>("--numa-node", "GPU-local NUMA node (-1 automatic)", "Execution"),
 reflection::option<BenchCommandRequest, &BenchCommandRequest::compiled_path>("--compiled", "Compiled dataset binary", "Dataset", "compiled", {}, true),
 reflection::option<BenchCommandRequest, &BenchCommandRequest::batch_size>("--batch-size", "Streaming batch size", "Execution", "batch_size"),
 reflection::option<BenchCommandRequest, &BenchCommandRequest::epochs>("--epochs", "Number of epochs", "Execution", "num_epochs")};
inline constexpr std::array kInfoOptions{reflection::option<InfoCommandRequest, &InfoCommandRequest::compiled_path>("--compiled", "Compiled dataset binary", "Dataset", "compiled", {}, true)};
inline constexpr std::array kCompileUnexposed{
 reflection::unexposed<data::CompilerConfig, &data::CompilerConfig::worker_cpus>("execution resolves the concrete CPU set from the container "
                                                                                 "allocation")};
static_assert((reflection::audit_descriptors(kCompileOptions, kCompileUnexposed), true));
static_assert((reflection::audit_descriptors(kBenchOptions), true));
static_assert((reflection::audit_descriptors(kInfoOptions), true));
[[nodiscard]] reflection::ParsedCommand<data::CompilerConfig> parse_compile_request(const std::span<const std::string_view> arguments) {
 auto parsed = reflection::parse<data::CompilerConfig>(arguments, kCompileOptions);
 if (!parsed) throw parsed.error();
 return std::move(*parsed);
}
[[nodiscard]] BenchCommandRequest parse_bench_request(const std::span<const std::string_view> arguments) {
 auto parsed = reflection::parse<BenchCommandRequest>(arguments, kBenchOptions);
 if (!parsed) throw parsed.error();
 return std::move(parsed->request);
}
[[nodiscard]] InfoCommandRequest parse_info_request(const std::span<const std::string_view> arguments) {
 auto parsed = reflection::parse<InfoCommandRequest>(arguments, kInfoOptions);
 if (!parsed) throw parsed.error();
 return std::move(parsed->request);
}
[[nodiscard]] std::vector<std::string_view> non_logging_arguments(const int argc, char** argv, const int start) {
 std::vector<std::string_view> result;
 result.reserve(argc > start ? static_cast<std::size_t>(argc - start) : 0U);
 for (int index = start; index < argc; ++index) {
  const std::string_view argument(argv[index]);
  if (argument == "--log-level" || argument == "--log-file" || argument == "--log-dir") {
   ++index;
   continue;
  }
  if (argument.starts_with("--log-level=") || argument.starts_with("--log-file=") || argument.starts_with("--log-dir=")) { continue; }
  result.push_back(argument);
 }
 return result;
}
void run_compile(const data::CompilerConfig& config) {
 const auto started = std::chrono::steady_clock::now();
 std::size_t last_done = 0U;
 std::size_t total = 0U;
 spdmon::ProgressBar bar("compile", 0U, "img");
 bar.set_postfix("scanning");
 const data::DatasetCompilePlan plan = data::DatasetCompiler::prepare(config, {config.split});
 struct ProgressState final {
  std::size_t* last_done;
  std::size_t* total;
  spdmon::ProgressBar* bar;
 } state{&last_done, &total, &bar};
 data::CompileTelemetry telemetry{plan.splits[0].image_count, {.context = &state, .report = [](void* context, const data::CompileProgress& progress) noexcept {
                                                                auto& progress_state = *static_cast<ProgressState*>(context);
                                                                if (progress.total != *progress_state.total) {
                                                                 *progress_state.total = progress.total;
                                                                 progress_state.bar->set_total(*progress_state.total);
                                                                }
                                                                if (progress.done > *progress_state.last_done) {
                                                                 progress_state.bar->add(progress.done - *progress_state.last_done);
                                                                 *progress_state.last_done = progress.done;
                                                                }
                                                                progress_state.bar->set_postfix("processed " + std::to_string(progress.done) + "/" + std::to_string(progress.total));
                                                               }}};
 data::DatasetCompiler::compile(plan, 0U, &telemetry);
 bar.close();
 const auto finished = std::chrono::steady_clock::now();
 const double seconds = std::chrono::duration<double>(finished - started).count();
 if (logging::enabled(spdlog::level::info)) {
  logging::info([&](auto& current) { current.info("compile: done in {:.1f} seconds", seconds); });
 } else {
  std::printf("compile: done in %.1f seconds\n", seconds);
 }
}
void run_info(const std::string& compiled_path) {
 const data::FileHeader header = data::read_compiled_header(compiled_path);
 std::printf("File: %s\n", compiled_path.c_str());
 std::printf("Images: %u\n", header.num_images);
 std::printf("Size: %ux%u\n", header.image_width, header.image_height);
 std::printf("Stride: %zu bytes/image (%.2f KB)\n", static_cast<std::size_t>(header.image_stride), static_cast<double>(header.image_stride) / 1024.0);
 std::printf("Classes: %u\n", header.num_classes);
 std::printf("Maximum instances per image: %u\n", header.max_instances_per_image);
 for (std::uint32_t index = 0U; index < header.num_classes; ++index) std::printf("  [%u] %s\n", index, header.class_names[index].data());
 std::printf("Batches (bs=1): %u\n", header.num_images);
}
void run_bench(const BenchCommandRequest& request) {
 data::DatasetLoader::Config config;
 config.loading = request;
 config.compiled_path = request.compiled_path;
 config.batch_size = request.batch_size;
 config.shuffle = true;
 data::DatasetLoader loader(config);
 std::printf("Benchmarking: %zu images, batch=%zu, stride=%zu, epochs=%d\n", loader.num_images(), config.batch_size, loader.image_stride(), request.epochs);
 for (int epoch = 0; epoch < request.epochs; ++epoch) {
  const auto started = std::chrono::steady_clock::now();
  loader.begin_epoch();
  data::Batch batch{};
  std::size_t total = 0U;
  while (loader.next_batch(batch)) {
   total += batch.num_images;
   loader.release_batch(batch);
  }
  loader.synchronize();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("Epoch %d: %zu images in %.2f sec (%.0f img/sec)\n", epoch, total, seconds, seconds > 0.0 ? static_cast<double>(total) / seconds : 0.0);
 }
}
using RootCommandHandler = int (*)(std::span<const std::string_view>, const logging::CliOverrides&, bool);
struct RootCommand final {
 std::string_view name;
 std::string_view description;
 RootCommandHandler handler;
};
int handle_compile(const std::span<const std::string_view> arguments, const logging::CliOverrides&, const bool help_requested) {
 if (help_requested) {
  std::puts(reflection::help("mmltk compile [options]", "Compile a raw dataset split", kCompileOptions).c_str());
  return 0;
 }
 auto parsed = parse_compile_request(arguments);
 if (parsed.presence.test(3U) && !parsed.presence.test(4U)) parsed.request.target_height = parsed.request.target_width;
 run_compile(parsed.request);
 return 0;
}
int handle_bench(const std::span<const std::string_view> arguments, const logging::CliOverrides&, const bool help_requested) {
 if (help_requested) {
  std::puts(reflection::help("mmltk bench [options]", "Benchmark streaming throughput", kBenchOptions).c_str());
  return 0;
 }
 run_bench(parse_bench_request(arguments));
 return 0;
}
int handle_info(const std::span<const std::string_view> arguments, const logging::CliOverrides&, const bool help_requested) {
 if (help_requested) {
  std::puts(reflection::help("mmltk info [options]", "Inspect compiled metadata", kInfoOptions).c_str());
  return 0;
 }
 run_info(parse_info_request(arguments).compiled_path);
 return 0;
}
#if MMLTK_BUILD_RFDETR_NATIVE
int handle_rfdetr(const std::span<const std::string_view> arguments, const logging::CliOverrides& logging_options, bool) {
 return mmltk::entrypoints::cli::handle_rfdetr_cli(arguments, logging_options);
}
#endif
inline constexpr std::array kRootCommands{
 RootCommand{"compile", "Compile a raw dataset split", &handle_compile},
 RootCommand{"bench", "Benchmark a compiled dataset", &handle_bench},
 RootCommand{"info", "Inspect compiled metadata", &handle_info},
#if MMLTK_BUILD_RFDETR_NATIVE
 RootCommand{"rfdetr", "RF-DETR model tooling, inference, evaluation, and training", &handle_rfdetr},
#endif
};
consteval bool root_commands_are_valid() {
 for (std::size_t left = 0U; left < kRootCommands.size(); ++left) {
  if (kRootCommands[left].name.empty() || kRootCommands[left].handler == nullptr) { return false; }
  for (std::size_t right = left + 1U; right < kRootCommands.size(); ++right) {
   if (kRootCommands[left].name == kRootCommands[right].name) return false;
  }
 }
 return true;
}
static_assert(root_commands_are_valid());
[[nodiscard]] const RootCommand* find_root_command(const std::string_view name) noexcept {
 const auto found = std::ranges::find(kRootCommands, name, &RootCommand::name);
 return found == kRootCommands.end() ? nullptr : &*found;
}
void print_root_help() {
 std::fputs(
  "High-throughput dataset compilation, inspection, and streaming "
  "benchmarks\nUsage: mmltk <command>\nCommands:",
  stdout);
 for (const auto& command : kRootCommands) mmltk::entrypoints::cli::print_command_help_line(command.name, command.description);
 std::fputc('\n', stdout);
}
}  // namespace
int main(const int argc, char** argv) {
 try {
  logging::CliOverrides logging_options;
  logging::initialize(logging::merge(logging::config_from_env("mmltk"), logging_options = logging::scan_cli_overrides(argc, argv)));
  (void)common_system::apply_process_execution_policy();
  const auto filtered = non_logging_arguments(argc, argv, 1);
  if (filtered.empty() || filtered.front() == "--help" || filtered.front() == "-h") {
   print_root_help();
   return 0;
  }
  const RootCommand* command = find_root_command(filtered.front());
  if (command == nullptr) { throw std::runtime_error("unknown command: " + std::string(filtered.front())); }
  const std::span<const std::string_view> arguments{filtered.begin() + 1, filtered.end()};
  const bool help_requested = std::ranges::find(arguments, std::string_view{"--help"}) != arguments.end() || std::ranges::find(arguments, std::string_view{"-h"}) != arguments.end();
  return command->handler(arguments, logging_options, help_requested);
 } catch (const std::exception& error) { logging::report_fatal("mmltk error", error.what()); } catch (...) {
  logging::report_fatal("mmltk error", "unknown exception");
 }
 return 1;
}
