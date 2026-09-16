#include "video_frame_convert.h"
#include <cuda_runtime.h>
#include <algorithm>
namespace mmltk::backend::media::video {
namespace {
__device__ float sample(VideoPlane plane, unsigned x, unsigned y) {
    const auto* address = plane.data + y * plane.pitch + static_cast<std::size_t>(x) * plane.step + plane.offset;
    const auto value = plane.depth + plane.shift > 8 ? *reinterpret_cast<const std::uint16_t*>(address) : *address;
    return static_cast<float>(value >> plane.shift) / static_cast<float>((1U << plane.depth) - 1U);
}
__global__ void convert(VideoColorConversion input, unsigned width, unsigned height, float* target) {
    const auto x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= width) return;
    // The capped grid covers tall admitted frames without a second launch.
    // One final increment stays below UINT_MAX for the source's int heights.
    const auto row_stride = blockDim.y * gridDim.y;
    for (auto y = blockIdx.y * blockDim.y + threadIdx.y; y < height; y += row_stride) {
        float first = sample(input.planes[0], x, y);
        float second = sample(input.planes[1], x >> input.chroma_x, y >> input.chroma_y);
        float third = sample(input.planes[2], x >> input.chroma_x, y >> input.chroma_y);
        if (!input.rgb) {
            const float scale = static_cast<float>(1U << (input.planes[0].depth - 8));
            const float maximum = static_cast<float>((1U << input.planes[0].depth) - 1U);
            const float yy = input.full_range ? first : (first * maximum - 16.0F * scale) / (219.0F * scale);
            const float cb = (second * maximum - 128.0F * scale) / ((input.full_range ? maximum : 224.0F * scale));
            const float cr = (third * maximum - 128.0F * scale) / ((input.full_range ? maximum : 224.0F * scale));
            first = yy + 2.0F * (1.0F - input.kr) * cr;
            third = yy + 2.0F * (1.0F - input.kb) * cb;
            second = (yy - input.kr * first - input.kb * third) / (1.0F - input.kr - input.kb);
        }
        unsigned target_x = x, target_y = y, target_width = width;
        switch (input.clockwise_quarters) {
            case 1U:
                target_x = height - 1U - y;
                target_y = x;
                target_width = height;
                break;
            case 2U:
                target_x = width - 1U - x;
                target_y = height - 1U - y;
                break;
            case 3U:
                target_x = y;
                target_y = width - 1U - x;
                target_width = height;
                break;
            default: break;
        }
        const auto offset = static_cast<std::size_t>(target_y) * target_width + target_x;
        const auto plane = static_cast<std::size_t>(width) * height;
        target[offset] = fminf(1.0F, fmaxf(0.0F, first));
        target[plane + offset] = fminf(1.0F, fmaxf(0.0F, second));
        target[2U * plane + offset] = fminf(1.0F, fmaxf(0.0F, third));
    }
}
}  // namespace
int convert_video_chw(VideoColorConversion input, std::uint32_t width, std::uint32_t height, float* target, cudaStream_t stream) noexcept {
    convert<<<dim3((width + 15U) / 16U, std::min((height + 15U) / 16U, 65535U)), dim3(16, 16), 0, stream>>>(input, width, height, target);
    return cudaGetLastError();
}
}  // namespace mmltk::backend::media::video
