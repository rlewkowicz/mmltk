#include "rfdetr/cuda_utils.h"
#include <cstdint>

#include <cmath>

namespace mmltk::rfdetr {
namespace {

struct AffineMatrix {
    float value[6];
};

__global__ void warp_affine_bgr_to_planar_kernel(const std::uint8_t* src, std::size_t src_pitch_bytes,
                                                 std::uint32_t src_width, std::uint32_t src_height, float* dst,
                                                 std::uint32_t dst_width, std::uint32_t dst_height, AffineMatrix d2s,
                                                 std::uint32_t edge) {
    const std::uint32_t position = blockDim.x * blockIdx.x + threadIdx.x;
    if (position >= edge) {
        return;
    }

    const std::uint32_t dx = position % dst_width;
    const std::uint32_t dy = position / dst_width;

    const float sampled_src_x =
        d2s.value[0] * static_cast<float>(dx) + d2s.value[1] * static_cast<float>(dy) + d2s.value[2] + 0.5f;
    const float sampled_src_y =
        d2s.value[3] * static_cast<float>(dx) + d2s.value[4] * static_cast<float>(dy) + d2s.value[5] + 0.5f;
    const float src_x = sampled_src_x;
    const float src_y = sampled_src_y;

    float b = 128.0f;
    float g = 128.0f;
    float r = 128.0f;

    if (src_x >= 0.0f && src_x < static_cast<float>(src_width) && src_y >= 0.0f &&
        src_y < static_cast<float>(src_height)) {
        const std::int32_t x_low = static_cast<std::int32_t>(floorf(src_x));
        const std::int32_t y_low = static_cast<std::int32_t>(floorf(src_y));
        const std::int32_t x_high = min(x_low + 1, static_cast<std::int32_t>(src_width) - 1);
        const std::int32_t y_high = min(y_low + 1, static_cast<std::int32_t>(src_height) - 1);

        const float lx = src_x - static_cast<float>(x_low);
        const float ly = src_y - static_cast<float>(y_low);
        const float hx = 1.0f - lx;
        const float hy = 1.0f - ly;
        const float w1 = hx * hy;
        const float w2 = lx * hy;
        const float w3 = hx * ly;
        const float w4 = lx * ly;

        const auto* row0 = src + static_cast<std::size_t>(y_low) * src_pitch_bytes;
        const auto* row1 = src + static_cast<std::size_t>(y_high) * src_pitch_bytes;
        const auto* p1 = row0 + static_cast<std::size_t>(x_low) * 3U;
        const auto* p2 = row0 + static_cast<std::size_t>(x_high) * 3U;
        const auto* p3 = row1 + static_cast<std::size_t>(x_low) * 3U;
        const auto* p4 = row1 + static_cast<std::size_t>(x_high) * 3U;

        b = w1 * static_cast<float>(p1[0]) + w2 * static_cast<float>(p2[0]) + w3 * static_cast<float>(p3[0]) +
            w4 * static_cast<float>(p4[0]);
        g = w1 * static_cast<float>(p1[1]) + w2 * static_cast<float>(p2[1]) + w3 * static_cast<float>(p3[1]) +
            w4 * static_cast<float>(p4[1]);
        r = w1 * static_cast<float>(p1[2]) + w2 * static_cast<float>(p2[2]) + w3 * static_cast<float>(p3[2]) +
            w4 * static_cast<float>(p4[2]);
    }

    r /= 255.0f;
    g /= 255.0f;
    b /= 255.0f;

    const std::uint32_t area = dst_width * dst_height;
    float* dst_r = dst + dy * dst_width + dx;
    float* dst_g = dst_r + area;
    float* dst_b = dst_g + area;

    *dst_r = r;
    *dst_g = g;
    *dst_b = b;
}

__global__ void vertical_flip_in_place_pitched_kernel(std::uint8_t* buffer, std::size_t pitch_bytes,
                                                      std::uint32_t width, std::uint32_t height) {
    const std::uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
    const std::uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
    const std::uint32_t half_height = height / 2U;
    if (x >= width || y >= half_height) {
        return;
    }

    const std::uint32_t flipped_y = height - 1U - y;
    auto* top_pixel = buffer + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * 3U;
    auto* bottom_pixel = buffer + static_cast<std::size_t>(flipped_y) * pitch_bytes + static_cast<std::size_t>(x) * 3U;
    for (int channel = 0; channel < 3; ++channel) {
        const std::uint8_t tmp = top_pixel[channel];
        top_pixel[channel] = bottom_pixel[channel];
        bottom_pixel[channel] = tmp;
    }
}

}  // namespace

const char* validate_bgr_split_to_planar_float_args(std::size_t src_pitch_bytes, std::uint32_t src_width,
                                                    std::uint32_t src_height, std::uint32_t dst_width,
                                                    std::uint32_t dst_height, cudaStream_t stream) {
    if (src_width == 0U || src_height == 0U) {
        return "source width and height must be positive";
    }
    if (dst_width == 0U || dst_height == 0U) {
        return "destination width and height must be positive";
    }
    if (stream == nullptr) {
        return "CUDA stream must be non-null";
    }
    constexpr std::size_t kBgr3BytesPerPixel = 3U;
    const std::size_t min_pitch_bytes = static_cast<std::size_t>(src_width) * kBgr3BytesPerPixel;
    if (src_pitch_bytes < min_pitch_bytes) {
        return "source pitch is smaller than the split width in bytes";
    }
    return nullptr;
}

cudaError_t launch_bgr_split_to_planar_float(const std::uint8_t* src, std::size_t src_pitch_bytes,
                                             std::uint32_t src_width, std::uint32_t src_height, float* dst,
                                             std::uint32_t dst_width, std::uint32_t dst_height, cudaStream_t stream) {
    if (src == nullptr || dst == nullptr || src_width == 0U || src_height == 0U || dst_width == 0U ||
        dst_height == 0U) {
        return cudaErrorInvalidValue;
    }
    if (validate_bgr_split_to_planar_float_args(src_pitch_bytes, src_width, src_height, dst_width, dst_height,
                                                stream) != nullptr) {
        return cudaErrorInvalidValue;
    }

    const float scale = fminf(static_cast<float>(dst_width) / static_cast<float>(src_width),
                              static_cast<float>(dst_height) / static_cast<float>(src_height));
    const float inverse_scale = scale > 0.0f ? 1.0f / scale : 0.0f;
    const float translate_x = -scale * static_cast<float>(src_width) * 0.5f + static_cast<float>(dst_width) * 0.5f;
    const float translate_y = -scale * static_cast<float>(src_height) * 0.5f + static_cast<float>(dst_height) * 0.5f;

    AffineMatrix d2s{};
    d2s.value[0] = inverse_scale;
    d2s.value[1] = 0.0f;
    d2s.value[2] = -translate_x * inverse_scale;
    d2s.value[3] = 0.0f;
    d2s.value[4] = inverse_scale;
    d2s.value[5] = -translate_y * inverse_scale;

    const std::uint32_t jobs = dst_width * dst_height;
    constexpr std::uint32_t kThreads = 256;
    const std::uint32_t blocks = (jobs + kThreads - 1U) / kThreads;
    (void)cudaGetLastError();
    warp_affine_bgr_to_planar_kernel<<<blocks, kThreads, 0, stream>>>(src, src_pitch_bytes, src_width, src_height, dst,
                                                                      dst_width, dst_height, d2s, jobs);
    return cudaPeekAtLastError();
}

cudaError_t launch_bgr_vertical_flip_in_place_pitched(std::uint8_t* buffer, std::size_t pitch_bytes,
                                                      std::uint32_t width, std::uint32_t height, cudaStream_t stream) {
    if (buffer == nullptr || width == 0U || height == 0U || stream == nullptr) {
        return cudaErrorInvalidValue;
    }

    constexpr std::size_t kBgr3BytesPerPixel = 3U;
    if (pitch_bytes < static_cast<std::size_t>(width) * kBgr3BytesPerPixel) {
        return cudaErrorInvalidValue;
    }
    if (height < 2U) {
        return cudaSuccess;
    }

    const dim3 block(16U, 16U);
    const std::uint32_t half_height = height / 2U;
    const dim3 grid((width + block.x - 1U) / block.x, (half_height + block.y - 1U) / block.y);
    vertical_flip_in_place_pitched_kernel<<<grid, block, 0, stream>>>(buffer, pitch_bytes, width, height);
    return cudaPeekAtLastError();
}

}  // namespace mmltk::rfdetr
