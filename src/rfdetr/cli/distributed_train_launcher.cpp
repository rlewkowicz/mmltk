#include "distributed_train_launcher.h"

#include "cpu_affinity.h"
#include "execution_policy.h"
#include "mmltk_logging.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace mmltk::rfdetr::cli_support {

namespace {

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
        {"--device-id", true}, {"--device-ids", true},      {"--workers", true},         {"--cpu-affinity", true},
        {"--dist-rank", true}, {"--dist-world-size", true}, {"--dist-store-file", true}, {"--dist-worker", false},
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

}  

int spawn_distributed_training_workers(const TrainOptions& options, int argc, char** argv) {
    const int world_size = static_cast<int>(options.device_ids.size());
    if (world_size < 2) {
        throw std::runtime_error("distributed RF-DETR train requires at least two device ids");
    }
#if !defined(USE_C10D_NCCL)
    (void)argc;
    (void)argv;
    throw std::runtime_error("distributed RF-DETR train requires a LibTorch build with NCCL/c10d enabled");
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
            throw std::runtime_error("rfdetr train device id is out of range for the visible CUDA device count");
        }
    }

    const auto store_path = distributed_store_path_for_parent();
    std::filesystem::remove(store_path);

    const std::vector<int> all_cpus =
        options.cpu_affinity.empty() ? mmltk::allowed_cpu_set() : mmltk::resolve_cpu_affinity(options.cpu_affinity);
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
            const int clamped_workers = mmltk::clamp_worker_count_to_cpus(rank_workers, shard_cpus.size(), 0, 3);
            const std::string subsystem = "rfdetr.distributed.rank" + std::to_string(rank);
            mmltk::log_worker_budget_clamp(subsystem.c_str(), rank_workers, clamped_workers, shard_cpus, 0, 3);
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
            mmltk::logging::logger("rfdetr.cli")
                ->error("mmltk rfdetr error: failed to exec distributed worker: {}", std::strerror(errno));
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

}  
