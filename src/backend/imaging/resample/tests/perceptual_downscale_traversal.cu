// SPDX-License-Identifier: MIT
#include "src/backend/imaging/resample/tests/perceptual_downscale_traversal.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale_accumulation.cuh"
#include <cuda_runtime.h>
namespace mmltk::backend::imaging::resample::test_perceptual {
namespace {
using namespace perceptual;
template <RgbPixelFormat Format, bool Integer>
__global__ void traversal_kernel(RgbConstImageView source, std::uint32_t width, std::uint32_t height, TraversalMoments* results) {
    __shared__ float table_storage[256];
    auto& transfer = *reinterpret_cast<TransferTable*>(table_storage);
    transfer.linear[threadIdx.x] = decode(float(threadIdx.x) * (1.0F / 255.0F));
    __syncthreads();
    for (std::size_t i = blockIdx.x * std::size_t(blockDim.x) + threadIdx.x; i < std::size_t(width) * height; i += std::size_t(blockDim.x) * gridDim.x) {
        const auto fx = footprint(source.layout.width, width, static_cast<std::uint32_t>(i % width));
        const auto fy = footprint(source.layout.height, height, static_cast<std::uint32_t>(i / width));
        MomentAccumulator retained, serial;
        float retained_alpha = 0, serial_alpha = 0;
        accumulate_strided<Format, Integer>(source, transfer, fx, fy, 0, 1, retained, retained_alpha);
        accumulate_serial<Format, Integer>(source, transfer, fx, fy, serial, serial_alpha);
        const Moment moments[]{retained.finish(), serial.finish()};
        for (unsigned path = 0; path < 2; ++path) {
            auto& result = results[2 * i + path];
            for (unsigned channel = 0; channel < 3; ++channel) {
                result.bits[channel] = __float_as_uint(moments[path].mean[channel]);
                result.bits[channel + 3] = __float_as_uint(moments[path].variance[channel]);
            }
            result.bits[6] = __float_as_uint(path == 0 ? retained.weight : serial.weight);
            result.bits[7] = __float_as_uint(path == 0 ? retained_alpha : serial_alpha);
        }
    }
}
template <RgbPixelFormat Format>
void launch_traversal(RgbConstImageView source, std::uint32_t width, std::uint32_t height, TraversalMoments* results, cudaStream_t stream) {
    const auto blocks = static_cast<unsigned>((std::size_t(width) * height + 255) / 256);
    if (source.layout.width % width == 0 && source.layout.height % height == 0)
        traversal_kernel<Format, true><<<blocks, 256, 0, stream>>>(source, width, height, results);
    else
        traversal_kernel<Format, false><<<blocks, 256, 0, stream>>>(source, width, height, results);
}
}  // namespace
cudaError_t compare_traversal(RgbConstImageView source, std::uint32_t width, std::uint32_t height, TraversalMoments* results, cudaStream_t stream) {
    switch (source.layout.format) {
        case RgbPixelFormat::RGB8: launch_traversal<RgbPixelFormat::RGB8>(source, width, height, results, stream); break;
        case RgbPixelFormat::RGBA8: launch_traversal<RgbPixelFormat::RGBA8>(source, width, height, results, stream); break;
        case RgbPixelFormat::PlanarUnitSrgbF32: launch_traversal<RgbPixelFormat::PlanarUnitSrgbF32>(source, width, height, results, stream); break;
    }
    return cudaGetLastError();
}
}  // namespace mmltk::backend::imaging::resample::test_perceptual
