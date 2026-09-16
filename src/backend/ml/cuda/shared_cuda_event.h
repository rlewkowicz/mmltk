#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_authority.h"

namespace mmltk::backend::ml::cuda {
class CudaEventPool final {
   public:
    static constexpr std::size_t max_capacity = 64U;
    class Lease final {
       public:
        Lease() noexcept = default;
        ~Lease() noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        void wait(std::uintptr_t stream, const char* context) const;
        void retire();
        [[nodiscard]] explicit operator bool() const noexcept;

       private:
        static constexpr std::size_t kStorageBytes = 64U;
        alignas(std::max_align_t) std::array<std::byte, kStorageBytes> storage_{};
        bool engaged_ = false;
        friend class CudaEventPool;
    };
    CudaEventPool(mmltk::frameworks::gpu::CudaDeviceOwner owner, std::size_t capacity,
                  mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement_authority);
    ~CudaEventPool() noexcept;
    CudaEventPool(const CudaEventPool&) = delete;
    CudaEventPool& operator=(const CudaEventPool&) = delete;
    CudaEventPool(CudaEventPool&&) = delete;
    CudaEventPool& operator=(CudaEventPool&&) = delete;
    [[nodiscard]] std::optional<Lease> record(std::uintptr_t stream, const char* context);
    [[nodiscard]] std::size_t capacity() const noexcept;

   private:
    struct State;
    std::unique_ptr<State> state_;
};
}  // namespace mmltk::backend::ml::cuda
