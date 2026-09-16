#include "src/common/system/numa_topology.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>
#include "src/common/system/cpu_affinity.h"
namespace mmltk::common::system {
namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read topology: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::vector<int> status_list(const std::string& name) {
    std::ifstream input("/proc/self/status");
    std::string line;
    while (std::getline(input, line))
        if (line.starts_with(name + ":")) return parse_cpu_list(line.substr(name.size() + 1));
    throw std::runtime_error("missing permitted topology: " + name);
}
}  // namespace
NumaTopology NumaTopology::Capture() {
    NumaTopology result;
    result.permitted_cpus = allowed_cpu_set();
    result.permitted_nodes = status_list("Mems_allowed_list");
    const std::filesystem::path root("/sys/devices/system");
    for (int node : parse_cpu_list(read(root / "node/online"))) {
        const auto directory = root / "node" / ("node" + std::to_string(node));
        const auto memory = read(directory / "meminfo");
        const auto begin = memory.find("MemTotal:");
        if (begin == std::string::npos) throw std::runtime_error("missing NUMA node memory size");
        result.nodes.push_back({node, std::stoull(memory.substr(begin + 9)) * 1024U});
        const auto cpu_text = read(directory / "cpulist");
        if (cpu_text.find_first_of("0123456789") == std::string::npos) continue;
        for (int cpu : parse_cpu_list(cpu_text)) {
            const auto topology = root / "cpu" / ("cpu" + std::to_string(cpu)) / "topology";
            result.cpus.push_back({cpu, node, std::stoi(read(topology / "physical_package_id")), std::stoi(read(topology / "core_id"))});
        }
    }
    return result;
}
std::vector<int> physical_core_order(const NumaTopology& topology, std::span<const int> eligible) {
    std::vector<int> ordered(eligible.begin(), eligible.end());
    std::ranges::sort(ordered);
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    std::vector<int> first, siblings;
    first.reserve(ordered.size());
    siblings.reserve(ordered.size());
    std::set<std::pair<int, int>> cores;
    for (int cpu : ordered) {
        const auto fact = std::ranges::find(topology.cpus, cpu, &CpuTopology::cpu);
        if (fact == topology.cpus.end()) throw std::invalid_argument("CPU has no physical topology");
        (cores.emplace(fact->package, fact->core).second ? first : siblings).push_back(cpu);
    }
    first.insert(first.end(), siblings.begin(), siblings.end());
    return first;
}
ExecutionPlacement resolve_placement(const NumaTopology& topology, int local_node, int requested_node, std::span<const int> eligible) {
    if (requested_node < -1 || local_node < -1) throw std::invalid_argument("invalid NUMA node");
    if (local_node < 0 && topology.nodes.size() == 1) local_node = topology.nodes.front().node;
    if (requested_node >= 0 && local_node >= 0 && requested_node != local_node) throw std::invalid_argument("numa_node contradicts GPU locality");
    const int node = requested_node >= 0 ? requested_node : local_node;
    if (node < 0) throw std::invalid_argument("GPU NUMA locality is unknown; select numa_node explicitly");
    const auto memory = std::ranges::find(topology.nodes, node, &MemoryNode::node);
    if (memory == topology.nodes.end() || memory->bytes == 0 || std::ranges::find(topology.permitted_nodes, node) == topology.permitted_nodes.end())
        throw std::invalid_argument("GPU NUMA memory node is unavailable or forbidden");
    std::vector<int> cpus;
    for (const auto& cpu : topology.cpus)
        if (cpu.node == node && std::ranges::find(topology.permitted_cpus, cpu.cpu) != topology.permitted_cpus.end() &&
            (eligible.empty() || std::ranges::find(eligible, cpu.cpu) != eligible.end()))
            cpus.push_back(cpu.cpu);
    if (cpus.empty()) throw std::invalid_argument("GPU NUMA node has no eligible permitted CPU");
    return {node, physical_core_order(topology, cpus), memory->bytes};
}
}  // namespace mmltk::common::system
