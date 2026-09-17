#pragma once
#include <memory>
#include <cstddef>
#include <string>
#include <vector>
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/common/system/numa_memory.h"
#include "src/backend/models/rfdetr/core/detail/runtime_workspace_fwd.h"
namespace mmltk::backend::models::rfdetr {
struct RuntimeConfig {
    bool h2d_dataloader = true;
    int workers = 0;
    int lanes = 1;
    int loader_prefetch_factor = 2;
    std::vector<int> cpu_affinity;
    mmltk::frameworks::gpu::DeviceExecution execution;
    std::vector<int> library_cpus;
};
struct RuntimeSplit {
    int loader_threads = 3;
    int gather_threads = 2;
    int lane_threads = 1;
    int cpu_threads = 1;
};
RuntimeConfig resolve_runtime_config(int requested_workers, int requested_lanes, int loader_prefetch_factor, const std::string& cpu_affinity_value, int device,
                                     int numa_node = -1);
RuntimeSplit split_runtime_workers(const RuntimeConfig& config);
class RuntimeContext;
class ScopedRuntimeContext final {
   public:
    explicit ScopedRuntimeContext(RuntimeContext* runtime, std::size_t lane = 0, MatcherWorkspace* workspace = nullptr) noexcept;
    ~ScopedRuntimeContext();
    ScopedRuntimeContext(const ScopedRuntimeContext&) = delete;
    ScopedRuntimeContext& operator=(const ScopedRuntimeContext&) = delete;

   private:
    RuntimeContext* previous_ = nullptr;
    std::size_t previous_lane_ = 0;
    MatcherWorkspace* previous_workspace_ = nullptr;
};
[[nodiscard]] RuntimeContext* active_runtime_context() noexcept;
[[nodiscard]] MatcherWorkspace* active_matcher_workspace();
class RuntimeContext {
   public:
    explicit RuntimeContext(const RuntimeConfig& config);
    ~RuntimeContext();
    [[nodiscard]] MatcherWorkspace& matcher_workspace();
    [[nodiscard]] LsapScratch& solver_workspace() { return *solver_workspaces_.at(cpu_pool_->current_worker_index()); }
    [[nodiscard]] const RuntimeConfig& config() const { return config_; }
    [[nodiscard]] const mmltk::frameworks::gpu::DeviceExecution& execution() const { return config_.execution; }
    [[nodiscard]] const RuntimeSplit& split() const { return split_; }
    [[nodiscard]] mmltk::common::concurrency::WorkerPool& cpu_pool() const { return *cpu_pool_; }
    [[nodiscard]] const std::vector<int>& loader_cpus() const { return loader_cpus_; }
    [[nodiscard]] const std::vector<int>& lane_cpus() const { return lane_cpus_; }
    [[nodiscard]] const std::vector<int>& cpu_cpus() const { return cpu_cpus_; }
    [[nodiscard]] std::string loader_affinity_string() const;
    [[nodiscard]] std::string lane_affinity_string() const;
    [[nodiscard]] std::string cpu_affinity_string() const;

   private:
    RuntimeConfig config_;
    RuntimeSplit split_;
    std::vector<int> loader_cpus_;
    std::vector<int> lane_cpus_;
    std::vector<int> cpu_cpus_;
    mmltk::common::system::NumaMemoryResource solver_memory_;
    std::vector<std::unique_ptr<LsapScratch>> solver_workspaces_;
    std::vector<std::unique_ptr<MatcherWorkspace>> matcher_workspaces_;
    std::shared_ptr<mmltk::common::concurrency::WorkerPool> cpu_pool_;
};
}  // namespace mmltk::backend::models::rfdetr
