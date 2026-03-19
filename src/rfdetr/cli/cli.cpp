#include "rfdetr/cli.h"

#include "cpu_affinity.h"
#include "rfdetr/backends.h"
#include "fastloader/rfdetr/checkpoint.h"
#include "rfdetr/predict.h"
#include "rfdetr/train.h"
#include "rfdetr/validate.h"
#include "dataset_compiler.h"
#include "rfdetr/progress_bar.h"
#include "CLI11.hpp"

#include <cuda_runtime_api.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
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

namespace fastloader::rfdetr {

namespace {

constexpr int kCliParseSuccess = static_cast<int>(CLI::ExitCodes::Success);

struct BuildEngineOptions {
    std::filesystem::path onnx_path;
    std::filesystem::path output_path;
    int device_id = 0;
    bool allow_fp16 = true;
};

struct InfoOptions {
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
    int device_id = 0;
};

struct PredictCliOptions : PredictOptions {};
struct EvaluateCliOptions : EvaluateOptions {};
struct TrainCliOptions : TrainOptions {};

struct PredictCliState {
    PredictCliOptions options;
    std::string compile_mode = "selective";
};

struct EvaluateCliState {
    EvaluateCliOptions options;
    std::string compile_mode = "selective";
};

struct TrainCliState {
    TrainCliOptions options;
    std::string device_ids;
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
};

int handle_parse_error(CLI::App& app, const CLI::ParseError& error) {
    app.exit(error);
    return error.get_exit_code() == kCliParseSuccess ? 0 : 1;
}

CompilationMode parse_compilation_mode(const std::string& value) {
    if (value == "none") return CompilationMode::kNone;
    if (value == "selective") return CompilationMode::kSelective;
    if (value == "full") return CompilationMode::kFullTrace;
    throw std::runtime_error("--compile-mode must be 'none', 'selective', or 'full', got: " + value);
}

template <typename Options>
void require_model_input(const Options& options, const char* command) {
    const size_t selected_input_count =
        static_cast<size_t>(!options.weights_path.empty()) +
        static_cast<size_t>(!options.onnx_path.empty()) +
        static_cast<size_t>(!options.tensorrt_path.empty());
    if (selected_input_count != 1) {
        throw std::runtime_error(
            std::string("rfdetr ") + command + " requires exactly one of --weights, --onnx, or --tensorrt");
    }
}

std::vector<int> parse_device_ids(const std::string& value) {
    std::vector<int> ids;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const std::string item = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (item.empty()) {
            throw std::runtime_error("rfdetr train --device-ids must not contain empty elements");
        }
        ids.push_back(std::stoi(item));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (ids.empty()) {
        throw std::runtime_error("rfdetr train --device-ids requires at least one CUDA device id");
    }
    return ids;
}

std::filesystem::path distributed_store_path_for_parent() {
    return std::filesystem::temp_directory_path() /
           ("fastloader_rfdetr_train_" + std::to_string(static_cast<long long>(::getpid())) + ".store");
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
    return std::vector<int>(cpus.begin() + static_cast<std::ptrdiff_t>(begin),
                            cpus.begin() + static_cast<std::ptrdiff_t>(begin + count));
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

int spawn_distributed_training_workers(const TrainCliOptions& options, int argc, char** argv) {
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
                                          ? fastloader::allowed_cpu_set()
                                          : fastloader::resolve_cpu_affinity(options.cpu_affinity);
    const auto base_args = distributed_worker_base_args(argc, argv);

    std::vector<pid_t> children;
    children.reserve(static_cast<size_t>(world_size));
    bool failed = false;
    int failed_status = 1;

    for (int rank = 0; rank < world_size; ++rank) {
        std::vector<std::string> worker_args = base_args;
        worker_args.push_back("--device-id");
        worker_args.push_back(std::to_string(options.device_ids[static_cast<size_t>(rank)]));
        worker_args.push_back("--workers");
        worker_args.push_back(std::to_string(shard_worker_budget(options.workers, rank, world_size)));
        const auto shard_cpus = shard_cpu_list(all_cpus, rank, world_size);
        if (!shard_cpus.empty()) {
            worker_args.push_back("--cpu-affinity");
            worker_args.push_back(fastloader::format_cpu_list(shard_cpus));
        }
        worker_args.push_back("--dist-worker");
        worker_args.push_back("--dist-rank");
        worker_args.push_back(std::to_string(rank));
        worker_args.push_back("--dist-world-size");
        worker_args.push_back(std::to_string(world_size));
        worker_args.push_back("--dist-store-file");
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
            std::fprintf(stderr, "fastloader rfdetr error: failed to exec distributed worker: %s\n", std::strerror(errno));
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

CLI::Option* add_path_option(CLI::App* command,
                             const std::string& name,
                             std::filesystem::path& value,
                             const char* description) {
    return command->add_option(name, value, description)->type_name("PATH");
}

void add_compile_mode_option(CLI::App* command, std::string& compile_mode) {
    command->add_option(
               "--compile-mode",
               compile_mode,
               "Native PyTorch compilation mode: none, selective, or full")
        ->type_name("MODE");
}

void add_model_input_options(CLI::App* command,
                             ModelArtifactRequest& request,
                             const char* tensorrt_description) {
    add_path_option(command, "--weights", request.weights_path, "RF-DETR checkpoint path");
    add_path_option(command, "--onnx", request.onnx_path, "ONNX model path");
    add_path_option(command, "--tensorrt", request.tensorrt_path, tensorrt_description);
}

void finalize_compile_options(const CompileCliOptions& options) {
    if (options.source_dir.empty() || options.output_dir.empty()) {
        throw std::runtime_error("rfdetr compile requires --source-dir and --output-dir");
    }
}

void finalize_info_options(const InfoOptions& options) {
    if (options.onnx_path.empty() == options.tensorrt_path.empty()) {
        throw std::runtime_error("rfdetr info requires exactly one of --onnx or --tensorrt");
    }
}

void finalize_build_engine_options(const BuildEngineOptions& options) {
    if (options.onnx_path.empty() || options.output_path.empty()) {
        throw std::runtime_error("rfdetr build-engine requires --onnx and --output");
    }
}

void finalize_predict_options(PredictCliState& state) {
    state.options.compilation_mode = parse_compilation_mode(state.compile_mode);
    if (state.options.compiled_path.empty() || state.options.output_path.empty()) {
        throw std::runtime_error("rfdetr predict requires --compiled and --output");
    }
    require_model_input(state.options, "predict");
}

void finalize_evaluate_options(EvaluateCliState& state) {
    state.options.compilation_mode = parse_compilation_mode(state.compile_mode);
    if (state.options.compiled_path.empty()) {
        throw std::runtime_error("rfdetr evaluate requires --compiled");
    }
    require_model_input(state.options, "evaluate");
}

void finalize_normalize_options(const NormalizeWeightsOptions& options) {
    if (options.input_path.empty() || options.output_path.empty()) {
        throw std::runtime_error("rfdetr normalize-weights requires --input and --output");
    }
}

void finalize_train_options(TrainCliState& state, bool saw_device_id, bool saw_device_ids) {
    state.options.compilation_mode = parse_compilation_mode(state.compile_mode);
    if (state.options.train_compiled_path.empty() ||
        state.options.val_compiled_path.empty() ||
        state.options.output_dir.empty()) {
        throw std::runtime_error("rfdetr train requires --train-compiled, --val-compiled, and --output-dir");
    }
    const size_t selected_input_count =
        static_cast<size_t>(!state.options.weights_path.empty()) +
        static_cast<size_t>(!state.options.resume_path.empty());
    if (selected_input_count != 1) {
        throw std::runtime_error("rfdetr train requires exactly one of --weights or --resume");
    }
    if (state.options.lr_scheduler != "step" && state.options.lr_scheduler != "cosine") {
        throw std::runtime_error("rfdetr train --lr-scheduler must be 'step' or 'cosine'");
    }
    if (saw_device_id && saw_device_ids) {
        throw std::runtime_error("rfdetr train accepts only one of --device-id or --device-ids");
    }
    if (saw_device_ids) {
        state.options.device_ids = parse_device_ids(state.device_ids);
        if (state.options.device_ids.size() == 1) {
            state.options.device_id = state.options.device_ids.front();
            state.options.device_ids.clear();
        }
    }
    if (state.options.distributed_worker) {
        if (state.options.distributed_rank < 0 ||
            state.options.distributed_world_size <= 1 ||
            state.options.distributed_store_path.empty()) {
            throw std::runtime_error("invalid internal RF-DETR distributed worker arguments");
        }
    } else if (state.options.distributed_world_size != 1 ||
               state.options.distributed_rank != 0 ||
               !state.options.distributed_store_path.empty()) {
        throw std::runtime_error("internal RF-DETR distributed worker options require --dist-worker");
    }
}

void finalize_validate_options(ValidationOptions& options) {
    if (options.compiled_path.empty()) {
        throw std::runtime_error("rfdetr validate requires --compiled");
    }
    if (options.onnx_path.empty() && options.tensorrt_path.empty()) {
        throw std::runtime_error("rfdetr validate requires at least one of --onnx or --tensorrt");
    }
    if (options.recompile && options.source_dir.empty()) {
        throw std::runtime_error("rfdetr validate --recompile requires --source");
    }
    if (options.split.empty()) {
        options.split = options.compiled_path.stem().string();
    }
    if (options.report_json_path.empty()) {
        options.report_json_path = options.compiled_path.parent_path() / "rfdetr-validation-report.json";
    }
    options.log_mode = ValidationLogMode::Interactive;
    options.write_report_json = true;
}

void run_compile(const CompileCliOptions& options) {
    for (const std::string split : {"train", "val"}) {
        CompilerConfig config;
        config.source_dir = options.source_dir.string();
        config.output_dir = options.output_dir.string();
        config.split = split;
        config.target_width = static_cast<uint32_t>(options.resolution);
        config.target_height = static_cast<uint32_t>(options.resolution);

        size_t last_done = 0;
        size_t total = 0;
        ProgressBar bar("compile " + split, 0, "img");
        DatasetCompiler::compile(
            config,
            [&bar, &last_done, &total](size_t done, size_t current_total) {
                if (current_total != total) {
                    total = current_total;
                    bar.set_total(total);
                }
                if (done > last_done) {
                    bar.add(done - last_done);
                    last_done = done;
                }

                const size_t num_images = (current_total - 2) / 2;
                if (done < num_images) {
                    bar.set_postfix("labels");
                } else if (done < 2 * num_images) {
                    bar.set_postfix("pixels");
                } else if (done == 2 * num_images + 1) {
                    bar.set_postfix("syncing");
                } else if (done == 2 * num_images + 2) {
                    bar.set_postfix("sidecar");
                } else {
                    bar.set_postfix("done");
                }
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
        app.name("fastloader rfdetr");
        app.option_defaults()->always_capture_default();
        app.require_subcommand(1);
        app.footer(
            "notes:\n"
            "  weights overlap mode requires --batch-size 1 when predict/evaluate --weights uses --lanes > 1\n"
            "  native train --lanes supports 0, 1, 2, or 3; with --lanes > 1, effective batch per rank is "
            "batch_size * lanes * grad_accum_steps");

        CompileCliOptions compile_options;
        auto* compile_cmd = app.add_subcommand(
            "compile",
            "Compile train and val splits for RF-DETR experiments");
        auto* compile_dataset = compile_cmd->add_option_group("Dataset");
        add_path_option(compile_dataset, "--source-dir", compile_options.source_dir,
                        "Source dataset root");
        add_path_option(compile_dataset, "--output-dir", compile_options.output_dir,
                        "Output directory for compiled train/val binaries");
        compile_dataset->add_option("--resolution", compile_options.resolution,
                                    "Square image resolution for both splits")
            ->type_name("INT");

        InfoOptions info_options;
        auto* info_cmd = app.add_subcommand(
            "info",
            "Inspect ONNX or TensorRT model metadata");
        auto* info_model = info_cmd->add_option_group("Model input");
        add_path_option(info_model, "--onnx", info_options.onnx_path, "ONNX model path");
        add_path_option(info_model, "--tensorrt", info_options.tensorrt_path,
                        "TensorRT engine path or ONNX path to build from");
        info_cmd->add_option("--device-id", info_options.device_id, "CUDA device id")
            ->type_name("INT");

        BuildEngineOptions build_engine_options;
        auto* build_engine_cmd = app.add_subcommand(
            "build-engine",
            "Build a TensorRT engine from an ONNX model");
        auto* build_engine_io = build_engine_cmd->add_option_group("Input and output");
        add_path_option(build_engine_io, "--onnx", build_engine_options.onnx_path, "ONNX model path");
        add_path_option(build_engine_io, "--output", build_engine_options.output_path,
                        "Output TensorRT engine path");
        auto* build_engine_exec = build_engine_cmd->add_option_group("Execution");
        build_engine_exec->add_option("--device-id", build_engine_options.device_id, "CUDA device id")
            ->type_name("INT");
        build_engine_exec->add_flag("--fp16,!--no-fp16", build_engine_options.allow_fp16,
                                    "Enable FP16 TensorRT kernels when supported");

        PredictCliState predict_state;
        auto* predict_cmd = app.add_subcommand(
            "predict",
            "Run RF-DETR inference and write predictions to JSON");
        auto* predict_dataset = predict_cmd->add_option_group("Dataset");
        add_path_option(predict_dataset, "--compiled", predict_state.options.compiled_path,
                        "Compiled dataset split (.bin)");
        add_path_option(predict_dataset, "--output", predict_state.options.output_path,
                        "Prediction JSON output path");
        auto* predict_model = predict_cmd->add_option_group("Model input");
        add_model_input_options(
            predict_model,
            predict_state.options,
            "TensorRT engine path or ONNX path to build from");
        auto* predict_execution = predict_cmd->add_option_group("Execution");
        predict_execution->add_option("--batch-size", predict_state.options.batch_size,
                                      "Batch size for inference")
            ->type_name("INT");
        predict_execution->add_option("--device-id", predict_state.options.device_id, "CUDA device id")
            ->type_name("INT");
        predict_execution->add_option("--threshold", predict_state.options.threshold,
                                      "Minimum score threshold for saved detections")
            ->type_name("FLOAT");
        predict_execution->add_option("--workers", predict_state.options.workers,
                                      "Dataset worker count")
            ->type_name("INT");
        predict_execution->add_option("--lanes", predict_state.options.lanes,
                                      "Parallel backend lane count")
            ->type_name("INT");
        predict_execution->add_option("--cpu-affinity", predict_state.options.cpu_affinity,
                                      "Linux CPU list or range string for workers");
        predict_execution->add_option("--backend", predict_state.options.backend,
                                      "Backend preference for ONNX/TensorRT artifacts: auto, onnx, or tensorrt");
        predict_execution->add_flag("--fp16,!--no-fp16", predict_state.options.allow_fp16,
                                    "Enable FP16 backend execution when supported");
        predict_execution->add_flag("--progress,!--no-progress", predict_state.options.progress_bar,
                                    "Render interactive progress output");
        add_compile_mode_option(predict_execution, predict_state.compile_mode);

        EvaluateCliState evaluate_state;
        auto* evaluate_cmd = app.add_subcommand(
            "evaluate",
            "Run RF-DETR evaluation on a compiled dataset split");
        evaluate_cmd->alias("eval");
        evaluate_cmd->alias("val");
        auto* evaluate_dataset = evaluate_cmd->add_option_group("Dataset");
        add_path_option(evaluate_dataset, "--compiled", evaluate_state.options.compiled_path,
                        "Compiled dataset split (.bin)");
        auto* evaluate_model = evaluate_cmd->add_option_group("Model input");
        add_model_input_options(
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
        add_compile_mode_option(evaluate_execution, evaluate_state.compile_mode);

        TrainCliState train_state;
        auto* train_cmd = app.add_subcommand(
            "train",
            "Train RF-DETR natively against compiled datasets");
        auto* train_dataset = train_cmd->add_option_group("Dataset");
        add_path_option(train_dataset, "--train-compiled", train_state.options.train_compiled_path,
                        "Compiled training split (.bin)");
        add_path_option(train_dataset, "--val-compiled", train_state.options.val_compiled_path,
                        "Compiled validation split (.bin)");
        add_path_option(train_dataset, "--test-compiled", train_state.options.test_compiled_path,
                        "Optional compiled test split (.bin)");
        auto* train_checkpoint = train_cmd->add_option_group("Checkpoint");
        add_path_option(train_checkpoint, "--output-dir", train_state.options.output_dir,
                        "Output directory for checkpoints and logs");
        add_path_option(train_checkpoint, "--weights", train_state.options.weights_path,
                        "Source checkpoint used for initialization");
        add_path_option(train_checkpoint, "--resume", train_state.options.resume_path,
                        "Existing native checkpoint to resume");
        auto* train_optimization = train_cmd->add_option_group("Optimization");
        train_optimization->add_option("--batch-size", train_state.options.batch_size,
                                       "Per-rank training batch size")
            ->type_name("INT");
        train_optimization->add_option("--val-batch-size", train_state.options.val_batch_size,
                                       "Validation batch size, 0 reuses --batch-size")
            ->type_name("INT");
        train_optimization->add_option("--epochs", train_state.options.epochs,
                                       "Number of training epochs")
            ->type_name("INT");
        train_optimization->add_option("--grad-accum-steps", train_state.options.grad_accum_steps,
                                       "Gradient accumulation steps")
            ->type_name("INT");
        train_optimization->add_option("--lr", train_state.options.lr, "Decoder/base learning rate")
            ->type_name("FLOAT");
        train_optimization->add_option("--lr-encoder", train_state.options.lr_encoder,
                                       "Encoder learning rate")
            ->type_name("FLOAT");
        train_optimization->add_option("--lr-component-decay", train_state.options.lr_component_decay,
                                       "Component-wise learning rate decay")
            ->type_name("FLOAT");
        train_optimization->add_option("--encoder-layer-decay", train_state.options.encoder_layer_decay,
                                       "Per-layer encoder learning rate decay")
            ->type_name("FLOAT");
        train_optimization->add_option("--weight-decay", train_state.options.weight_decay,
                                       "AdamW weight decay")
            ->type_name("FLOAT");
        train_optimization->add_option("--lr-drop", train_state.options.lr_drop,
                                       "Step scheduler drop epoch")
            ->type_name("INT");
        train_optimization->add_option("--lr-scheduler", train_state.options.lr_scheduler,
                                       "Learning rate scheduler: step or cosine");
        train_optimization->add_option("--lr-min-factor", train_state.options.lr_min_factor,
                                       "Minimum LR multiplier for cosine decay")
            ->type_name("FLOAT");
        train_optimization->add_option("--warmup-epochs", train_state.options.warmup_epochs,
                                       "Warmup duration in epochs")
            ->type_name("FLOAT");
        train_optimization->add_option("--clip-max-norm", train_state.options.clip_max_norm,
                                       "Gradient clipping max norm")
            ->type_name("FLOAT");
        train_optimization->add_flag("--fused-optimizer,!--no-fused-optimizer",
                                     train_state.options.fused_optimizer,
                                     "Use the native fused optimizer when available");
        train_optimization->add_flag("--use-ema,!--no-ema", train_state.options.use_ema,
                                     "Maintain an EMA shadow model");
        train_optimization->add_option("--ema-decay", train_state.options.ema_decay,
                                       "EMA decay factor")
            ->type_name("FLOAT");
        train_optimization->add_option("--ema-tau", train_state.options.ema_tau,
                                       "EMA warmup tau in steps")
            ->type_name("INT");
        train_optimization->add_option("--eval-max-dets", train_state.options.eval_max_dets,
                                       "Maximum detections per image during validation")
            ->type_name("INT");
        auto* train_execution = train_cmd->add_option_group("Execution");
        auto* train_device_id = train_execution->add_option("--device-id", train_state.options.device_id,
                                                            "Single CUDA device id")
                                    ->type_name("INT");
        auto* train_device_ids = train_execution->add_option(
            "--device-ids",
            train_state.device_ids,
            "Comma-separated CUDA device ids for distributed training");
        train_device_ids->type_name("LIST");
        train_execution->add_option("--workers", train_state.options.workers,
                                    "Dataset worker count")
            ->type_name("INT");
        train_execution->add_option("--lanes", train_state.options.lanes,
                                    "Parallel backend lane count")
            ->type_name("INT");
        train_execution->add_option("--cpu-affinity", train_state.options.cpu_affinity,
                                    "Linux CPU list or range string for workers");
        train_execution->add_option("--prefetch-factor", train_state.options.prefetch_factor,
                                    "Per-rank prefetch factor")
            ->type_name("INT");
        train_execution->add_option("--print-freq", train_state.options.print_freq,
                                    "Logging frequency in training steps")
            ->type_name("INT");
        train_execution->add_option("--seed", train_state.options.seed, "Random seed")
            ->type_name("INT");
        train_execution->add_flag("--amp,!--no-amp", train_state.options.amp,
                                  "Enable automatic mixed precision");
        train_execution->add_flag("--progress,!--no-progress", train_state.options.progress_bar,
                                  "Render interactive progress output");
        add_compile_mode_option(train_execution, train_state.compile_mode);
        auto* train_internal = train_cmd->add_option_group("Distributed (internal)");
        train_internal->add_flag("--dist-worker", train_state.options.distributed_worker,
                                 "Internal flag for forked distributed workers");
        train_internal->add_option("--dist-rank", train_state.options.distributed_rank,
                                   "Internal worker rank")
            ->type_name("INT");
        train_internal->add_option("--dist-world-size", train_state.options.distributed_world_size,
                                   "Internal worker world size")
            ->type_name("INT");
        add_path_option(train_internal, "--dist-store-file", train_state.options.distributed_store_path,
                        "Internal distributed rendezvous file");

        NormalizeWeightsOptions normalize_options;
        auto* normalize_cmd = app.add_subcommand(
            "normalize-weights",
            "Convert an upstream RF-DETR checkpoint into fastloader's native format");
        auto* normalize_io = normalize_cmd->add_option_group("Input and output");
        add_path_option(normalize_io, "--input", normalize_options.input_path,
                        "Input upstream checkpoint path");
        add_path_option(normalize_io, "--output", normalize_options.output_path,
                        "Output native checkpoint path");

        ValidationOptions validate_options;
        auto* validate_cmd = app.add_subcommand(
            "validate",
            "Compare ONNX and TensorRT backends against a compiled dataset split");
        auto* validate_dataset = validate_cmd->add_option_group("Dataset");
        add_path_option(validate_dataset, "--compiled", validate_options.compiled_path,
                        "Compiled dataset split (.bin)");
        add_path_option(validate_dataset, "--source", validate_options.source_dir,
                        "Source dataset root used for optional recompiles");
        validate_dataset->add_option("--split", validate_options.split,
                                     "Source dataset split name used when recompiling");
        validate_dataset->add_option("--resolution", validate_options.resolution,
                                     "Square resolution for recompiles")
            ->type_name("INT");
        validate_dataset->add_flag("--recompile", validate_options.recompile,
                                   "Recompile the source dataset before validation");
        auto* validate_model = validate_cmd->add_option_group("Model input");
        add_path_option(validate_model, "--onnx", validate_options.onnx_path, "ONNX model path");
        add_path_option(validate_model, "--tensorrt", validate_options.tensorrt_path,
                        "TensorRT engine path or ONNX path to build from");
        add_path_option(validate_model, "--save-engine", validate_options.save_engine_path,
                        "Optional path to save a built TensorRT engine");
        auto* validate_reports = validate_cmd->add_option_group("Reports");
        add_path_option(validate_reports, "--report-json", validate_options.report_json_path,
                        "Validation report JSON path");
        validate_reports->add_option("--eval-order", validate_options.eval_order,
                                     "Comma-separated backend evaluation order");
        auto* validate_execution = validate_cmd->add_option_group("Execution");
        validate_execution->add_option("--limit-images", validate_options.limit_images,
                                       "Limit the number of validated images")
            ->type_name("INT");
        validate_execution->add_option("--alignment-images", validate_options.alignment_images,
                                       "Number of images used for backend alignment probes")
            ->type_name("INT");
        validate_execution->add_option("--eval-max-dets", validate_options.eval_max_dets,
                                       "Maximum detections per image during evaluation")
            ->type_name("INT");
        validate_execution->add_option("--device-id", validate_options.device_id, "CUDA device id")
            ->type_name("INT");
        validate_execution->add_option("--workers", validate_options.workers,
                                       "Dataset worker count")
            ->type_name("INT");
        validate_execution->add_option("--batch-size", validate_options.batch_size,
                                       "Backend batch size")
            ->type_name("INT");
        validate_execution->add_option("--cpu-affinity", validate_options.cpu_affinity,
                                       "Linux CPU list or range string for workers");
        validate_execution->add_flag("--profile", validate_options.profile,
                                     "Collect detailed timing metrics during validation");
        validate_execution->add_flag("--fp16,!--no-fp16", validate_options.allow_fp16,
                                     "Enable FP16 TensorRT execution when supported");

        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(argc - 1));
        args.emplace_back("fastloader rfdetr");
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
            std::unique_ptr<InferenceBackend> backend =
                !info_options.onnx_path.empty()
                    ? make_onnx_backend(info_options.onnx_path, info_options.device_id)
                    : make_tensorrt_backend(info_options.tensorrt_path, info_options.device_id, true);
            print_model_metadata(backend->info(), 0, 0, ValidationLogMode::Interactive);
            return 0;
        }

        if (build_engine_cmd->parsed()) {
            finalize_build_engine_options(build_engine_options);
            make_tensorrt_backend(
                build_engine_options.onnx_path,
                build_engine_options.device_id,
                build_engine_options.allow_fp16,
                build_engine_options.output_path);
            std::fprintf(stderr, "tensorrt: wrote engine to %s\n", build_engine_options.output_path.c_str());
            return 0;
        }

        if (predict_cmd->parsed()) {
            finalize_predict_options(predict_state);
            const PredictionRunResult result = run_prediction(predict_state.options);
            write_prediction_json(predict_state.options, result);
            print_prediction_summary(predict_state.options, result);
            return 0;
        }

        if (evaluate_cmd->parsed()) {
            finalize_evaluate_options(evaluate_state);
            const EvaluationRunResult result = run_evaluation(evaluate_state.options);
            print_evaluation_summary(evaluate_state.options, result);
            return 0;
        }

        if (train_cmd->parsed()) {
            finalize_train_options(
                train_state,
                train_device_id->count() > 0,
                train_device_ids->count() > 0);
            if (!train_state.options.distributed_worker && train_state.options.device_ids.size() > 1) {
                return spawn_distributed_training_workers(train_state.options, argc, argv);
            }
            const TrainRunResult result = run_training(train_state.options);
            print_training_summary(train_state.options, result);
            return 0;
        }

        if (normalize_cmd->parsed()) {
            finalize_normalize_options(normalize_options);
            const NativeCheckpoint checkpoint =
                normalize_checkpoint_to_native(normalize_options.input_path, normalize_options.output_path);
            std::fprintf(stderr,
                         "rfdetr.normalize-weights: wrote %zu tensors for preset=%s to %s\n",
                         checkpoint.state_dict.size(),
                         checkpoint.metadata.preset_name.c_str(),
                         normalize_options.output_path.c_str());
            return 0;
        }

        if (validate_cmd->parsed()) {
            finalize_validate_options(validate_options);
            const ValidationRunResult result = run_validation(validate_options);
            write_validation_report(validate_options, result);
            print_validation_run_summary(validate_options, result);
            return 0;
        }

        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fastloader rfdetr error: %s\n", error.what());
        return 1;
    }
}

} // namespace fastloader::rfdetr
