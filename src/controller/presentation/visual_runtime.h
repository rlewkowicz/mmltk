#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/image_workspace.h"
#include "src/frameworks/reflection/reflected_field_policy.h"

namespace mmltk::frameworks::gpu {
class SystemImageRuntime;
class ImageProductRevisionSequence;
struct SystemImageRuntimeConfig;
}  // namespace mmltk::frameworks::gpu

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

using VisualRuntimeFactory = std::function<std::unique_ptr<mmltk::frameworks::gpu::SystemImageRuntime>(
    std::shared_ptr<mmltk::frameworks::gpu::ImageProductRevisionSequence>)>;

struct VisualWorkspaceRequest final {
    std::uint64_t product_owner = 0U;
    std::uint64_t product_revision = 0U;
    mmltk::frameworks::gpu::ImageWorkspaceLayout layout{};
    mmltk::frameworks::gpu::DeviceExecution display_execution{};
    std::uint64_t admitted_allocation = 0U;
    // Wake-only response; no producer or graphics work executes in the sink.
    std::function<void()> ready;
};

void configure_visual_workspace_finalization(mmltk::frameworks::gpu::SystemImageRuntimeConfig&);

[[nodiscard]] mmltk::frameworks::gpu::DeviceExecution resolve_visual_device_execution(const VisualDeviceSettings&);

MMLTK_REFLECT_FIELDS(VisualDeviceSettings)

}  // namespace mmltk::controller
