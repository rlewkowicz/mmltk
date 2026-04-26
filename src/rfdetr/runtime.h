#pragma once

#include "worker_pool.h"

#include <memory>
#include <string>
#include <vector>

namespace mmltk::rfdetr {

struct RuntimeConfig {
    int workers = 0;
    int lanes = 1;
    std::vector<int> cpu_affinity;
};

struct RuntimeSplit {
    int loader_threads = 2;
    int gather_threads = 1;
    int lane_threads = 1;
    int cpu_threads = 1;
};

RuntimeConfig resolve_runtime_config(int requested_workers, int requested_lanes, const std::string& cpu_affinity_value);
RuntimeSplit split_runtime_workers(const RuntimeConfig& config);

class RuntimeContext {
   public:
    explicit RuntimeContext(const RuntimeConfig& config);

    [[nodiscard]] const RuntimeConfig& config() const {
        return config_;
    }
    [[nodiscard]] const RuntimeSplit& split() const {
        return split_;
    }
    [[nodiscard]] mmltk::WorkerPool& cpu_pool() const {
        return *cpu_pool_;
    }
    [[nodiscard]] const std::vector<int>& loader_cpus() const {
        return loader_cpus_;
    }
    [[nodiscard]] const std::vector<int>& lane_cpus() const {
        return lane_cpus_;
    }
    [[nodiscard]] const std::vector<int>& cpu_cpus() const {
        return cpu_cpus_;
    }
    [[nodiscard]] std::string loader_affinity_string() const;
    [[nodiscard]] std::string lane_affinity_string() const;
    [[nodiscard]] std::string cpu_affinity_string() const;

   private:
    RuntimeConfig config_;
    RuntimeSplit split_;
    std::vector<int> loader_cpus_;
    std::vector<int> lane_cpus_;
    std::vector<int> cpu_cpus_;
    std::shared_ptr<mmltk::WorkerPool> cpu_pool_;
};

}  // namespace mmltk::rfdetr
