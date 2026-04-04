#include "cuda_test_utils.h"
#include "rfdetr/cuda_utils.h"

#include "support/catch2_compat.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using mmltk::rfdetr::launch_bgr_split_to_planar_float;
using mmltk::rfdetr::launch_bgr_vertical_flip_in_place_pitched;

bool has_cuda_device() {
    int device_count = 0;
    return cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0;
}

bool nearly_equal(float a, float b, float epsilon = 1.0e-6f) {
    return std::fabs(a - b) <= epsilon;
}

void test_vertical_flip_in_place_reverses_rows() {
    constexpr std::uint32_t width = 2;
    constexpr std::uint32_t height = 3;
    const std::vector<std::uint8_t> input = {
        1, 2, 3, 4, 5, 6,
        11, 12, 13, 14, 15, 16,
        21, 22, 23, 24, 25, 26,
    };
    const std::vector<std::uint8_t> expected = {
        21, 22, 23, 24, 25, 26,
        11, 12, 13, 14, 15, 16,
        1, 2, 3, 4, 5, 6,
    };

    CUDA_ASSERT_OK(cudaSetDevice(0));
    cudaStream_t stream = nullptr;
    CUDA_ASSERT_OK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

    std::uint8_t* device = nullptr;
    std::size_t pitch_bytes = 0;
    CUDA_ASSERT_OK(cudaMallocPitch(reinterpret_cast<void**>(&device),
                                   &pitch_bytes,
                                   static_cast<std::size_t>(width) * 3U,
                                   height));
    CUDA_ASSERT_OK(cudaMemcpy2DAsync(device,
                                     pitch_bytes,
                                     input.data(),
                                     static_cast<std::size_t>(width) * 3U,
                                     static_cast<std::size_t>(width) * 3U,
                                     height,
                                     cudaMemcpyHostToDevice,
                                     stream));
    CUDA_ASSERT_OK(launch_bgr_vertical_flip_in_place_pitched(device, pitch_bytes, width, height, stream));
    CUDA_ASSERT_OK(cudaStreamSynchronize(stream));

    std::vector<std::uint8_t> output(expected.size(), 0U);
    CUDA_ASSERT_OK(cudaMemcpy2D(output.data(),
                                static_cast<std::size_t>(width) * 3U,
                                device,
                                pitch_bytes,
                                static_cast<std::size_t>(width) * 3U,
                                height,
                                cudaMemcpyDeviceToHost));

    assert(output == expected);

    CUDA_ASSERT_OK(cudaFree(device));
    CUDA_ASSERT_OK(cudaStreamDestroy(stream));
}

void test_split_to_planar_preserves_bottom_row_orientation() {
    constexpr std::uint32_t width = 1;
    constexpr std::uint32_t height = 2;
    const std::vector<std::uint8_t> input = {
        10, 20, 30,
        200, 150, 100,
    };

    CUDA_ASSERT_OK(cudaSetDevice(0));
    cudaStream_t stream = nullptr;
    CUDA_ASSERT_OK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

    std::uint8_t* src_device = nullptr;
    std::size_t src_pitch_bytes = 0;
    CUDA_ASSERT_OK(cudaMallocPitch(reinterpret_cast<void**>(&src_device),
                                   &src_pitch_bytes,
                                   static_cast<std::size_t>(width) * 3U,
                                   height));
    CUDA_ASSERT_OK(cudaMemcpy2DAsync(src_device,
                                     src_pitch_bytes,
                                     input.data(),
                                     static_cast<std::size_t>(width) * 3U,
                                     static_cast<std::size_t>(width) * 3U,
                                     height,
                                     cudaMemcpyHostToDevice,
                                     stream));

    float* dst_device = nullptr;
    CUDA_ASSERT_OK(cudaMalloc(reinterpret_cast<void**>(&dst_device),
                              static_cast<std::size_t>(3U * width * height) * sizeof(float)));
    CUDA_ASSERT_OK(launch_bgr_split_to_planar_float(src_device,
                                                    src_pitch_bytes,
                                                    width,
                                                    height,
                                                    dst_device,
                                                    width,
                                                    height,
                                                    stream));
    CUDA_ASSERT_OK(cudaStreamSynchronize(stream));

    std::vector<float> output(static_cast<std::size_t>(3U) * width * height, 0.0f);
    CUDA_ASSERT_OK(cudaMemcpy(output.data(),
                              dst_device,
                              output.size() * sizeof(float),
                              cudaMemcpyDeviceToHost));

    constexpr float kInv255 = 1.0f / 255.0f;
    assert(nearly_equal(output[1], 100.0f * kInv255));
    assert(nearly_equal(output[3], 150.0f * kInv255));
    assert(nearly_equal(output[5], 200.0f * kInv255));

    CUDA_ASSERT_OK(cudaFree(dst_device));
    CUDA_ASSERT_OK(cudaFree(src_device));
    CUDA_ASSERT_OK(cudaStreamDestroy(stream));
}

} // namespace

void test_live_preprocess_cuda_smoke() {
    if (!has_cuda_device()) {
        SKIP("no CUDA device available");
    }

    test_vertical_flip_in_place_reverses_rows();
    test_split_to_planar_preserves_bottom_row_orientation();
}

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][live_preprocess][cuda]", test_live_preprocess_cuda_smoke);
