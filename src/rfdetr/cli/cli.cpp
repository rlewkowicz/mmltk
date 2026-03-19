#include "rfdetr/cli.h"

#include "cpu_affinity.h"
#include "rfdetr/backends.h"
#include "fastloader/rfdetr/checkpoint.h"
#include "rfdetr/predict.h"
#include "rfdetr/train.h"
#include "rfdetr/validate.h"
#include "dataset_compiler.h"
#include "rfdetr/progress_bar.h"

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

struct NormalizeWeightsOptions {
    std::filesystem::path input_path;
    std::filesystem::path output_path;
};

struct CompileCliOptions {
    std::filesystem::path source_dir;
    std::filesystem::path output_dir;
    int resolution = 432;
};

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

std::string require_value(int& index, int argc, char** argv, const char* option) {
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + option);
    }
    ++index;
    return argv[index];
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

std::optional<CompileCliOptions> parse_compile_args(int argc, char** argv) {
    if (argc < 3 || std::string(argv[2]) != "compile") {
        return std::nullopt;
    }
    CompileCliOptions options;
    for (int index = 3; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--source-dir") {
            options.source_dir = require_value(index, argc, argv, "--source-dir");
        } else if (arg == "--output-dir") {
            options.output_dir = require_value(index, argc, argv, "--output-dir");
        } else if (arg == "--resolution") {
            options.resolution = std::stoi(require_value(index, argc, argv, "--resolution"));
        } else {
            throw std::runtime_error("unknown rfdetr compile option: " + arg);
        }
    }
    if (options.source_dir.empty() || options.output_dir.empty()) {
        throw std::runtime_error("rfdetr compile requires --source-dir and --output-dir");
    }
    return options;
}

std::optional<InfoOptions> parse_info_args(int argc, char** argv) {
    if (argc < 3 || std::string(argv[2]) != "info") {
        return std::nullopt;
    }
    InfoOptions options;
    for (int index = 3; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--onnx") {
            options.onnx_path = require_value(index, argc, argv, "--onnx");
        } else if (arg == "--tensorrt") {
            options.tensorrt_path = require_value(index, argc, argv, "--tensorrt");
        } else if (arg == "--device-id") {
            options.device_id = std::stoi(require_value(index, argc, argv, "--device-id"));
        } else {
            throw std::runtime_error("unknown rfdetr info option: " + arg);
        }
    }
    if (options.onnx_path.empty() == options.tensorrt_path.empty()) {
        throw std::runtime_error("rfdetr info requires exactly one of --onnx or --tensorrt");
    }
    return options;
}

std::optional<BuildEngineOptions> parse_build_engine_args(int argc, char** argv) {
    if (argc < 3 || std::string(argv[2]) != "build-engine") {
        return std::nullopt;
    }
    BuildEngineOptions options;
    for (int index = 3; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--onnx") {
            options.onnx_path = require_value(index, argc, argv, "--onnx");
        } else if (arg == "--output") {
            options.output_path = require_value(index, argc, argv, "--output");
        } else if (arg == "--device-id") {
            options.device_id = std::stoi(require_value(index, argc, argv, "--device-id"));
        } else if (arg == "--fp16") {
            options.allow_fp16 = true;
        } else if (arg == "--no-fp16") {
            options.allow_fp16 = false;
        } else {
            throw std::runtime_error("unknown rfdetr build-engine option: " + arg);
        }
    }
    if (options.onnx_path.empty() || options.output_path.empty()) {
        throw std::runtime_error("rfdetr build-engine requires --onnx and --output");
    }
    return options;
}

std::optional<ValidationOptions> parse_validate_args(int argc, char** argv) {
    if (argc < 3 || std::string(argv[2]) != "validate") {
        return std::nullopt;
    }
    ValidationOptions options;
    for (int index = 3; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--compiled") {
            options.compiled_path = require_value(index, argc, argv, "--compiled");
        } else if (arg == "--source") {
            options.source_dir = require_value(index, argc, argv, "--source");
        } else if (arg == "--split") {
            options.split = require_value(index, argc, argv, "--split");
        } else if (arg == "--resolution") {
            options.resolution = static_cast<uint32_t>(std::stoul(require_value(index, argc, argv, "--resolution")));
        } else if (arg == "--onnx") {
            options.onnx_path = require_value(index, argc, argv, "--onnx");
        } else if (arg == "--tensorrt") {
            options.tensorrt_path = require_value(index, argc, argv, "--tensorrt");
        } else if (arg == "--save-engine") {
            options.save_engine_path = require_value(index, argc, argv, "--save-engine");
        } else if (arg == "--report-json") {
            options.report_json_path = require_value(index, argc, argv, "--report-json");
        } else if (arg == "--eval-order") {
            options.eval_order = require_value(index, argc, argv, "--eval-order");
        } else if (arg == "--limit-images") {
            options.limit_images = static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--limit-images")));
        } else if (arg == "--alignment-images") {
            options.alignment_images =
                static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--alignment-images")));
        } else if (arg == "--eval-max-dets") {
            options.eval_max_dets =
                static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--eval-max-dets")));
        } else if (arg == "--device-id") {
            options.device_id = std::stoi(require_value(index, argc, argv, "--device-id"));
        } else if (arg == "--workers") {
            options.workers = std::stoi(require_value(index, argc, argv, "--workers"));
        } else if (arg == "--batch-size") {
            options.batch_size = static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--batch-size")));
        } else if (arg == "--cpu-affinity") {
            options.cpu_affinity = require_value(index, argc, argv, "--cpu-affinity");
        } else if (arg == "--recompile") {
            options.recompile = true;
        } else if (arg == "--profile") {
            options.profile = true;
        } else if (arg == "--fp16") {
            options.allow_fp16 = true;
        } else if (arg == "--no-fp16") {
            options.allow_fp16 = false;
        } else {
            throw std::runtime_error("unknown rfdetr validate option: " + arg);
        }
    }

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
    return options;
}

std::optional<PredictCliOptions> parse_predict_args(int argc, char** argv) {
    if (argc < 3 || std::string(argv[2]) != "predict") {
        return std::nullopt;
    }
    PredictCliOptions options;
    for (int index = 3; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--compiled") {
            options.compiled_path = require_value(index, argc, argv, "--compiled");
        } else if (arg == "--weights") {
            options.weights_path = require_value(index, argc, argv, "--weights");
        } else if (arg == "--onnx") {
            options.onnx_path = require_value(index, argc, argv, "--onnx");
        } else if (arg == "--tensorrt") {
            options.tensorrt_path = require_value(index, argc, argv, "--tensorrt");
        } else if (arg == "--output") {
            options.output_path = require_value(index, argc, argv, "--output");
        } else if (arg == "--batch-size") {
            options.batch_size = static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--batch-size")));
        } else if (arg == "--device-id") {
            options.device_id = std::stoi(require_value(index, argc, argv, "--device-id"));
        } else if (arg == "--threshold") {
            options.threshold = std::stof(require_value(index, argc, argv, "--threshold"));
        } else if (arg == "--workers") {
            options.workers = std::stoi(require_value(index, argc, argv, "--workers"));
        } else if (arg == "--lanes") {
            options.lanes = std::stoi(require_value(index, argc, argv, "--lanes"));
        } else if (arg == "--cpu-affinity") {
            options.cpu_affinity = require_value(index, argc, argv, "--cpu-affinity");
        } else if (arg == "--backend") {
            options.backend = require_value(index, argc, argv, "--backend");
        } else if (arg == "--fp16") {
            options.allow_fp16 = true;
        } else if (arg == "--no-fp16") {
            options.allow_fp16 = false;
        } else if (arg == "--progress") {
            options.progress_bar = true;
        } else if (arg == "--no-progress") {
            options.progress_bar = false;
        } else if (arg == "--compile-mode") {
            options.compilation_mode = parse_compilation_mode(require_value(index, argc, argv, "--compile-mode"));
        } else {
            throw std::runtime_error("unknown rfdetr predict option: " + arg);
        }
    }

    if (options.compiled_path.empty() || options.output_path.empty()) {
        throw std::runtime_error("rfdetr predict requires --compiled and --output");
    }
    require_model_input(options, "predict");
    return options;
}

std::optional<EvaluateCliOptions> parse_evaluate_args(int argc, char** argv) {
    if (argc < 3) {
        return std::nullopt;
    }
    const std::string command = argv[2];
    if (command != "evaluate" && command != "eval" && command != "val") {
        return std::nullopt;
    }
    EvaluateCliOptions options;
    for (int index = 3; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--compiled") {
            options.compiled_path = require_value(index, argc, argv, "--compiled");
        } else if (arg == "--weights") {
            options.weights_path = require_value(index, argc, argv, "--weights");
        } else if (arg == "--onnx") {
            options.onnx_path = require_value(index, argc, argv, "--onnx");
        } else if (arg == "--tensorrt") {
            options.tensorrt_path = require_value(index, argc, argv, "--tensorrt");
        } else if (arg == "--batch-size") {
            options.batch_size = static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--batch-size")));
        } else if (arg == "--device-id") {
            options.device_id = std::stoi(require_value(index, argc, argv, "--device-id"));
        } else if (arg == "--limit-images") {
            options.limit_images = static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--limit-images")));
        } else if (arg == "--eval-max-dets") {
            options.eval_max_dets = static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--eval-max-dets")));
        } else if (arg == "--workers") {
            options.workers = std::stoi(require_value(index, argc, argv, "--workers"));
        } else if (arg == "--lanes") {
            options.lanes = std::stoi(require_value(index, argc, argv, "--lanes"));
        } else if (arg == "--cpu-affinity") {
            options.cpu_affinity = require_value(index, argc, argv, "--cpu-affinity");
        } else if (arg == "--backend") {
            options.backend = require_value(index, argc, argv, "--backend");
        } else if (arg == "--fp16") {
            options.allow_fp16 = true;
        } else if (arg == "--no-fp16") {
            options.allow_fp16 = false;
        } else if (arg == "--progress") {
            options.progress_bar = true;
        } else if (arg == "--no-progress") {
            options.progress_bar = false;
        } else if (arg == "--compile-mode") {
            options.compilation_mode = parse_compilation_mode(require_value(index, argc, argv, "--compile-mode"));
        } else {
            throw std::runtime_error("unknown rfdetr evaluate option: " + arg);
        }
    }

    if (options.compiled_path.empty()) {
        throw std::runtime_error("rfdetr evaluate requires --compiled");
    }
    require_model_input(options, "evaluate");
    return options;
}

std::optional<NormalizeWeightsOptions> parse_normalize_weights_args(int argc, char** argv) {
    if (argc < 3 || std::string(argv[2]) != "normalize-weights") {
        return std::nullopt;
    }
    NormalizeWeightsOptions options;
    for (int index = 3; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--input") {
            options.input_path = require_value(index, argc, argv, "--input");
        } else if (arg == "--output") {
            options.output_path = require_value(index, argc, argv, "--output");
        } else {
            throw std::runtime_error("unknown rfdetr normalize-weights option: " + arg);
        }
    }
    if (options.input_path.empty() || options.output_path.empty()) {
        throw std::runtime_error("rfdetr normalize-weights requires --input and --output");
    }
    return options;
}

std::optional<TrainCliOptions> parse_train_args(int argc, char** argv) {
    if (argc < 3 || std::string(argv[2]) != "train") {
        return std::nullopt;
    }

    TrainCliOptions options;
    bool saw_device_id = false;
    bool saw_device_ids = false;
    for (int index = 3; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--train-compiled") {
            options.train_compiled_path = require_value(index, argc, argv, "--train-compiled");
        } else if (arg == "--val-compiled") {
            options.val_compiled_path = require_value(index, argc, argv, "--val-compiled");
        } else if (arg == "--test-compiled") {
            options.test_compiled_path = require_value(index, argc, argv, "--test-compiled");
        } else if (arg == "--output-dir") {
            options.output_dir = require_value(index, argc, argv, "--output-dir");
        } else if (arg == "--weights") {
            options.weights_path = require_value(index, argc, argv, "--weights");
        } else if (arg == "--resume") {
            options.resume_path = require_value(index, argc, argv, "--resume");
        } else if (arg == "--batch-size") {
            options.batch_size = static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--batch-size")));
        } else if (arg == "--val-batch-size") {
            options.val_batch_size = static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--val-batch-size")));
        } else if (arg == "--epochs") {
            options.epochs = std::stoi(require_value(index, argc, argv, "--epochs"));
        } else if (arg == "--grad-accum-steps") {
            options.grad_accum_steps = std::stoi(require_value(index, argc, argv, "--grad-accum-steps"));
        } else if (arg == "--lr") {
            options.lr = std::stod(require_value(index, argc, argv, "--lr"));
        } else if (arg == "--lr-encoder") {
            options.lr_encoder = std::stod(require_value(index, argc, argv, "--lr-encoder"));
        } else if (arg == "--lr-component-decay") {
            options.lr_component_decay = std::stod(require_value(index, argc, argv, "--lr-component-decay"));
        } else if (arg == "--encoder-layer-decay") {
            options.encoder_layer_decay = std::stod(require_value(index, argc, argv, "--encoder-layer-decay"));
        } else if (arg == "--weight-decay") {
            options.weight_decay = std::stod(require_value(index, argc, argv, "--weight-decay"));
        } else if (arg == "--lr-drop") {
            options.lr_drop = std::stoi(require_value(index, argc, argv, "--lr-drop"));
        } else if (arg == "--lr-scheduler") {
            options.lr_scheduler = require_value(index, argc, argv, "--lr-scheduler");
            if (options.lr_scheduler != "step" && options.lr_scheduler != "cosine") {
                throw std::runtime_error("rfdetr train --lr-scheduler must be 'step' or 'cosine'");
            }
        } else if (arg == "--lr-min-factor") {
            options.lr_min_factor = std::stod(require_value(index, argc, argv, "--lr-min-factor"));
        } else if (arg == "--warmup-epochs") {
            options.warmup_epochs = std::stod(require_value(index, argc, argv, "--warmup-epochs"));
        } else if (arg == "--clip-max-norm") {
            options.clip_max_norm = std::stod(require_value(index, argc, argv, "--clip-max-norm"));
        } else if (arg == "--fused-optimizer") {
            options.fused_optimizer = true;
        } else if (arg == "--no-fused-optimizer") {
            options.fused_optimizer = false;
        } else if (arg == "--use-ema") {
            options.use_ema = true;
        } else if (arg == "--no-ema") {
            options.use_ema = false;
        } else if (arg == "--ema-decay") {
            options.ema_decay = std::stod(require_value(index, argc, argv, "--ema-decay"));
        } else if (arg == "--ema-tau") {
            options.ema_tau = std::stoi(require_value(index, argc, argv, "--ema-tau"));
        } else if (arg == "--device-id") {
            options.device_id = std::stoi(require_value(index, argc, argv, "--device-id"));
            saw_device_id = true;
        } else if (arg == "--device-ids") {
            options.device_ids = parse_device_ids(require_value(index, argc, argv, "--device-ids"));
            saw_device_ids = true;
        } else if (arg == "--workers") {
            options.workers = std::stoi(require_value(index, argc, argv, "--workers"));
        } else if (arg == "--lanes") {
            options.lanes = std::stoi(require_value(index, argc, argv, "--lanes"));
        } else if (arg == "--cpu-affinity") {
            options.cpu_affinity = require_value(index, argc, argv, "--cpu-affinity");
        } else if (arg == "--prefetch-factor") {
            options.prefetch_factor = std::stoi(require_value(index, argc, argv, "--prefetch-factor"));
        } else if (arg == "--print-freq") {
            options.print_freq = std::stoi(require_value(index, argc, argv, "--print-freq"));
        } else if (arg == "--seed") {
            options.seed = std::stoi(require_value(index, argc, argv, "--seed"));
        } else if (arg == "--eval-max-dets") {
            options.eval_max_dets = static_cast<size_t>(std::stoull(require_value(index, argc, argv, "--eval-max-dets")));
        } else if (arg == "--amp") {
            options.amp = true;
        } else if (arg == "--no-amp") {
            options.amp = false;
        } else if (arg == "--progress") {
            options.progress_bar = true;
        } else if (arg == "--no-progress") {
            options.progress_bar = false;
        } else if (arg == "--dist-worker") {
            options.distributed_worker = true;
        } else if (arg == "--dist-rank") {
            options.distributed_rank = std::stoi(require_value(index, argc, argv, "--dist-rank"));
        } else if (arg == "--dist-world-size") {
            options.distributed_world_size = std::stoi(require_value(index, argc, argv, "--dist-world-size"));
        } else if (arg == "--dist-store-file") {
            options.distributed_store_path = require_value(index, argc, argv, "--dist-store-file");
        } else if (arg == "--compile-mode") {
            options.compilation_mode = parse_compilation_mode(require_value(index, argc, argv, "--compile-mode"));
        } else {
            throw std::runtime_error("unknown rfdetr train option: " + arg);
        }
    }

    if (options.train_compiled_path.empty() || options.val_compiled_path.empty() || options.output_dir.empty()) {
        throw std::runtime_error("rfdetr train requires --train-compiled, --val-compiled, and --output-dir");
    }
    const size_t selected_input_count =
        static_cast<size_t>(!options.weights_path.empty()) +
        static_cast<size_t>(!options.resume_path.empty());
    if (selected_input_count != 1) {
        throw std::runtime_error("rfdetr train requires exactly one of --weights or --resume");
    }
    if (saw_device_id && saw_device_ids) {
        throw std::runtime_error("rfdetr train accepts only one of --device-id or --device-ids");
    }
    if (saw_device_ids && options.device_ids.size() == 1) {
        options.device_id = options.device_ids.front();
        options.device_ids.clear();
    }
    if (options.distributed_worker) {
        if (options.distributed_rank < 0 || options.distributed_world_size <= 1 || options.distributed_store_path.empty()) {
            throw std::runtime_error("invalid internal RF-DETR distributed worker arguments");
        }
    } else if (options.distributed_world_size != 1 || options.distributed_rank != 0 || !options.distributed_store_path.empty()) {
        throw std::runtime_error("internal RF-DETR distributed worker options require --dist-worker");
    }
    return options;
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

void print_validate_usage() {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  fastloader rfdetr predict --compiled <split.bin> --weights <weights.pt> --output <predictions.json> "
                 "[--batch-size <n>] [--device-id <n>] [--threshold <f>] [--workers <n>] [--lanes <n>] "
                 "[--cpu-affinity <list>] [--progress|--no-progress]\n"
                 "  fastloader rfdetr predict --compiled <split.bin> (--onnx <model.onnx> | --tensorrt <model.engine>) "
                 "--output <predictions.json> [--batch-size <n>] [--device-id <n>] [--threshold <f>] [--workers <n>] "
                 "[--lanes <n>] [--cpu-affinity <list>] [--backend auto|onnx|tensorrt] [--progress|--no-progress]\n"
                 "  fastloader rfdetr evaluate|eval|val --compiled <split.bin> --weights <weights.pt> [--batch-size <n>] "
                 "[--device-id <n>] [--limit-images <n>] [--eval-max-dets <n>] [--workers <n>] [--lanes <n>] "
                 "[--cpu-affinity <list>] [--progress|--no-progress]\n"
                 "  fastloader rfdetr evaluate|eval|val --compiled <split.bin> (--onnx <model.onnx> | --tensorrt <model.engine>) "
                 "[--batch-size <n>] [--device-id <n>] [--limit-images <n>] [--eval-max-dets <n>] [--workers <n>] "
                 "[--lanes <n>] [--cpu-affinity <list>] [--backend auto|onnx|tensorrt] [--progress|--no-progress]\n"
                 "  fastloader rfdetr train --train-compiled <train.bin> --val-compiled <val.bin> --output-dir <dir> "
                 "(--weights <weights.pt> | --resume <checkpoint.pt>) [--epochs <n>] [--batch-size <n>] [--val-batch-size <n>] "
                 "[--grad-accum-steps <n>] [--lr <f>] [--lr-encoder <f>] [--lr-component-decay <f>] "
                 "[--encoder-layer-decay <f>] [--weight-decay <f>] [--lr-drop <n>] [--lr-scheduler step|cosine] "
                 "[--lr-min-factor <f>] [--warmup-epochs <f>] [--clip-max-norm <f>] "
                 "[--fused-optimizer|--no-fused-optimizer] [--use-ema|--no-ema] [--ema-decay <f>] [--ema-tau <n>] "
                 "[--device-id <n> | --device-ids <n,n,...>] [--workers <n>] [--lanes <n>] [--cpu-affinity <list>] [--prefetch-factor <n>] "
                 "[--print-freq <n>] [--seed <n>] [--eval-max-dets <n>] [--amp|--no-amp] [--progress|--no-progress]\n"
                 "  fastloader rfdetr validate --compiled <split.bin> [--source <dataset_dir> --split <name> "
                 "--resolution <n> --recompile] [--onnx <model.onnx>] [--tensorrt <model.engine|model.onnx>] "
                 "[--save-engine <path.engine>] [--workers <n>] [--batch-size <n>] [--cpu-affinity <list>] "
                 "[--profile] [--report-json <path>]\n"
                 "  note: weights overlap mode requires --batch-size 1 when predict/evaluate --weights uses --lanes > 1\n"
                 "  note: native train --lanes supports 0, 1, 2, or 3; with --lanes > 1, effective batch per rank is batch_size * lanes * grad_accum_steps\n"
                 "  fastloader rfdetr normalize-weights --input <weights.pt> --output <native.pt>\n"
                 "  fastloader rfdetr build-engine --onnx <model.onnx> --output <model.engine> [--fp16]\n"
                 "  fastloader rfdetr compile --source-dir <dir> --output-dir <dir> [--resolution <n>]\n"
                 "  fastloader rfdetr info (--onnx <model.onnx> | --tensorrt <model.engine|model.onnx>)\n");
}

} // namespace

int handle_cli(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "rfdetr") {
        return -1;
    }
    try {
        if (const auto compile = parse_compile_args(argc, argv)) {
            run_compile(*compile);
            return 0;
        }

        if (const auto info = parse_info_args(argc, argv)) {
            std::unique_ptr<InferenceBackend> backend =
                !info->onnx_path.empty() ? make_onnx_backend(info->onnx_path, info->device_id)
                                         : make_tensorrt_backend(info->tensorrt_path, info->device_id, true);
            print_model_metadata(backend->info(), 0, 0, ValidationLogMode::Interactive);
            return 0;
        }

        if (const auto build_engine = parse_build_engine_args(argc, argv)) {
            make_tensorrt_backend(
                build_engine->onnx_path,
                build_engine->device_id,
                build_engine->allow_fp16,
                build_engine->output_path);
            std::fprintf(stderr, "tensorrt: wrote engine to %s\n", build_engine->output_path.c_str());
            return 0;
        }

        if (const auto predict = parse_predict_args(argc, argv)) {
            const PredictionRunResult result = run_prediction(*predict);
            write_prediction_json(*predict, result);
            print_prediction_summary(*predict, result);
            return 0;
        }

        if (const auto evaluate = parse_evaluate_args(argc, argv)) {
            const EvaluationRunResult result = run_evaluation(*evaluate);
            print_evaluation_summary(*evaluate, result);
            return 0;
        }

        if (const auto train = parse_train_args(argc, argv)) {
            if (!train->distributed_worker && train->device_ids.size() > 1) {
                return spawn_distributed_training_workers(*train, argc, argv);
            }
            const TrainRunResult result = run_training(*train);
            print_training_summary(*train, result);
            return 0;
        }

        if (const auto normalize = parse_normalize_weights_args(argc, argv)) {
            const NativeCheckpoint checkpoint =
                normalize_checkpoint_to_native(normalize->input_path, normalize->output_path);
            std::fprintf(stderr,
                         "rfdetr.normalize-weights: wrote %zu tensors for preset=%s to %s\n",
                         checkpoint.state_dict.size(),
                         checkpoint.metadata.preset_name.c_str(),
                         normalize->output_path.c_str());
            return 0;
        }

        if (const auto validate = parse_validate_args(argc, argv)) {
            const ValidationRunResult result = run_validation(*validate);
            write_validation_report(*validate, result);
            print_validation_run_summary(*validate, result);
            return 0;
        }

        print_validate_usage();
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fastloader rfdetr error: %s\n", error.what());
        return 1;
    }
}

} // namespace fastloader::rfdetr
