#include "src/backend/imaging/raster/caption_raster.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <vector>
TEST_CASE("bundled named captions retain Unicode glyphs painter order and clipped image edges", "[raster][cuda][caption]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace raster = mmltk::backend::imaging::raster;
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 const auto execution = gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture());
 gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
 gpu::SystemImageRuntime runtime({.device = 0, .numa_node = execution.placement.numa_node, .execution = execution, .adopted_context = context});
 raster::CaptionRaster captions(context);
 const std::array<std::string, 2> names{"café", "détection"};
 captions.Prepare(names);
 const auto draw = [&](std::span<const raster::NamedCaption> items) {
  auto candidate = runtime.AcquireOutput();
  runtime.PublishRetained(candidate, 64U, 32U, [&](auto clean, auto, auto stream) {
   REQUIRE(cudaMemset2DAsync(reinterpret_cast<void*>(clean.data), clean.descriptor.pitch_bytes, 0, clean.descriptor.row_bytes(), clean.descriptor.height, reinterpret_cast<cudaStream_t>(stream)) ==
           cudaSuccess);
   captions.Draw(clean, items, stream);
  });
  static_cast<void>(runtime.CommitOutput(std::move(candidate)));
  auto image = runtime.Borrow();
  auto plane = image.plane(0).plane();
  context.Bind();
  std::vector<std::uint8_t> pixels(64U * 32U * 4U);
  REQUIRE(cudaMemcpy2D(pixels.data(), 64U * 4U, reinterpret_cast<void*>(plane.data), plane.descriptor.pitch_bytes, 64U * 4U, 32U, cudaMemcpyDeviceToHost) == cudaSuccess);
  return pixels;
 };
 const std::array first{raster::NamedCaption{0U, 0, 0, {255U, 255U, 255U}}};
 const auto white = draw(first);
 CHECK(white[0] == 255U);
 std::size_t glyph_pixels = 0;
 for (std::size_t index = 0; index < 64U * 22U; ++index)
  if (white[index * 4U + 3U] && white[index * 4U] < 255U) ++glyph_pixels;
 CHECK(glyph_pixels > 20U);
 captions.Prepare(names);
 CHECK(draw(first) == white);
 const std::array ordered{first[0], raster::NamedCaption{1U, 0, 0, {0U, 0U, 0U}}};
 const auto dark = draw(ordered);
 CHECK(dark[0] == 0U);
 CHECK(dark != white);
 const std::array clipped{raster::NamedCaption{1U, 60, 28, {255U, 0U, 0U}}};
 const auto edge = draw(clipped);
 CHECK(edge[(28U * 64U + 59U) * 4U + 3U] == 0U);
 CHECK(edge[(28U * 64U + 60U) * 4U] == 255U);
 CHECK(edge.back() == 255U);
 // Prepare uses its upload stream; draw immediately consumes the catalog on
 // SystemImageRuntime's independent stream, including catalog replacement.
 captions.Prepare(std::array<std::string, 2>{"WWWW", "éééé"});
 CHECK(draw(first) != white);
 captions.Prepare(names);
 CHECK(draw(first) == white);
 std::vector<raster::NamedCaption> dense(8192U, first[0]);
 dense.back() = ordered.back();
 const std::array last{dense.back()};
 CHECK(draw(dense) == draw(last));
 dense.push_back(first[0]);
 CHECK_THROWS(draw(dense));
 CHECK_THROWS(captions.Prepare(std::array<std::string, 1>{std::string(257, 'x')}));
 CHECK_THROWS(captions.Prepare(std::array<std::string, 1>{"\xF8\x90\x80\x80"}));
 captions.Prepare(names);
 CHECK(draw(first) == white);
 auto numeric = first;
 numeric[0].suffix = " 0.25";
 const auto confidence = draw(numeric);
 numeric[0].suffix = " 0.75";
 CHECK(draw(numeric) != confidence);
 captions.Prepare(names);
 numeric[0].suffix = " 0.25";
 CHECK(draw(numeric) == confidence);
}
