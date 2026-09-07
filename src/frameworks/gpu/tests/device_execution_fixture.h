#pragma once
#include "src/frameworks/gpu/device_execution.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <stdexcept>

namespace mmltk::frameworks::gpu::test_support {
// Hardware may legitimately have no reported locality on a multi-node host.
// Verify rejection before deliberately choosing an eligible node for the test.
inline DeviceExecution selected_test_device(int ordinal, const mmltk::common::system::NumaTopology& topology) {
    try {
        return resolve_device_execution(ordinal, topology);
    } catch (const std::invalid_argument& error) {
        REQUIRE(std::string_view(error.what()).find("unknown") != std::string_view::npos);
        REQUIRE(topology.nodes.size() > 1);
    }
    for (int node : topology.permitted_nodes) {
        if (std::ranges::none_of(topology.cpus, [&](const auto& cpu) {
                return cpu.node == node && std::ranges::find(topology.permitted_cpus, cpu.cpu) != topology.permitted_cpus.end();
            }))
            continue;
        const auto selected = resolve_device_execution(ordinal, topology, node);
        CHECK(selected.placement.numa_node == node);
        return selected;
    }
    throw std::runtime_error("hardware test has no permitted NUMA node with an eligible CPU");
}
}  // namespace mmltk::frameworks::gpu::test_support
