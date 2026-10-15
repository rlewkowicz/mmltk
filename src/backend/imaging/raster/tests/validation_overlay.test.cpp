#include <catch2/catch_test_macros.hpp>
#include <array>
#include <algorithm>
#include <string_view>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <memory>
#include "src/test_support/cuda_test_utils.hpp"
#include "src/backend/imaging/raster/image_containment.h"
#include "src/backend/imaging/raster/skeleton_projection.h"
#include <limits>
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
  std::array<float, 2> confidences{0.437F, 0.4F};
  std::int64_t count = 2;
 } input;
 auto storage = mmltk::testsupport::allocate_cuda_test_object<Input>();
 auto* device = storage.get();
 const auto draw = [&](bool add, bool device_count = false, const float* threshold = nullptr) {
  REQUIRE(cudaMemcpyAsync(device, &input, sizeof(input), cudaMemcpyHostToDevice, stream.get()) == cudaSuccess);
  REQUIRE(raster::raster_instance_overlay_rgba(
           {.overlay = {reinterpret_cast<std::uint8_t*>(device), 16U, 4, 1},
            .instances =
             {reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, boxes)), reinterpret_cast<const std::uint8_t*>(device) + offsetof(Input, colors),
              reinterpret_cast<const int*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, labels)), 2,
              device_count ? reinterpret_cast<const std::int64_t*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, count)) : nullptr},
            .masks = reinterpret_cast<const bool*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, masks)),
            .mask_alpha = 96U,
            .box_thickness = 0,
            .stream = {stream.get()},
            .labels = false,
            .add_rgb_to_existing = add,
            .confidences = threshold ? reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, confidences)) : nullptr,
            .confidence_threshold = threshold ? *threshold : 0.0F}) == cudaSuccess);
  std::array<std::uint8_t, 16> output{};
  REQUIRE(cudaMemcpyAsync(output.data(), device, output.size(), cudaMemcpyDeviceToHost, stream.get()) == cudaSuccess);
  REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
  return output;
 };
 CHECK(raster::raster_instance_overlay_rgba({}) == cudaSuccess);  // Empty static count preserves its no-op contract.
 CHECK(draw(true) == std::array<std::uint8_t, 16>{255, 255, 255, 96, 255, 60, 40, 177, 3, 4, 5, 81, 10, 20, 30, 96});
 CHECK(draw(false) == std::array<std::uint8_t, 16>{10, 20, 30, 96, 10, 20, 30, 96, 0, 0, 0, 0, 10, 20, 30, 96});
 for (const float threshold : {0.0F, 0.4F, 0.437F, 1.0F, 0.0F}) {
  const auto output = draw(false, false, &threshold);
  const auto expected = threshold <= 0.4F ? std::array<std::uint8_t, 4>{10, 20, 30, 96} : threshold <= 0.437F ? std::array<std::uint8_t, 4>{70, 80, 90, 96} : std::array<std::uint8_t, 4>{};
  CHECK(std::equal(expected.begin(), expected.end(), output.begin()));
 }
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
 auto storage = mmltk::testsupport::allocate_cuda_test_object<Input>();
 auto* device = storage.get();
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
      REQUIRE(raster::raster_instance_overlay_rgba(
               {.overlay = {base + guard, pitch, width, height},
                .instances =
                 {reinterpret_cast<const float*>(base + offsetof(Input, boxes)), base + offsetof(Input, colors), reinterpret_cast<const int*>(base + offsetof(Input, labels)), 3,
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

TEST_CASE("Semantic compositing preserves transparent alpha while opaque callers keep their pixels", "[raster][cuda][composite]") {
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 mmltk::testsupport::ScopedTestStream stream;
 struct Planes {
  std::array<std::uint8_t,20> base{9,8,7,0, 100,0,0,128, 100,0,0,255, 9,8,7,128, 9,8,7,128};
  std::array<std::uint8_t,20> over{0,100,0,128, 0,100,0,128, 0,100,0,128, 2,3,4,0, 2,3,4,255};
 } input;
 auto device = mmltk::testsupport::allocate_cuda_test_object<Planes>();
 const auto draw = [&](bool preserve) {
  REQUIRE(cudaMemcpyAsync(device.get(),&input,sizeof(input),cudaMemcpyHostToDevice,stream.get()) == cudaSuccess);
  auto* bytes = reinterpret_cast<std::uint8_t*>(device.get());
  REQUIRE(raster::composite_rgba({.base_rgba=raster::pitched_rgba_target(bytes,20,5,1),
   .overlay_rgba={bytes+offsetof(Planes,over),20,5,1},.stream={stream.get()},.preserve_alpha=preserve}) == cudaSuccess);
  Planes result;
  REQUIRE(cudaMemcpyAsync(&result,device.get(),sizeof(result),cudaMemcpyDeviceToHost,stream.get()) == cudaSuccess);
  REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
  return result.base;
 };
 CHECK(draw(true) == std::array<std::uint8_t,20>{0,100,0,128, 33,66,0,191, 49,50,0,255, 9,8,7,128, 2,3,4,255});
 CHECK(draw(false) == std::array<std::uint8_t,20>{4,54,3,255, 49,50,0,255, 49,50,0,255, 9,8,7,128, 2,3,4,255});
}
TEST_CASE("Accumulated semantic alpha keeps clean bytes and bounds two-stage composite quantization", "[raster][cuda][alpha]") {
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 mmltk::testsupport::ScopedTestStream stream;
 constexpr std::array<std::uint8_t,9> alphas{0,1,2,64,115,127,128,254,255};
 constexpr std::array<std::array<std::uint8_t,3>,4> colors{{{255,0,0},{0,255,255},{255,255,255},{1,2,3}}};
 constexpr std::size_t count = alphas.size() * alphas.size() * colors.size();
 using Pixels = std::array<std::uint8_t,count*4>;
 struct Planes { Pixels clean{}, first{}, second{}, prior{}, semantic{}, accumulated{}; } input;
 for (std::size_t color=0; color<colors.size(); ++color) for (std::size_t first=0; first<alphas.size(); ++first) for (std::size_t second=0; second<alphas.size(); ++second) {
  const auto offset=((color*alphas.size()+first)*alphas.size()+second)*4;
  for (std::size_t channel=0; channel<3; ++channel) {
   input.clean[offset+channel]=std::array<std::uint8_t,3>{16,24,96}[channel];
   input.first[offset+channel]=colors[color][channel];
   input.second[offset+channel]=std::array<std::uint8_t,3>{0,100,0}[channel];
  }
  input.clean[offset+3]=255; input.first[offset+3]=alphas[first]; input.second[offset+3]=alphas[second];
 }
 input.prior=input.clean; input.semantic=input.first;
 auto storage=mmltk::testsupport::allocate_cuda_test_object<Planes>();
 REQUIRE(cudaMemcpyAsync(storage.get(),&input,sizeof(input),cudaMemcpyHostToDevice,stream.get()) == cudaSuccess);
 auto* base=reinterpret_cast<std::uint8_t*>(storage.get());
 const auto layer=[&](std::size_t destination,std::size_t overlay,bool preserve=false) {
  REQUIRE(raster::composite_rgba({.base_rgba=raster::pitched_rgba_target(base+destination,count*4,count,1),
   .overlay_rgba={base+overlay,count*4,count,1},.stream={stream.get()},.preserve_alpha=preserve}) == cudaSuccess);
 };
 // The original opaque API performs the two successive byte-rounding steps.
 layer(offsetof(Planes,prior),offsetof(Planes,first));
 layer(offsetof(Planes,prior),offsetof(Planes,second));
 layer(offsetof(Planes,semantic),offsetof(Planes,second),true);
 REQUIRE(raster::finalize_rgba({.clean={base+offsetof(Planes,clean),count*4,count,1},.semantic={base+offsetof(Planes,semantic),count*4,count,1},
  .destination={base+offsetof(Planes,accumulated),count*4,count,1},.stream={stream.get()}}) == cudaSuccess);
 Planes result;
 REQUIRE(cudaMemcpyAsync(&result,storage.get(),sizeof(result),cudaMemcpyDeviceToHost,stream.get()) == cudaSuccess);
 REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
 CHECK(result.clean == input.clean);
 unsigned maximum=0;
 for (std::size_t pixel=0; pixel<count; ++pixel) {
  const auto first=input.first[pixel*4+3], second=input.second[pixel*4+3];
  CHECK(result.accumulated[pixel*4+3] == 255);
  for (std::size_t channel=0; channel<3; ++channel) {
   const auto before=static_cast<int>(result.prior[pixel*4+channel]), after=static_cast<int>(result.accumulated[pixel*4+channel]);
   const auto delta=static_cast<unsigned>(before>after?before-after:after-before);
   maximum=std::max(maximum,delta);
   if (first==0 || second==0 || first==255 || second==255) CHECK(delta == 0);
  }
 }
 CAPTURE(maximum);
 CHECK(maximum <= 3); // Includes accumulated RGB/alpha and final RGBA8 truncation.
 constexpr auto overlap=(4U*alphas.size()+6U)*4U; // Live's 115-alpha mask, then 128-alpha manual overlay.
 CHECK(result.prior[overlap] == 61); CHECK(result.prior[overlap+2] == 25);
 CHECK(result.accumulated[overlap] == 60); CHECK(result.accumulated[overlap+2] == 26);
}
TEST_CASE("Shared skeleton projection keeps confidence visibility and covariance in source coordinates", "[raster][keypoints]") {
 namespace catalog = mmltk::backend::data::catalog;
 const catalog::ClassKeypointSchema schema{{"left", "right", "missing"}, {{0, 1}, {1, 2}}, {1, 0, 2}, {.1, .1, .1}};
 const std::array samples{raster::SkeletonSample{2.25F, 3.75F, .5F, {4, 0, 9}}, raster::SkeletonSample{8, 5, .49F, {1, 0, 1}}, raster::SkeletonSample{20, 20, 1, {}}};
 std::vector<std::uint32_t> words;
 const auto predicted = raster::append_skeleton_projection(words, schema, [&](std::size_t joint) { return samples[joint]; }, {}, {2, 3, 1, -1});
 CHECK(words[predicted.points] == 6);
 CHECK(words[predicted.points + 1] == 10);
 CHECK(predicted.edge_count == 0);
 CHECK(predicted.uncertainty_point_count == 16);
 CHECK(words[predicted.uncertainty_points] == 10);
 const std::array visibility{catalog::KeypointVisibility::Visible, catalog::KeypointVisibility::Occluded, catalog::KeypointVisibility::Unannotated};
 const auto truth = raster::append_skeleton_projection(words, schema, [&](std::size_t joint) { return samples[joint]; }, visibility);
 CHECK(truth.edge_count == 1);
 CHECK(words[truth.edges] == 0);
 CHECK(words[truth.edges + 1] == 1);
 CHECK(truth.uncertainty_point_count == 32);
 CHECK(static_cast<std::int32_t>(words[truth.points + 4]) < 0);
}

TEST_CASE("Shared skeleton projection retains each producer's half-pixel convention", "[raster][keypoints][boundary]") {
 namespace catalog = mmltk::backend::data::catalog;
 const catalog::ClassKeypointSchema schema{{"a", "b", "c"}, {}, {0, 1, 2}, {.1, .1, .1}};
 const std::array samples{raster::SkeletonSample{.5F, -.5F, 1, {}}, raster::SkeletonSample{1.5F, -1.5F, 1, {}}, raster::SkeletonSample{2.5F, -2.5F, 1, {}}};
 std::vector<std::uint32_t> words;
 for (const auto rounding : {raster::SkeletonRounding::AwayFromZero, raster::SkeletonRounding::ToEven}) {
  words.clear();
  static_cast<void>(raster::append_skeleton_projection(words, schema, [&](std::size_t joint) { return samples[joint]; }, {}, {}, rounding));
  const std::array<std::int32_t, 6> expected = rounding == raster::SkeletonRounding::ToEven ? std::array<std::int32_t, 6>{0, 0, 2, -2, 2, -2} : std::array<std::int32_t, 6>{1, -1, 2, -2, 3, -3};
  REQUIRE(words.size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) CHECK(static_cast<std::int32_t>(words[index]) == expected[index]);
 }
}

TEST_CASE("Skeleton batches admit full catalog schemas without maximum-sized scratch", "[raster][skeleton][boundary]") {
 namespace catalog = mmltk::backend::data::catalog;
 catalog::ClassKeypointSchema schema;
 for (std::size_t joint = 0; joint < catalog::kKeypointsPerClassCapacity; ++joint) {
  schema.names.push_back("joint-" + std::to_string(joint));
  schema.horizontal_flip.push_back(joint);
  schema.oks_sigmas.push_back(.1);
 }
 for (std::size_t edge = 0; edge < catalog::kSkeletonEdgeCapacity; ++edge)
  schema.skeleton.push_back({static_cast<catalog::JointIndex>(edge % 256), static_cast<catalog::JointIndex>((edge % 256 + 1 + edge / 256) % 256)});
 std::vector<std::uint32_t> words;
 constexpr std::size_t candidates = 500;
 for (std::size_t index = 0; index < candidates; ++index) {
  const auto projected = raster::append_skeleton_projection(words, schema, [](std::size_t joint) { return raster::SkeletonSample{float(joint), 16, 1, {4,1,4}}; });
  CHECK(projected.point_count == 256);
  CHECK(projected.edge_count == 512);
  CHECK(projected.uncertainty_point_count == 256 * 16);
 }
 CHECK(words.size() == candidates * (256U * 66U + 512U * 2U));
 CHECK(words.size() * sizeof(std::uint32_t) > 32U * 1024U * 1024U);
 CHECK(words.capacity() < raster::kSkeletonProjectionWordCapacity);
 const auto capacity = words.capacity();
 words.clear();
 const auto projected = raster::append_skeleton_projection(words, schema, [](std::size_t joint) { return raster::SkeletonSample{float(joint),16,1,{}}; });
 CHECK(projected.uncertainty_point_count == 0);
 CHECK(words.capacity() == capacity);
 const auto accepted = words;
 schema.skeleton.push_back({0, 256});
 CHECK_THROWS_AS(raster::append_skeleton_projection(words, schema, [](std::size_t) { return raster::SkeletonSample{}; }), std::invalid_argument);
 CHECK(words == accepted);
 schema.skeleton.pop_back();
 schema.skeleton.back().second = 256;
 CHECK_THROWS_AS(raster::append_skeleton_projection(words, schema, [](std::size_t) { return raster::SkeletonSample{}; }), std::invalid_argument);
 CHECK(words == accepted);
}
