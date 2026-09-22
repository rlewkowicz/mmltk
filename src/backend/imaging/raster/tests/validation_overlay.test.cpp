#include <catch2/catch_test_macros.hpp>
#include <array>
#include <algorithm>
#include <string_view>
#include <cstdint>
#include <cstddef>
#include <memory>
#include "src/test_support/cuda_test_utils.hpp"
#include "src/backend/imaging/raster/image_containment.h"
import mmltk.backend.imaging.raster;
namespace raster = mmltk::backend::imaging::raster;
TEST_CASE("rectangular atlas containment preserves aspect and square containment", "[raster][atlas]") {
 CHECK(raster::contain_image(400U, 200U, 256U, 192U) == raster::ImageContainRect{0U, 32U, 256U, 128U});
 CHECK(raster::contain_image(200U, 400U, 256U, 192U) == raster::ImageContainRect{80U, 0U, 96U, 192U});
 CHECK(raster::contain_image(400U, 200U, 256U, 256U) == raster::ImageContainRect{0U, 64U, 256U, 128U});
 CHECK(raster::contain_image(0U, 200U, 256U, 192U) == raster::ImageContainRect{});
}
TEST_CASE("validation adds completed layers once with unchanged alpha and ordinary overlay defaults", "[raster][cuda][validation]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 mmltk::testsupport::ScopedTestStream stream;
 // Two predictions overlap. The second wins within the prediction layer.
 // Four pixels exercise complement, saturating arbitrary sum, GT only, Det only.
 struct Input {
  std::array<std::uint8_t, 16> pixels{245, 235, 225, 96, 250, 40, 10, 177, 3, 4, 5, 81, 0, 0, 0, 0};
  std::array<float, 8> boxes{};
  std::array<std::uint8_t, 6> colors{70, 80, 90, 10, 20, 30};
  std::array<int, 2> labels{0, 1};
  std::array<bool, 8> masks{true, true, false, true, true, true, false, true};
  std::int64_t count = 2;
 } input;
 Input* device = nullptr;
 REQUIRE(cudaMalloc(reinterpret_cast<void**>(&device), sizeof(Input)) == cudaSuccess);
 auto release = [](Input* value) { static_cast<void>(cudaFree(value)); };
 std::unique_ptr<Input, decltype(release)> storage(device, release);
 const auto draw = [&](bool add, bool device_count = false) {
  REQUIRE(cudaMemcpyAsync(device, &input, sizeof(input), cudaMemcpyHostToDevice, stream.get()) == cudaSuccess);
  REQUIRE(raster::raster_instance_overlay_rgba({.overlay = {reinterpret_cast<std::uint8_t*>(device), 16U, 4, 1},
           .instances = {reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, boxes)), reinterpret_cast<const std::uint8_t*>(device) + offsetof(Input, colors),
            reinterpret_cast<const int*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, labels)), 2,
            device_count ? reinterpret_cast<const std::int64_t*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, count)) : nullptr},
           .masks = reinterpret_cast<const bool*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, masks)),
           .mask_alpha = 96U,
           .box_thickness = 0,
           .stream = {stream.get()},
           .labels = false,
           .add_rgb_to_existing = add}) == cudaSuccess);
  std::array<std::uint8_t, 16> output{};
  REQUIRE(cudaMemcpyAsync(output.data(), device, output.size(), cudaMemcpyDeviceToHost, stream.get()) == cudaSuccess);
  REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
  return output;
 };
 CHECK(raster::raster_instance_overlay_rgba({}) == cudaSuccess);  // Empty static count preserves its no-op contract.
 CHECK(draw(true) == std::array<std::uint8_t, 16>{255, 255, 255, 96, 255, 60, 40, 177, 3, 4, 5, 81, 10, 20, 30, 96});
 CHECK(draw(false) == std::array<std::uint8_t, 16>{10, 20, 30, 96, 10, 20, 30, 96, 0, 0, 0, 0, 10, 20, 30, 96});
 input.count = 1;
 CHECK(draw(false, true) == std::array<std::uint8_t, 16>{70, 80, 90, 96, 70, 80, 90, 96, 0, 0, 0, 0, 70, 80, 90, 96});
 input.count = 0;
 CHECK(draw(false, true) == std::array<std::uint8_t, 16>{});
}
TEST_CASE("overwrite raster agrees with forward host painting for overlaps labels and hidden layers", "[raster][cuda][validation]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 mmltk::testsupport::ScopedTestStream stream;
 constexpr int width = 31, height = 29;
 constexpr std::size_t pitch = width * 4U + 7U, guard = 11U;
 struct Input {
  std::array<std::uint8_t, guard * 2U + pitch * height> pixels;
  std::array<float, 12U> boxes{2, 17, 18, 25, 4, 18, 20, 27, -2, 16, 15, 23};
  std::array<std::uint8_t, 9U> colors{17, 61, 137, 231, 83, 42, 3, 199, 211};
  std::array<int, 3U> labels{0, 1, 10};
  std::array<bool, width * height * 3U> masks{};
  std::int64_t count = 0;
 } input;
 input.pixels.fill(0xA7U);
 for (int instance = 0; instance < 3; ++instance)
  for (int pixel = 0; pixel < width * height; ++pixel) input.masks[instance * width * height + pixel] = (pixel + instance) % 5 != 0;
 Input* device = nullptr;
 REQUIRE(cudaMalloc(reinterpret_cast<void**>(&device), sizeof(Input)) == cudaSuccess);
 auto release = [](Input* value) { static_cast<void>(cudaFree(value)); };
 std::unique_ptr<Input, decltype(release)> storage(device, release);
 // These literal glyphs are host-owned expected shapes, stamped forwards like
 // an ordinary painter; neither CUDA hit helpers nor reverse search are used.
 constexpr std::array<std::array<std::string_view, 7U>, 2U> glyphs{{{".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}, {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}}};
 for (const std::int64_t count : {-3LL, 0LL, 1LL, 2LL, 3LL, 99LL})
  for (bool boxes : {false, true})
   for (bool labels : {false, true})
    for (bool masks : {false, true})
     for (bool add : {false, true}) {
      CAPTURE(count, boxes, labels, masks, add);
      input.count = count;
      auto expected = input.pixels;
      std::array<std::array<std::uint8_t, 4U>, width * height> layer{};
      const int admitted = static_cast<int>(std::clamp<std::int64_t>(count, 0, 3));
      const auto paint = [&](int x, int y, int instance, std::uint8_t alpha) {
       if (x < 0 || x >= width || y < 0 || y >= height) return;
       auto& pixel = layer[y * width + x];
       for (int channel = 0; channel < 3; ++channel) pixel[channel] = input.colors[instance * 3 + channel];
       pixel[3] = alpha;
      };
      if (masks)
       for (int instance = 0; instance < admitted; ++instance)
        for (int pixel = 0; pixel < width * height; ++pixel)
         if (input.masks[instance * width * height + pixel]) paint(pixel % width, pixel / width, instance, 96U);
      for (int instance = 0; instance < admitted; ++instance) {
       const int left = static_cast<int>(input.boxes[instance * 4]), top = static_cast<int>(input.boxes[instance * 4 + 1]);
       const int right = static_cast<int>(input.boxes[instance * 4 + 2]), bottom = static_cast<int>(input.boxes[instance * 4 + 3]);
       if (boxes)
        for (int y = top - 2; y <= bottom + 2; ++y)
         for (int x = left - 2; x <= right + 2; ++x)
          if (!(x >= left && x <= right && y >= top && y <= bottom)) paint(x, y, instance, 255U);
       if (labels) {
        const auto text = instance == 0 ? std::string_view{"0"} : instance == 1 ? std::string_view{"1"} : std::string_view{"10"};
        for (std::size_t digit = 0U; digit < text.size(); ++digit)
         for (int row = 0; row < 7; ++row)
          for (int column = 0; column < 5; ++column)
           if (glyphs[text[digit] - '0'][row][column] == '#')
            for (int dy = 0; dy < 2; ++dy)
             for (int dx = 0; dx < 2; ++dx) paint(left + static_cast<int>(digit) * 12 + column * 2 + dx, top - 16 + row * 2 + dy, instance, 255U);
       }
      }
      for (int y = 0; y < height; ++y)
       for (int x = 0; x < width; ++x) {
        const auto offset = guard + y * pitch + x * 4U;
        const auto pixel = layer[y * width + x];
        for (std::size_t channel = 0U; channel < 4U; ++channel)
         expected[offset + channel] =
          add ? (channel == 3U ? input.pixels[offset + channel] : static_cast<std::uint8_t>(std::min(255, input.pixels[offset + channel] + pixel[channel]))) : pixel[channel];
       }
      REQUIRE(cudaMemcpyAsync(device, &input, sizeof(input), cudaMemcpyHostToDevice, stream.get()) == cudaSuccess);
      const auto base = reinterpret_cast<std::uint8_t*>(device);
      REQUIRE(raster::raster_instance_overlay_rgba({.overlay = {base + guard, pitch, width, height},
               .instances = {reinterpret_cast<const float*>(base + offsetof(Input, boxes)), base + offsetof(Input, colors), reinterpret_cast<const int*>(base + offsetof(Input, labels)), 3,
                reinterpret_cast<const std::int64_t*>(base + offsetof(Input, count))},
               .masks = masks ? reinterpret_cast<const bool*>(base + offsetof(Input, masks)) : nullptr,
               .mask_alpha = 96U,
               .box_thickness = boxes ? 2 : 0,
               .stream = {stream.get()},
               .labels = labels,
               .add_rgb_to_existing = add}) == cudaSuccess);
      decltype(input.pixels) actual{};
      REQUIRE(cudaMemcpyAsync(actual.data(), device, actual.size(), cudaMemcpyDeviceToHost, stream.get()) == cudaSuccess);
      REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
      CHECK(actual == expected);
     }
}
