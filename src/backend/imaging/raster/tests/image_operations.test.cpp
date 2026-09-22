#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <array>
#include <limits>
#include <cstdint>
#include <vector>
#include "src/test_support/cuda_test_utils.hpp"
#include "src/backend/imaging/raster/image_operations.h"
#include "src/backend/imaging/raster/chw_image.h"
import mmltk.backend.imaging.raster;
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
 CUDA_ASSERT_OK(cudaMemcpy2DAsync(image.device, image.pitch_bytes, host.data(), static_cast<std::size_t>(width) * 3U, static_cast<std::size_t>(width) * 3U, height, cudaMemcpyHostToDevice, stream));
 return image;
}
void test_vertical_flip_in_place_reverses_rows() {
 constexpr std::uint32_t width = 2;
 constexpr std::uint32_t height = 3;
 const std::vector<std::uint8_t> input = {
  1,
  2,
  3,
  4,
  5,
  6,
  11,
  12,
  13,
  14,
  15,
  16,
  21,
  22,
  23,
  24,
  25,
  26,
 };
 const std::vector<std::uint8_t> expected = {
  21,
  22,
  23,
  24,
  25,
  26,
  11,
  12,
  13,
  14,
  15,
  16,
  1,
  2,
  3,
  4,
  5,
  6,
 };
 cudaStream_t stream = create_nonblocking_stream_on_device_zero();
 const PitchedDeviceImage image = upload_pitched_bgr(input, width, height, stream);
 CUDA_ASSERT_OK(static_cast<cudaError_t>(launch_bgr_vertical_flip_in_place_pitched(image.device, image.pitch_bytes, width, height, reinterpret_cast<std::uintptr_t>(stream))));
 CUDA_ASSERT_OK(cudaStreamSynchronize(stream));
 std::vector<std::uint8_t> output(expected.size(), 0U);
 CUDA_ASSERT_OK(cudaMemcpy2D(output.data(), static_cast<std::size_t>(width) * 3U, image.device, image.pitch_bytes, static_cast<std::size_t>(width) * 3U, height, cudaMemcpyDeviceToHost));
 CHECK(output == expected);
 CUDA_ASSERT_OK(cudaFree(image.device));
 CUDA_ASSERT_OK(cudaStreamDestroy(stream));
}
void test_split_to_planar_preserves_bottom_row_orientation() {
 constexpr std::uint32_t width = 1;
 constexpr std::uint32_t height = 2;
 const std::vector<std::uint8_t> input = {
  10,
  20,
  30,
  200,
  150,
  100,
 };
 cudaStream_t stream = create_nonblocking_stream_on_device_zero();
 const PitchedDeviceImage source = upload_pitched_bgr(input, width, height, stream);
 float* dst_device = nullptr;
 CUDA_ASSERT_OK(cudaMalloc(reinterpret_cast<void**>(&dst_device), static_cast<std::size_t>(3U * width * height) * sizeof(float)));
 CUDA_ASSERT_OK(static_cast<cudaError_t>(launch_bgr_split_to_planar_float(source.device, source.pitch_bytes, width, height, dst_device, width, height, reinterpret_cast<std::uintptr_t>(stream))));
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
TEST_CASE("Flat mask runs match independent pixel membership through clipped rows and guards", "[backend][cuda][raster]") {
 namespace raster = mmltk::backend::imaging::raster;
 if (!has_cuda_device()) SKIP("no CUDA device available");
 auto stream = create_nonblocking_stream_on_device_zero();
 cudaDeviceProp properties{};
 CUDA_ASSERT_OK(cudaGetDeviceProperties(&properties, 0));
 INFO(properties.name << " CC " << properties.major << '.' << properties.minor);
 constexpr std::size_t guard = 19U;
 constexpr std::uint8_t untouched = 0xA7;
 const std::array<std::uint8_t, 4U> color{17U, 103U, 211U, 92U};
 std::uint32_t* pairs = nullptr;
 CUDA_ASSERT_OK(cudaMalloc(reinterpret_cast<void**>(&pairs), 12U * sizeof(std::uint32_t)));
 const std::array<std::array<int, 2U>, 7U> extents{{{13, 9}, {1, 1}, {1, 1024}, {3, 1024}, {255, 9}, {256, 9}, {257, 9}}};
 for (const auto [width, height] : extents) {
  CAPTURE(width, height);
  const std::size_t pitch = static_cast<std::size_t>(width) * 4U + 7U;
  std::uint8_t* storage = nullptr;
  CUDA_ASSERT_OK(cudaMalloc(reinterpret_cast<void**>(&storage), guard * 2U + pitch * height));
  const auto row_width = static_cast<std::uint32_t>(width);
  const auto pixel_count = row_width * static_cast<std::uint32_t>(height);
  const std::array<std::vector<std::uint32_t>, 14U> cases{
   {{}, {0U, 0U}, {11U, 32U}, {0U, std::numeric_limits<std::uint32_t>::max()}, {116U, 6U, 117U, 9U, std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()},
    {2U, 4U, 12U, 29U, 63U, 21U, 101U, 2U}, {0U, pixel_count}, {row_width / 2U, 2U * row_width}, {row_width - 1U, row_width + 2U}, {row_width - 1U, 1U}, {row_width, 1U}, {row_width, 2U * row_width},
    {pixel_count - 1U, std::numeric_limits<std::uint32_t>::max()}, {0U, row_width, row_width + 1U, row_width, 3U * row_width, 2U * row_width, pixel_count, 1U}}};
  const std::array<raster::IntRect, 14U> clips{{{0, 0, width, height}, {5, 3, 6, 4}, {4, 1, 9, 5}, {2, 2, 2, 7}, {1, 5, 8, 5}, {-7, -9, 4, 2}, {width, height, 20, 20},
   {width / 2, 0, width / 2 + 1, height}, {1, 0, width - 1, height}, {0, 1, width, height - 1}, {0, 0, 0, height}, {-4, -3, -1, height}, {0, height + 1, width, height + 3}, {0, -4, width, -1}}};
  for (const auto& runs : cases)
   for (const auto clip : clips) {
    CAPTURE(runs, clip.x1, clip.y1, clip.x2, clip.y2);
    std::vector<std::uint8_t> expected(guard * 2U + pitch * height, untouched);
    // Evaluate each destination pixel independently: no row-span clipping,
    // shared helper, or output from the device contributes to this oracle.
    for (int y = 0; y < height; ++y)
     for (int x = 0; x < width; ++x) {
      if (x < clip.x1 || x >= clip.x2 || y < clip.y1 || y >= clip.y2) continue;
      const auto pixel = static_cast<std::uint64_t>(y * width + x);
      bool covered = false;
      for (std::size_t run = 0; run < runs.size(); run += 2U) covered |= pixel >= runs[run] && pixel - runs[run] < runs[run + 1U];
      if (covered)
       for (std::size_t channel = 0; channel != color.size(); ++channel) expected[guard + y * pitch + x * 4U + channel] = color[channel];
     }
    CUDA_ASSERT_OK(cudaMemsetAsync(storage, untouched, expected.size(), stream));
    if (!runs.empty()) CUDA_ASSERT_OK(cudaMemcpyAsync(pairs, runs.data(), runs.size() * sizeof(std::uint32_t), cudaMemcpyHostToDevice, stream));
    CHECK(raster::raster_mask_runs_rgba({.overlay = {storage + guard, pitch, width, height},
           .run_pairs = runs.empty() ? nullptr : pairs,
           .run_count = static_cast<std::uint32_t>(runs.size() / 2U),
           .color = {color[0], color[1], color[2], color[3]},
           .stream = {reinterpret_cast<void*>(stream)},
           .clip = clip}) == (runs.empty() ? cudaErrorInvalidValue : cudaSuccess));
    CUDA_ASSERT_OK(cudaStreamSynchronize(stream));
    std::vector<std::uint8_t> actual(expected.size());
    CUDA_ASSERT_OK(cudaMemcpy(actual.data(), storage, actual.size(), cudaMemcpyDeviceToHost));
    CHECK(actual == expected);
   }
  CUDA_ASSERT_OK(cudaFree(storage));
 }
 CUDA_ASSERT_OK(cudaFree(pairs));
 CUDA_ASSERT_OK(cudaStreamDestroy(stream));
}

TEST_CASE("RGB byte and CHW conversion preserve every byte with odd extents and padded pitch", "[backend][cuda][raster]") {
 namespace raster = mmltk::backend::imaging::raster;
 if (!has_cuda_device()) SKIP("no CUDA device available");
 auto stream = create_nonblocking_stream_on_device_zero();
 constexpr std::uint32_t width = 257U, height = 3U;
 constexpr std::size_t count = width * height, pitch = width * 4U + 13U, guard = 7U;
 std::vector<std::uint8_t> rgb(count * 3U), expected(guard * 2U + pitch * height, 0xA7U);
 std::vector<float> chw(count * 3U);
 for (std::size_t pixel = 0U; pixel < count; ++pixel) {
  const auto output = guard + pixel / width * pitch + pixel % width * 4U;
  for (std::size_t channel = 0U; channel < 3U; ++channel) {
   const auto value = static_cast<std::uint8_t>(pixel + channel * 79U);
   rgb[pixel * 3U + channel] = value;
   chw[channel * count + pixel] = static_cast<float>(value) * (1.0F / 255.0F);
   expected[output + channel] = value;
  }
  expected[output + 3U] = 255U;
 }
 void* source = nullptr;
 std::uint8_t* destination = nullptr;
 CUDA_ASSERT_OK(cudaMalloc(&source, chw.size() * sizeof(float)));
 CUDA_ASSERT_OK(cudaMalloc(reinterpret_cast<void**>(&destination), expected.size()));
 for (bool bytes : {false, true}) {
  CUDA_ASSERT_OK(cudaMemcpyAsync(source, bytes ? static_cast<const void*>(rgb.data()) : chw.data(), bytes ? rgb.size() : chw.size() * sizeof(float), cudaMemcpyHostToDevice, stream));
  CUDA_ASSERT_OK(cudaMemsetAsync(destination, 0xA7, expected.size(), stream));
  const auto convert = [&](std::uint32_t w, std::uint32_t h, std::size_t row) {
   return bytes ? raster::rgb8_to_rgba(static_cast<const std::uint8_t*>(source), w, h, destination + guard, row, stream)
                : raster::chw_float_to_rgba(static_cast<const float*>(source), w, h, destination + guard, row, stream);
  };
  CHECK(convert(0U, height, pitch) == cudaErrorInvalidValue);
  CHECK(convert(width, 0U, pitch) == cudaErrorInvalidValue);
  CHECK(convert(width, height, width * 4U - 1U) == cudaErrorInvalidValue);
  CUDA_ASSERT_OK(static_cast<cudaError_t>(convert(width, height, pitch)));
  CUDA_ASSERT_OK(cudaStreamSynchronize(stream));
  std::vector<std::uint8_t> actual(expected.size());
  CUDA_ASSERT_OK(cudaMemcpy(actual.data(), destination, actual.size(), cudaMemcpyDeviceToHost));
  CHECK(actual == expected);
 }
 CHECK(raster::rgb8_to_rgba(nullptr, width, height, destination, pitch, stream) == cudaErrorInvalidValue);
 CHECK(raster::rgb8_to_rgba(static_cast<const std::uint8_t*>(source), width, height, nullptr, pitch, stream) == cudaErrorInvalidValue);
 CUDA_ASSERT_OK(cudaFree(destination));
 CUDA_ASSERT_OK(cudaFree(source));
 CUDA_ASSERT_OK(cudaStreamDestroy(stream));
}
