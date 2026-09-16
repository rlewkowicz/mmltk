#pragma once
#include <ATen/ATen.h>
#include <memory>
#include "src/frameworks/gpu/pinned_host_buffer.h"
namespace mmltk::backend::ml::cuda {
// Tensor storage retains the registered extent, including when a view escapes
// its original owner. Shapes describe active elements, never reserved capacity.
class NumaHostTensor final {
   public:
    explicit NumaHostTensor(int device, std::shared_ptr<void> context_custody = {});
    [[nodiscard]] at::Tensor view(at::IntArrayRef shape, at::ScalarType dtype);
    [[nodiscard]] std::size_t capacity_bytes() const noexcept;
    // Only an owner that has completed all CUDA work may use this. Escaped
    // tensor views prevent physical release.
    [[nodiscard]] CUresult ReleaseSettled() noexcept;

   private:
    int device_;
    std::shared_ptr<void> context_custody_;
    std::shared_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> storage_;
};
[[nodiscard]] at::Tensor numa_empty(at::IntArrayRef shape, at::ScalarType dtype, int device = -1);
[[nodiscard]] at::Tensor numa_readback(const at::Tensor& source);
}  // namespace mmltk::backend::ml::cuda
