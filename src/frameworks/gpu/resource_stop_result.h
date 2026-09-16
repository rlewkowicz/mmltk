#pragma once
#include <cuda_runtime_api.h>
namespace mmltk::frameworks::gpu {
enum class ResourceStopDisposition : unsigned char {
    Closing,
    Released,
    TerminalRetained,
};
struct ResourceStopResult final {
    cudaError_t failure = cudaSuccess;
    ResourceStopDisposition disposition = ResourceStopDisposition::Released;
    [[nodiscard]] bool stopped() const noexcept { return disposition != ResourceStopDisposition::Closing; }
    [[nodiscard]] bool released() const noexcept { return disposition == ResourceStopDisposition::Released && failure == cudaSuccess; }
    [[nodiscard]] bool custody_retained() const noexcept { return disposition == ResourceStopDisposition::TerminalRetained; }
};
}  // namespace mmltk::frameworks::gpu
