#include "cuda_vulkan_fill.h"
namespace {
__global__ void fill_image(void* destination, std::size_t pitch, std::uint32_t width, std::uint32_t height, std::uint32_t value) {
    const auto x = blockIdx.x * blockDim.x + threadIdx.x;
    const auto y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x < width && y < height) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<char*>(destination) + y * pitch);
        row[x] = cuda_vulkan_pixel(value, x, y);
    }
}
}  // namespace
cudaError_t cuda_vulkan_fill(void* destination, std::size_t pitch, std::uint32_t width, std::uint32_t height, std::uint32_t value, cudaStream_t stream) {
    fill_image<<<dim3((width + 15U) / 16U, (height + 15U) / 16U), dim3(16, 16), 0, stream>>>(destination, pitch, width, height, value);
    return cudaGetLastError();
}
