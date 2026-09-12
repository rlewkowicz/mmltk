#pragma once

#include <memory>
#include "src/common/io/scoped_fd.h"
#include "src/frameworks/gpu/image_workspace.h"

namespace mmltk::frameworks::gpu::test_support {
// Hardware-only fixture; Vulkan never enters the production GPU target.
class VulkanWorkspaceFixture final {
   public:
    explicit VulkanWorkspaceFixture(int device, std::uint32_t width, std::uint32_t height);
    ~VulkanWorkspaceFixture();
    VulkanWorkspaceFixture(const VulkanWorkspaceFixture&) = delete;
    VulkanWorkspaceFixture& operator=(const VulkanWorkspaceFixture&) = delete;
    [[nodiscard]] const ImageWorkspaceLayout& layout() const noexcept;
    [[nodiscard]] mmltk::common::io::ScopedFd Export() const;

   private:
    struct State;
    std::unique_ptr<State> state_;
};
}  // namespace mmltk::frameworks::gpu::test_support
