#include <catch2/catch_test_macros.hpp>
#include <array>
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
    } input;
    Input* device = nullptr;
    REQUIRE(cudaMalloc(reinterpret_cast<void**>(&device), sizeof(Input)) == cudaSuccess);
    auto release = [](Input* value) { static_cast<void>(cudaFree(value)); };
    std::unique_ptr<Input, decltype(release)> storage(device, release);
    const auto draw = [&](bool add) {
        REQUIRE(cudaMemcpyAsync(device, &input, sizeof(input), cudaMemcpyHostToDevice, stream.get()) == cudaSuccess);
        REQUIRE(raster::raster_instance_overlay_rgba({
            .overlay = {reinterpret_cast<std::uint8_t*>(device), 16U, 4, 1},
            .instances = {reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, boxes)),
                          reinterpret_cast<const std::uint8_t*>(device) + offsetof(Input, colors),
                          reinterpret_cast<const int*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, labels)), 2},
            .masks = reinterpret_cast<const bool*>(reinterpret_cast<std::uint8_t*>(device) + offsetof(Input, masks)),
            .mask_alpha = 96U, .box_thickness = 0, .stream = {stream.get()}, .labels = false, .add_rgb_to_existing = add}) == cudaSuccess);
        std::array<std::uint8_t, 16> output{};
        REQUIRE(cudaMemcpyAsync(output.data(), device, output.size(), cudaMemcpyDeviceToHost, stream.get()) == cudaSuccess);
        REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
        return output;
    };
    CHECK(draw(true) == std::array<std::uint8_t, 16>{255, 255, 255, 96, 255, 60, 40, 177, 3, 4, 5, 81, 10, 20, 30, 96});
    CHECK(draw(false) == std::array<std::uint8_t, 16>{10, 20, 30, 96, 10, 20, 30, 96, 0, 0, 0, 0, 10, 20, 30, 96});
}
