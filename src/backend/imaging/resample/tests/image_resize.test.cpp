#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/imaging/resample/tests/perceptual_downscale_reference.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale_math.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale_views.h"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <limits>
#include <cmath>
#include <bit>
#include <vector>
#include <cstring>
#include <new>
#include <utility>
#include <future>
#if defined(MMLTK_TEST_TORCH_RGB_RESIZE)
#include <ATen/Parallel.h>
#include <omp.h>
#endif
#include "avir_float4_sse.h"
namespace mmltk::backend::imaging::resample::perceptual {
struct CpuDownscalerTestAccess {
 using Step = CpuDownscaler::PreparationStep;
 using Storage = std::array<std::pair<const void*, std::size_t>, 8>;
 static void quantize(const CpuDownscaler& owner, const float* values, std::uint8_t* codes) { owner.quantize_block(values, codes); }
 static const CpuTransferTable& transfer(const CpuDownscaler& owner) { return owner.transfer_; }
 static void fail_before(CpuDownscaler& owner, Step step) { owner.fail_before_ = step; }
 static bool prepared(const CpuDownscaler& owner) { return owner.prepared_; }
 static bool armed(const CpuDownscaler& owner) { return owner.fail_before_ != Step::None; }
 static std::size_t horizontal_size(const CpuDownscaler& owner) { return owner.x_.size(); }
 // Reconstruct the final retained row with the original scalar arithmetic.
 // Both required moment rows are still live after execution; this compares
 // SIMD output with scalar operations on identical compensated moments.
 static void scalar_final_row(const CpuDownscaler& owner, RgbMutableImageView destination, bool quantized_planar = false) {
  const auto width = destination.layout.width, y = destination.layout.height - 1;
  const auto moment = [&](unsigned row, unsigned x) {
   Moment result;
   for (unsigned k = 0; k < 3; ++k) {
    result.mean[k] = owner.moments_[row % 2][k * width + x];
    result.variance[k] = owner.moments_[row % 2][(k + 3) * width + x];
   }
   return result;
  };
  const auto coefficient = [&](unsigned row, unsigned x) {
   const auto right = std::min(x + 1, width - 1), below = std::min(row + 1, y);
   const auto result = patch(moment(row, x), moment(row, right), moment(below, x), moment(below, right));
   for (unsigned k = 0; k < 3; ++k) {
    CAPTURE(row, x, k);
    CHECK(result.ratio[k] == owner.coefficients_[row % 2][k * width + x]);
    CHECK(result.center[k] == owner.coefficients_[row % 2][(k + 3) * width + x]);
   }
   return result;
  };
  for (unsigned x = 0; x < width; ++x) {
   const auto left = x ? x - 1 : 0, previous = y ? y - 1 : y;
   const auto color = reconstruct(moment(y, x), coefficient(y, x), coefficient(y, left), coefficient(previous, x), coefficient(previous, left));
   if (quantized_planar) {
    store<RgbPixelFormat::RGB8, true>(destination, x, y, color, 1, owner.transfer_.thresholds);
    continue;
   }
   switch (destination.layout.format) {
    case RgbPixelFormat::RGB8: store<RgbPixelFormat::RGB8>(destination, x, y, color, 1, owner.transfer_.thresholds); break;
    case RgbPixelFormat::RGBA8: store<RgbPixelFormat::RGBA8>(destination, x, y, color, owner.alpha_[y % 2][x], owner.transfer_.thresholds); break;
    case RgbPixelFormat::PlanarUnitSrgbF32: store<RgbPixelFormat::PlanarUnitSrgbF32>(destination, x, y, color, 1, owner.transfer_.thresholds); break;
   }
  }
 }
 static Storage storage(const CpuDownscaler& owner) {
  return {
   {{owner.x_.data(), owner.x_.capacity()}, {owner.y_.data(), owner.y_.capacity()}, {owner.moments_[0].data(), owner.moments_[0].capacity()}, {owner.moments_[1].data(), owner.moments_[1].capacity()},
    {owner.coefficients_[0].data(), owner.coefficients_[0].capacity()}, {owner.coefficients_[1].data(), owner.coefficients_[1].capacity()}, {owner.alpha_[0].data(), owner.alpha_[0].capacity()},
    {owner.alpha_[1].data(), owner.alpha_[1].capacity()}}
  };
 }
};
}  // namespace mmltk::backend::imaging::resample::perceptual
using namespace mmltk::backend::imaging::resample;
namespace {
std::vector<uint8_t> make_test_image(int width, int height) {
 std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
 for (int y = 0; y < height; ++y) {
  for (int x = 0; x < width; ++x) {
   const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3;
   pixels[index + 0] = static_cast<uint8_t>((x * 17 + y * 13) & 0xFF);
   pixels[index + 1] = static_cast<uint8_t>((x * 7 + y * 29 + 11) & 0xFF);
   pixels[index + 2] = static_cast<uint8_t>((x * 31 + y * 5 + 19) & 0xFF);
  }
 }
 return pixels;
}
void test_resize_plan_respects_budget() {
 for (int workers = 1; workers <= 32; ++workers) {
  const ResizeWorkerPlan plan = plan_rgb_resize_workers(workers, true, true);
  REQUIRE(plan.image_workers == workers);
  REQUIRE(plan.resize_threads_per_image == 1);
 }
}
void test_single_thread_resize_is_stable() {
 const std::vector<uint8_t> source = make_test_image(61, 47);
 std::vector<uint8_t> first_pass(static_cast<size_t>(143) * static_cast<size_t>(109) * 3U);
 std::vector<uint8_t> second_pass(first_pass.size());
 RgbImageResizer resizer(1);
 resizer.resize(source.data(), 61, 47, first_pass.data(), 143, 109);
 resizer.resize(source.data(), 61, 47, second_pass.data(), 143, 109);
 REQUIRE(first_pass == second_pass);
}
void test_resizer_rejects_internal_threading() {
 bool threw = false;
 try {
  RgbImageResizer resizer(4);
 } catch (const std::runtime_error&) { threw = true; }
 REQUIRE(threw);
}
void test_same_size_copy_preserves_pixels() {
 const std::vector<uint8_t> source = make_test_image(23, 19);
 std::vector<uint8_t> output(source.size(), 0);
 RgbImageResizer resizer(1);
 resizer.resize(source.data(), 23, 19, output.data(), 23, 19);
 REQUIRE(source == output);
}
void test_parallel_worker_local_resizers_are_stable() {
 const std::vector<uint8_t> source = make_test_image(211, 157);
 for (bool perceptual : {false, true})
  for (const auto& dims : std::array<std::array<int, 2>, 3>{{{332, 332}, {97, 71}, {16, 1}}}) {
   std::vector<std::vector<uint8_t>> outputs(8, std::vector<uint8_t>(static_cast<size_t>(dims[0]) * dims[1] * 3U));
   std::vector<std::thread> threads;
   threads.reserve(outputs.size());
   for (auto& output : outputs) {
    threads.emplace_back([&source, &output, dims, perceptual] {
     RgbImageResizer resizer(1, perceptual);
     resizer.resize(source.data(), 211, 157, output.data(), dims[0], dims[1]);
    });
   }
   for (std::thread& thread : threads) { thread.join(); }
   for (size_t index = 1; index < outputs.size(); ++index) { REQUIRE(outputs[index] == outputs[0]); }
  }
}
}  // namespace
TEST_CASE("resize plans respect worker budgets", "[backend][data][image_resize]") { test_resize_plan_respects_budget(); }
TEST_CASE("single-thread resize is stable", "[backend][data][image_resize]") { test_single_thread_resize_is_stable(); }
TEST_CASE("resizer rejects internal threading", "[backend][data][image_resize]") { test_resizer_rejects_internal_threading(); }
TEST_CASE("same-size resize preserves pixels", "[backend][data][image_resize]") { test_same_size_copy_preserves_pixels(); }
TEST_CASE("worker-local resizers remain stable", "[backend][data][image_resize]") { test_parallel_worker_local_resizers_are_stable(); }
TEST_CASE("perceptual resampling matches independent moments and full-area geometry", "[backend][data][image_resize][perceptual]") {
 using namespace test_perceptual;
 RgbImageResizer resizer;
 for (auto format : formats)
  for (const auto& dims : geometries)
   for (unsigned pattern = 0; pattern < 8; ++pattern) {
    INFO("format " << static_cast<int>(format) << " source " << dims[0] << "x" << dims[1] << " destination " << dims[2] << "x" << dims[3] << " pattern " << pattern);
    Image source(dims[0], dims[1], format, 3), output(dims[2], dims[3], format, 5);
    source.fill(pattern);
    const auto expected = reference(source, dims[2], dims[3], 5);
    resizer.downscale(source.read(), output.write());
    REQUIRE(maximum_error(output, expected) <= reference_tolerance(format));
    REQUIRE(padding_intact(output));
    const auto retained = output.storage;
    resizer.downscale(source.read(), output.write());
    REQUIRE(std::memcmp(output.storage.data(), retained.data(), retained.size() * sizeof(float)) == 0);
   }
}
TEST_CASE("perceptual SIMD handles lane boundaries and unequal fractional footprints", "[backend][data][image_resize][perceptual]") {
 using namespace test_perceptual;
 RgbImageResizer resizer;
 for (auto format : formats)
  for (unsigned width : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 15U, 16U, 17U})
   for (const auto& dims : std::array<std::array<unsigned, 4>, 6>{
         {{width * 2 + 1, 11, width, 7}, {width * 2, 11, width, 7}, {width * 2 + 1, 12, width, 6}, {width * 2, 12, width, 6}, {width * 2 + 1, 1, width, 1}, {width * 3 - 1, 101, width, 7}}
        })
    for (unsigned pattern : {0U, 3U, 5U, 6U, 7U}) {
     INFO("format " << static_cast<int>(format) << " geometry " << dims[0] << "x" << dims[1] << " -> " << dims[2] << "x" << dims[3] << " pattern " << pattern);
     Image source(dims[0], dims[1], format, 3), output(dims[2], dims[3], format, 5);
     source.fill(pattern);
     resizer.downscale(source.read(), output.write());
     REQUIRE(maximum_error(output, reference(source, dims[2], dims[3])) <= reference_tolerance(format));
     REQUIRE(padding_intact(source));
     REQUIRE(padding_intact(output));
    }
}
TEST_CASE("AVIR SIMD rounding matches scalar float arithmetic at signed half boundaries", "[backend][data][image_resize]") {
 const auto check = [](const std::array<float, 4>& values) {
  const auto mode = _MM_GET_ROUNDING_MODE();
  std::array<float, 4> actual{};
  avir::round(avir::float4::loadu(values.data())).storeu(actual.data());
  REQUIRE(_MM_GET_ROUNDING_MODE() == mode);
  for (unsigned lane = 0; lane < 4; ++lane) {
   INFO("round input " << values[lane]);
   REQUIRE(std::bit_cast<std::uint32_t>(actual[lane]) == std::bit_cast<std::uint32_t>(avir::round(values[lane])));
  }
 };
 check({-0.0F, 0.0F, -0.125F, 0.125F});
 for (int integer = -256; integer <= 256; ++integer) {
  const float half = static_cast<float>(integer) + 0.5F;
  check({std::nextafter(half, -std::numeric_limits<float>::infinity()), half, std::nextafter(half, std::numeric_limits<float>::infinity()), half + 0.125F});
 }
}
TEST_CASE("AVIR float4 preserves scalar resize accuracy and production selection", "[backend][data][image_resize]") {
 using namespace test_perceptual;
 avir::CImageResizer<> scalar(8);
 avir::CImageResizer<avir::fpclass_float4> simd(8);
 RgbImageResizer production;
 constexpr std::array<std::array<unsigned, 4>, 9> sizes{
  {{31, 23, 7, 5}, {31, 23, 8, 7}, {31, 23, 9, 13}, {31, 23, 17, 11}, {17, 13, 35, 29}, {17, 13, 17, 13}, {1, 17, 1, 9}, {19, 1, 33, 1}, {17, 13, 9, 19}}
 };
 for (const auto& dims : sizes)
  for (unsigned pattern = 0; pattern < 8; ++pattern) {
   INFO("geometry " << dims[0] << "x" << dims[1] << " -> " << dims[2] << "x" << dims[3] << " pattern " << pattern);
   Image source(dims[0], dims[1], RgbPixelFormat::RGB8, 3);
   source.fill(pattern);
   auto packed = make_test_image(static_cast<int>(dims[0]), static_cast<int>(dims[1]));
   for (unsigned y = 0; y < dims[1]; ++y)
    std::memcpy(packed.data() + std::size_t(y) * dims[0] * 3, reinterpret_cast<const std::uint8_t*>(source.storage.data()) + y * source.layout.row_stride_bytes, dims[0] * 3);
   const auto count = std::size_t(dims[2]) * dims[3] * 3;
   std::vector<std::uint8_t> expected(count + 8, 0xCD), actual(expected), selected(expected);
   scalar.resizeImage(packed.data(), dims[0], dims[1], 0, expected.data(), dims[2], dims[3], 3, 0.0, nullptr);
   simd.resizeImage(packed.data(), dims[0], dims[1], 0, actual.data(), dims[2], dims[3], 3, 0.0, nullptr);
   production.resize(packed.data(), dims[0], dims[1], selected.data(), dims[2], dims[3]);
   if (dims[0] == dims[2] && dims[1] == dims[3])
    REQUIRE(std::equal(packed.begin(), packed.end(), selected.begin()));
   else
    REQUIRE(actual == selected);
   for (std::size_t i = 0; i < count; ++i) REQUIRE(std::abs(int(actual[i]) - int(expected[i])) <= 1);
   for (const auto* output : {&expected, &actual, &selected}) REQUIRE(std::all_of(output->begin() + count, output->end(), [](auto value) { return value == 0xCD; }));
  }
}
namespace {
void check_prepared_pixels(perceptual::CpuDownscaler& resizer, const test_perceptual::Image& source, test_perceptual::Image& output) {
 using namespace test_perceptual;
 const auto expected = reference(source, output.layout.width, output.layout.height, 5);
 resizer.run(source.read(), output.write());
 REQUIRE(maximum_error(output, expected) <= reference_tolerance(source.layout.format));
 REQUIRE(padding_intact(output));
 REQUIRE(perceptual::CpuDownscalerTestAccess::prepared(resizer));
}
void check_preparation_recovery(
 RgbPixelFormat prior_format, RgbPixelFormat requested_format, const std::array<unsigned, 4>& requested, perceptual::CpuDownscalerTestAccess::Step failure, bool prior_first, bool supplied) {
 using namespace test_perceptual;
 using Access = perceptual::CpuDownscalerTestAccess;
 INFO("prior format " << static_cast<int>(prior_format) << " requested format " << static_cast<int>(requested_format) << " geometry " << requested[0] << "x" << requested[1] << " -> " << requested[2]
                      << "x" << requested[3] << " boundary " << static_cast<int>(failure) << " prior first " << prior_first << " supplied " << supplied);
 perceptual::CpuDownscaler resizer;
 const auto width = std::max(9U, requested[2]), height = std::max(7U, requested[3]);
 const auto bytes = perceptual::CpuDownscaler::workspace_bytes(width, height, true);
 std::vector<std::byte> backing(supplied ? bytes + 63 : 0, std::byte{0xFF});
 if (supplied) {
  const auto aligned = (reinterpret_cast<std::uintptr_t>(backing.data()) + 63) / 64 * 64;
  resizer.bind(width, height, true, {reinterpret_cast<std::byte*>(aligned), bytes});
 }
 Image prior_source(17, 13, prior_format, 3), prior_output(9, 7, prior_format, 5);
 Image source(requested[0], requested[1], requested_format, 3), output(requested[2], requested[3], requested_format, 5);
 prior_source.fill(5);
 source.fill(7);
 check_prepared_pixels(resizer, prior_source, prior_output);
 const auto before = Access::storage(resizer);
 const auto untouched = output.storage;
 Access::fail_before(resizer, failure);
 REQUIRE_THROWS_AS(resizer.run(source.read(), output.write()), std::bad_alloc);
 REQUIRE_FALSE(Access::armed(resizer));
 REQUIRE_FALSE(Access::prepared(resizer));
 REQUIRE(std::memcmp(output.storage.data(), untouched.data(), untouched.size() * sizeof(float)) == 0);
 const auto interrupted = Access::storage(resizer);
 for (std::size_t i = 0; i < before.size(); ++i) REQUIRE(interrupted[i].second >= before[i].second);
 if (failure == Access::Step::VerticalAxis) REQUIRE(Access::horizontal_size(resizer) == requested[2]);
 // Both first-retry orders are needed: succeeding at one geometry must not
 // mask a stale-key error when the other is the first use after failure.
 for (bool use_prior : {prior_first, !prior_first}) {
  auto& input = use_prior ? prior_source : source;
  auto& result = use_prior ? prior_output : output;
  check_prepared_pixels(resizer, input, result);
  const auto retained = Access::storage(resizer);
  const auto pixels = result.storage;
  // Even the earliest preparation checkpoint must stay unvisited for a
  // successful unchanged configuration, with stable capacity/addresses.
  Access::fail_before(resizer, Access::Step::HorizontalAxis);
  check_prepared_pixels(resizer, input, result);
  REQUIRE(Access::armed(resizer));
  Access::fail_before(resizer, Access::Step::None);
  REQUIRE(Access::storage(resizer) == retained);
  REQUIRE(std::memcmp(result.storage.data(), pixels.data(), pixels.size() * sizeof(float)) == 0);
 }
}
}  // namespace
TEST_CASE("perceptual CPU preparation retains safe recovery at each storage boundary", "[backend][data][image_resize][perceptual]") {
 using Access = perceptual::CpuDownscalerTestAccess;
 using Step = Access::Step;
 const bool supplied = GENERATE(false, true);
 constexpr std::array steps{Step::HorizontalAxis, Step::VerticalAxis, Step::FirstMoment, Step::SecondMoment, Step::FirstCoefficient, Step::SecondCoefficient, Step::FirstAlpha, Step::SecondAlpha};
 constexpr std::array<std::array<unsigned, 4>, 3> requests{{{19, 101, 7, 99}, {29, 23, 19, 17}, {18, 14, 9, 7}}};
 for (auto prior : test_perceptual::formats)
  for (auto requested : test_perceptual::formats)
   for (const auto& geometry : requests)
    for (auto step : steps)
     for (bool prior_first : {false, true}) {
      if (requested != RgbPixelFormat::RGBA8 && (step == Step::FirstAlpha || step == Step::SecondAlpha)) continue;
      check_preparation_recovery(prior, requested, geometry, step, prior_first, supplied);
     }
}
TEST_CASE("perceptual CPU first alpha preparation invalidates an unchanged geometry on failure", "[backend][data][image_resize][perceptual]") {
 using Step = perceptual::CpuDownscalerTestAccess::Step;
 const bool supplied = GENERATE(false, true);
 for (auto prior : {RgbPixelFormat::RGB8, RgbPixelFormat::PlanarUnitSrgbF32})
  for (auto step : {Step::FirstAlpha, Step::SecondAlpha})
   for (bool prior_first : {false, true}) check_preparation_recovery(prior, RgbPixelFormat::RGBA8, {17, 13, 9, 7}, step, prior_first, supplied);
}
TEST_CASE("perceptual CPU identity copies preserve exact pixels and padding in every format", "[backend][data][image_resize][perceptual]") {
 using namespace test_perceptual;
 using Access = perceptual::CpuDownscalerTestAccess;
 RgbImageResizer resizer;
 for (auto format : formats) {
  Image source(17, 13, format, 3), output(17, 13, format, 5), direct(17, 13, format, 5);
  source.fill(7);
  const auto original = source.storage;
  resizer.downscale(source.read(), source.write());
  REQUIRE(std::memcmp(source.storage.data(), original.data(), original.size() * sizeof(float)) == 0);
  resizer.downscale(source.read(), output.write());
  REQUIRE(maximum_error(output, source) == 0);
  REQUIRE(padding_intact(output));
  perceptual::CpuDownscaler owner;
  Access::fail_before(owner, Access::Step::HorizontalAxis);
  owner.run(source.read(), source.write());
  REQUIRE(std::memcmp(source.storage.data(), original.data(), original.size() * sizeof(float)) == 0);
  owner.run(source.read(), direct.write());
  REQUIRE(std::memcmp(direct.storage.data(), output.storage.data(), output.storage.size() * sizeof(float)) == 0);
  REQUIRE(Access::armed(owner));
  REQUIRE_FALSE(Access::prepared(owner));
  REQUIRE(owner.retained_bytes() == 0);
 }
}
TEST_CASE("optional perceptual selection preserves default AVIR and enlargement", "[backend][data][image_resize][perceptual]") {
 const auto source = make_test_image(17, 13);
 RgbImageResizer defaults, disabled(1, false), enabled(1, true);
 for (const auto& dims : std::array<std::array<int, 2>, 3>{{{7, 5}, {25, 19}, {17, 13}}}) {
  std::vector<std::uint8_t> a(static_cast<std::size_t>(dims[0]) * dims[1] * 3), b(a.size()), c(a.size());
  defaults.resize(source.data(), 17, 13, a.data(), dims[0], dims[1]);
  disabled.resize(source.data(), 17, 13, b.data(), dims[0], dims[1]);
  enabled.resize(source.data(), 17, 13, c.data(), dims[0], dims[1]);
  REQUIRE(a == b);
  if (dims[0] >= 17)
   REQUIRE(a == c);
  else
   REQUIRE(a != c);
 }
}
TEST_CASE("perceptual alpha coverage ignores hidden saturated color", "[backend][data][image_resize][perceptual]") {
 using namespace test_perceptual;
 Image source(3, 1, RgbPixelFormat::RGBA8), output(1, 1, RgbPixelFormat::RGBA8);
 for (unsigned x = 0; x < 3; ++x) {
  source.set(x, 0, 0, x == 0 ? 1 : 0);
  source.set(x, 0, 1, x == 1 ? 1 : 0);
  source.set(x, 0, 2, x == 2 ? 1 : 0);
  source.set(x, 0, 3, x == 0 ? 1 : 0);
 }
 RgbImageResizer resizer;
 resizer.downscale(source.read(), output.write());
 REQUIRE(output.at(0, 0, 0) >= 254.0 / 255);
 REQUIRE(output.at(0, 0, 1) <= 1.0 / 255);
 REQUIRE(output.at(0, 0, 2) <= 1.0 / 255);
 REQUIRE(output.at(0, 0, 3) == 85.0 / 255);
 source.set(0, 0, 3, 0);
 resizer.downscale(source.read(), output.write());
 for (unsigned k = 0; k < 4; ++k) REQUIRE(output.at(0, 0, k) == 0);
}
TEST_CASE("perceptual transfer thresholds are monotonic and continuously accurate", "[backend][data][image_resize][perceptual]") {
 perceptual::TransferTable table;
 for (unsigned i = 0; i < 256; ++i) table.linear[i] = perceptual::decode(float(i) * (1.0F / 255.0F));
 unsigned previous = 0;
 for (unsigned i = 0; i <= 65536; ++i) {
  const double linear = double(i) / 65536;
  const unsigned code = table.quantize(static_cast<float>(linear));
  REQUIRE(code >= previous);
  previous = code;
  REQUIRE(std::abs(double(code) - test_perceptual::oetf(linear) * 255) < 1.0001);
  REQUIRE(std::abs(perceptual::encode(static_cast<float>(linear)) - test_perceptual::oetf(linear)) < 2e-7);
 }
 for (double v : {0.0, 0.040449, 0.04045, 0.040451, 1.0}) REQUIRE(std::abs(perceptual::decode(static_cast<float>(v)) - test_perceptual::eotf(v)) < 2e-7);
}
TEST_CASE("perceptual low variance keeps the specified ratio-two threshold", "[backend][data][image_resize][perceptual]") {
 for (float variance : {0.0F, 0.999e-6F, 1.001e-6F}) {
  const float ratio = perceptual::contrast_ratio(variance, variance * 8);
  REQUIRE(std::abs(ratio - (variance < 1e-6F ? 2.0F : 3.0F)) < 1e-6F);
 }
 using namespace test_perceptual;
 RgbImageResizer resizer;
 for (double delta : {0.001999, 0.002001}) {
  Image source(4, 1, RgbPixelFormat::PlanarUnitSrgbF32), output(2, 1, RgbPixelFormat::PlanarUnitSrgbF32);
  for (unsigned x = 0; x < 4; ++x)
   for (unsigned k = 0; k < 3; ++k) source.set(x, 0, k, oetf(0.3 + (x >= 2 ? delta : 0) + (x % 2 ? 0.01 : -0.01)));
  resizer.downscale(source.read(), output.write());
  REQUIRE(maximum_error(output, reference(source, 2, 1)) < 2e-6);
  for (unsigned source_width : {34U, 35U}) {
   auto simd_source = threshold_source(source_width, 6, delta);
   Image simd_output(17, 3, RgbPixelFormat::PlanarUnitSrgbF32);
   resizer.downscale(simd_source.read(), simd_output.write());
   REQUIRE(maximum_error(simd_output, reference(simd_source, 17, 3)) < 2e-6);
  }
 }
}
TEST_CASE("perceptual checked views reject unsafe geometry before writing", "[backend][data][image_resize][perceptual]") {
 using namespace test_perceptual;
 Image source(17, 13, RgbPixelFormat::RGB8), output(7, 5, RgbPixelFormat::RGB8);
 source.fill(5);
 RgbImageResizer resizer;
 auto bad = source.read();
 bad.layout.capacity_bytes = 1;
 REQUIRE_THROWS(resizer.downscale(bad, output.write()));
 bad = source.read();
 bad.layout.row_stride_bytes = 1;
 REQUIRE_THROWS(resizer.downscale(bad, output.write()));
 bad = source.read();
 bad.layout.width = 0;
 REQUIRE_THROWS(resizer.downscale(bad, output.write()));
 bad = source.read();
 bad.data = nullptr;
 REQUIRE_THROWS(resizer.downscale(bad, output.write()));
 bad = source.read();
 bad.layout.row_stride_bytes = std::numeric_limits<std::size_t>::max();
 REQUIRE_THROWS(resizer.downscale(bad, output.write()));
 auto overlap = output.write();
 overlap.data = source.storage.data();
 REQUIRE_THROWS(resizer.downscale(source.read(), overlap));
 REQUIRE_NOTHROW(resizer.downscale(source.read(), source.write()));
 REQUIRE_THROWS(resizer.downscale(output.read(), source.write()));
 Image floats(17, 13, RgbPixelFormat::PlanarUnitSrgbF32);
 auto float_bad = floats.read();
 float_bad.layout.plane_stride_bytes = 4;
 REQUIRE_THROWS(resizer.downscale(float_bad, output.write()));
 float_bad = floats.read();
 float_bad.data = reinterpret_cast<const std::uint8_t*>(floats.storage.data()) + 1;
 REQUIRE_THROWS(resizer.downscale(float_bad, output.write()));
 REQUIRE(std::all_of(output.storage.begin(), output.storage.end(), [](float v) { return std::bit_cast<std::uint32_t>(v) == 0xCDCDCDCDU; }));
}
TEST_CASE("perceptual float unit-domain sanitation remains finite", "[backend][data][image_resize][perceptual]") {
 using namespace test_perceptual;
 RgbImageResizer resizer;
 constexpr double values[]{-1, std::numeric_limits<double>::quiet_NaN(), 0.04045, 2, std::numeric_limits<double>::infinity()};
 for (const auto& dims : std::array<std::array<unsigned, 2>, 2>{{{5, 2}, {23, 9}}}) {
  Image source(dims[0], 1, RgbPixelFormat::PlanarUnitSrgbF32), output(dims[1], 1, RgbPixelFormat::PlanarUnitSrgbF32);
  for (unsigned x = 0; x < dims[0]; ++x)
   for (unsigned k = 0; k < 3; ++k) source.set(x, 0, k, values[x % 5]);
  resizer.downscale(source.read(), output.write());
  REQUIRE(maximum_error(output, reference(source, dims[1], 1)) < 2e-6);
  for (unsigned x = 0; x < dims[1]; ++x)
   for (unsigned k = 0; k < 3; ++k) {
    REQUIRE(std::isfinite(output.at(x, 0, k)));
    REQUIRE(output.at(x, 0, k) >= 0);
    REQUIRE(output.at(x, 0, k) <= 1);
   }
 }
}
TEST_CASE("perceptual logical admission preserves format alignment alias and overflow failures", "[backend][data][image_resize][perceptual]") {
 using namespace test_perceptual;
 RgbImageResizer resizer;
 Image source(17, 13, RgbPixelFormat::RGB8, 3), output(9, 7, RgbPixelFormat::RGB8, 5);
 const auto untouched = output.storage;
 auto bad = source.read();
 bad.layout.format = static_cast<RgbPixelFormat>(255);
 REQUIRE_THROWS_AS(resizer.downscale(bad, output.write()), std::invalid_argument);
 bad = source.read();
 bad.layout.plane_stride_bytes = 4;
 REQUIRE_THROWS_AS(resizer.downscale(bad, output.write()), std::invalid_argument);
 bad = source.read();
 bad.layout.height = 0;
 REQUIRE_THROWS_AS(resizer.downscale(bad, output.write()), std::invalid_argument);
 bad = source.read();
 bad.data = reinterpret_cast<const void*>(std::numeric_limits<std::uintptr_t>::max() - 3);
 REQUIRE_THROWS_AS(resizer.downscale(bad, output.write()), std::overflow_error);
 bad = source.read();
 bad.layout.row_stride_bytes = std::numeric_limits<std::size_t>::max();
 REQUIRE_THROWS_AS(resizer.downscale(bad, output.write()), std::overflow_error);
 auto alias = source.write();
 alias.layout.row_stride_bytes += 4;
 alias.layout.capacity_bytes += 4 * 13;
 REQUIRE_THROWS_AS(resizer.downscale(source.read(), alias), std::invalid_argument);
 Image floats(17, 13, RgbPixelFormat::PlanarUnitSrgbF32, 3), float_output(9, 7, RgbPixelFormat::PlanarUnitSrgbF32, 5);
 REQUIRE_THROWS_AS(resizer.downscale(floats.read(), output.write()), std::invalid_argument);
 for (bool plane : {false, true}) {
  auto unaligned = floats.read();
  if (plane)
   ++unaligned.layout.plane_stride_bytes;
  else
   ++unaligned.layout.row_stride_bytes;
  REQUIRE_THROWS_AS(resizer.downscale(unaligned, float_output.write()), std::invalid_argument);
 }
 auto bad_planes = floats.read();
 bad_planes.layout.plane_stride_bytes = std::numeric_limits<std::size_t>::max() - 3;
 bad_planes.layout.capacity_bytes = std::numeric_limits<std::size_t>::max();
 REQUIRE_THROWS_AS(resizer.downscale(bad_planes, float_output.write()), std::overflow_error);
 REQUIRE(std::memcmp(output.storage.data(), untouched.data(), untouched.size() * sizeof(float)) == 0);
 REQUIRE(padding_intact(float_output));
}
TEST_CASE("resize geometry explicitly chooses stretch or rounded letterbox", "[backend][data][image_resize]") {
 const auto stretch = compute_image_resize_geometry(65, 49, 31, 29, ImageResizeMode::Stretch);
 CHECK(stretch.resized_width == 31U);
 CHECK(stretch.resized_height == 29U);
 CHECK(stretch.offset_x == 0U);
 CHECK(stretch.offset_y == 0U);
 const auto letterbox = compute_image_resize_geometry(65, 49, 31, 29, ImageResizeMode::Letterbox);
 CHECK(letterbox.resized_width == 31U);
 CHECK(letterbox.resized_height == 23U);
 CHECK(letterbox.offset_x == 0U);
 CHECK(letterbox.offset_y == 3U);
 const auto thin = compute_image_resize_geometry(10000, 1, 8, 8, ImageResizeMode::Letterbox);
 CHECK(thin.resized_height == 1U);
 CHECK_THROWS(compute_image_resize_geometry(0, 1, 8, 8, ImageResizeMode::Stretch));
 CHECK_THROWS(compute_image_resize_geometry(1, 1, 8, 8, static_cast<ImageResizeMode>(255)));
}
namespace {
void check_compiler_projection(
 RgbImageResizer& resizer, const std::vector<std::uint8_t>& source, std::uint32_t width, std::uint32_t height, std::uint32_t target_width, std::uint32_t target_height, ImageResizeMode mode) {
 const auto geometry = compute_image_resize_geometry(width, height, target_width, target_height, mode);
 std::vector<std::uint8_t> bytes(std::size_t(geometry.resized_width) * geometry.resized_height * 3U);
 resizer.resize(source.data(), static_cast<int>(width), static_cast<int>(height), bytes.data(), static_cast<int>(geometry.resized_width), static_cast<int>(geometry.resized_height));
 const auto plane = std::size_t(target_width) * target_height;
 std::vector<float> expected(plane * 3U + 8U, -17.0F), actual(expected);
 letterboxed_rgb_hwc_u8_to_nchw_f32(bytes.data(), expected.data(), geometry.resized_width, geometry.resized_height, target_width, target_height, geometry.offset_x, geometry.offset_y);
 resizer.resize_to_planar({source.data(), {width, height, std::size_t(width) * 3U, 0U, source.size(), RgbPixelFormat::RGB8}},
  {actual.data(), {target_width, target_height, std::size_t(target_width) * sizeof(float), plane * sizeof(float), plane * 3U * sizeof(float), RgbPixelFormat::PlanarUnitSrgbF32}}, mode);
 REQUIRE(std::memcmp(actual.data(), expected.data(), expected.size() * sizeof(float)) == 0);
}
}  // namespace
TEST_CASE("compiler planar projection preserves every quantized RGB8 bit", "[backend][data][image_resize][perceptual]") {
 constexpr std::array<std::array<std::uint32_t, 4>, 18> sizes{
  {{32, 18, 16, 9}, {34, 18, 17, 9}, {16, 10, 8, 5}, {14, 10, 7, 5}, {17, 13, 9, 7}, {17, 13, 17, 5}, {17, 13, 7, 13}, {17, 13, 1, 1}, {1, 17, 1, 7}, {31, 3, 9, 11}, {17, 13, 17, 13},
   {17, 13, 23, 19}, {17, 13, 9, 19}, {15, 11, 7, 7}, {17, 11, 8, 7}, {31, 11, 15, 7}, {33, 11, 16, 7}, {35, 11, 17, 7}}
 };
 for (const bool enabled : {false, true}) {
  RgbImageResizer resizer(1, enabled);
  for (const auto& dims : sizes)
   for (const auto mode : {ImageResizeMode::Stretch, ImageResizeMode::Letterbox})
    for (unsigned pattern = 0U; pattern < 4U; ++pattern) {
     INFO("enabled " << enabled << " source " << dims[0] << "x" << dims[1] << " target " << dims[2] << "x" << dims[3] << " mode " << static_cast<int>(mode) << " pattern " << pattern);
     auto source = make_test_image(static_cast<int>(dims[0]), static_cast<int>(dims[1]));
     std::uint32_t random = 0x792719U;
     for (std::size_t i = 0; i < source.size(); ++i) {
      random = random * 1664525U + 1013904223U;
      if (pattern == 0U) source[i] = 11U;
      if (pattern == 1U) source[i] = (i / 3U % dims[0] >= dims[0] / 2U) ? 255U : 0U;
      if (pattern == 2U) source[i] = ((i / 3U + i / 3U / dims[0]) & 1U) ? 255U : 0U;
      if (pattern == 3U) source[i] = static_cast<std::uint8_t>(random >> 24U);
     }
     check_compiler_projection(resizer, source, dims[0], dims[1], dims[2], dims[3], mode);
    }
  for (unsigned code = 0U; code < 256U; ++code) {
   const std::vector<std::uint8_t> source(34U * 6U * 3U, static_cast<std::uint8_t>(code));
   check_compiler_projection(resizer, source, 34, 6, 17, 3, ImageResizeMode::Stretch);
   check_compiler_projection(resizer, source, 34, 6, 19, 5, ImageResizeMode::Letterbox);
  }
 }
}
TEST_CASE("quantized planar preparation failure leaves the complete canvas untouched and permits retry", "[backend][data][image_resize][perceptual]") {
 using Access = perceptual::CpuDownscalerTestAccess;
 using Step = Access::Step;
 const bool supplied = GENERATE(false, true);
 const auto dims = GENERATE((std::array<unsigned, 4>{19, 13, 9, 7}), (std::array<unsigned, 4>{18, 14, 9, 7}), (std::array<unsigned, 4>{9, 1, 9, 1}));
 for (const auto step : {Step::HorizontalAxis, Step::VerticalAxis, Step::FirstMoment, Step::SecondMoment, Step::FirstCoefficient, Step::SecondCoefficient}) {
  INFO("geometry " << dims[0] << "x" << dims[1] << " -> " << dims[2] << "x" << dims[3] << " boundary " << static_cast<int>(step));
  perceptual::CpuDownscaler owner;
  const auto workspace_size = perceptual::CpuDownscaler::workspace_bytes(dims[2], dims[3], false);
  std::vector<std::byte> backing(supplied ? workspace_size + 63 : 0, std::byte{0xFF});
  if (supplied) {
   const auto aligned = (reinterpret_cast<std::uintptr_t>(backing.data()) + 63) / 64 * 64;
   owner.bind(dims[2], dims[3], false, {reinterpret_cast<std::byte*>(aligned), workspace_size});
  }
  test_perceptual::Image source(dims[0], dims[1], RgbPixelFormat::RGB8, 3), destination(dims[2], dims[3], RgbPixelFormat::PlanarUnitSrgbF32, 5);
  source.fill(5);
  const auto untouched = destination.storage;
  Access::fail_before(owner, step);
  REQUIRE_THROWS_AS(owner.run_quantized_planar(source.read(), destination.write()), std::bad_alloc);
  CHECK_FALSE(Access::armed(owner));
  CHECK_FALSE(Access::prepared(owner));
  CHECK(std::memcmp(untouched.data(), destination.storage.data(), untouched.size() * sizeof(float)) == 0);
  owner.run_quantized_planar(source.read(), destination.write());
  REQUIRE(Access::prepared(owner));
  test_perceptual::Image bytes(dims[2], dims[3], RgbPixelFormat::RGB8, 5);
  if (dims[0] == dims[2] && dims[1] == dims[3]) {
   // The explicit API filters unchanged geometry. Its one-row case lets the
   // retained scalar-row oracle check every pixel without an identity copy.
   Access::scalar_final_row(owner, bytes.write());
  } else {
   RgbImageResizer reference;
   reference.downscale(source.read(), bytes.write());
  }
  for (unsigned y = 0U; y < dims[3]; ++y)
   for (unsigned x = 0U; x < dims[2]; ++x)
    for (unsigned k = 0U; k < 3U; ++k) {
     const auto code = static_cast<unsigned>(std::lround(bytes.at(x, y, k) * 255.0));
     const float expected = float(code) * (1.0F / 255.0F);
     CHECK(static_cast<float>(destination.at(x, y, k)) == expected);
    }
  CHECK(test_perceptual::padding_intact(destination));
  const auto retained = Access::storage(owner);
  const auto pixels = destination.storage;
  Access::fail_before(owner, Step::HorizontalAxis);
  owner.run_quantized_planar(source.read(), destination.write());
  CHECK(Access::armed(owner));
  CHECK(Access::storage(owner) == retained);
  CHECK(std::memcmp(destination.storage.data(), pixels.data(), pixels.size() * sizeof(float)) == 0);
 }
}
TEST_CASE("compiler projection rejects invalid canvases before writing padding", "[backend][data][image_resize][perceptual]") {
 const auto source = make_test_image(19, 13);
 std::vector<float> destination(11U * 11U * 3U, -19.0F);
 const RgbConstImageView input{source.data(), {19U, 13U, 57U, 0U, source.size(), RgbPixelFormat::RGB8}};
 const RgbMutableImageView output{destination.data(), {11U, 11U, 44U, 484U, destination.size() * sizeof(float), RgbPixelFormat::PlanarUnitSrgbF32}};
 RgbImageResizer resizer(1, true);
 auto bad = output;
 --bad.layout.capacity_bytes;
 CHECK_THROWS(resizer.resize_to_planar(input, bad, ImageResizeMode::Letterbox));
 bad = output;
 bad.layout.plane_stride_bytes -= sizeof(float);
 CHECK_THROWS(resizer.resize_to_planar(input, bad, ImageResizeMode::Letterbox));
 CHECK_THROWS(resizer.resize_to_planar(input, output, static_cast<ImageResizeMode>(255)));
 auto alias = input;
 alias.data = destination.data();
 CHECK_THROWS(resizer.resize_to_planar(alias, output, ImageResizeMode::Letterbox));
 CHECK(std::ranges::all_of(destination, [](float value) { return value == -19.0F; }));
 check_compiler_projection(resizer, source, 19, 13, 11, 11, ImageResizeMode::Letterbox);
}
TEST_CASE("Supplied resizer preparation preserves RGB and planar numerical output", "[imaging][resize][workspace]") {
 using namespace mmltk::backend::imaging::resample;
 const bool perceptual = GENERATE(false, true);
 const auto mode = GENERATE(ImageResizeMode::Stretch, ImageResizeMode::Letterbox);
 const std::uint32_t sw = 31, sh = 19, dw = 11, dh = 9;
 std::vector<std::uint8_t> source(sw * sh * 3);
 for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<std::uint8_t>((i * 71 + i / 3) % 256);
 const auto plan = RgbImageResizer::workspace_plan(sw, sh, dw, dh, mode, perceptual);
 std::vector<std::byte> backing(plan.bytes() + 63);
 const auto aligned = (reinterpret_cast<std::uintptr_t>(backing.data()) + 63) / 64 * 64;
 std::span<std::byte> memory(reinterpret_cast<std::byte*>(aligned), plan.bytes());
 RgbImageResizer supplied(1, perceptual), reference(1, perceptual);
 supplied.prepare(plan, memory);
 std::vector<float> expected(dw * dh * 3), actual(expected.size(), -1);
 const RgbConstImageView input{source.data(), {sw, sh, sw * 3, 0, source.size(), RgbPixelFormat::RGB8}};
 const RgbImageLayout output{dw, dh, dw * sizeof(float), dw * dh * sizeof(float), expected.size() * sizeof(float), RgbPixelFormat::PlanarUnitSrgbF32};
 reference.resize_to_planar(input, {expected.data(), output}, mode);
 supplied.resize_to_planar(input, {actual.data(), output}, mode);
 CHECK(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(float)) == 0);
 const auto retained = supplied.retained_bytes();
 supplied.prepare(plan, memory);
 supplied.resize_to_planar(input, {actual.data(), output}, mode);
 CHECK(supplied.retained_bytes() == retained);
 CHECK(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(float)) == 0);
 CHECK_THROWS_AS(supplied.prepare(plan, memory.first(memory.size() - 1)), std::length_error);
}
TEST_CASE("eight-lane transfer lookup preserves every threshold bucket and adjacent float", "[backend][data][image_resize][perceptual]") {
 using Access = perceptual::CpuDownscalerTestAccess;
 perceptual::CpuDownscaler owner, peer;
 const auto& transfer = Access::transfer(owner);
 const auto& table = transfer.thresholds;
 CHECK(&transfer == &Access::transfer(peer));
 constexpr float infinity = std::numeric_limits<float>::infinity();
 const auto check = [&](const std::array<float, 8>& values) {
  std::array<std::uint8_t, 8> output{};
  Access::quantize(owner, values.data(), output.data());
  for (unsigned lane = 0; lane < output.size(); ++lane) {
   CAPTURE(lane, values[lane]);
   CHECK(output[lane] == table.quantize(values[lane]));
  }
  return output;
 };
 for (unsigned code = 0; code < 256; ++code) {
  const float threshold = table.linear[code];
  CAPTURE(code);
  CHECK(std::bit_cast<std::uint32_t>(threshold) == std::bit_cast<std::uint32_t>(perceptual::decode(float(code) * (1.0F / 255.0F))));
  if (code) REQUIRE(double(threshold) - double(table.linear[code - 1]) > 1.0 / perceptual::CpuTransferTable::bucket_count);
  const std::array values{threshold, std::nextafter(threshold, -infinity), std::nextafter(threshold, infinity), -1.0F, 2.0F, std::numeric_limits<float>::quiet_NaN(), -infinity, infinity};
  const auto output = check(values);
  CHECK(output[0] == code);
  CHECK(output[1] == (code ? code - 1 : 0));
  CHECK(output[2] == code);
 }
 constexpr auto buckets = perceptual::CpuTransferTable::bucket_count;
 for (unsigned bucket = 0; bucket <= buckets; ++bucket) {
  CAPTURE(bucket);
  const float lower = float(bucket) * (1.0F / buckets), upper = float(bucket + 1) * (1.0F / buckets);
  const float last = std::nextafter(upper, -infinity);
  const auto base = transfer.base_code[bucket];
  CHECK(base == table.quantize(lower));
  // Exhaustive occupancy proof on the actual table: the highest Float32
  // within each bucket is no more than one scalar code above its base.
  CHECK(table.quantize(last) >= base);
  CHECK(table.quantize(last) - base <= 1);
  check({lower, std::nextafter(lower, -infinity), std::nextafter(lower, infinity), (lower + upper) * 0.5F, last, upper, std::nextafter(upper, infinity), -lower});
 }
 check(
  {-0.0F, 0.0F, std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(), -std::numeric_limits<float>::min(),
   std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max()});
 CHECK(transfer.base_code.back() == 255);
}
TEST_CASE("checked resize plans retain their selected algorithm independently of storage", "[backend][data][image_resize][workspace]") {
 const std::array<std::pair<std::array<unsigned, 2>, RgbResizePath>, 11> cases{
  {{{640, 480}, RgbResizePath::PerceptualFractional}, {{768, 768}, RgbResizePath::PerceptualInteger}, {{384, 384}, RgbResizePath::Identity}, {{640, 360}, RgbResizePath::Avir},
   {{320, 240}, RgbResizePath::Avir}, {{1024, 768}, RgbResizePath::PerceptualFractional}, {{2048, 2048}, RgbResizePath::PerceptualFractional}, {{384, 768}, RgbResizePath::PerceptualInteger},
   {{768, 384}, RgbResizePath::PerceptualInteger}, {{384, 480}, RgbResizePath::PerceptualFractional}, {{640, 384}, RgbResizePath::PerceptualFractional}}
 };
 for (const auto& [source, expected] : cases) {
  const auto plan = RgbImageResizer::workspace_plan(source[0], source[1], 384, 384, ImageResizeMode::Stretch, true);
  CHECK(plan.path() == expected);
  CHECK((plan.bytes() == 0) == (expected == RgbResizePath::Identity));
  if (source[0] >= 384 && source[1] >= 384) {
   CHECK(perceptual::classify_geometry(source[0], source[1], 384, 384) == (expected == RgbResizePath::Identity ? RgbResizePath::PerceptualInteger : expected));
   if (expected != RgbResizePath::Identity) CHECK(plan.bytes() == perceptual::CpuDownscaler::workspace_bytes(384, 384, false));
  }
 }
 CHECK_THROWS(RgbImageResizer::workspace_plan(0, 1, 1, 1, ImageResizeMode::Stretch, true));
 CHECK_THROWS(RgbImageResizer::workspace_plan(std::numeric_limits<unsigned>::max(), 1, 384, 384, ImageResizeMode::Stretch, false));
}
TEST_CASE("explicit supplied overwrite sizing preserves contents and ordinary resize clears", "[backend][data][image_resize][workspace]") {
 std::array<std::uint8_t, 19> memory;
 memory.fill(173);
 mmltk::common::types::WorkspaceBuffer<std::uint8_t> buffer;
 buffer.bind(memory);
 buffer.resize_for_overwrite(17);
 CHECK(buffer.size() == 17);
 CHECK(std::ranges::all_of(memory, [](auto value) { return value == 173; }));
 buffer.clear();
 buffer.resize(17);
 CHECK(std::ranges::all_of(memory.begin(), memory.begin() + 17, [](auto value) { return value == 0; }));
 CHECK(memory[17] == 173);
 CHECK_THROWS_AS(buffer.resize_for_overwrite(20), std::length_error);
 CHECK(buffer.size() == 17);
}
TEST_CASE("one supplied arena supports poisoned mixed geometry and rejects stale plan use", "[backend][data][image_resize][workspace]") {
 const auto mode = GENERATE(ImageResizeMode::Stretch, ImageResizeMode::Letterbox);
 const auto perceptual = GENERATE(false, true);
 constexpr std::array<std::array<unsigned, 4>, 7> geometries{{{17, 13, 9, 7}, {18, 14, 9, 7}, {9, 7, 9, 7}, {19, 1, 8, 9}, {1, 19, 1, 8}, {8, 7, 17, 13}, {31, 19, 16, 9}}};
 std::vector<RgbResizeWorkspacePlan> plans;
 std::size_t maximum = 0;
 for (const auto& d : geometries) {
  plans.push_back(RgbImageResizer::workspace_plan(d[0], d[1], d[2], d[3], mode, perceptual));
  const auto content = compute_image_resize_geometry(d[0], d[1], d[2], d[3], mode);
  if (perceptual && content.resized_width <= d[0] && content.resized_height <= d[1]) {
   const bool identity = content.resized_width == d[0] && content.resized_height == d[1];
   CHECK(plans.back().path() == (identity ? RgbResizePath::Identity : perceptual::classify_geometry(d[0], d[1], content.resized_width, content.resized_height)));
  }
  maximum = std::max(maximum, plans.back().bytes());
 }
 std::vector<std::byte> backing(maximum + 127, std::byte{0x53});
 const auto aligned = (reinterpret_cast<std::uintptr_t>(backing.data()) + 63) / 64 * 64;
 const std::span memory(reinterpret_cast<std::byte*>(aligned), maximum);
 RgbImageResizer supplied(1, perceptual), owned(1, perceptual);
 // Repeated and shuffled geometry must not depend on retained pixels, old
 // coefficient contents, or an allocator from a prior arena lease.
 for (const auto i : {0U, 5U, 1U, 4U, 6U, 2U, 3U, 5U, 0U}) {
  const auto& d = geometries[i];
  const auto source = make_test_image(d[0], d[1]);
  std::vector<float> actual(d[2] * d[3] * 3, -19), expected(actual);
  const RgbConstImageView input{source.data(), {d[0], d[1], d[0] * 3, 0, source.size(), RgbPixelFormat::RGB8}};
  const RgbImageLayout layout{d[2], d[3], d[2] * sizeof(float), d[2] * d[3] * sizeof(float), actual.size() * sizeof(float), RgbPixelFormat::PlanarUnitSrgbF32};
  std::fill(memory.begin(), memory.end(), std::byte{0xA7});
  supplied.prepare(plans[i], memory);
  RgbResizeWork observed;
  supplied.resize_to_planar(input, {actual.data(), layout}, mode, &observed);
  owned.resize_to_planar(input, {expected.data(), layout}, mode);
  CHECK(actual == expected);
  CHECK(supplied.retained_bytes() == plans[i].bytes());
  CHECK(observed.preparations == (plans[i].path() == RgbResizePath::Identity ? 0 : 1));
  CHECK(observed.allocation_bytes <= plans[i].bytes());
  CHECK((observed.allocation_requests > 0) == (plans[i].path() == RgbResizePath::Avir));
  // All writes stay within the admitted range, despite a larger supplied span.
  CHECK(std::all_of(memory.begin() + plans[i].bytes(), memory.end(), [](auto byte) { return byte == std::byte{0xA7}; }));
  auto changed = input;
  --changed.layout.width;
  if (changed.layout.width) {
   changed.layout.row_stride_bytes -= 3;
   std::fill(actual.begin(), actual.end(), -19);
   CHECK_THROWS_AS(supplied.resize_to_planar(changed, {actual.data(), layout}, mode), std::invalid_argument);
   CHECK(std::ranges::all_of(actual, [](auto value) { return value == -19; }));
  }
  if (plans[i].bytes()) {
   CHECK_THROWS_AS(supplied.prepare(plans[i], memory.first(plans[i].bytes() - 1)), std::length_error);
   supplied.prepare(plans[i], memory);
   supplied.resize_to_planar(input, {actual.data(), layout}, mode);
   CHECK(actual == expected);
  }
 }
}
TEST_CASE("SIMD final rows retain scalar patch reconstruction and output bits", "[backend][data][image_resize][perceptual]") {
 using namespace test_perceptual;
 using Access = perceptual::CpuDownscalerTestAccess;
 const bool tight = GENERATE(false, true);
 for (auto format : formats)
  for (unsigned width : {1U, 3U, 7U, 8U, 9U, 15U, 16U, 17U})
   for (unsigned height : {1U, 5U})
    for (unsigned extra : {0U, 1U})
     for (unsigned pattern : {0U, 3U, 5U, 7U}) {
      CAPTURE(static_cast<int>(format), width, height, extra, pattern, tight);
      Image source(width * 2 + extra, height * 2 + extra, format, tight ? 0 : 3), actual(width, height, format, tight ? 0 : 5), scalar(width, height, format, tight ? 0 : 5);
      // Image's usual byte rows are float-aligned. Exercise legal tightly
      // packed RGB rows as well, including a last three-byte source pixel.
      if (tight && format == RgbPixelFormat::RGB8)
       for (auto* image : {&source, &actual, &scalar}) {
        image->layout.row_stride_bytes = image->layout.width * 3;
        image->layout.capacity_bytes = image->layout.row_stride_bytes * image->layout.height;
       }
      source.fill(pattern);
      auto expected = actual;
      perceptual::CpuDownscaler owner, owned;
      owned.run(source.read(), expected.write());
      const bool alpha = format == RgbPixelFormat::RGBA8;
      const auto bytes = perceptual::CpuDownscaler::workspace_bytes(width, height, alpha);
      std::vector<std::byte> backing(bytes + 127);
      const auto aligned = (reinterpret_cast<std::uintptr_t>(backing.data()) + 63) / 64 * 64;
      const std::span memory(reinterpret_cast<std::byte*>(aligned), bytes);
      for (const auto poison : {std::byte{0xA7}, std::byte{0xFF}}) {
       std::ranges::fill(backing, poison);
       owner.bind(width, height, alpha, memory);
       owner.run(source.read(), actual.write());
       CHECK(std::memcmp(actual.storage.data(), expected.storage.data(), actual.storage.size() * sizeof(float)) == 0);
       CHECK(std::all_of(backing.data(), memory.data(), [=](auto value) { return value == poison; }));
       CHECK(std::all_of(memory.data() + memory.size(), backing.data() + backing.size(), [=](auto value) { return value == poison; }));
      }
      const auto compare_final_row = [&](const Image& result, const Image& reference) {
       const auto layout = result.layout;
       const unsigned planes = layout.format == RgbPixelFormat::PlanarUnitSrgbF32 ? 3 : 1;
       const std::size_t row_bytes = width * (layout.format == RgbPixelFormat::RGB8 ? 3 : 4);
       for (unsigned plane = 0; plane < planes; ++plane) {
        CAPTURE(plane);
        const auto offset = plane * layout.plane_stride_bytes + (height - 1) * layout.row_stride_bytes;
        CHECK(std::memcmp(static_cast<const std::byte*>(result.read().data) + offset, static_cast<const std::byte*>(reference.read().data) + offset, row_bytes) == 0);
       }
      };
      Access::scalar_final_row(owner, scalar.write());
      compare_final_row(actual, scalar);
      CHECK(padding_intact(source));
      CHECK(padding_intact(actual));
      const auto* end = reinterpret_cast<const std::uint8_t*>(actual.storage.data());
      CHECK(std::all_of(end + actual.layout.capacity_bytes, end + actual.storage.size() * sizeof(float), [](auto byte) { return byte == 0xCD; }));
      if (format == RgbPixelFormat::RGB8) {
       Image planar(width, height, RgbPixelFormat::PlanarUnitSrgbF32, tight ? 0 : 5), scalar_planar(planar);
       owner.run_quantized_planar(source.read(), planar.write());
       Access::scalar_final_row(owner, scalar_planar.write(), true);
       compare_final_row(planar, scalar_planar);
       for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
         for (unsigned k = 0; k < 3; ++k) {
          const auto code = static_cast<unsigned>(std::lround(actual.at(x, y, k) * 255.0));
          CHECK(std::bit_cast<std::uint32_t>(static_cast<float>(planar.at(x, y, k))) == std::bit_cast<std::uint32_t>(float(code) * (1.0F / 255.0F)));
         }
       CHECK(padding_intact(planar));
      }
     }
}
namespace {
class ObservedAvirWorkspace final : public avir::detail::CWorkspaceAllocator {
public:
 std::size_t requests = 0, bytes = 0;
 std::size_t fail_request = std::numeric_limits<std::size_t>::max();
 char* acquire(std::size_t count, bool) override {
  if (requests++ == fail_request) throw std::bad_alloc();
  bytes += (count + 63) / 64 * 64;
  return new char[count];
 }
 void release(char* pointer, std::size_t) override { delete[] pointer; }
};
}  // namespace
TEST_CASE("AVIR checked layout matches all execution reservations and preparation is fallible before stores", "[backend][data][image_resize][workspace]") {
 using Resizer = avir::CImageResizer<avir::fpclass_float4>;
 for (const auto& d : std::array<std::array<int, 4>, 5>{{{19, 3, 8, 9}, {17, 13, 9, 7}, {18, 14, 9, 7}, {1, 19, 1, 8}, {8, 7, 17, 13}}}) {
  const auto source = make_test_image(d[0], d[1]);
  std::vector<std::uint8_t> output(std::size_t(d[2]) * d[3] * 3), expected(output);
  Resizer{}.resizeImage(source.data(), d[0], d[1], 0, expected.data(), d[2], d[3], 3, 0.0);
  const auto layout = Resizer::workspaceBytes<std::uint8_t, std::uint8_t>(d[0], d[1], d[2], d[3], 3);
  ObservedAvirWorkspace successful;
  {
   avir::detail::CWorkspaceScope scope(successful);
   Resizer prepared;
   prepared.resizeImage(source.data(), d[0], d[1], 0, output.data(), d[2], d[3], 3, 0.0);
  }
  CHECK(output == expected);
  CHECK(successful.bytes == layout);
  REQUIRE(successful.requests > 1);
  for (const auto fail : {std::size_t{0}, successful.requests - 1}) {
   std::fill(output.begin(), output.end(), 179);
   ObservedAvirWorkspace failing;
   failing.fail_request = fail;
   {
    avir::detail::CWorkspaceScope scope(failing);
    CHECK_THROWS_AS(([&] {
     Resizer prepared;
     prepared.resizeImage(source.data(), d[0], d[1], 0, output.data(), d[2], d[3], 3, 0.0);
    }()),
     std::bad_alloc);
   }
   CHECK(std::ranges::all_of(output, [](auto value) { return value == 179; }));
   Resizer{}.resizeImage(source.data(), d[0], d[1], 0, output.data(), d[2], d[3], 3, 0.0);
   CHECK(output == expected);
  }
 }
}
TEST_CASE("explicit Torch RGB methods preserve geometry float output and bounded workspace", "[image_resize][torch_resize][workspace]") {
 CHECK_THROWS_AS(RgbImageResizer::require_method(static_cast<RgbResizeMethod>(255)), std::invalid_argument);
 const auto method = GENERATE(RgbResizeMethod::TorchBilinear, RgbResizeMethod::TorchBilinearAntialias);
#if !defined(MMLTK_TEST_TORCH_RGB_RESIZE)
 CHECK_THROWS_AS(RgbImageResizer::require_method(method), std::runtime_error);
 CHECK_NOTHROW(RgbImageResizer::require_method(RgbResizeMethod::Native));
#else
 const auto mode = GENERATE(ImageResizeMode::Stretch, ImageResizeMode::Letterbox);
 const auto source = make_test_image(17, 13);
 CHECK_THROWS(RgbImageResizer::workspace_plan(UINT32_MAX, UINT32_MAX, 1, 1, mode, false, RgbPixelFormat::RGB8, nullptr, method));
 for (const auto size : {std::array{17U, 13U}, std::array{7U, 5U}, std::array{27U, 19U}, std::array{1U, 1U}}) {
  const auto width = size[0], height = size[1];
  const auto plan = RgbImageResizer::workspace_plan(17, 13, width, height, mode, true, RgbPixelFormat::RGB8, nullptr, method);
  const auto geometry = compute_image_resize_geometry(17, 13, width, height, mode);
  CHECK(plan.method() == method);
  if (plan.path() != RgbResizePath::Identity) {
   CHECK(plan.path() == RgbResizePath::Torch);
   CHECK(plan.bytes() == (17U * 13U + geometry.resized_width * geometry.resized_height) * 3U * sizeof(float));
   CHECK(plan.opaque_bytes() > plan.bytes());
  } else {
   CHECK(plan.bytes() == 0);
   CHECK(plan.opaque_bytes() == 0);
  }
  std::vector<std::byte> allocation(plan.bytes() + 128, std::byte{0x7D});
  auto* start = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(allocation.data()) + 63U) & ~std::uintptr_t{63U});
  const std::span memory(start, plan.bytes());
  std::vector<float> actual(width * height * 3, -1), expected(actual.size());
  const RgbImageLayout layout{width, height, width * sizeof(float), width * height * sizeof(float), actual.size() * sizeof(float), RgbPixelFormat::PlanarUnitSrgbF32};
  const RgbConstImageView input{source.data(), {17, 13, 17 * 3, 0, source.size(), RgbPixelFormat::RGB8}};
  RgbImageResizer supplied, owned(1, false, method);
  supplied.prepare(plan, memory);
  supplied.resize_to_planar(input, {actual.data(), layout}, mode);
  owned.resize_to_planar(input, {expected.data(), layout}, mode);
  CHECK(actual == expected);  // Stored perceptual choice cannot alter a Torch method.
  if (plan.path() != RgbResizePath::Identity && geometry.resized_width > 1 && geometry.resized_height > 1)
   CHECK(std::ranges::any_of(actual, [](float value) { return std::abs(value * 255.F - std::round(value * 255.F)) > 1e-3F; }));
  CHECK(std::ranges::all_of(actual, [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }));
  CHECK(supplied.retained_bytes() == plan.bytes());
  CHECK(std::ranges::all_of(std::span(start + plan.bytes(), 32), [](std::byte value) { return value == std::byte{0x7D}; }));
  for (unsigned channel = 0; channel < 3; ++channel)
   for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x)
     if (x < geometry.offset_x || y < geometry.offset_y || x >= geometry.offset_x + geometry.resized_width || y >= geometry.offset_y + geometry.resized_height)
      CHECK(actual[channel * width * height + y * width + x] == 0.F);
  if (plan.bytes()) CHECK_THROWS_AS(supplied.prepare(plan, memory.first(plan.bytes() - 1)), std::length_error);
  if (plan.path() == RgbResizePath::Identity) {
   rgb_hwc_u8_to_nchw_f32(source.data(), expected.data(), 17, 13);
   CHECK(actual == expected);
  }
 }
#endif
}
#if defined(MMLTK_TEST_TORCH_RGB_RESIZE)
TEST_CASE("Torch RGB workers retain independent local CPU limits", "[image_resize][torch_resize][concurrency]") {
 const auto work = [](int threads) {
  (void)at::get_num_threads();
  const int previous = omp_get_max_threads();
  struct Restore {
   int value;
   ~Restore() { omp_set_num_threads(value); }
  } restore{previous};
  omp_set_num_threads(threads);
  const auto source = make_test_image(53, 37);
  std::vector<float> result(17 * 11 * 3);
  RgbImageResizer resizer(1, false, RgbResizeMethod::TorchBilinearAntialias);
  for (unsigned pass = 0; pass < 3; ++pass) {
   resizer.resize_to_planar({source.data(), {53, 37, 159, 0, source.size(), RgbPixelFormat::RGB8}},
    {result.data(), {17, 11, 17 * sizeof(float), 17 * 11 * sizeof(float), result.size() * sizeof(float), RgbPixelFormat::PlanarUnitSrgbF32}}, ImageResizeMode::Stretch);
   if (at::get_num_threads() != threads) throw std::runtime_error("Torch resize changed caller CPU limit");
  }
  return result;
 };
 auto first = std::async(std::launch::async, work, 2);
 auto second = std::async(std::launch::async, work, 3);
 CHECK(first.get() == second.get());
}
#endif
