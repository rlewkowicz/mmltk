#include "rfdetr/runtime.h"

#include "execution_policy.h"
#include "mmltk_logging.h"

#include <ATen/Context.h>
#include <torch/script.h>

#include <algorithm>
#include <cstdio>
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
    std::sort(out.begin(), out.end());
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

} // namespace

RuntimeConfig resolve_runtime_config(int requested_workers,
                                     int requested_lanes,
                                     const std::string& cpu_affinity_value) {
    RuntimeConfig config;
    config.cpu_affinity =
        cpu_affinity_value.empty() ? mmltk::allowed_cpu_set()
                                   : mmltk::resolve_cpu_affinity(cpu_affinity_value);
    const int available_workers = static_cast<int>(std::max<size_t>(config.cpu_affinity.size(), 1));
    const int default_workers = available_workers;
    const int requested_total = clamp_positive(requested_workers, std::max(1, default_workers));
    if (available_workers >= 3) {
        config.workers =
            mmltk::clamp_worker_count_to_cpus(requested_total, config.cpu_affinity.size(), 0, 3);
        mmltk::log_worker_budget_clamp("rfdetr.runtime",
                                            requested_total,
                                            config.workers,
                                            config.cpu_affinity,
                                            0,
                                            3);
    } else {
        config.workers = std::max(3, requested_total);
        mmltk::logging::logger("exec")->warn(
            "rfdetr.runtime cpuset={} has fewer than 3 CPUs; runtime helper threads will overlap",
            mmltk::format_cpu_list(config.cpu_affinity));
    }
    config.lanes = clamp_positive(requested_lanes, 1);
    return config;
}

RuntimeSplit split_runtime_workers(const RuntimeConfig& config) {
    RuntimeSplit split;
    const int total = std::max(1, config.workers);
    split.lane_threads = std::max(1, std::min(config.lanes, total));

    int loader_threads = total >= 8 ? 4 : (total >= 4 ? 2 : 1);
    loader_threads = std::min(loader_threads, std::max(1, total - split.lane_threads));
    if (loader_threads >= total) {
        loader_threads = std::max(1, total - 1);
    }

    split.loader_threads = std::max(1, loader_threads);
    split.gather_threads = std::max(0, split.loader_threads - 1);
    split.cpu_threads = std::max(1, total - split.loader_threads - split.lane_threads);

    if (split.loader_threads + split.lane_threads + split.cpu_threads > total) {
        split.cpu_threads = std::max(1, total - split.loader_threads - split.lane_threads);
    }
    return split;
}

RuntimeContext::RuntimeContext(const RuntimeConfig& config)
    : config_(config),
      split_(split_runtime_workers(config)) {
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
    if (loader_cpus_.empty()) {
        loader_cpus_ = config_.cpu_affinity;
    }
    if (lane_cpus_.empty()) {
        lane_cpus_ = config_.cpu_affinity;
    }
    if (cpu_cpus_.empty()) {
        cpu_cpus_ = config_.cpu_affinity;
    }
    cpu_pool_ = std::make_shared<mmltk::WorkerPool>(
        static_cast<size_t>(split_.cpu_threads),
        cpu_cpus_,
        "rfdetrcpu");
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

} // namespace mmltk::rfdetr
