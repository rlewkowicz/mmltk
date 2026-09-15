#pragma once

#include <functional>
#include <optional>

#include "src/controller/contracts/compute.h"
#include "src/frameworks/gpu/device_execution.h"

namespace mmltk::controller {

struct DirectComputeConfiguration final {
    std::optional<mmltk::frameworks::gpu::DeviceExecution> execution;
    [[nodiscard]] bool valid() const noexcept {
        return execution && execution->device >= 0 && execution->placement.numa_node >= 0 && !execution->placement.cpus.empty();
    }
};

using ComputeProgressSink = std::function<void(const contracts::ComputeProgress&)>;

}  // namespace mmltk::controller
