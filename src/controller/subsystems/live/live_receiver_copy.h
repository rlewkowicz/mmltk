#pragma once
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
#include "src/frameworks/gpu/image_types.h"
namespace mmltk::controller::detail {
struct LiveReceiverCopyOperations final {
    cudaError_t (*wait)(std::uintptr_t stream, std::uintptr_t event) noexcept = nullptr;
    cudaError_t (*copy)(mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t source, std::size_t source_pitch,
                        std::uintptr_t stream) noexcept = nullptr;
    cudaError_t (*synchronize)(std::uintptr_t stream) noexcept = nullptr;
    [[nodiscard]] bool valid() const noexcept { return wait != nullptr && copy != nullptr && synchronize != nullptr; }
};
[[nodiscard]] LiveReceiverCopyOperations native_live_receiver_copy_operations() noexcept;
[[nodiscard]] cudaError_t copy_live_receiver_frame(mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t source, std::size_t source_pitch,
                                                   std::uintptr_t ready_event, std::uintptr_t stream, const LiveReceiverCopyOperations&) noexcept;
}  // namespace mmltk::controller::detail
