#include "nis_sharpen_reference.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <vector>
namespace {
struct DeviceRelease final {
 void operator()(void* data) const noexcept {
  if (cudaFree(data) != cudaSuccess) std::terminate();
 }
};
using DeviceStorage = std::unique_ptr<void, DeviceRelease>;
DeviceStorage allocate(std::size_t bytes) {
 void* pointer = nullptr;
 REQUIRE(cudaMalloc(&pointer, bytes) == cudaSuccess);
 return DeviceStorage{pointer};
}
}  // namespace
TEST_CASE("Basic selected sharpening matches independent dual-direction arithmetic and guards", "[upscale_gpu]") {
 namespace nis = mmltk::backend::imaging::upscale::image_upscaler_nis;
 namespace reference = mmltk::backend::imaging::upscale::tests;
 if (mmltk::testsupport::checked_cuda_device_count() == 0) SKIP("CUDA device unavailable");
 const auto device = mmltk::testsupport::select_cuda_test_device(0);
 INFO(device);
 const auto output_width = GENERATE(1U, 3U, 19U, 257U);
 const auto output_height = GENERATE(1U, 5U);
 const bool cropped = GENERATE(false, true);
 // The final pattern feeds both sharpeners identical unchanged scale output.
 const auto pattern = GENERATE(0U, 1U, 2U, 3U, 4U, 5U);
 const nis::Configuration config{
  .source_width = output_width + (cropped ? 3U : 0U),
  .source_height = output_height + (cropped ? 2U : 0U),
  .crop_x = cropped ? 1U : 0U,
  .crop_y = cropped ? 1U : 0U,
  .crop_width = (output_width + 3U) / 4U,
  .crop_height = (output_height + 3U) / 4U,
  .output_width = output_width,
  .output_height = output_height,
 };
 const std::size_t source_pitch = config.source_width * 4U + 37U;
 const std::size_t target_pitch = output_width * 4U + 23U;
 std::vector<std::uint8_t> source(source_pitch * config.source_height + 31U, 0xD7);
 for (std::uint32_t y = 0U; y < config.source_height; ++y)
  for (std::uint32_t x = 0U; x < config.source_width; ++x)
   for (std::uint32_t c = 0U; c < 4U; ++c) source[y * source_pitch + x * 4U + c] = static_cast<std::uint8_t>((x * 71U + y * 13U + c * 43U) % 256U);
 const auto scratch = nis::scratch_requirements(config);
 REQUIRE(scratch.has_value());
 REQUIRE(scratch->scaled_bytes == static_cast<std::size_t>(output_width) * output_height * reference::scaled_pixel_bytes());
 auto input = allocate(source.size());
 auto horizontal = allocate(scratch->horizontal_bytes);
 auto scaled = allocate(scratch->scaled_bytes);
 const std::size_t bytes = target_pitch * output_height + 61U;
 auto actual_device = allocate(bytes);
 auto expected_device = allocate(bytes);
 mmltk::testsupport::ScopedTestStream stream;
 REQUIRE(cudaMemcpyAsync(input.get(), source.data(), source.size(), cudaMemcpyHostToDevice, stream.get()) == cudaSuccess);
 REQUIRE(cudaMemsetAsync(actual_device.get(), 0xA9, bytes, stream.get()) == cudaSuccess);
 REQUIRE(cudaMemsetAsync(expected_device.get(), 0xA9, bytes, stream.get()) == cudaSuccess);
 if (pattern == 5U)
  REQUIRE(nis::launch_scale(input.get(), source_pitch, horizontal.get(), scaled.get(), config, stream.get()) == cudaSuccess);
 else
  REQUIRE(reference::prepare_sharpen_pixels(scaled.get(), config, pattern, stream.get()) == cudaSuccess);
 REQUIRE(nis::launch_sharpen(input.get(), source_pitch, scaled.get(), static_cast<std::uint8_t*>(actual_device.get()), target_pitch, config, stream.get()) == cudaSuccess);
 REQUIRE(reference::sharpen_reference(input.get(), source_pitch, scaled.get(), static_cast<std::uint8_t*>(expected_device.get()), target_pitch, config, stream.get()) == cudaSuccess);
 REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
 std::vector<std::uint8_t> actual(bytes), expected(bytes), unchanged(source.size());
 REQUIRE(cudaMemcpy(actual.data(), actual_device.get(), bytes, cudaMemcpyDeviceToHost) == cudaSuccess);
 REQUIRE(cudaMemcpy(expected.data(), expected_device.get(), bytes, cudaMemcpyDeviceToHost) == cudaSuccess);
 REQUIRE(cudaMemcpy(unchanged.data(), input.get(), source.size(), cudaMemcpyDeviceToHost) == cudaSuccess);
 CHECK(actual == expected);
 CHECK(unchanged == source);
 for (std::uint32_t y = 0U; y < output_height; ++y) {
  for (std::uint32_t x = 0U; x < output_width; ++x) {
   const auto sx = config.crop_x + (static_cast<std::uint64_t>(x) * config.crop_width + config.crop_width / 2U) / output_width;
   const auto sy = config.crop_y + (static_cast<std::uint64_t>(y) * config.crop_height + config.crop_height / 2U) / output_height;
   CHECK(actual[y * target_pitch + x * 4U + 3U] == source[sy * source_pitch + sx * 4U + 3U]);
  }
  for (std::size_t byte = output_width * 4U; byte < target_pitch; ++byte) REQUIRE(actual[y * target_pitch + byte] == 0xA9);
 }
 for (std::size_t byte = target_pitch * output_height; byte < bytes; ++byte) REQUIRE(actual[byte] == 0xA9);
}
