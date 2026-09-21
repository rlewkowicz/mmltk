#pragma once
#include "src/common/system/numa_topology.h"
#include <algorithm>
#include <stdexcept>
namespace mmltk::common::system::test_support {
[[nodiscard]] inline CpuTopology first_permitted_cpu(const NumaTopology& topology) {
 if (topology.permitted_cpus.empty()) throw std::runtime_error("test topology has no permitted CPUs");
 const auto found = std::ranges::find(topology.cpus, topology.permitted_cpus.front(), &CpuTopology::cpu);
 if (found == topology.cpus.end()) throw std::runtime_error("test topology does not map its first permitted CPU");
 return *found;
}
}  // namespace mmltk::common::system::test_support
