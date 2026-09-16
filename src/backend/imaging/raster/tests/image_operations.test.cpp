#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>
#include "src/test_support/cuda_test_utils.hpp"
#include "src/backend/imaging/raster/image_operations.h"
namespace {
using mmltk::backend::imaging::raster::launch_bgr_split_to_planar_float;
using mmltk::backend::imaging::raster::launch_bgr_vertical_flip_in_place_pitched;
bool has_cuda_device() {
    int device_count = 0;
    return cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0;
}
bool nearly_equal(float a, float b, float epsilon = 1.0e-6f) { return std::fabs(a - b) <= epsilon; }
cudaStream_t create_nonblocking_stream_on_device_zero() {
    CUDA_ASSERT_OK(cudaSetDevice(0));
    cudaStream_t stream = nullptr;
    CUDA_ASSERT_OK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    return stream;
}
struct PitchedDeviceImage {
    std::uint8_t* device = nullptr;
    std::size_t pitch_bytes = 0;
};
// Allocates a pitched BGR image on the device and uploads `host` (width * 3 bytes per row) into it.
PitchedDeviceImage upload_pitched_bgr(const std::vector<std::uint8_t>& host, const std::uint32_t width, const std::uint32_t height, const cudaStream_t stream) {
    PitchedDeviceImage image;
    CUDA_ASSERT_OK(cudaMallocPitch(reinterpret_cast<void**>(&image.device), &image.pitch_bytes, static_cast<std::size_t>(width) * 3U, height));
    CUDA_ASSERT_OK(cudaMemcpy2DAsync(image.device, image.pitch_bytes, host.data(), static_cast<std::size_t>(width) * 3U, static_cast<std::size_t>(width) * 3U,
                                     height, cudaMemcpyHostToDevice, stream));
    return image;
}
void test_vertical_flip_in_place_reverses_rows() {
    constexpr std::uint32_t width = 2;
    constexpr std::uint32_t height = 3;
    const std::vector<std::uint8_t> input = {
        1, 2, 3, 4, 5, 6, 11, 12, 13, 14, 15, 16, 21, 22, 23, 24, 25, 26,
    };
    const std::vector<std::uint8_t> expected = {
        21, 22, 23, 24, 25, 26, 11, 12, 13, 14, 15, 16, 1, 2, 3, 4, 5, 6,
    };
    cudaStream_t stream = create_nonblocking_stream_on_device_zero();
    const PitchedDeviceImage image = upload_pitched_bgr(input, width, height, stream);
    CUDA_ASSERT_OK(static_cast<cudaError_t>(
        launch_bgr_vertical_flip_in_place_pitched(image.device, image.pitch_bytes, width, height, reinterpret_cast<std::uintptr_t>(stream))));
    CUDA_ASSERT_OK(cudaStreamSynchronize(stream));
    std::vector<std::uint8_t> output(expected.size(), 0U);
    CUDA_ASSERT_OK(cudaMemcpy2D(output.data(), static_cast<std::size_t>(width) * 3U, image.device, image.pitch_bytes, static_cast<std::size_t>(width) * 3U,
                                height, cudaMemcpyDeviceToHost));
    CHECK(output == expected);
    CUDA_ASSERT_OK(cudaFree(image.device));
    CUDA_ASSERT_OK(cudaStreamDestroy(stream));
}
void test_split_to_planar_preserves_bottom_row_orientation() {
    constexpr std::uint32_t width = 1;
    constexpr std::uint32_t height = 2;
    const std::vector<std::uint8_t> input = {
        10, 20, 30, 200, 150, 100,
    };
    cudaStream_t stream = create_nonblocking_stream_on_device_zero();
    const PitchedDeviceImage source = upload_pitched_bgr(input, width, height, stream);
    float* dst_device = nullptr;
    CUDA_ASSERT_OK(cudaMalloc(reinterpret_cast<void**>(&dst_device), static_cast<std::size_t>(3U * width * height) * sizeof(float)));
    CUDA_ASSERT_OK(static_cast<cudaError_t>(launch_bgr_split_to_planar_float(source.device, source.pitch_bytes, width, height, dst_device, width, height,
                                                                             reinterpret_cast<std::uintptr_t>(stream))));
    CUDA_ASSERT_OK(cudaStreamSynchronize(stream));
    std::vector<float> output(static_cast<std::size_t>(3U) * width * height, 0.0f);
    CUDA_ASSERT_OK(cudaMemcpy(output.data(), dst_device, output.size() * sizeof(float), cudaMemcpyDeviceToHost));
    constexpr float kInv255 = 1.0f / 255.0f;
    CHECK(nearly_equal(output[1], 100.0f * kInv255));
    CHECK(nearly_equal(output[3], 150.0f * kInv255));
    CHECK(nearly_equal(output[5], 200.0f * kInv255));
    CUDA_ASSERT_OK(cudaFree(dst_device));
    CUDA_ASSERT_OK(cudaFree(source.device));
    CUDA_ASSERT_OK(cudaStreamDestroy(stream));
}
}  // namespace
TEST_CASE("CUDA image utilities preserve orientation and planar layout", "[backend][ml][cuda]") {
    if (!has_cuda_device()) { SKIP("no CUDA device available"); }
    test_vertical_flip_in_place_reverses_rows();
    test_split_to_planar_preserves_bottom_row_orientation();
}
