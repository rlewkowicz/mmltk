#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Ort {
struct SessionOptions;
}

namespace mmltk::backend::imaging::upscale::shiftlut {

// The runtime owns physical storage across ORT graph fallback and releases it
// explicitly after provider settlement, before destroying the session.
class Operators final {
   public:
    explicit Operators(std::size_t decision_capacity_pixels = 0);
    ~Operators();
    Operators(const Operators&) = delete;
    Operators& operator=(const Operators&) = delete;
    void Register(Ort::SessionOptions&);
    cudaError_t Release() noexcept;
    // Verification-only opt-in: copy captured integer lookup inputs after the
    // caller settles inference. Ordinary runtimes allocate no decision storage.
    cudaError_t ReadDecisions(std::span<std::int8_t>) noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Effect-only cold allocation evidence; never used to order or select work.
std::uint64_t allocation_count() noexcept;

}  // namespace mmltk::backend::imaging::upscale::shiftlut
