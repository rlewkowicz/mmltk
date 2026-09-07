#pragma once

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace mmltk::frameworks::gpu {

namespace test_support {
struct ExportedImageBufferTestAccess;
}

class ExportedImageBuffer final {
   public:
    ExportedImageBuffer() = default;
    ~ExportedImageBuffer() noexcept;
    ExportedImageBuffer(const ExportedImageBuffer&) = delete;
    ExportedImageBuffer& operator=(const ExportedImageBuffer&) = delete;

    [[nodiscard]] bool allocate(int device_id, std::uint32_t width, std::uint32_t height, std::size_t pitch_bytes,
                                std::size_t minimum_allocation_bytes, std::string* error_message);
    [[nodiscard]] int export_descriptor(std::string* error_message) const;
    [[nodiscard]] cudaError_t Release() noexcept;

    [[nodiscard]] inline CUdeviceptr data() const noexcept { return device_ptr_; }
    [[nodiscard]] inline std::size_t pitch_bytes() const noexcept { return pitch_bytes_; }
    [[nodiscard]] inline bool empty() const noexcept { return device_ptr_ == 0U; }
    [[nodiscard]] inline std::size_t allocation_size() const noexcept { return allocation_size_; }
    [[nodiscard]] inline bool owns_resources() const noexcept { return allocation_ != 0U || address_ != 0U || mapping_active_; }

   private:
    struct ReleaseOperations final {
        CUresult (*unmap)(CUdeviceptr, std::size_t) = nullptr;
        CUresult (*free_address)(CUdeviceptr, std::size_t) = nullptr;
        CUresult (*release_allocation)(CUmemGenericAllocationHandle) = nullptr;
    };

    [[nodiscard]] cudaError_t Release(const ReleaseOperations&) noexcept;

    CUmemGenericAllocationHandle allocation_ = 0U;
    CUdeviceptr address_ = 0U;
    CUdeviceptr device_ptr_ = 0U;
    std::size_t allocation_size_ = 0U;
    std::size_t reserved_bytes_ = 0U;
    std::size_t pitch_bytes_ = 0U;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    bool mapping_active_ = false;

    friend struct test_support::ExportedImageBufferTestAccess;
};

}  // namespace mmltk::frameworks::gpu
