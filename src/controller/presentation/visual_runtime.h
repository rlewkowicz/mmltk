#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/reflection/reflected_field_policy.h"

namespace mmltk::frameworks::gpu {
class SystemImageRuntime;
}

namespace mmltk::controller {

struct VisualDeviceSettings final {
    int device = -1;
    std::uint32_t maximum_width = 0U;
    std::uint32_t maximum_height = 0U;
    int numa_node = -1;
    [[nodiscard]] constexpr bool valid() const noexcept {
        return device >= 0 && numa_node >= -1 && maximum_width != 0U && maximum_height != 0U;
    }
};

using VisualRuntimeFactory = std::function<std::unique_ptr<mmltk::frameworks::gpu::SystemImageRuntime>()>;

[[nodiscard]] mmltk::frameworks::gpu::DeviceExecution resolve_visual_device_execution(const VisualDeviceSettings&);

MMLTK_REFLECT_FIELDS(VisualDeviceSettings)

}  // namespace mmltk::controller
