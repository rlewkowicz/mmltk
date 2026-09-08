#include "detail/shiftlut_onnx_ops.h"

#include <cuda_runtime.h>

namespace mmltk::backend::imaging::upscale::shiftlut {
namespace {

struct Position {
    int y;
    int x;
};

// Coordinates in rot90(input, rotation) -> coordinates in input.
__device__ Position unrotate(int y, int x, int height, int width, int rotation) {
    switch (rotation) {
        case 1: return {x, width - 1 - y};
        case 2: return {height - 1 - y, width - 1 - x};
        case 3: return {height - 1 - x, y};
        default: return {y, x};
    }
}

__device__ float centered(const float* input, int channel, int y, int x,
                          int height, int width, int rotation) {
    const auto source = unrotate(y, x, height, width, rotation);
    return fminf(127.0F, fmaxf(-128.0F, input[(channel * height + source.y) * width + source.x] - 128.0F));
}

__device__ float high(float value) { return floorf(value / 4.0F); }
__device__ float low(float value) { return truncf(value - floorf(value / 4.0F) * 4.0F); }
__device__ float clamp_high(float value) { return fminf(31.0F, fmaxf(-32.0F, value)); }

// Upstream Query is followed by Python sum in input order and torch.round.
// Explicit additions prohibit reassociation. PyTorch's CUDA scalar division
// multiplies by its FP32 reciprocal (BinaryDivTrueKernel.cu), then round uses
// ties-to-even; retain that rounding boundary rather than reassociating it.
__device__ float rounded_average(float sum, float reciprocal) {
    return nearbyintf(__fmul_rn(sum, reciprocal));
}

__device__ float lookup(const float* table, int output, int input, int domain, int value, int inputs) {
    return table[(output * inputs + input) * domain + value];
}

__device__ int shifted_pixel(int y, int x, int height, int width, const float* shifts, int channel) {
    return min(height - 1, max(0, y + static_cast<int>(shifts[channel]))) * width +
           min(width - 1, max(0, x + static_cast<int>(shifts[16 + channel])));
}

__global__ void record_shifted_decisions(const std::int8_t* source, const float* shifts, std::int8_t* target,
                                        int height, int width) {
    const int plane = height * width;
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index >= 12 * 16 * plane) return;
    const int batch = index / (16 * plane);
    const int channel = index / plane % 16;
    const int rotated_width = batch / 3 % 2 ? height : width;
    const int rotated_height = batch / 3 % 2 ? width : height;
    const int pixel = shifted_pixel(index % plane / rotated_width, index % rotated_width,
                                    rotated_height, rotated_width, shifts, channel);
    target[index] = source[(batch * 16 + channel) * plane + pixel];
}

__global__ void initial_depthwise(const float* input, const float* tables, std::int8_t* target, int height, int width) {
    const int plane = height * width;
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index >= 12 * 16 * plane) return;
    const int batch = index / (16 * plane);
    const int channel = index / plane % 16;
    const int rotation = batch / 3;
    const int rotated_width = rotation % 2 ? height : width;
    const int rotated_height = rotation % 2 ? width : height;
    const int y = index % plane / rotated_width;
    const int x = index % rotated_width;
    float msb = 0;
    float lsb = 0;
    for (int tap = 0; tap < 9; ++tap) {
        const auto sample = centered(input, batch % 3,
            min(rotated_height - 1, max(0, y + tap / 3 - 1)),
            min(rotated_width - 1, max(0, x + tap % 3 - 1)), height, width, rotation);
        msb = __fadd_rn(msb, lookup(tables + kLowElements, channel, tap, 64, static_cast<int>(high(sample)) + 32, 9));
        lsb = __fadd_rn(lsb, lookup(tables, channel, tap, 4, static_cast<int>(low(sample)), 9));
    }
    const auto residual = centered(input, batch % 3, y, x, height, width, rotation);
    const float high_result = rounded_average(msb, 1.0F / 9.0F) + high(residual);
    const float low_result = fminf(3.0F, fmaxf(0.0F, rounded_average(lsb, 1.0F / 9.0F) + low(residual)));
    target[index] = static_cast<std::int8_t>(clamp_high(high_result + low_result));
}

__global__ void depthwise(const std::int8_t* source, const float* table, std::int8_t* target, int height, int width) {
    const int plane = height * width;
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index >= 12 * 16 * plane) return;
    const int batch = index / (16 * plane);
    const int channel = index / plane % 16;
    const int rotated_width = batch / 3 % 2 ? height : width;
    const int rotated_height = batch / 3 % 2 ? width : height;
    const int y = index % plane / rotated_width;
    const int x = index % rotated_width;
    const auto* image = source + (batch * 16 + channel) * plane;
    float sum = 0;
    for (int tap = 0; tap < 9; ++tap) {
        const int sy = min(rotated_height - 1, max(0, y + tap / 3 - 1));
        const int sx = min(rotated_width - 1, max(0, x + tap % 3 - 1));
        sum = __fadd_rn(sum, lookup(table, channel, tap, 64, static_cast<int>(image[sy * rotated_width + sx]) + 32, 9));
    }
    target[index] = static_cast<std::int8_t>(clamp_high(rounded_average(sum, 1.0F / 9.0F) + source[index]));
}

__global__ void shifted_pointwise(const std::int8_t* source, const float* table, const float* shifts,
                                  std::int8_t* target, int height, int width) {
    const int plane = height * width;
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index >= 12 * 16 * plane) return;
    const int batch = index / (16 * plane);
    const int channel = index / plane % 16;
    const int rotated_width = batch / 3 % 2 ? height : width;
    const int rotated_height = batch / 3 % 2 ? width : height;
    const int y = index % plane / rotated_width;
    const int x = index % rotated_width;
    float sum = 0;
    float residual = 0;
    for (int in = 0; in < 16; ++in) {
        const int pixel = shifted_pixel(y, x, rotated_height, rotated_width, shifts, in);
        const float value = source[(batch * 16 + in) * plane + pixel];
        if (in == channel) residual = value;
        sum = __fadd_rn(sum, lookup(table, channel, in, 64, static_cast<int>(value) + 32, 16));
    }
    target[index] = static_cast<std::int8_t>(clamp_high(rounded_average(sum, 1.0F / 16.0F) + residual));
}

__global__ void restore(const std::int8_t* source, const float* table, float* output, int height, int width) {
    const int plane = height * width;
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index >= 3 * 16 * plane) return;
    const int rgb = index / (16 * plane);
    const int y = index % (16 * plane) / (width * 4);
    const int x = index % (width * 4);
    float accumulated = 0;
    for (int rotation = 0; rotation < 4; ++rotation) {
        // Rotate an output coordinate into each branch before pixel shuffle.
        const int rotated_width = rotation % 2 ? height : width;
        const int rotated_height = rotation % 2 ? width : height;
        const auto position = unrotate(y, x, rotated_height * 4, rotated_width * 4, (4 - rotation) % 4);
        const int channel = position.y % 4 * 4 + position.x % 4;
        const int pixel = position.y / 4 * rotated_width + position.x / 4;
        const int batch = rotation * 3 + rgb;
        float sum = 0;
        for (int in = 0; in < 16; ++in) {
            const int value = static_cast<int>(source[(batch * 16 + in) * plane + pixel]) + 32;
            sum = __fadd_rn(sum, lookup(table, channel, in, 64, value, 16));
        }
        const float restored = rounded_average(sum, 1.0F / 16.0F) + source[(batch * 16 + channel) * plane + pixel];
        accumulated += fminf(127.0F, fmaxf(-128.0F, restored));
    }
    output[index] = fminf(255.0F, fmaxf(0.0F, accumulated + 128.0F));
}

}  // namespace

cudaError_t enqueue(const float* input, const float* tables, std::int8_t* first, std::int8_t* second,
             float* output, std::uint32_t height, std::uint32_t width, cudaStream_t stream, std::int8_t* decisions) {
    const auto blocks = (12U * 16U * height * width + 255U) / 256U;
    const std::size_t decision_elements = 12U * 16U * height * width;
    initial_depthwise<<<blocks, 256, 0, stream>>>(input, tables, first, height, width);
    for (std::size_t stage = 0; stage < kStages; ++stage) {
        const auto* table = tables + kLowElements + stage * (kDepthwiseElements + kPointwiseElements);
        if (stage != 0) depthwise<<<blocks, 256, 0, stream>>>(second, table, first, height, width);
        if (decisions != nullptr) record_shifted_decisions<<<blocks, 256, 0, stream>>>(
            first, tables + kShiftOffset + stage * 32, decisions + stage * 2 * decision_elements, height, width);
        shifted_pointwise<<<blocks, 256, 0, stream>>>(first, table + kDepthwiseElements,
            tables + kShiftOffset + stage * 32, second, height, width);
        if (decisions != nullptr) {
            const auto copied = cudaMemcpyAsync(decisions + (stage * 2 + 1) * decision_elements,
                second, decision_elements * sizeof(std::int8_t), cudaMemcpyDeviceToDevice, stream);
            if (copied != cudaSuccess) return copied;
        }
    }
    restore<<<(3U * 16U * height * width + 255U) / 256U, 256, 0, stream>>>(
        second, tables + kUpOffset, output, height, width);
    return cudaPeekAtLastError();
}

}  // namespace mmltk::backend::imaging::upscale::shiftlut
