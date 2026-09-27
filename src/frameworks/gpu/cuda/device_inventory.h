#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace mmltk::frameworks::gpu {
inline constexpr std::size_t kCudaDeviceCapacity = 64U;
struct CudaDeviceFact final {
 int ordinal = 0;
 MMLTK_MAX_BYTES(256U) std::string name;
 std::uint64_t total_vram = 0U;
 bool operator==(const CudaDeviceFact&) const = default;
};
MMLTK_REFLECT_FIELDS(CudaDeviceFact)
[[nodiscard]] std::vector<CudaDeviceFact> discover_cuda_devices();
}  // namespace mmltk::frameworks::gpu
