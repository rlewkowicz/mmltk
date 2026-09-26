#include "tool_launch.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include <signal.h>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "cli_output.h"
#include "rfdetr_cli_options.h"
#include "spdmon/spdmon.hpp"
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/contract/cli.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/backend/models/rfdetr/training/training_partition.h"
#include "src/backend/models/rfdetr/training/train.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/common/system/runtime_paths.h"
#include "src/controller/services/train_command.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
import mmltk.common.logging.mmltk_logging;
namespace data = mmltk::backend::data;
namespace logging = mmltk::common::logging;
namespace reflection = mmltk::frameworks::reflection;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace services = mmltk::controller::services;
namespace mmltk::entrypoints::cli {
namespace {
using namespace rfdetr_options;
template <class Request, std::size_t Count>
void print_command_help(const rfdetr::RfdetrCommandDescriptor& command, const std::array<reflection::OptionDescriptor<Request>, Count>& options) {
 std::puts(reflection::help("mmltk rfdetr " + std::string(command.name) + " [options]", command.description, options).c_str());
}
template <class Request, class Options>
[[nodiscard]] Request parse_request(const std::span<const std::string_view> arguments, const Options& options) {
 auto parsed = reflection::parse<Request>(arguments, options);
 if (!parsed) throw parsed.error();
 return std::move(parsed->request);
}
void apply_train_presence(TrainCliRequest& state, const reflection::PresenceSet& presence);
[[nodiscard]] TrainCliRequest parse_train_request(const std::span<const std::string_view> arguments) {
 auto parsed = reflection::parse<TrainCliRequest>(arguments, kTrainOptions);
 if (!parsed) throw parsed.error();
 apply_train_presence(parsed->request, parsed->presence);
 return std::move(parsed->request);
}
void finalize_compile_request(CompileCliRequest& request) {
 if (request.benchmark_resolution != 0) {
  if (request.output_dir.empty()) request.output_dir = "./compiled";
  return;
 }
 if (request.source_dir.empty() || request.output_dir.empty()) { throw std::runtime_error("rfdetr compile requires --source-dir and --output-dir"); }
 if (!request.cache_dir.empty()) {
  throw std::runtime_error(
   "rfdetr compile --cache-dir requires "
   "--compile-benchmark-dataset");
 }
 if (request.overwrite) {
  throw std::runtime_error(
   "rfdetr compile --overwrite requires "
   "--compile-benchmark-dataset");
 }
}
std::atomic<bool> benchmark_cancel_requested{false};
void request_benchmark_cancel(int) noexcept { benchmark_cancel_requested.store(true, std::memory_order_relaxed); }
class BenchmarkSignalScope final {
public:
 BenchmarkSignalScope() {
  benchmark_cancel_requested.store(false, std::memory_order_relaxed);
  struct sigaction action{};
  action.sa_handler = &request_benchmark_cancel;
  ::sigemptyset(&action.sa_mask);
  if (::sigaction(SIGINT, &action, &previous_interrupt_) != 0) { throw std::system_error(errno, std::generic_category(), "cannot install benchmark signal handler"); }
  if (::sigaction(SIGTERM, &action, &previous_terminate_) != 0) {
   const int failure = errno;
   (void)::sigaction(SIGINT, &previous_interrupt_, nullptr);
   throw std::system_error(failure, std::generic_category(), "cannot install benchmark signal handler");
  }
 }
 ~BenchmarkSignalScope() {
  (void)::sigaction(SIGTERM, &previous_terminate_, nullptr);
  (void)::sigaction(SIGINT, &previous_interrupt_, nullptr);
 }
 BenchmarkSignalScope(const BenchmarkSignalScope&) = delete;
 BenchmarkSignalScope& operator=(const BenchmarkSignalScope&) = delete;

private:
 struct sigaction previous_interrupt_{};
 struct sigaction previous_terminate_{};
};
void run_compile(const CompileCliRequest& request) {
 if (request.benchmark_resolution != 0) {
  BenchmarkSignalScope signal_scope;
  data::BenchmarkCompilerConfig config;
  config.output_dir = request.output_dir;
  config.cache_dir = request.cache_dir;
  config.resolution = static_cast<std::uint32_t>(request.benchmark_resolution);
  config.num_workers = request.num_workers;
  config.overwrite = request.overwrite;
  config.perceptual_downscale = request.perceptual_downscale;
  config.resize_mode = request.resize_mode;
  config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(benchmark_cancel_requested);
  config.progress = [](const data::BenchmarkCompileProgress& progress) {
   spdmon::ProgressBar::log(data::format_dataset_compile_tracks(progress.tracks));
   if (!progress.activity.empty()) spdmon::ProgressBar::log(progress.activity);
   for (const auto& source : progress.sources)
    if (!source.activity.empty() || source.transfer || source.complete) {
     spdmon::ProgressBar::log(std::string(data::benchmark_source_label(source.source)) + " · " + data::format_benchmark_source_status(source, "Acquiring") + " · " +
                              std::to_string(source.completed_bytes) + "/" + (source.byte_total_known ? std::to_string(source.total_bytes) : "?") + " bytes · " +
                              std::to_string(source.completed_images) + "/" + std::to_string(source.total_images) + " images · " + std::to_string(source.invalidated_images) + " invalidated");
    }
  };
  if (logging::enabled(spdlog::level::trace)) {
   config.trace = [](const std::string_view event, const std::string_view fields) {
    if (fields.empty()) { return; }
    logging::log_if_enabled("rfdetr.benchmark", spdlog::level::trace, [&](auto& current) { current.trace("{{\"event\":\"{}\",\"fields\":{}}}", event, fields); });
   };
  }
  data::compile_benchmark_dataset(std::move(config));
  return;
 }
 data::CompilerConfig config;
 config.perceptual_downscale = request.perceptual_downscale;
 config.resize_mode = request.resize_mode;
 config.source_dir = request.source_dir.string();
 config.output_dir = request.output_dir.string();
 config.target_width = static_cast<std::uint32_t>(request.resolution);
 config.target_height = static_cast<std::uint32_t>(request.resolution);
 config.num_workers = request.num_workers;
 config.cuda_mask_batch_size = request.cuda_mask_batch_size;
 config.cuda_device_id = request.cuda_device_id;
 const auto plan = data::DatasetCompiler::prepare(config, {"train", "val"});
 for (std::size_t split_index = 0U; split_index < plan.splits.size(); ++split_index) {
  std::size_t completed = 0U;
  spdmon::ProgressBar bar("compile " + plan.splits[split_index].split, 0U, "img");
  struct ProgressState final {
   std::size_t* completed;
   spdmon::ProgressBar* bar;
  } state{&completed, &bar};
  data::CompileTelemetry telemetry{plan.splits[split_index].image_count, {.context = &state, .report = [](void* context, const data::CompileProgress& progress) noexcept {
                                                                           auto& progress_state = *static_cast<ProgressState*>(context);
                                                                           progress_state.bar->set_total(progress.total);
                                                                           progress_state.bar->set_postfix(data::format_dataset_compile_tracks(progress.tracks));
                                                                           if (progress.done > *progress_state.completed) {
                                                                            progress_state.bar->add(progress.done - *progress_state.completed);
                                                                            *progress_state.completed = progress.done;
                                                                           }
                                                                          }}};
  data::DatasetCompiler::compile(plan, split_index, &telemetry);
  bar.close();
 }
}
void append_logging_arguments(std::vector<std::string>& arguments, const logging::CliOverrides& logging_options) {
 if (logging_options.level) {
  const auto level_name = spdlog::level::to_string_view(*logging_options.level);
  arguments.push_back("--log-level=" + std::string(level_name.data(), level_name.size()));
 }
 if (logging_options.log_file) arguments.push_back("--log-file=" + logging_options.log_file->string());
 if (logging_options.log_dir) arguments.push_back("--log-dir=" + logging_options.log_dir->string());
}
[[noreturn]] void exec_onnx_info_tool(const std::filesystem::path& model_path, const logging::CliOverrides& logging_options) {
 const auto tool = mmltk::entrypoints::cli::resolve_sibling_tool_path("mmltk-rfdetr-onnx-info").string();
 std::vector<std::string> arguments{tool, model_path.string()};
 append_logging_arguments(arguments, logging_options);
 auto argv = mmltk::entrypoints::cli::make_exec_argv(arguments);
 ::execv(argv.front(), argv.data());
 throw std::system_error(errno, std::generic_category(), "failed to exec ONNX info helper");
}
void finalize_predict_request(PredictCliRequest& state) {
 if (state.request.output_path.empty()) throw std::invalid_argument("rfdetr predict requires --output");
 state.request.image_inputs.clear();
 const auto count = state.image_paths.size();
 const auto source_kind = count == 0U ? rfdetr::PredictSourceKind::CompiledDataset : rfdetr::PredictSourceKind::ImageFiles;
 state.request.image_inputs.reserve(count);
 for (std::size_t index = 0U; index < count; ++index) {
  auto& path = state.image_paths[index];
  auto source_name = path.filename().string();
  state.request.image_inputs.push_back({
   .image_path = std::move(path),
   .source_name = std::move(source_name),
   .image_id = static_cast<std::int64_t>(index),
  });
 }
 decltype(state.image_paths){}.swap(state.image_paths);
 state.request.source_kind = source_kind;
 state.request = rfdetr::finalize_predict_request(std::move(state.request));
}
void apply_train_presence(TrainCliRequest& state, const reflection::PresenceSet& presence) {
 constexpr auto json_index = reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, &TrainCliRequest::request_json>());
 if (presence.test(json_index)) {
  for (std::size_t index = 0; index < kTrainOptions.size(); ++index)
   if (index != json_index && presence.test(index)) throw std::invalid_argument("--request-json is mutually exclusive with scalar train options");
  state.request = rfdetr::decode_train_request_json(state.request_json);
  return;
 }
 using Relation = reflection::catalog_provider_relation<TrainRecipeCatalog>;
 constexpr auto selector = Recipe::path<&TrainRecipeSettings::optimizer>;
 Relation::VisitMembers([&]<class Entry>() {
  constexpr auto destination = reflection::rebase_member_path<TrainCliRequest, TrainRecipeSettings>(selector, Entry::destination);
  constexpr auto index = reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, destination>());
  if (presence.test(index)) Relation::template set_override<Entry::destination>(state.request.recipe.overrides);
 });
 rfdetr::resolve_train_recipe(state.request.recipe);
 constexpr auto device_id = Train::path<&TrainRequest::device_id>;
 constexpr auto device_ids = Train::path<&TrainRequest::device_ids>;
 constexpr auto device_id_index = reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, device_id>());
 constexpr auto device_ids_index = reflection::unique_descriptor_index(kTrainOptions, reflection::accessor_member_identity<TrainCliRequest, device_ids>());
 if (presence.test(device_id_index) && presence.test(device_ids_index)) { throw std::runtime_error("rfdetr train accepts only one of --device-id or --device-ids"); }
}
class DistributedTrainingProcess final {
public:
 static int Run(const TrainRequest& request, const logging::CliOverrides& logging_options) {
  const auto partitions = rfdetr::select_distributed_training_partitions(request);
  if (partitions.size() < 2U) throw std::logic_error("distributed training requires multiple partitions");
  const auto logging_config = logging::merge(logging::config_from_env("mmltk"), logging_options);
  const auto store = std::filesystem::temp_directory_path() / ("mmltk_rfdetr_train_" + std::to_string(static_cast<long long>(::getpid())) + ".store");
  std::filesystem::remove(store);
  std::vector<pid_t> children;
  children.reserve(partitions.size());
  try {
   for (const auto& partition : partitions) {
    auto worker = request;
    worker.distributed_worker = true;
    worker.distributed_rank = partition.rank;
    worker.distributed_world_size = partition.world_size;
    worker.distributed_store_path = store;
    rfdetr::apply_training_partition(worker, partition);
    auto arguments = services::build_train_command_arguments(worker);
    auto worker_logging = logging_options;
    if (logging_config.enabled()) {
     // Rotating sinks belong to one process; ranks must not rotate or replace
     // one another's file when they start or reach the rotation limit.
     const auto base = logging_config.log_file.value_or(logging_config.log_dir.value_or(request.output_dir) / "mmltk.log");
     worker_logging.log_file = base.parent_path() / (base.stem().string() + ".rank-" + std::to_string(partition.rank) + base.extension().string());
    }
    append_logging_arguments(arguments, worker_logging);
    arguments.insert(arguments.begin(), mmltk::common::system::runtime_paths::current_executable_path().string());
    const pid_t pid = ::fork();
    if (pid < 0) throw std::system_error(errno, std::generic_category(), "failed to fork RF-DETR worker");
    if (pid == 0) {
     std::vector<char*> argv;
     argv.reserve(arguments.size() + 1U);
     for (auto& argument : arguments) argv.push_back(argument.data());
     argv.push_back(nullptr);
     ::execv(argv.front(), argv.data());
     std::_Exit(127);
    }
    children.push_back(pid);
   }
  } catch (...) {
   for (const pid_t child : children) (void)::kill(child, SIGTERM);
   for (const pid_t child : children) (void)::waitpid(child, nullptr, 0);
   std::filesystem::remove(store);
   throw;
  }
  int result = 0;
  std::size_t remaining = children.size();
  while (remaining > 0U) {
   int status = 0;
   const pid_t completed = ::waitpid(-1, &status, 0);
   if (completed < 0) {
    if (errno == EINTR) continue;
    result = 1;
    for (const pid_t child : children) (void)::kill(child, SIGTERM);
    break;
   }
   --remaining;
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    for (const pid_t child : children) {
     if (child != completed) (void)::kill(child, SIGTERM);
    }
   }
  }
  while (::waitpid(-1, nullptr, 0) > 0) {}
  std::filesystem::remove(store);
  return result;
 }
};
int dispatch_command(const rfdetr::RfdetrCommandDescriptor& descriptor, const std::span<const std::string_view> arguments, const bool help_requested, const logging::CliOverrides& logging_options) {
 switch (descriptor.command) {
  case rfdetr::RfdetrCommand::Compile: {
   if (help_requested) {
    print_command_help(descriptor, kCompileOptions);
    return 0;
   }
   auto request = parse_request<CompileCliRequest>(arguments, kCompileOptions);
   finalize_compile_request(request);
   run_compile(request);
   return 0;
  }
  case rfdetr::RfdetrCommand::Info: {
   if (help_requested) {
    print_command_help(descriptor, kInfoOptions);
    return 0;
   }
   const auto request = parse_request<InfoCliRequest>(arguments, kInfoOptions);
   if (static_cast<unsigned>(!request.onnx_path.empty()) + static_cast<unsigned>(!request.tensorrt_path.empty()) != 1U) {
    throw std::runtime_error(
     "rfdetr info requires exactly one of --onnx or "
     "--tensorrt");
   }
   if (!request.onnx_path.empty()) exec_onnx_info_tool(request.onnx_path, logging_options);
   rfdetr::ModelArtifactRequest artifacts;
   artifacts.tensorrt_path = request.tensorrt_path;
   const rfdetr::ModelInfo info = rfdetr::inspect_tensorrt_model(artifacts, request.device_id);
   rfdetr::print_model_metadata(info, 0U, 0U, rfdetr::ValidationLogMode::Interactive);
   return 0;
  }
  case rfdetr::RfdetrCommand::BuildEngine: {
   if (help_requested) {
    print_command_help(descriptor, kBuildEngineOptions);
    return 0;
   }
   const auto request = parse_request<BuildEngineRequest>(arguments, kBuildEngineOptions);
   rfdetr::build_tensorrt_engine(request);
   return 0;
  }
  case rfdetr::RfdetrCommand::ExportOnnx: {
   if (help_requested) {
    print_command_help(descriptor, kExportOnnxOptions);
    return 0;
   }
   const auto request = parse_request<ExportOnnxRequest>(arguments, kExportOnnxOptions);
   rfdetr::export_onnx(request);
   return 0;
  }
  case rfdetr::RfdetrCommand::Predict: {
   if (help_requested) {
    print_command_help(descriptor, kPredictOptions);
    return 0;
   }
   auto state = parse_request<PredictCliRequest>(arguments, kPredictOptions);
   finalize_predict_request(state);
   const auto result = rfdetr::run_prediction(state.request);
   rfdetr::print_prediction_summary(state.request, result);
   return 0;
  }
  case rfdetr::RfdetrCommand::Evaluate: {
   if (help_requested) {
    print_command_help(descriptor, kEvaluateOptions);
    return 0;
   }
   auto request = parse_request<EvaluateRequest>(arguments, kEvaluateOptions);
   if (request.compiled_path.empty()) throw std::runtime_error("rfdetr evaluate requires --compiled");
   request = rfdetr::finalize_evaluate_request(std::move(request));
   const auto result = rfdetr::run_evaluation(request);
   rfdetr::print_evaluation_summary(request, result);
   return 0;
  }
  case rfdetr::RfdetrCommand::Validate: {
   if (help_requested) {
    print_command_help(descriptor, kValidateOptions);
    return 0;
   }
   auto request = parse_request<ValidateRequest>(arguments, kValidateOptions);
   if (request.compiled_path.empty()) throw std::runtime_error("rfdetr validate requires --compiled");
   request = rfdetr::finalize_validate_request(std::move(request));
   const auto result = rfdetr::run_validation(request);
   rfdetr::write_validation_report(request, result);
   rfdetr::print_validation_run_summary(request, result);
   return 0;
  }
  case rfdetr::RfdetrCommand::Train: {
   if (help_requested) {
    print_command_help(descriptor, kTrainOptions);
    return 0;
   }
   auto state = parse_train_request(arguments);
   auto request = rfdetr::finalize_train_request(std::move(state.request));
   if (!request.distributed_worker && request.device_ids.size() > 1U) { return DistributedTrainingProcess::Run(request, logging_options); }
   const auto result = rfdetr::run_training(request);
   rfdetr::print_training_summary(request, result);
   return 0;
  }
  case rfdetr::RfdetrCommand::NormalizeWeights: {
   if (help_requested) {
    print_command_help(descriptor, kNormalizeOptions);
    return 0;
   }
   auto request = parse_request<NormalizeWeightsRequest>(arguments, kNormalizeOptions);
   if (request.input_path.empty() || request.output_path.empty()) { throw std::runtime_error("rfdetr normalize-weights requires --input and --output"); }
   request.input_path = std::filesystem::absolute(request.input_path).lexically_normal();
   request.output_path = std::filesystem::absolute(request.output_path).lexically_normal();
   const auto checkpoint = rfdetr::normalize_checkpoint_to_native(request.input_path, request.output_path, request.class_layout_path);
   if (logging::enabled(spdlog::level::info)) {
    logging::info("rfdetr.cli",
     [&](auto& current) { current.info("rfdetr.normalize-weights: wrote {} tensors for preset={} to {}", checkpoint.tensor_count(), checkpoint.metadata.preset_name, request.output_path.string()); });
   } else {
    std::printf("rfdetr.normalize-weights: wrote %zu tensors for preset=%s to %s\n", checkpoint.tensor_count(), checkpoint.metadata.preset_name.c_str(), request.output_path.c_str());
   }
   return 0;
  }
 }
 throw std::logic_error("RF-DETR command vocabulary is incomplete");
}
void print_rfdetr_help() {
 std::fputs(
  "RF-DETR model tooling for compilation, inference, evaluation, and "
  "training\nUsage: mmltk rfdetr <command>\nCommands:",
  stdout);
 for (const auto& command : rfdetr::kRfdetrCommands) mmltk::entrypoints::cli::print_command_help_line(command.name, command.description);
 std::fputc('\n', stdout);
}
}  // namespace
int handle_rfdetr_cli(const std::span<const std::string_view> arguments, const logging::CliOverrides& logging_options) {
 if (arguments.empty() || arguments.front() == "--help" || arguments.front() == "-h") {
  print_rfdetr_help();
  return 0;
 }
 const auto command = rfdetr::parse_rfdetr_command(arguments.front());
 if (!command) {
  logging::report_fatal("mmltk rfdetr unknown command", arguments.front());
  return 1;
 }
 const auto* descriptor = rfdetr::rfdetr_command_descriptor(*command);
 if (descriptor == nullptr) throw std::logic_error("RF-DETR command descriptor is missing");
 const auto command_arguments = arguments.subspan(1U);
 const bool help_requested =
  std::ranges::find(command_arguments, std::string_view{"--help"}) != command_arguments.end() || std::ranges::find(command_arguments, std::string_view{"-h"}) != command_arguments.end();
 try {
  return dispatch_command(*descriptor, command_arguments, help_requested, logging_options);
 } catch (const rfdetr::TrainingPeerCancelled&) {
  // The session's rank-zero terminal owns the already-claimed first cause.
  return 1;
 } catch (const std::exception& error) { logging::report_fatal("mmltk rfdetr error", error.what(), std::nullopt, "rfdetr.cli"); } catch (...) {
  logging::report_fatal("mmltk rfdetr error", "unknown exception", std::nullopt, "rfdetr.cli");
 }
 return 1;
}
}  // namespace mmltk::entrypoints::cli
