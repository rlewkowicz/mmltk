#include <ATen/Context.h>
#include <ATen/Parallel.h>
#include <atomic>
#include <mutex>
#include "src/frameworks/gpu/device_execution.h"
#include <torch/script.h>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/execution_policy.h"
#include "src/backend/models/rfdetr/core/detail/lsap_scratch.h"
#include "src/backend/models/rfdetr/core/detail/matcher_workspace.h"
#include "runtime.h"
import mmltk.common.logging.mmltk_logging;
namespace mmltk::backend::models::rfdetr {
namespace {
thread_local RuntimeContext* g_runtime_context = nullptr;
thread_local std::size_t g_runtime_lane = 0;
thread_local MatcherWorkspace* g_matcher_workspace = nullptr;
int clamp_positive(int value, int fallback) { return value > 0 ? value : fallback; }
std::vector<int> take_cpu_slice(const std::vector<int>& cpus, size_t begin, size_t count) {
    if (cpus.empty() || count == 0) { return {}; }
    std::vector<int> out;
    out.reserve(count);
    for (size_t index = 0; index < count; ++index) { out.push_back(cpus[(begin + index) % cpus.size()]); }
    std::vector<int> unique;
    for (int cpu : out)
        if (std::ranges::find(unique, cpu) == unique.end()) unique.push_back(cpu);
    out = std::move(unique);
    return out;
}
std::string join_cpu_list(const std::vector<int>& cpus) {
    std::string value;
    for (size_t index = 0; index < cpus.size(); ++index) {
        if (index > 0) { value += ","; }
        value += std::to_string(cpus[index]);
    }
    return value;
}
}  // namespace
ScopedRuntimeContext::ScopedRuntimeContext(RuntimeContext* const runtime, std::size_t lane, MatcherWorkspace* workspace) noexcept
    : previous_(g_runtime_context), previous_lane_(g_runtime_lane), previous_workspace_(g_matcher_workspace) {
    g_runtime_context = runtime;
    g_runtime_lane = lane;
    g_matcher_workspace = workspace;
}
ScopedRuntimeContext::~ScopedRuntimeContext() {
    g_runtime_context = previous_;
    g_runtime_lane = previous_lane_;
    g_matcher_workspace = previous_workspace_;
}
RuntimeContext* active_runtime_context() noexcept { return g_runtime_context; }
MatcherWorkspace* active_matcher_workspace() {
    return g_matcher_workspace ? g_matcher_workspace : (g_runtime_context ? &g_runtime_context->matcher_workspace() : nullptr);
}
MatcherWorkspace& RuntimeContext::matcher_workspace() { return *matcher_workspaces_.at(g_runtime_lane); }
RuntimeConfig resolve_runtime_config(int requested_workers, int requested_lanes, int loader_prefetch_factor, const std::string& cpu_affinity_value, int device,
                                     int numa_node) {
    if (loader_prefetch_factor < 0 || loader_prefetch_factor == std::numeric_limits<int>::max()) {
        throw std::runtime_error("RF-DETR runtime loader prefetch factor is out of range");
    }
    RuntimeConfig config;
    const auto topology = mmltk::common::system::NumaTopology::Capture();
    config.library_cpus = topology.permitted_cpus;
    config.execution = mmltk::frameworks::gpu::resolve_device_execution(device, topology, numa_node, cpu_affinity_value);
    config.cpu_affinity = config.execution.placement.cpus;
    const int available_workers = static_cast<int>(std::max<size_t>(config.cpu_affinity.size(), 1));
    const int default_workers = available_workers;
    const int requested_total = clamp_positive(requested_workers, std::max(1, default_workers));
    if (available_workers >= 3) {
        config.workers = mmltk::common::system::clamp_worker_count_to_cpus(requested_total, config.cpu_affinity.size(), 0, 3);
        if (config.workers != requested_total) {
            mmltk::common::logging::warn([&](auto& logger) {
                logger.warn(
                    "rfdetr.runtime worker budget clamped requested={} resolved={} cpuset={} reserved={} "
                    "minimum={}",
                    requested_total, config.workers, mmltk::common::system::format_cpu_list(config.cpu_affinity), 0, 3);
            });
        }
    } else {
        config.workers = std::max(3, requested_total);
        mmltk::common::logging::warn([&](auto& logger) {
            logger.warn("rfdetr.runtime cpuset={} has fewer than 3 CPUs; runtime helper threads will overlap",
                        mmltk::common::system::format_cpu_list(config.cpu_affinity));
        });
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
        mmltk::common::logging::warn([&](auto& logger) {
            logger.warn("rfdetr.runtime worker split clamped loader={}->{} lanes={}->{} cpu={} total={}", requested_loader_threads, split.loader_threads,
                        requested_lanes, split.lane_threads, split.cpu_threads, total);
        });
    }
    return split;
}
RuntimeContext::RuntimeContext(const RuntimeConfig& config)
    : config_(config), split_(split_runtime_workers(config)), solver_memory_(config.execution.placement.numa_node) {
    for (int lane = 0; lane <= split_.lane_threads; ++lane)
        matcher_workspaces_.push_back(std::make_unique<MatcherWorkspace>(config.execution.placement.numa_node, config.h2d_dataloader));
    solver_workspaces_.reserve(static_cast<std::size_t>(split_.cpu_threads));
    for (int worker = 0; worker < split_.cpu_threads; ++worker) solver_workspaces_.push_back(std::make_unique<LsapScratch>(&solver_memory_));
    // Library helper teams initialize once with the owner's pre-pinning process
    // eligibility. Product solver work remains on the native runtime pool.
    static std::once_flag initialized;
    std::call_once(initialized, [&] {
        const auto previous = mmltk::common::system::allowed_cpu_set();
        mmltk::common::system::set_thread_affinity(config_.library_cpus);
        try {
            at::init_num_threads();
            std::atomic<bool> bad_affinity{false};
            at::parallel_for(0, std::max(2, at::get_num_threads()) * 2, 1, [&](std::int64_t, std::int64_t) {
                try {
                    if (at::get_num_threads() > 1 && config_.library_cpus.size() > 1 && mmltk::common::system::allowed_cpu_set().size() == 1)
                        bad_affinity.store(true);
                } catch (...) { bad_affinity.store(true); }
            });
            if (bad_affinity.load()) throw std::runtime_error("library helper team inherited a single creator CPU");
        } catch (...) {
            mmltk::common::system::set_thread_affinity(previous);
            throw;
        }
        mmltk::common::system::set_thread_affinity(previous);
    });
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
    if (split_.loader_threads > 0 && loader_cpus_.empty()) { loader_cpus_ = config_.cpu_affinity; }
    if (lane_cpus_.empty()) { lane_cpus_ = config_.cpu_affinity; }
    if (cpu_cpus_.empty()) { cpu_cpus_ = config_.cpu_affinity; }
    cpu_pool_ = std::make_shared<mmltk::common::concurrency::WorkerPool>(static_cast<size_t>(split_.cpu_threads), cpu_cpus_, "rfdetrcpu", 0U,
                                                                         &config_.execution.placement, false);
}
RuntimeContext::~RuntimeContext() = default;
std::string RuntimeContext::loader_affinity_string() const { return join_cpu_list(loader_cpus_); }
std::string RuntimeContext::lane_affinity_string() const { return join_cpu_list(lane_cpus_); }
std::string RuntimeContext::cpu_affinity_string() const { return join_cpu_list(cpu_cpus_); }
}  // namespace mmltk::backend::models::rfdetr
