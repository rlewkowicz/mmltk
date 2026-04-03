#include "rfdetr/cli.h"
#include "rfdetr/cli/workflow_cli_shared.h"

#include "cpu_affinity.h"
#include "execution_policy.h"
#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/model.h"
#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/train.h"
#include "mmltk/rfdetr/validate.h"
#include "mmltk/rfdetr/workflow_requests.h"
#include "rfdetr/checkpoint_internal.h"
#include "rfdetr/tool_launch_utils.h"
#include "rfdetr/backends.h"
#include "dataset_compiler.h"
#include "mmltk_logging.h"
#include "spdmon/spdmon.hpp"
#include "CLI11.hpp"

#include <cuda_runtime_api.h>

#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace mmltk::rfdetr {

namespace {

constexpr int kCliParseSuccess = static_cast<int>(CLI::ExitCodes::Success);

struct InfoOptions {
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
    int device_id = 0;
};

struct EvaluateCliOptions : EvaluateOptions {};

struct EvaluateCliState {
    EvaluateCliOptions options;
    std::string compile_mode = "selective";
};

struct NormalizeWeightsOptions {
    std::filesystem::path input_path;
    std::filesystem::path output_path;
};

struct CompileCliOptions {
    std::filesystem::path source_dir;
    std::filesystem::path output_dir;
    int resolution = 432;
    int num_workers = -1;
    int cuda_mask_batch_size = 0;
    int cuda_device_id = 0;
};

int handle_parse_error(CLI::App& app, const CLI::ParseError& error) {
    app.exit(error);
    return error.get_exit_code() == kCliParseSuccess ? 0 : 1;
}

[[noreturn]] void exec_onnx_info_tool(const std::filesystem::path& model_path) {
    const std::filesystem::path tool_path = resolve_sibling_tool_path("mmltk-rfdetr-onnx-info");
    const std::string tool = tool_path.string();
    const std::string model = model_path.string();
    std::vector<std::string> args{tool, model};
    std::vector<char*> raw_args = make_exec_argv(args);
    ::execv(raw_args.front(), raw_args.data());
    throw std::runtime_error("failed to exec ONNX info helper " + tool + ": " + std::strerror(errno));
}

std::filesystem::path distributed_store_path_for_parent() {
    return std::filesystem::temp_directory_path() /
           ("mmltk_rfdetr_train_" + std::to_string(static_cast<long long>(::getpid())) + ".store");
}

std::vector<int> shard_cpu_list(const std::vector<int>& cpus, int rank, int world_size) {
    if (cpus.empty()) {
        return {};
    }
    const size_t rank_index = static_cast<size_t>(std::max(0, rank));
    const size_t world = static_cast<size_t>(std::max(1, world_size));
    const size_t base = cpus.size() / world;
    const size_t remainder = cpus.size() % world;
    const size_t extra = rank_index < remainder ? 1 : 0;
    const size_t count = base + extra;
    const size_t begin = base * rank_index + std::min(rank_index, remainder);
    if (count == 0) {
        return {cpus[rank_index % cpus.size()]};
    }
    return {cpus.begin() + static_cast<std::ptrdiff_t>(begin),
            cpus.begin() + static_cast<std::ptrdiff_t>(begin + count)};
}

int shard_worker_budget(int requested_workers, int rank, int world_size) {
    if (requested_workers <= 0) {
        return 0;
    }
    const int base = requested_workers / std::max(1, world_size);
    const int remainder = requested_workers % std::max(1, world_size);
    return std::max(3, base + (rank < remainder ? 1 : 0));
}

std::vector<std::string> distributed_worker_base_args(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc) + 8);
    for (int index = 0; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }

    const std::map<std::string, bool> filtered = {
        {"--device-id", true},
        {"--device-ids", true},
        {"--workers", true},
        {"--cpu-affinity", true},
        {"--dist-rank", true},
        {"--dist-world-size", true},
        {"--dist-store-file", true},
        {"--dist-worker", false},
    };

    std::vector<std::string> normalized;
    normalized.reserve(args.size());
    for (size_t index = 0; index < args.size(); ++index) {
        const auto found = filtered.find(args[index]);
        if (found == filtered.end()) {
            normalized.push_back(args[index]);
            continue;
        }
        if (found->second) {
            ++index;
        }
    }
    return normalized;
}

int spawn_distributed_training_workers(const TrainOptions& options, int argc, char** argv) {
    const int world_size = static_cast<int>(options.device_ids.size());
    if (world_size < 2) {
        throw std::runtime_error("distributed RF-DETR train requires at least two device ids");
    }
#if !defined(USE_C10D_NCCL)
    (void)argc;
    (void)argv;
    throw std::runtime_error(
        "distributed RF-DETR train requires a LibTorch build with NCCL/c10d enabled");
#endif

    std::set<int> unique_ids(options.device_ids.begin(), options.device_ids.end());
    if (static_cast<int>(unique_ids.size()) != world_size) {
        throw std::runtime_error("rfdetr train --device-ids must not contain duplicates");
    }

    int visible_devices = 0;
    const cudaError_t cuda_status = ::cudaGetDeviceCount(&visible_devices);
    if (cuda_status != cudaSuccess) {
        throw std::runtime_error(std::string("failed to query CUDA devices: ") + ::cudaGetErrorString(cuda_status));
    }
    for (const int device_id : options.device_ids) {
        if (device_id < 0 || device_id >= visible_devices) {
            throw std::runtime_error(
                "rfdetr train device id is out of range for the visible CUDA device count");
        }
    }

    const auto store_path = distributed_store_path_for_parent();
    std::filesystem::remove(store_path);

    const std::vector<int> all_cpus = options.cpu_affinity.empty()
                                          ? mmltk::allowed_cpu_set()
                                          : mmltk::resolve_cpu_affinity(options.cpu_affinity);
    const auto base_args = distributed_worker_base_args(argc, argv);

    std::vector<pid_t> children;
    children.reserve(static_cast<size_t>(world_size));
    bool failed = false;
    int failed_status = 1;

    for (int rank = 0; rank < world_size; ++rank) {
        std::vector<std::string> worker_args = base_args;
        worker_args.emplace_back("--device-id");
        worker_args.push_back(std::to_string(options.device_ids[static_cast<size_t>(rank)]));
        const auto shard_cpus = shard_cpu_list(all_cpus, rank, world_size);
        int rank_workers = shard_worker_budget(options.workers, rank, world_size);
        if (!shard_cpus.empty() && static_cast<int>(shard_cpus.size()) >= 3) {
            const int clamped_workers =
                mmltk::clamp_worker_count_to_cpus(rank_workers, shard_cpus.size(), 0, 3);
            const std::string subsystem = "rfdetr.distributed.rank" + std::to_string(rank);
            mmltk::log_worker_budget_clamp(subsystem.c_str(),
                                                rank_workers,
                                                clamped_workers,
                                                shard_cpus,
                                                0,
                                                3);
            rank_workers = clamped_workers;
        }
        worker_args.emplace_back("--workers");
        worker_args.push_back(std::to_string(rank_workers));
        if (!shard_cpus.empty()) {
            worker_args.emplace_back("--cpu-affinity");
            worker_args.push_back(mmltk::format_cpu_list(shard_cpus));
        }
        worker_args.emplace_back("--dist-worker");
        worker_args.emplace_back("--dist-rank");
        worker_args.push_back(std::to_string(rank));
        worker_args.emplace_back("--dist-world-size");
        worker_args.push_back(std::to_string(world_size));
        worker_args.emplace_back("--dist-store-file");
        worker_args.push_back(store_path.string());

        pid_t pid = ::fork();
        if (pid < 0) {
            std::filesystem::remove(store_path);
            throw std::runtime_error(std::string("failed to fork RF-DETR distributed worker: ") + std::strerror(errno));
        }
        if (pid == 0) {
            std::vector<char*> raw_args;
            raw_args.reserve(worker_args.size() + 1);
            for (auto& value : worker_args) {
                raw_args.push_back(value.data());
            }
            raw_args.push_back(nullptr);
            ::execvp(raw_args.front(), raw_args.data());
            mmltk::logging::logger("rfdetr.cli")->error(
                "mmltk rfdetr error: failed to exec distributed worker: {}",
                std::strerror(errno));
            std::_Exit(127);
        }
        children.push_back(pid);
    }

    size_t completed = 0;
    while (completed < children.size()) {
        int status = 0;
        const pid_t pid = ::waitpid(-1, &status, 0);
        if (pid < 0) {
            failed = true;
            failed_status = 1;
            break;
        }
        ++completed;
        if ((WIFEXITED(status) && WEXITSTATUS(status) == 0) || (WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM)) {
            continue;
        }
        failed = true;
        failed_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        for (const pid_t child : children) {
            if (child != pid) {
                ::kill(child, SIGTERM);
            }
        }
    }

    while (::waitpid(-1, nullptr, 0) > 0) {
    }
    std::filesystem::remove(store_path);
    return failed ? failed_status : 0;
}

void finalize_compile_options(const CompileCliOptions& options) {
    if (options.source_dir.empty() || options.output_dir.empty()) {
        throw std::runtime_error("rfdetr compile requires --source-dir and --output-dir");
    }
    if (options.cuda_mask_batch_size < 0) {
        throw std::runtime_error("rfdetr compile --cuda-mask-batch-size must be non-negative");
    }
}

void finalize_info_options(const InfoOptions& options) {
    cli_shared::require_exactly_one_model_input(options.onnx_path, options.tensorrt_path, "info");
}

void finalize_evaluate_options(EvaluateCliState& state) {
    state.options.compilation_mode = cli_shared::parse_compilation_mode(state.compile_mode);
    if (state.options.compiled_path.empty()) {
        throw std::runtime_error("rfdetr evaluate requires --compiled");
    }
    cli_shared::require_exactly_one_model_input(state.options, "evaluate");
}

void finalize_normalize_options(NormalizeWeightsOptions& options) {
    if (options.input_path.empty() || options.output_path.empty()) {
        throw std::runtime_error("rfdetr normalize-weights requires --input and --output");
    }
    options.input_path = detail::canonical_checkpoint_path(options.input_path);
    options.output_path = detail::canonical_checkpoint_path(options.output_path);
}

void run_compile(const CompileCliOptions& options) {
    for (const std::string split : {"train", "val"}) {
        CompilerConfig config;
        config.source_dir = options.source_dir.string();
        config.output_dir = options.output_dir.string();
        config.split = split;
        config.target_width = static_cast<uint32_t>(options.resolution);
        config.target_height = static_cast<uint32_t>(options.resolution);
        config.num_workers = options.num_workers;
        config.cuda_mask_batch_size = options.cuda_mask_batch_size;
        config.cuda_device_id = options.cuda_device_id;

        size_t last_done = 0;
        size_t total = 0;
        size_t progress_pulse = 0;
        spdmon::ProgressBar bar("compile " + split, 0, "img");
        DatasetCompiler::compile(
            config,
            [&bar, &last_done, &total, &progress_pulse](const CompileProgress& progress) {
                if (progress.total != total) {
                    total = progress.total;
                    bar.set_total(total);
                }
                if (progress.done > last_done) {
                    bar.add(progress.done - last_done);
                    last_done = progress.done;
                }
                bar.set_postfix(spdmon::format_compile_postfix(progress, progress_pulse++));
            });
        bar.close();
    }
}

} // namespace

int handle_cli(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "rfdetr") {
        return -1;
    }
    try {
        CLI::App app{"RF-DETR model tooling for compilation, inference, evaluation, and training"};
        app.name("mmltk rfdetr");
        app.option_defaults()->always_capture_default();
        app.require_subcommand(1);
        std::string log_level_option;
        std::string log_file_option;
        std::string log_dir_option;
        app.add_option("--log-level", log_level_option,
                       "Logging level (trace, debug, info, warn, error, critical, off)");
        app.add_option("--log-file", log_file_option, "Explicit log file path")
            ->type_name("PATH");
        app.add_option("--log-dir", log_dir_option,
                       "Log directory for the default rotating file sink")
            ->type_name("PATH");
        app.footer(
            "notes:\n"
            "  weights overlap mode requires --batch-size 1 when predict/evaluate --weights uses --lanes > 1\n"
            "  native train --lanes accepts any non-negative value; with --lanes > 1, effective batch per rank is "
            "batch_size * lanes * grad_accum_steps");

        CompileCliOptions compile_options;
        auto* compile_cmd = app.add_subcommand(
            "compile",
            "Compile train and val splits for RF-DETR experiments");
        auto* compile_dataset = compile_cmd->add_option_group("Dataset");
        cli_shared::add_path_option(compile_dataset, "--source-dir", compile_options.source_dir,
                                    "Source dataset root");
        cli_shared::add_path_option(compile_dataset, "--output-dir", compile_options.output_dir,
                                    "Output directory for compiled train/val binaries");
        compile_dataset->add_option("--resolution", compile_options.resolution,
                                    "Square image resolution for both splits")
            ->type_name("INT");
        compile_dataset->add_option("--workers", compile_options.num_workers,
                                    "Total CPU worker budget for compile; 0 or negative selects all available CPUs")
            ->type_name("INT");
        compile_dataset->add_option("--cuda-mask-batch-size", compile_options.cuda_mask_batch_size,
                                    "Batched CUDA mask-resize task count; 0 disables GPU mask resizing")
            ->type_name("INT");
        compile_dataset->add_option("--cuda-device-id", compile_options.cuda_device_id,
                                    "CUDA device id used for batched mask resizing")
            ->type_name("INT");

        InfoOptions info_options;
        auto* info_cmd = app.add_subcommand(
            "info",
            "Inspect ONNX or TensorRT model metadata");
        auto* info_model = info_cmd->add_option_group("Model input");
        cli_shared::add_path_option(info_model, "--onnx", info_options.onnx_path, "ONNX model path");
        cli_shared::add_path_option(info_model, "--tensorrt", info_options.tensorrt_path,
                                    "TensorRT engine path or ONNX path to build from");
        info_cmd->add_option("--device-id", info_options.device_id, "CUDA device id")
            ->type_name("INT");

        BuildEngineRequest build_engine_request;
        auto* build_engine_cmd = app.add_subcommand(
            "build-engine",
            "Build a TensorRT engine from an ONNX model");
        auto* build_engine_io = build_engine_cmd->add_option_group("Input and output");
        auto* build_engine_exec = build_engine_cmd->add_option_group("Execution");
        cli_shared::add_build_engine_request_options(build_engine_io, build_engine_exec, build_engine_request);

        ExportOnnxRequest export_onnx_request;
        auto* export_onnx_cmd = app.add_subcommand(
            "export-onnx",
            "Export .pt weights to ONNX format");
        auto* export_onnx_io = export_onnx_cmd->add_option_group("Input and output");
        auto* export_onnx_exec = export_onnx_cmd->add_option_group("Execution");
        cli_shared::add_export_onnx_request_options(export_onnx_io, export_onnx_exec, export_onnx_request);

        auto* predict_cmd = app.add_subcommand(
            "predict",
            "Run RF-DETR inference and write predictions to JSON");
        auto predict_state = cli_shared::add_predict_command_options(predict_cmd);

        EvaluateCliState evaluate_state;
        auto* evaluate_cmd = app.add_subcommand(
            "evaluate",
            "Run RF-DETR evaluation on a compiled dataset split");
        evaluate_cmd->alias("eval");
        evaluate_cmd->alias("val");
        auto* evaluate_dataset = evaluate_cmd->add_option_group("Dataset");
        cli_shared::add_path_option(evaluate_dataset, "--compiled", evaluate_state.options.compiled_path,
                                    "Compiled dataset split (.bin)");
        auto* evaluate_model = evaluate_cmd->add_option_group("Model input");
        cli_shared::add_model_input_options(
            evaluate_model,
            evaluate_state.options,
            "TensorRT engine path or ONNX path to build from");
        auto* evaluate_execution = evaluate_cmd->add_option_group("Execution");
        evaluate_execution->add_option("--batch-size", evaluate_state.options.batch_size,
                                       "Batch size for evaluation")
            ->type_name("INT");
        evaluate_execution->add_option("--device-id", evaluate_state.options.device_id, "CUDA device id")
            ->type_name("INT");
        evaluate_execution->add_option("--limit-images", evaluate_state.options.limit_images,
                                       "Limit the number of evaluated images")
            ->type_name("INT");
        evaluate_execution->add_option("--eval-max-dets", evaluate_state.options.eval_max_dets,
                                       "Maximum detections per image during evaluation")
            ->type_name("INT");
        evaluate_execution->add_option("--workers", evaluate_state.options.workers,
                                       "Dataset worker count")
            ->type_name("INT");
        evaluate_execution->add_option("--lanes", evaluate_state.options.lanes,
                                       "Parallel backend lane count")
            ->type_name("INT");
        evaluate_execution->add_option("--cpu-affinity", evaluate_state.options.cpu_affinity,
                                       "Linux CPU list or range string for workers");
        evaluate_execution->add_option("--backend", evaluate_state.options.backend,
                                       "Backend preference for ONNX/TensorRT artifacts: auto, onnx, or tensorrt");
        evaluate_execution->add_flag("--fp16,!--no-fp16", evaluate_state.options.allow_fp16,
                                     "Enable FP16 backend execution when supported");
        evaluate_execution->add_flag("--progress,!--no-progress", evaluate_state.options.progress_bar,
                                     "Render interactive progress output");
        cli_shared::add_compile_mode_option(evaluate_execution, evaluate_state.compile_mode);

        auto* train_cmd = app.add_subcommand(
            "train",
            "Train RF-DETR natively against compiled datasets");
        auto train_state = cli_shared::add_train_command_options(train_cmd);

        NormalizeWeightsOptions normalize_options;
        auto* normalize_cmd = app.add_subcommand(
            "normalize-weights",
            "Convert an upstream RF-DETR checkpoint into mmltk's native format");
        auto* normalize_io = normalize_cmd->add_option_group("Input and output");
        cli_shared::add_path_option(normalize_io, "--input", normalize_options.input_path,
                                    "Input upstream checkpoint path");
        cli_shared::add_path_option(normalize_io, "--output", normalize_options.output_path,
                                    "Output native checkpoint path");

        auto* validate_cmd = app.add_subcommand(
            "validate",
            "Compare ONNX and TensorRT backends against a compiled dataset split");
        auto validate_state = cli_shared::add_validate_command_options(validate_cmd);

        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(argc - 1));
        args.emplace_back("mmltk rfdetr");
        for (int index = 2; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }
        std::vector<const char*> raw_args;
        raw_args.reserve(args.size());
        for (const auto& arg : args) {
            raw_args.push_back(arg.c_str());
        }

        try {
            app.parse(static_cast<int>(raw_args.size()), raw_args.data());
        } catch (const CLI::ParseError& error) {
            return handle_parse_error(app, error);
        }

        if (compile_cmd->parsed()) {
            finalize_compile_options(compile_options);
            run_compile(compile_options);
            return 0;
        }

        if (info_cmd->parsed()) {
            finalize_info_options(info_options);
            if (!info_options.onnx_path.empty()) {
                exec_onnx_info_tool(info_options.onnx_path);
            }
            std::unique_ptr<InferenceBackend> backend =
                make_tensorrt_backend(info_options.tensorrt_path, info_options.device_id, true);
            print_model_metadata(backend->info(), 0, 0, ValidationLogMode::Interactive);
            return 0;
        }

        if (build_engine_cmd->parsed()) {
            validate_build_engine_request(build_engine_request);
            make_tensorrt_backend(
                build_engine_request.onnx_path,
                build_engine_request.device_id,
                build_engine_request.allow_fp16,
                build_engine_request.output_path);
            mmltk::logging::logger("rfdetr.cli")->info("tensorrt: wrote engine to {}",
                                                      build_engine_request.output_path.string());
            return 0;
        }

        if (export_onnx_cmd->parsed()) {
            validate_export_onnx_request(export_onnx_request);
            export_weights_to_onnx(
                export_onnx_request.weights_path,
                export_onnx_request.output_path,
                export_onnx_request.device_id,
                export_onnx_request.opset_version,
                export_onnx_request.simplify);
            return 0;
        }

        if (predict_cmd->parsed()) {
            cli_shared::finalize_predict_command(predict_state);
            const PredictOptions options = to_predict_options(predict_state.request);
            const PredictionRunResult result = run_prediction(options);
            write_prediction_json(options, result);
            print_prediction_summary(options, result);
            return 0;
        }

        if (evaluate_cmd->parsed()) {
            finalize_evaluate_options(evaluate_state);
            const EvaluationRunResult result = run_evaluation(evaluate_state.options);
            print_evaluation_summary(evaluate_state.options, result);
            return 0;
        }

        if (train_cmd->parsed()) {
            cli_shared::finalize_train_command(train_state);
            const TrainOptions options = to_train_options(train_state.request);
            if (!options.distributed_worker && options.device_ids.size() > 1) {
                return spawn_distributed_training_workers(options, argc, argv);
            }
            const TrainRunResult result = run_training(options);
            print_training_summary(options, result);
            return 0;
        }

        if (normalize_cmd->parsed()) {
            finalize_normalize_options(normalize_options);
            const NativeCheckpoint checkpoint =
                normalize_checkpoint_to_native(normalize_options.input_path, normalize_options.output_path);
            mmltk::logging::logger("rfdetr.cli")->info(
                "rfdetr.normalize-weights: wrote {} tensors for preset={} to {}",
                checkpoint.state_dict.size(),
                checkpoint.metadata.preset_name,
                normalize_options.output_path.string());
            return 0;
        }

        if (validate_cmd->parsed()) {
            cli_shared::finalize_validate_command(validate_state);
            const ValidationOptions options = to_validate_options(validate_state.request);
            const ValidationRunResult result = run_validation(options);
            write_validation_report(options, result);
            print_validation_run_summary(options, result);
            return 0;
        }

        return 1;
    } catch (const std::exception& error) {
        mmltk::logging::logger("rfdetr.cli")->error("mmltk rfdetr error: {}", error.what());
        return 1;
    }
}

} // namespace mmltk::rfdetr
