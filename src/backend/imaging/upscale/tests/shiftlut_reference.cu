#include "shiftlut_reference.h"
#include <cuda_runtime.h>

namespace mmltk::backend::imaging::upscale::tests {
namespace {

// Independent boundary oracle: the full-image normalization expression and
// per-pixel channel loop from 55ee9927, compiled under the same NVCC policy.
// It deliberately does not call production tile preparation.
__global__ void normalize(const std::uint8_t* source, std::size_t pitch, std::uint32_t width,
                          std::uint32_t height, float* target) {
    const std::uint64_t pixel = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::uint64_t count = static_cast<std::uint64_t>(width) * height;
    if (pixel >= count) return;
    const auto* rgba = source + static_cast<std::size_t>(pixel / width) * pitch + static_cast<std::size_t>(pixel % width) * 4;
    for (std::uint32_t channel = 0; channel < 3; ++channel) {
        const float mean = channel == 0 ? 0.485F : channel == 1 ? 0.456F : 0.406F;
        const float deviation = channel == 0 ? 0.229F : channel == 1 ? 0.224F : 0.225F;
        const float rgb = static_cast<float>(rgba[channel]) * (1.0F / 255.0F);
        target[static_cast<std::uint64_t>(channel) * count + pixel] = (rgb - mean) / deviation;
    }
}

}  // namespace

void normalize_reference(const std::uint8_t* source, std::size_t pitch, std::uint32_t width,
                         std::uint32_t height, float* target, cudaStream_t stream) {
    const auto count = static_cast<std::uint64_t>(width) * height;
    normalize<<<static_cast<unsigned int>((count + 255) / 256), 256, 0, stream>>>(source, pitch, width, height, target);
}

}  // namespace mmltk::backend::imaging::upscale::tests
