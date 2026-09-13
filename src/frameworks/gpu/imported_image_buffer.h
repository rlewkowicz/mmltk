#pragma once

#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "src/common/io/scoped_fd.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"

namespace mmltk::frameworks::gpu {
namespace test_support {
struct ImportedImageBufferTestAccess;
}

// One independent Vulkan payload. Every native alias shares this owner, including
// its CUDA context and the unconsumed FD retaining backing after browser exit.
class ImportedImageBuffer final {
   public:
    ImportedImageBuffer();
    ~ImportedImageBuffer() noexcept;
    ImportedImageBuffer(const ImportedImageBuffer&) = delete;
    ImportedImageBuffer& operator=(const ImportedImageBuffer&) = delete;

    [[nodiscard]] bool Import(DeviceContext, mmltk::common::io::ScopedFd, const ImageWorkspaceLayout&, std::uint64_t identity,
                              std::string* error);
    [[nodiscard]] cudaError_t Release() noexcept;
    [[nodiscard]] cudaError_t release_failure() const noexcept { return release_failure_; }
    [[nodiscard]] CUdeviceptr data() const noexcept;
    [[nodiscard]] std::size_t pitch_bytes() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return data() == 0U; }
    [[nodiscard]] std::size_t allocation_size() const noexcept;
    [[nodiscard]] bool owns_resources() const noexcept;
    [[nodiscard]] std::shared_ptr<ImportedImageBuffer> ImportAlias(DeviceContext) const;

   private:
    struct Resources final {
        std::optional<DeviceContext> context;
        mmltk::common::io::ScopedFd backing;
        CUexternalMemory memory = nullptr;
        CUdeviceptr mapped_base = 0U;
        CUdeviceptr data = 0U;
        ImageWorkspaceLayout layout;
        std::uint64_t identity = 0U;
    };
    struct ReleaseOperations final {
        CUresult (*free_mapping)(CUdeviceptr);
        CUresult (*destroy_memory)(CUexternalMemory);
    };
    [[nodiscard]] cudaError_t Release(const ReleaseOperations&) noexcept;
    std::shared_ptr<Resources> resources_;
    cudaError_t release_failure_ = cudaSuccess;
    TerminalCudaRetirementOwner terminal_{1U};
    TerminalCudaRetirementLease retention_ = ReserveTerminalCudaLease(terminal_);
    friend struct test_support::ImportedImageBufferTestAccess;
};
}  // namespace mmltk::frameworks::gpu
