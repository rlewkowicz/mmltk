#include "src/backend/imaging/upscale/detail/image_upscaler_cuda.h"
#include "src/backend/imaging/upscale/detail/shiftlut_onnx_ops.h"
#include "shiftlut_reference.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cuda_runtime_api.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

class DeviceBuffer final {
   public:
    explicit DeviceBuffer(std::size_t bytes) { REQUIRE(cudaMalloc(&data_, bytes) == cudaSuccess); }
    ~DeviceBuffer() {
        if (cudaFree(data_) != cudaSuccess) std::terminate();
    }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    template <typename T> T* as() noexcept { return static_cast<T*>(data_); }
   private:
    void* data_ = nullptr;
};

class Stream final {
   public:
    Stream() { REQUIRE(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking) == cudaSuccess); }
    ~Stream() {
        if (cudaStreamSynchronize(stream_) != cudaSuccess || cudaStreamDestroy(stream_) != cudaSuccess) std::terminate();
    }
    cudaStream_t get() const noexcept { return stream_; }
   private:
    cudaStream_t stream_ = nullptr;
};

int reflected(int coordinate, int extent) {
    if (extent == 1) return 0;
    const int period = (extent - 1) * 2;
    coordinate = (coordinate % period + period) % period;
    return coordinate < extent ? coordinate : period - coordinate;
}

std::vector<std::uint8_t> oracle(const char* kind, std::uint32_t width, std::uint32_t height, int pattern,
                               std::size_t bytes) {
    const auto path = std::filesystem::path(MMLTK_TEST_SOURCE_ROOT) / "build/validation/atlas-viewer-residency/shiftlut" /
        (std::string(kind) + "_" + std::to_string(width) + "_" + std::to_string(height) + "_" +
         std::to_string(pattern) + ".rgba");
    INFO("Independent upstream oracle required: " << path << "; run ./mmltk --export-shiftlut --source ../ShiftLUT --verify-only");
    REQUIRE(std::filesystem::is_regular_file(path));
    REQUIRE(std::filesystem::file_size(path) == bytes);
    std::vector<std::uint8_t> result(bytes);
    std::ifstream input(path, std::ios::binary);
    REQUIRE(static_cast<bool>(input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(bytes))));
    return result;
}

}  // namespace

TEST_CASE("Direct neural tile preparation preserves FP32 lookup decisions and pitched crop borders", "[upscale_gpu]") {
    namespace tiles = mmltk::backend::imaging::upscale::image_upscaler_cuda;
    const auto width = GENERATE(1U, 3U, 19U, 257U);
    const auto height = GENERATE(1U, 5U);
    const bool lut = GENERATE(false, true);
    const std::size_t pitch = width * 4U + 37U;
    std::vector<std::uint8_t> source(pitch * height, 0xD7);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            for (std::uint32_t c = 0; c < 4; ++c)
                source[y * pitch + x * 4 + c] = static_cast<std::uint8_t>((y * 97 + x * 13 + c * 71) % 256);
    DeviceBuffer device_source(source.size());
    DeviceBuffer device_tile(3U * 256U * 256U * sizeof(float));
    DeviceBuffer device_reference(3U * width * height * sizeof(float));
    Stream stream;
    REQUIRE(cudaMemcpy(device_source.as<void>(), source.data(), source.size(), cudaMemcpyHostToDevice) == cudaSuccess);
    mmltk::backend::imaging::upscale::tests::normalize_reference(device_source.as<std::uint8_t>(), pitch, width, height,
                                                              device_reference.as<float>(), stream.get());
    const std::uint32_t halo = lut ? 32U : 16U;
    const std::uint32_t crop_x = width > 1 ? 1U : 0U;
    const auto crop_width = width - crop_x;
    const auto origin = crop_width > 192 ? 192U : 0U;
    const tiles::Tile tile{.origin_x = origin, .origin_y = 0,
                           .core_width = std::min(192U, crop_width - origin), .core_height = height};
    tiles::prepare_tile(device_source.as<std::uint8_t>(), pitch, width, height, crop_x, 0,
                        crop_width, height, tile, lut, halo, device_tile.as<float>(), stream.get());
    REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
    std::vector<float> actual(3U * 256U * 256U);
    REQUIRE(cudaMemcpy(actual.data(), device_tile.as<void>(), actual.size() * sizeof(float), cudaMemcpyDeviceToHost) == cudaSuccess);
    std::vector<float> reference(static_cast<std::size_t>(3U) * width * height);
    REQUIRE(cudaMemcpy(reference.data(), device_reference.as<void>(), reference.size() * sizeof(float), cudaMemcpyDeviceToHost) == cudaSuccess);
    constexpr std::array means{0.485F, 0.456F, 0.406F};
    constexpr std::array deviations{0.229F, 0.224F, 0.225F};
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto channel = index / (256U * 256U);
        const auto x = reflected(static_cast<int>(tile.origin_x + index % 256U) - static_cast<int>(halo),
                                 static_cast<int>(crop_width)) + static_cast<int>(crop_x);
        const auto y = reflected(static_cast<int>(index / 256U % 256U) - static_cast<int>(halo), static_cast<int>(height));
        const float byte = source[static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4 + channel];
        const float normalized = std::fma(byte, 1.0F / 255.0F, -means[channel]) / deviations[channel];
        REQUIRE(normalized == reference[channel * width * height + static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)]);
        const float expected = std::clamp(std::fma(normalized, deviations[channel], means[channel]), 0.0F, 1.0F) * (lut ? 255.0F : 1.0F);
        REQUIRE(actual[index] == expected);
        REQUIRE(std::floor(actual[index] / 4.0F) == std::floor(expected / 4.0F));
    }
    std::vector<std::uint8_t> unchanged(source.size());
    REQUIRE(cudaMemcpy(unchanged.data(), device_source.as<void>(), unchanged.size(), cudaMemcpyDeviceToHost) == cudaSuccess);
    CHECK(unchanged == source);
}

TEST_CASE("Direct tile stitching preserves rounding ties alpha seams and pitched guard bytes", "[upscale_gpu]") {
    namespace tiles = mmltk::backend::imaging::upscale::image_upscaler_cuda;
    const bool lut = GENERATE(false, true);
    constexpr std::uint32_t width = 196, height = 12;
    constexpr std::size_t pitch = width * 4 + 23;
    constexpr std::size_t tile_plane = 1024U * 1024U;
    std::vector<float> model(3U * tile_plane);
    for (std::size_t index = 0; index < model.size(); ++index) {
        const float value = static_cast<float>(index % 515) * 0.5F - 1.0F;
        model[index] = lut ? value : value / 255.0F;
    }
    std::vector<std::uint8_t> result(pitch * height + 61, 0xA9);
    DeviceBuffer input(model.size() * sizeof(float));
    DeviceBuffer target(result.size());
    Stream stream;
    REQUIRE(cudaMemcpy(input.as<void>(), model.data(), model.size() * sizeof(float), cudaMemcpyHostToDevice) == cudaSuccess);
    REQUIRE(cudaMemcpy(target.as<void>(), result.data(), result.size(), cudaMemcpyHostToDevice) == cudaSuccess);
    constexpr std::array<tiles::Tile, 2> layout{{
        {.origin_x = 0, .origin_y = 0, .core_width = 47, .core_height = 3},
        {.origin_x = 47, .origin_y = 0, .core_width = 2, .core_height = 3},
    }};
    const auto halo = lut ? 32U : 16U;
    for (const auto tile : layout)
        tiles::stitch_tile(input.as<float>(), tile, lut, halo, target.as<std::uint8_t>(), pitch, width, height, stream.get());
    REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
    REQUIRE(cudaMemcpy(result.data(), target.as<void>(), result.size(), cudaMemcpyDeviceToHost) == cudaSuccess);
    for (const auto tile : layout) {
        for (std::uint32_t y = 0; y < tile.core_height * 4; ++y) {
            for (std::uint32_t x = 0; x < tile.core_width * 4; ++x) {
                const auto destination = static_cast<std::size_t>(y) * pitch + (tile.origin_x * 4 + x) * 4;
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto value = model[c * tile_plane + (y + halo * 4) * 1024 + x + halo * 4];
                    const auto expected = static_cast<std::uint8_t>(std::nearbyint(std::clamp(value * (lut ? 1.0F / 255.0F : 1.0F), 0.0F, 1.0F) * 255.0F));
                    REQUIRE(result[destination + c] == expected);
                }
                REQUIRE(result[destination + 3] == 255);
            }
        }
    }
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t byte = width * 4; byte < pitch; ++byte)
            REQUIRE(result[y * pitch + byte] == 0xA9);
    for (std::size_t byte = pitch * height; byte < result.size(); ++byte) REQUIRE(result[byte] == 0xA9);
}

TEST_CASE("Resident ShiftLUT tiled RGBA matches independent upstream oracles across graph replay and capacity changes", "[upscale_gpu]") {
    namespace tiles = mmltk::backend::imaging::upscale::image_upscaler_cuda;
    namespace lut = mmltk::backend::imaging::upscale::shiftlut;
    const bool graph = GENERATE(false, true);
    Stream stream;
    lut::Operators operators;
    Ort::Env environment{ORT_LOGGING_LEVEL_ERROR, "shiftlut_equivalence"};
    Ort::SessionOptions options;
    operators.Register(options);
    Ort::CUDAProviderOptions cuda_options;
    cuda_options.Update(std::unordered_map<std::string, std::string>{
        {"device_id", "0"}, {"enable_cuda_graph", graph ? "1" : "0"}, {"use_tf32", "0"},
    });
    cuda_options.UpdateWithValue("user_compute_stream", stream.get());
    options.AppendExecutionProvider_CUDA_V2(*cuda_options);
    const auto model = std::filesystem::path(MMLTK_TEST_SOURCE_ROOT) / "src/backend/imaging/upscale/assets/ShiftLUT_fp32.onnx";
    // Explicitly destroy all ORT bindings/session before the physical owner.
    {
        Ort::Session session{environment, model.c_str(), options};
        DeviceBuffer input(3U * 256U * 256U * sizeof(float));
        DeviceBuffer output(3U * 1024U * 1024U * sizeof(float));
        DeviceBuffer source((197U * 4U + 31U) * 193U);
        DeviceBuffer target((197U * 16U + 47U) * 193U * 4U);
        constexpr std::array<std::int64_t, 4> input_shape{1, 3, 256, 256}, output_shape{1, 3, 1024, 1024};
        Ort::MemoryInfo memory{"Cuda", OrtArenaAllocator, 0, OrtMemTypeDefault};
        auto input_value = Ort::Value::CreateTensor<float>(memory, input.as<float>(), 3U * 256U * 256U,
                                                          input_shape.data(), input_shape.size());
        auto output_value = Ort::Value::CreateTensor<float>(memory, output.as<float>(), 3U * 1024U * 1024U,
                                                           output_shape.data(), output_shape.size());
        Ort::IoBinding binding{session};
        binding.BindInput("image", input_value);
        binding.BindOutput("upscaled", output_value);
        Ort::RunOptions run;
        const auto allocations = lut::allocation_count();
        for (const auto geometry : std::array<std::array<std::uint32_t, 2>, 4>{{{1, 1}, {5, 3}, {197, 193}, {5, 3}}}) {
            const auto width = geometry[0], height = geometry[1];
            const std::size_t source_pitch = width * 4U + 31U, target_pitch = width * 16U + 47U;
            for (int pattern = 0; pattern < 2; ++pattern) {
                const auto pixels = oracle("source", width, height, pattern, width * height * 4U);
                const auto expected = oracle("expected", width, height, pattern, width * height * 64U);
                REQUIRE(cudaMemcpy2DAsync(source.as<void>(), source_pitch, pixels.data(), width * 4U,
                    width * 4U, height, cudaMemcpyHostToDevice, stream.get()) == cudaSuccess);
                REQUIRE(cudaMemsetAsync(target.as<void>(), 0xB7, target_pitch * height * 4U, stream.get()) == cudaSuccess);
                for (std::uint32_t y = 0; y < height; y += 192U) {
                    for (std::uint32_t x = 0; x < width; x += 192U) {
                        const tiles::Tile tile{.origin_x = x, .origin_y = y,
                            .core_width = std::min(192U, width - x), .core_height = std::min(192U, height - y)};
                        tiles::prepare_tile(source.as<std::uint8_t>(), source_pitch, width, height, 0, 0,
                            width, height, tile, true, 32U, input.as<float>(), stream.get());
                        session.Run(run, binding);
                        tiles::stitch_tile(output.as<float>(), tile, true, 32U, target.as<std::uint8_t>(),
                            target_pitch, width * 4U, height * 4U, stream.get());
                    }
                }
                REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
                CHECK(lut::allocation_count() == allocations);
                std::vector<std::uint8_t> actual(target_pitch * height * 4U);
                REQUIRE(cudaMemcpy(actual.data(), target.as<void>(), actual.size(), cudaMemcpyDeviceToHost) == cudaSuccess);
                for (std::size_t y = 0; y < height * 4U; ++y) {
                    REQUIRE(std::equal(expected.begin() + static_cast<std::ptrdiff_t>(y * width * 16U),
                        expected.begin() + static_cast<std::ptrdiff_t>((y + 1) * width * 16U),
                        actual.begin() + static_cast<std::ptrdiff_t>(y * target_pitch)));
                    for (std::size_t byte = width * 16U; byte < target_pitch; ++byte) REQUIRE(actual[y * target_pitch + byte] == 0xB7);
                }
            }
        }
        REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
    }
    REQUIRE(operators.Release() == cudaSuccess);
}
