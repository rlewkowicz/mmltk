#include "src/backend/data/image_resize.h"
#include "src/backend/data/tests/perceptual_downscale_reference.h"
#include "src/backend/data/detail/perceptual_downscale_math.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <limits>
#include <cmath>
#include <bit>
#include <vector>

using namespace mmltk::backend::data;

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
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (size_t index = 1; index < outputs.size(); ++index) {
        REQUIRE(outputs[index] == outputs[0]);
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
    for (auto format:formats) for (const auto& dims:geometries) for (unsigned pattern=0;pattern<8;++pattern) {
        INFO("format "<<static_cast<int>(format)<<" source "<<dims[0]<<"x"<<dims[1]<<" destination "<<dims[2]<<"x"<<dims[3]<<" pattern "<<pattern);
        Image source(dims[0],dims[1],format,3),output(dims[2],dims[3],format,5);
        source.fill(pattern);
        const auto expected=reference(source,dims[2],dims[3],5);
        resizer.downscale(source.read(),output.write());
        REQUIRE(maximum_error(output,expected)<=(format==RgbPixelFormat::PlanarUnitSrgbF32 ? 2e-6:1.0/255+1e-12));
        REQUIRE(padding_intact(output));
        const auto retained=output.storage;
        resizer.downscale(source.read(),output.write());
        REQUIRE(output.storage==retained);
    }
}
TEST_CASE("optional perceptual selection preserves default AVIR and enlargement", "[backend][data][image_resize][perceptual]") {
    const auto source=make_test_image(17,13);
    RgbImageResizer defaults,disabled(1,false),enabled(1,true);
    for (const auto& dims:std::array<std::array<int,2>,3>{{{7,5},{25,19},{17,13}}}) {
        std::vector<std::uint8_t> a(std::size_t(dims[0])*dims[1]*3),b(a.size()),c(a.size());
        defaults.resize(source.data(),17,13,a.data(),dims[0],dims[1]);
        disabled.resize(source.data(),17,13,b.data(),dims[0],dims[1]);
        enabled.resize(source.data(),17,13,c.data(),dims[0],dims[1]);
        REQUIRE(a==b);
        if (dims[0]>=17) REQUIRE(a==c);
        else REQUIRE(a!=c);
    }
}
TEST_CASE("perceptual alpha coverage ignores hidden saturated color", "[backend][data][image_resize][perceptual]") {
    using namespace test_perceptual;
    Image source(3,1,RgbPixelFormat::RGBA8),output(1,1,RgbPixelFormat::RGBA8);
    for (unsigned x=0;x<3;++x) {
        source.set(x,0,0,x==0 ? 1:0);source.set(x,0,1,x==1 ? 1:0);source.set(x,0,2,x==2 ? 1:0);
        source.set(x,0,3,x==0 ? 1:0);
    }
    RgbImageResizer resizer;
    resizer.downscale(source.read(),output.write());
    REQUIRE(output.at(0,0,0)>=254.0/255);REQUIRE(output.at(0,0,1)<=1.0/255);REQUIRE(output.at(0,0,2)<=1.0/255);
    REQUIRE(output.at(0,0,3)==85.0/255);
    source.set(0,0,3,0);
    resizer.downscale(source.read(),output.write());
    for (unsigned k=0;k<4;++k) REQUIRE(output.at(0,0,k)==0);
}
TEST_CASE("perceptual transfer thresholds are monotonic and continuously accurate", "[backend][data][image_resize][perceptual]") {
    perceptual::TransferTable table;
    for (unsigned i=0;i<256;++i) table.linear[i]=perceptual::decode(float(i)*(1.0F/255.0F));
    unsigned previous=0;
    for (unsigned i=0;i<=65536;++i) {
        const double linear=double(i)/65536;
        const unsigned code=table.quantize(linear);
        REQUIRE(code>=previous);previous=code;
        REQUIRE(std::abs(double(code)-test_perceptual::oetf(linear)*255)<1.0001);
        REQUIRE(std::abs(perceptual::encode(static_cast<float>(linear))-test_perceptual::oetf(linear))<2e-7);
    }
    for (double v:{0.0,0.040449,0.04045,0.040451,1.0})
        REQUIRE(std::abs(perceptual::decode(static_cast<float>(v))-test_perceptual::eotf(v))<2e-7);
}
TEST_CASE("perceptual low variance keeps the specified ratio-two threshold", "[backend][data][image_resize][perceptual]") {
    for (float variance:{0.0F,0.999e-6F,1.001e-6F}) {
        const float ratio=perceptual::contrast_ratio(variance,variance*8);
        REQUIRE(std::abs(ratio-(variance<1e-6F ? 2.0F:3.0F))<1e-6F);
    }
    using namespace test_perceptual;
    RgbImageResizer resizer;
    for (double delta:{0.001999,0.002001}) {
        Image source(4,1,RgbPixelFormat::PlanarUnitSrgbF32),output(2,1,RgbPixelFormat::PlanarUnitSrgbF32);
        for (unsigned x=0;x<4;++x) for (unsigned k=0;k<3;++k)
            source.set(x,0,k,oetf(0.3+(x>=2 ? delta:0)+(x%2 ? 0.01:-0.01)));
        resizer.downscale(source.read(),output.write());
        REQUIRE(maximum_error(output,reference(source,2,1))<2e-6);
        auto simd_source=threshold_source(34,6,delta);
        Image simd_output(17,3,RgbPixelFormat::PlanarUnitSrgbF32);
        resizer.downscale(simd_source.read(),simd_output.write());
        REQUIRE(maximum_error(simd_output,reference(simd_source,17,3))<2e-6);
    }
}
TEST_CASE("perceptual checked views reject unsafe geometry before writing", "[backend][data][image_resize][perceptual]") {
    using namespace test_perceptual;
    Image source(17,13,RgbPixelFormat::RGB8),output(7,5,RgbPixelFormat::RGB8);
    source.fill(5);
    RgbImageResizer resizer;
    auto bad=source.read();
    bad.layout.capacity_bytes=1;REQUIRE_THROWS(resizer.downscale(bad,output.write()));
    bad=source.read();bad.layout.row_stride_bytes=1;REQUIRE_THROWS(resizer.downscale(bad,output.write()));
    bad=source.read();bad.layout.width=0;REQUIRE_THROWS(resizer.downscale(bad,output.write()));
    bad=source.read();bad.data=nullptr;REQUIRE_THROWS(resizer.downscale(bad,output.write()));
    bad=source.read();bad.layout.row_stride_bytes=std::numeric_limits<std::size_t>::max();REQUIRE_THROWS(resizer.downscale(bad,output.write()));
    auto overlap=output.write();overlap.data=source.storage.data();REQUIRE_THROWS(resizer.downscale(source.read(),overlap));
    REQUIRE_NOTHROW(resizer.downscale(source.read(),source.write()));
    REQUIRE_THROWS(resizer.downscale(output.read(),source.write()));
    Image floats(17,13,RgbPixelFormat::PlanarUnitSrgbF32);
    auto float_bad=floats.read();float_bad.layout.plane_stride_bytes=4;REQUIRE_THROWS(resizer.downscale(float_bad,output.write()));
    float_bad=floats.read();float_bad.data=reinterpret_cast<const std::uint8_t*>(floats.storage.data())+1;REQUIRE_THROWS(resizer.downscale(float_bad,output.write()));
    REQUIRE(std::all_of(output.storage.begin(),output.storage.end(),[](float v){return std::bit_cast<std::uint32_t>(v)==0xCDCDCDCDU;}));
}
TEST_CASE("perceptual float unit-domain sanitation remains finite", "[backend][data][image_resize][perceptual]") {
    using namespace test_perceptual;
    Image source(5,1,RgbPixelFormat::PlanarUnitSrgbF32),output(2,1,RgbPixelFormat::PlanarUnitSrgbF32);
    constexpr double values[]{-1,std::numeric_limits<double>::quiet_NaN(),0.04045,2,std::numeric_limits<double>::infinity()};
    for(unsigned x=0;x<5;++x) for(unsigned k=0;k<3;++k) source.set(x,0,k,values[x]);
    RgbImageResizer resizer;
    resizer.downscale(source.read(),output.write());
    REQUIRE(maximum_error(output,reference(source,2,1))<2e-6);
    for(unsigned x=0;x<2;++x) for(unsigned k=0;k<3;++k) {
        REQUIRE(std::isfinite(output.at(x,0,k)));REQUIRE(output.at(x,0,k)>=0);REQUIRE(output.at(x,0,k)<=1);
    }
}
