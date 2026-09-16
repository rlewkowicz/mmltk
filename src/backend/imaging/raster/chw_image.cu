#include "chw_image.h"
#include <cuda_runtime.h>
namespace mmltk::backend::imaging::raster {
namespace {
__global__ void convert_chw(const float* source, std::uint32_t width, std::uint32_t height, std::uint8_t* destination, std::size_t pitch) {
    const auto x = blockIdx.x * blockDim.x + threadIdx.x;
    const auto y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const auto plane = static_cast<std::size_t>(width) * height;
    const auto offset = static_cast<std::size_t>(y) * width + x;
    auto* pixel = destination + y * pitch + static_cast<std::size_t>(x) * 4U;
    for (unsigned channel = 0U; channel < 3U; ++channel)
        pixel[channel] = static_cast<std::uint8_t>(__float2uint_rn(fminf(1.0F, fmaxf(0.0F, source[channel * plane + offset])) * 255.0F));
    pixel[3] = 255U;
}
}  // namespace
int chw_float_to_rgba(const float* source, std::uint32_t width, std::uint32_t height, std::uint8_t* destination, std::size_t pitch,
                      cudaStream_t stream) noexcept {
    if (!source || !destination || !width || !height || pitch < static_cast<std::size_t>(width) * 4U) return cudaErrorInvalidValue;
    convert_chw<<<dim3((width + 15U) / 16U, (height + 15U) / 16U), dim3(16U, 16U), 0U, stream>>>(source, width, height, destination, pitch);
    return cudaGetLastError();
}
}  // namespace mmltk::backend::imaging::raster
