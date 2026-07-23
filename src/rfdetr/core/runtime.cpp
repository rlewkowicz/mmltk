#include "rfdetr/runtime.h"

#include "execution_policy.h"
#include "mmltk_logging.h"

#include <ATen/Context.h>
#include <torch/script.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace mmltk::rfdetr {

namespace {

int clamp_positive(int value, int fallback) {
    return value > 0 ? value : fallback;
}

std::vector<int> take_cpu_slice(const std::vector<int>& cpus, size_t begin, size_t count) {
    if (cpus.empty() || count == 0) {
        return {};
    }
    std::vector<int> out;
    out.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        out.push_back(cpus[(begin + index) % cpus.size()]);
    }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string join_cpu_list(const std::vector<int>& cpus) {
    std::string value;
    for (size_t index = 0; index < cpus.size(); ++index) {
        if (index > 0) {
            value += ",";
        }
        value += std::to_string(cpus[index]);
    }
    return value;
}

}  

RuntimeConfig resolve_runtime_config(int requested_workers, int requested_lanes, int loader_prefetch_factor,
                                     const std::string& cpu_affinity_value) {
    if (loader_prefetch_factor < 0 || loader_prefetch_factor == std::numeric_limits<int>::max()) {
        throw std::runtime_error("RF-DETR runtime loader prefetch factor is out of range");
    }
    RuntimeConfig config;
    config.cpu_affinity =
        cpu_affinity_value.empty() ? mmltk::allowed_cpu_set() : mmltk::resolve_cpu_affinity(cpu_affinity_value);
    const int available_workers = static_cast<int>(std::max<size_t>(config.cpu_affinity.size(), 1));
    const int default_workers = available_workers;
    const int requested_total = clamp_positive(requested_workers, std::max(1, default_workers));
    if (available_workers >= 3) {
        config.workers = mmltk::clamp_worker_count_to_cpus(requested_total, config.cpu_affinity.size(), 0, 3);
        mmltk::log_worker_budget_clamp("rfdetr.runtime", requested_total, config.workers, config.cpu_affinity, 0, 3);
    } else {
        config.workers = std::max(3, requested_total);
        mmltk::logging::logger("exec")->warn(
            "rfdetr.runtime cpuset={} has fewer than 3 CPUs; runtime helper threads will overlap",
            mmltk::format_cpu_list(config.cpu_affinity));
    }
    config.lanes = clamp_positive(requested_lanes, 1);
    config.loader_prefetch_factor = loader_prefetch_factor;
    return config;
}

RuntimeSplit split_runtime_workers(const RuntimeConfig& config) {
    RuntimeSplit split;
    const int total = std::max(1, config.workers);
    const int requested_lanes = std::max(1, config.lanes);
    const int requested_loader_threads = config.loader_prefetch_factor > 0 ? config.loader_prefetch_factor + 1 : 0;

    if (total < 3) {
        split.loader_threads = requested_loader_threads > 0 ? 1 : 0;
        split.gather_threads = split.loader_threads > 0 ? 1 : 0;
        split.lane_threads = 1;
        split.cpu_threads = 1;
        return split;
    }

    const int minimum_loader_threads = requested_loader_threads > 0 ? 1 : 0;
    split.lane_threads = std::min(requested_lanes, total - minimum_loader_threads - 1);
    const int loader_capacity = total - split.lane_threads - 1;
    split.loader_threads = std::min(requested_loader_threads, loader_capacity);
    split.gather_threads = split.loader_threads > 0 ? std::max(1, split.loader_threads - 1) : 0;
    split.cpu_threads = total - split.loader_threads - split.lane_threads;

    if (split.loader_threads != requested_loader_threads || split.lane_threads != requested_lanes) {
        mmltk::logging::logger("exec")->warn(
            "rfdetr.runtime worker split clamped loader={}->{} lanes={}->{} cpu={} total={}", requested_loader_threads,
            split.loader_threads, requested_lanes, split.lane_threads, split.cpu_threads, total);
    }
    return split;
}

RuntimeContext::RuntimeContext(const RuntimeConfig& config) : config_(config), split_(split_runtime_workers(config)) {
    at::globalContext().setAllowTF32CuDNN(false);
    at::globalContext().setAllowTF32CuBLAS(false);
    at::globalContext().setFlushDenormal(false);
    at::globalContext().setWarnOnAccumulateGradStreamMismatch(false);
    torch::jit::setGraphExecutorOptimize(true);

    size_t offset = 0;
    loader_cpus_ = take_cpu_slice(config_.cpu_affinity, offset, static_cast<size_t>(split_.loader_threads));
    offset += static_cast<size_t>(split_.loader_threads);
    lane_cpus_ = take_cpu_slice(config_.cpu_affinity, offset, static_cast<size_t>(split_.lane_threads));
    offset += static_cast<size_t>(split_.lane_threads);
    cpu_cpus_ = take_cpu_slice(config_.cpu_affinity, offset, static_cast<size_t>(split_.cpu_threads));
    if (split_.loader_threads > 0 && loader_cpus_.empty()) {
        loader_cpus_ = config_.cpu_affinity;
    }
    if (lane_cpus_.empty()) {
        lane_cpus_ = config_.cpu_affinity;
    }
    if (cpu_cpus_.empty()) {
        cpu_cpus_ = config_.cpu_affinity;
    }
    cpu_pool_ = std::make_shared<mmltk::WorkerPool>(static_cast<size_t>(split_.cpu_threads), cpu_cpus_, "rfdetrcpu");
}

std::string RuntimeContext::loader_affinity_string() const {
    return join_cpu_list(loader_cpus_);
}

std::string RuntimeContext::lane_affinity_string() const {
    return join_cpu_list(lane_cpus_);
}

std::string RuntimeContext::cpu_affinity_string() const {
    return join_cpu_list(cpu_cpus_);
}

}  
