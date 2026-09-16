#pragma once
#include <cuda.h>
#include <cstddef>
#include <cstdint>
namespace mmltk::frameworks::gpu {
class CudaPitchedBufferView {
   public:
    [[nodiscard]] CUdeviceptr data() const noexcept { return device_ptr_; }
    [[nodiscard]] std::size_t pitch_bytes() const noexcept { return pitch_bytes_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] bool empty() const noexcept { return device_ptr_ == 0; }

   protected:
    CUdeviceptr device_ptr_ = 0;
    std::size_t pitch_bytes_ = 0U;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
};
}  // namespace mmltk::frameworks::gpu
