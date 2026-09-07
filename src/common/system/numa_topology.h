#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mmltk::common::system {
struct CpuTopology final {
    int cpu = -1;
    int node = -1;
    int package = -1;
    int core = -1;
};
struct MemoryNode final {
    int node = -1;
    std::uint64_t bytes = 0;
};
// Captured by the runtime owner before narrowing any of its workers. Child
// runtimes receive resolved values; they never rediscover the creator's mask.
struct NumaTopology final {
    std::vector<int> permitted_cpus;
    std::vector<int> permitted_nodes;
    std::vector<CpuTopology> cpus;
    std::vector<MemoryNode> nodes;
    [[nodiscard]] static NumaTopology Capture();
};
struct ExecutionPlacement final {
    int numa_node = -1;
    std::vector<int> cpus;
    std::uint64_t node_bytes = 0;
};
[[nodiscard]] ExecutionPlacement resolve_placement(const NumaTopology&, int local_node, int requested_node = -1,
                                                   std::span<const int> eligible_cpus = {});
[[nodiscard]] std::vector<int> physical_core_order(const NumaTopology&, std::span<const int> eligible);
}  // namespace mmltk::common::system
