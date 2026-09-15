#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include "async_test_utils.hpp"
#include "src/backend/media/video/video_file_source.h"
TEST_CASE("video file source rejects remote and absent inputs", "[video]") {
    using mmltk::backend::media::video::VideoFileSource;
    REQUIRE_THROWS_AS(VideoFileSource("https://example.invalid/video.mp4", 0, 1U, {}), std::invalid_argument);
    REQUIRE_THROWS_AS(VideoFileSource("/nonexistent/prediction-video.mp4", 0, 1U, {}), std::invalid_argument);
}

#include <array>
#include <cmath>
#include <fstream>
#include <unistd.h>
#include <cuda_runtime_api.h>
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/image_buffer.h"
TEST_CASE("local YUV video delivers sequential colors timing and EOF", "[video][gpu]") {
    const auto path = std::filesystem::temp_directory_path() / ("mmltk-video-" + std::to_string(::getpid()) + ".y4m");
    const mmltk::testsupport::ScopedTestCleanup remove_file{[&] { std::filesystem::remove(path); }};
    {
        std::ofstream file(path, std::ios::binary);
        file << "YUV4MPEG2 W2 H2 F4:1 Ip A1:1 C444\n";
        for (const auto luminance : {16U, 235U}) {
            file << "FRAME\n";
            for (unsigned pixel = 0U; pixel < 4U; ++pixel) file.put(static_cast<char>(luminance));
            for (unsigned pixel = 0U; pixel < 8U; ++pixel) file.put(static_cast<char>(128U));
        }
    }
    const auto execution = mmltk::frameworks::gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    mmltk::common::system::ScopedExecutionPolicy policy({execution.placement.cpus, "video-test", 0, execution.placement.numa_node, -10, false});
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream = nullptr;
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup destroy_stream{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    {
        auto source = std::make_unique<mmltk::backend::media::video::VideoFileSource>(path, 0, reinterpret_cast<std::uintptr_t>(stream), std::stop_token{});
        REQUIRE(source->frames_per_second() == 4.0);
        for (std::uint64_t index = 0U; index < 2U; ++index) {
            const auto frame = source->Next();
            REQUIRE(frame.has_value());
            REQUIRE(frame->index == index);
            REQUIRE(frame->width == 2U);
            REQUIRE(frame->height == 2U);
            REQUIRE(frame->presentation_seconds.has_value());
            CHECK(*frame->presentation_seconds == static_cast<double>(index) / 4.0);
            std::array<float, 12U> pixels{};
            REQUIRE(cudaMemcpyAsync(pixels.data(), frame->chw, sizeof(pixels), cudaMemcpyDeviceToHost, stream) == cudaSuccess);
            REQUIRE(cudaStreamSynchronize(stream) == cudaSuccess);
            for (float value : pixels) CHECK(std::abs(value - static_cast<float>(index)) < 0.02F);
        }
        CHECK_FALSE(source->Next().has_value());
        mmltk::frameworks::gpu::DeviceContext other(0, mmltk::frameworks::gpu::cuda_image_copy_backend());
        other.Bind();
        CUcontext before{};
        REQUIRE(cuCtxGetCurrent(&before) == CUDA_SUCCESS);
        source.reset();
        CUcontext after{};
        REQUIRE(cuCtxGetCurrent(&after) == CUDA_SUCCESS);
        CHECK(after == before);
    }
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    std::stop_source stop;
    stop.request_stop();
    mmltk::backend::media::video::VideoFileSource cancelled(path, 0, reinterpret_cast<std::uintptr_t>(stream), stop.get_token());
    CHECK_FALSE(cancelled.Next().has_value());
}

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}
namespace {
void write_rotated_video(const std::filesystem::path& path, double rotation) {
    AVFormatContext* output = nullptr;
    REQUIRE(avformat_alloc_output_context2(&output, nullptr, "mp4", path.c_str()) >= 0);
    const mmltk::testsupport::ScopedTestCleanup release_output{[&] {
        if (output->pb) avio_closep(&output->pb);
        avformat_free_context(output);
    }};
    const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    REQUIRE(encoder);
    auto* codec = avcodec_alloc_context3(encoder);
    auto* frame = av_frame_alloc();
    auto* packet = av_packet_alloc();
    const mmltk::testsupport::ScopedTestCleanup release_codec{[&] {
        av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&codec);
    }};
    REQUIRE(codec); REQUIRE(frame); REQUIRE(packet);
    codec->width = 64; codec->height = 32; codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = {1, 4}; codec->framerate = {4, 1}; codec->max_b_frames = 2;
    codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    REQUIRE(avcodec_open2(codec, encoder, nullptr) >= 0);
    auto* track = avformat_new_stream(output, nullptr);
    REQUIRE(track);
    track->time_base = codec->time_base;
    REQUIRE(avcodec_parameters_from_context(track->codecpar, codec) >= 0);
    auto* matrix = av_stream_new_side_data(track, AV_PKT_DATA_DISPLAYMATRIX, 9U * sizeof(std::int32_t));
    REQUIRE(matrix);
    av_display_rotation_set(reinterpret_cast<std::int32_t*>(matrix), rotation);
    REQUIRE(avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
    REQUIRE(avformat_write_header(output, nullptr) >= 0);
    frame->format = codec->pix_fmt; frame->width = codec->width; frame->height = codec->height;
    REQUIRE(av_frame_get_buffer(frame, 32) >= 0);
    const auto drain = [&] {
        while (true) {
            const auto status = avcodec_receive_packet(codec, packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
            REQUIRE(status >= 0);
            av_packet_rescale_ts(packet, codec->time_base, track->time_base);
            packet->stream_index = track->index;
            REQUIRE(av_interleaved_write_frame(output, packet) >= 0);
            av_packet_unref(packet);
        }
    };
    for (int index = 0; index < 5; ++index) {
        REQUIRE(av_frame_make_writable(frame) >= 0);
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 64; ++x) frame->data[0][y * frame->linesize[0] + x] = x < 32 ? 16U : 235U;
        for (int plane = 1; plane < 3; ++plane) for (int y = 0; y < 16; ++y)
            std::fill_n(frame->data[plane] + y * frame->linesize[plane], 32, 128U);
        frame->pts = index;
        REQUIRE(avcodec_send_frame(codec, frame) >= 0);
        drain();
    }
    REQUIRE(avcodec_send_frame(codec, nullptr) >= 0);
    drain();
    REQUIRE(av_write_trailer(output) >= 0);
}
}
TEST_CASE("local video display rotations preserve decoded pixels and delayed frame drain", "[video][gpu]") {
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream = nullptr;
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release_stream{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    for (int rotation : {0, 90, 180, 270}) {
        const auto path = std::filesystem::temp_directory_path() / ("mmltk-rotated-" + std::to_string(::getpid()) + ".mp4");
        const mmltk::testsupport::ScopedTestCleanup remove{[&] { std::filesystem::remove(path); }};
        write_rotated_video(path, rotation);
        mmltk::backend::media::video::VideoFileSource source(path, 0, reinterpret_cast<std::uintptr_t>(stream), {});
        std::uint64_t count = 0;
        std::optional<double> previous;
        while (const auto frame = source.Next()) {
            REQUIRE(frame->index == count++);
            REQUIRE(frame->width == (rotation % 180 ? 32U : 64U));
            REQUIRE(frame->height == (rotation % 180 ? 64U : 32U));
            REQUIRE(frame->presentation_seconds);
            if (previous) CHECK(*frame->presentation_seconds > *previous);
            previous = frame->presentation_seconds;
            std::array<float, 64U * 32U * 3U> pixels{};
            REQUIRE(cudaMemcpyAsync(pixels.data(), frame->chw, sizeof(pixels), cudaMemcpyDeviceToHost, stream) == cudaSuccess);
            REQUIRE(cudaStreamSynchronize(stream) == cudaSuccess);
            const auto low = rotation == 0 ? 8U * 64U + 8U : rotation == 90 ? 8U * 32U + 8U : rotation == 180 ? 8U * 64U + 56U : 56U * 32U + 8U;
            const auto high = rotation == 0 ? 8U * 64U + 56U : rotation == 90 ? 56U * 32U + 8U : rotation == 180 ? 8U * 64U + 8U : 8U * 32U + 8U;
            CHECK(pixels[low] < 0.1F); CHECK(pixels[high] > 0.9F);
        }
        CHECK(count == 5U);
        CHECK_FALSE(source.Next());
    }
}

#include "src/backend/media/video/video_frame_convert.h"
TEST_CASE("video conversion preserves planar and semiplanar channel layouts", "[video][gpu]") {
    using namespace mmltk::backend::media::video;
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    std::uint8_t* device = nullptr;
    float* output = nullptr;
    REQUIRE(cudaMalloc(reinterpret_cast<void**>(&device), 12U) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release_input{[&] { static_cast<void>(cudaFree(device)); }};
    REQUIRE(cudaMalloc(reinterpret_cast<void**>(&output), 12U * sizeof(float)) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release_output{[&] { static_cast<void>(cudaFree(output)); }};
    for (bool semiplanar : {false, true}) {
        const std::array<std::uint8_t, 12U> bytes{235, 235, 235, 235, 128, 128, 128, 128, 128, 128, 128, 128};
        REQUIRE(cudaMemcpy(device, bytes.data(), bytes.size(), cudaMemcpyHostToDevice) == cudaSuccess);
        VideoColorConversion conversion;
        conversion.planes[0] = {device, 2U, 1, 0, 0, 8};
        conversion.planes[1] = {device + 4, semiplanar ? 2U : 1U, semiplanar ? 2 : 1, 0, 0, 8};
        conversion.planes[2] = {device + (semiplanar ? 4 : 5), semiplanar ? 2U : 1U, semiplanar ? 2 : 1, semiplanar ? 1 : 0, 0, 8};
        REQUIRE(convert_video_chw(conversion, 2U, 2U, output, 0U) == cudaSuccess);
        std::array<float, 12U> pixels{};
        REQUIRE(cudaMemcpy(pixels.data(), output, sizeof(pixels), cudaMemcpyDeviceToHost) == cudaSuccess);
        for (auto value : pixels) CHECK(std::abs(value - 1.0F) < 0.01F);
    }
}
