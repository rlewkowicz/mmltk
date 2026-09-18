#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/imaging/resample/tests/perceptual_downscale_reference.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale_math.h"
#include "src/backend/imaging/resample/detail/perceptual_downscale.h"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
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
namespace mmltk::backend::imaging::resample::perceptual {
struct CpuDownscalerTestAccess {
    using Step = CpuDownscaler::PreparationStep;
    using Storage = std::array<std::pair<const void*, std::size_t>, 8>;
    static void fail_before(CpuDownscaler& owner, Step step) { owner.fail_before_ = step; }
    static bool prepared(const CpuDownscaler& owner) { return owner.prepared_; }
    static bool armed(const CpuDownscaler& owner) { return owner.fail_before_ != Step::None; }
    static std::size_t horizontal_size(const CpuDownscaler& owner) { return owner.x_.size(); }
    static Storage storage(const CpuDownscaler& owner) {
        return {{{owner.x_.data(), owner.x_.capacity()},
                 {owner.y_.data(), owner.y_.capacity()},
                 {owner.moments_[0].data(), owner.moments_[0].capacity()},
                 {owner.moments_[1].data(), owner.moments_[1].capacity()},
                 {owner.coefficients_[0].data(), owner.coefficients_[0].capacity()},
                 {owner.coefficients_[1].data(), owner.coefficients_[1].capacity()},
                 {owner.alpha_[0].data(), owner.alpha_[0].capacity()},
                 {owner.alpha_[1].data(), owner.alpha_[1].capacity()}}};
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
    std::vector<std::vector<uint8_t>> outputs(8, std::vector<uint8_t>(static_cast<size_t>(332) * static_cast<size_t>(332) * 3U));
    std::vector<std::thread> threads;
    threads.reserve(outputs.size());
    for (auto& output : outputs) {
        threads.emplace_back([&source, &output] {
            RgbImageResizer resizer(1);
            resizer.resize(source.data(), 211, 157, output.data(), 332, 332);
        });
    }
    for (std::thread& thread : threads) { thread.join(); }
    for (size_t index = 1; index < outputs.size(); ++index) { REQUIRE(outputs[index] == outputs[0]); }
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
                INFO("format " << static_cast<int>(format) << " source " << dims[0] << "x" << dims[1] << " destination " << dims[2] << "x" << dims[3]
                               << " pattern " << pattern);
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
namespace {
void check_prepared_pixels(perceptual::CpuDownscaler& resizer, const test_perceptual::Image& source, test_perceptual::Image& output) {
    using namespace test_perceptual;
    const auto expected = reference(source, output.layout.width, output.layout.height, 5);
    resizer.run(source.read(), output.write());
    REQUIRE(maximum_error(output, expected) <= reference_tolerance(source.layout.format));
    REQUIRE(padding_intact(output));
    REQUIRE(perceptual::CpuDownscalerTestAccess::prepared(resizer));
}
void check_preparation_recovery(RgbPixelFormat prior_format, RgbPixelFormat requested_format, const std::array<unsigned, 4>& requested,
                                perceptual::CpuDownscalerTestAccess::Step failure, bool prior_first) {
    using namespace test_perceptual;
    using Access = perceptual::CpuDownscalerTestAccess;
    INFO("prior format " << static_cast<int>(prior_format) << " requested format " << static_cast<int>(requested_format) << " geometry " << requested[0] << "x"
                         << requested[1] << " -> " << requested[2] << "x" << requested[3] << " boundary " << static_cast<int>(failure) << " prior first "
                         << prior_first);
    perceptual::CpuDownscaler resizer;
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
    constexpr std::array steps{Step::HorizontalAxis,   Step::VerticalAxis,      Step::FirstMoment, Step::SecondMoment,
                               Step::FirstCoefficient, Step::SecondCoefficient, Step::FirstAlpha,  Step::SecondAlpha};
    constexpr std::array<std::array<unsigned, 4>, 2> requests{{{19, 101, 7, 99}, {29, 23, 19, 17}}};
    for (auto prior : test_perceptual::formats)
        for (auto requested : test_perceptual::formats)
            for (const auto& geometry : requests)
                for (auto step : steps)
                    for (bool prior_first : {false, true}) {
                        if (requested != RgbPixelFormat::RGBA8 && (step == Step::FirstAlpha || step == Step::SecondAlpha)) continue;
                        check_preparation_recovery(prior, requested, geometry, step, prior_first);
                    }
}
TEST_CASE("perceptual CPU first alpha preparation invalidates an unchanged geometry on failure", "[backend][data][image_resize][perceptual]") {
    using Step = perceptual::CpuDownscalerTestAccess::Step;
    for (auto prior : {RgbPixelFormat::RGB8, RgbPixelFormat::PlanarUnitSrgbF32})
        for (auto step : {Step::FirstAlpha, Step::SecondAlpha})
            for (bool prior_first : {false, true}) check_preparation_recovery(prior, RgbPixelFormat::RGBA8, {17, 13, 9, 7}, step, prior_first);
}
TEST_CASE("perceptual CPU identity copies preserve exact pixels and padding in every format", "[backend][data][image_resize][perceptual]") {
    using namespace test_perceptual;
    RgbImageResizer resizer;
    for (auto format : formats) {
        Image source(17, 13, format, 3), output(17, 13, format, 5);
        source.fill(7);
        const auto original = source.storage;
        resizer.downscale(source.read(), source.write());
        REQUIRE(std::memcmp(source.storage.data(), original.data(), original.size() * sizeof(float)) == 0);
        resizer.downscale(source.read(), output.write());
        REQUIRE(maximum_error(output, source) == 0);
        REQUIRE(padding_intact(output));
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
        auto simd_source = threshold_source(34, 6, delta);
        Image simd_output(17, 3, RgbPixelFormat::PlanarUnitSrgbF32);
        resizer.downscale(simd_source.read(), simd_output.write());
        REQUIRE(maximum_error(simd_output, reference(simd_source, 17, 3)) < 2e-6);
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
    Image source(5, 1, RgbPixelFormat::PlanarUnitSrgbF32), output(2, 1, RgbPixelFormat::PlanarUnitSrgbF32);
    constexpr double values[]{-1, std::numeric_limits<double>::quiet_NaN(), 0.04045, 2, std::numeric_limits<double>::infinity()};
    for (unsigned x = 0; x < 5; ++x)
        for (unsigned k = 0; k < 3; ++k) source.set(x, 0, k, values[x]);
    RgbImageResizer resizer;
    resizer.downscale(source.read(), output.write());
    REQUIRE(maximum_error(output, reference(source, 2, 1)) < 2e-6);
    for (unsigned x = 0; x < 2; ++x)
        for (unsigned k = 0; k < 3; ++k) {
            REQUIRE(std::isfinite(output.at(x, 0, k)));
            REQUIRE(output.at(x, 0, k) >= 0);
            REQUIRE(output.at(x, 0, k) <= 1);
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
    CHECK(stretch.resized_width == 31U); CHECK(stretch.resized_height == 29U);
    CHECK(stretch.offset_x == 0U); CHECK(stretch.offset_y == 0U);
    const auto letterbox = compute_image_resize_geometry(65, 49, 31, 29, ImageResizeMode::Letterbox);
    CHECK(letterbox.resized_width == 31U); CHECK(letterbox.resized_height == 23U);
    CHECK(letterbox.offset_x == 0U); CHECK(letterbox.offset_y == 3U);
    const auto thin = compute_image_resize_geometry(10000, 1, 8, 8, ImageResizeMode::Letterbox);
    CHECK(thin.resized_height == 1U);
    CHECK_THROWS(compute_image_resize_geometry(0, 1, 8, 8, ImageResizeMode::Stretch));
    CHECK_THROWS(compute_image_resize_geometry(1, 1, 8, 8, static_cast<ImageResizeMode>(255)));
}
