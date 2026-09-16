#pragma once
#include <ATen/ATen.h>
#include <cstddef>
#include <memory>
#include <span>
namespace mmltk::backend::ml::cuda {
// One immutable serialization snapshot, with reusable ordinal-addressed storage.
// Reserve the complete next boundary before Stage; Complete precedes CPU reads.
// Each touched device owns one submission stream and joins the producer stream
// selected at Reserve; callers settle other producers before reserving.
// Callers must destroy archive/graph/raw-reader views before Release or Begin.
class TensorReadbackBuffers final {
   public:
    TensorReadbackBuffers();
    ~TensorReadbackBuffers() noexcept;
    TensorReadbackBuffers(const TensorReadbackBuffers&) = delete;
    TensorReadbackBuffers& operator=(const TensorReadbackBuffers&) = delete;
    void Begin();
    void Reserve(std::span<const at::Tensor> sources, std::size_t first_slot = 0);
    [[nodiscard]] at::Tensor Stage(std::size_t slot);
    void Complete();
    void Release();
    void ReleaseSettled();
    [[nodiscard]] std::size_t capacity_bytes() const noexcept;
    [[nodiscard]] bool admission_open() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::ml::cuda
