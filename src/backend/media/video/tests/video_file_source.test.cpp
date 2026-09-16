#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <cuda_runtime_api.h>
#include <stdexcept>
#include <limits>
#include <cerrno>
#include <functional>
#include <vector>
#include <sys/wait.h>
#include <spdlog/spdlog.h>
#include "src/common/system/numa_memory.h"
#include <utility>
#include "src/test_support/async_test_utils.hpp"
#include "src/backend/media/video/video_file_source.h"
#include "src/frameworks/gpu/cuda_context_scope.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/backend/media/video/video_frame_convert.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/log.h>
}
import mmltk.common.logging.mmltk_logging;
namespace mmltk::backend::media::video::test_support {
struct VideoFileSourceTestAccess final {
    static auto Limits(VideoFrameCapacity capacity) { return VideoFileSource::StorageLimits(capacity); }
    static void ConfigureLogging(void (*callback)(void*, int, const char*, va_list) = nullptr) { VideoFileSource::ConfigureLogging(callback); }
    static std::unique_ptr<VideoFileSource> Create(const std::filesystem::path& path, VideoFrameCapacity capacity, int device, std::uintptr_t stream,
                                                   std::stop_token stop, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement,
                                                   mmltk::frameworks::gpu::CudaContextApi context_api) {
        return std::unique_ptr<VideoFileSource>(
            new VideoFileSource(path, capacity, device, stream, stop, std::move(retirement), &cudaStreamSynchronize, context_api));
    }
};
}  // namespace mmltk::backend::media::video::test_support
TEST_CASE("video file source rejects remote and absent inputs", "[video]") {
    using mmltk::backend::media::video::VideoFileSource;
    REQUIRE_THROWS_AS(VideoFileSource("https://example.invalid/video.mp4", {4U}, 0, 1U, {}), std::invalid_argument);
    REQUIRE_THROWS_AS(VideoFileSource("/nonexistent/prediction-video.mp4", {4U}, 0, 1U, {}), std::invalid_argument);
}
TEST_CASE("video capacity admits checked pixels and bounded replacement storage", "[video]") {
    using Access = mmltk::backend::media::video::test_support::VideoFileSourceTestAccess;
    REQUIRE_THROWS_AS(Access::Limits({0U}), std::invalid_argument);
    REQUIRE_THROWS_AS(Access::Limits({std::numeric_limits<std::size_t>::max()}), std::invalid_argument);
    const auto limits = Access::Limits({4U});
    CHECK(limits.Pixels(1, 1) == 1U);
    CHECK(limits.Pixels(2, 2) == 4U);
    CHECK_THROWS_AS(limits.Pixels(1, 5), std::invalid_argument);
    CHECK_THROWS_AS(limits.Pixels(0, 2), std::invalid_argument);
    CHECK_THROWS_AS(limits.Pixels(-1, 2), std::invalid_argument);
    CHECK_THROWS_AS(limits.Pixels(2, std::numeric_limits<int>::max()), std::invalid_argument);
    CHECK_NOTHROW(limits.Declared(0, 0));
    CHECK_NOTHROW(limits.Declared(0, 4));
    CHECK_THROWS_AS(limits.Declared(0, 5), std::invalid_argument);
    CHECK_THROWS_AS(limits.Declared(-1, 0), std::invalid_argument);
    CHECK(limits.chw_bytes == 12U * sizeof(float));
    CHECK(limits.rgb_bytes == 12U);
    CHECK(limits.pinned_bytes == mmltk::common::system::host_page_size());
    CHECK(limits.owned_bytes == 2U * limits.chw_bytes + 4U * limits.pinned_bytes);
    const auto page = mmltk::common::system::host_page_size();
    const auto rounded = Access::Limits({page / 3U + 1U});
    CHECK(rounded.pinned_bytes == 2U * page);
    // Individual tensors fit here, but simultaneous incumbents/replacements do not.
    CHECK_THROWS_AS(Access::Limits({std::numeric_limits<std::size_t>::max() / 36U + 1U}), std::invalid_argument);
    const auto large = Access::Limits({(std::numeric_limits<std::size_t>::max() - 8U * page) / 36U});
    CHECK(large.owned_bytes <= std::numeric_limits<std::size_t>::max());
    CHECK_THROWS_AS(large.Pixels(std::numeric_limits<int>::max() / 3 + 1, 1), std::invalid_argument);
}
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
    auto retirement = std::make_shared<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>(1U);
    REQUIRE_THROWS_AS(mmltk::backend::media::video::VideoFileSource("/nonexistent/predict-video", {4U}, 0, 1U, {}, retirement), std::invalid_argument);
    CHECK(retirement->admission_open());
    for (auto pixels : {0U, 1U, 3U}) {
        CHECK_THROWS_AS(mmltk::backend::media::video::VideoFileSource(path, {pixels}, 0, reinterpret_cast<std::uintptr_t>(stream), {}, retirement),
                        std::invalid_argument);
        CHECK(retirement->admission_open());
        CHECK(retirement->fact().reservations == 0U);
    }
    {
        auto source = std::make_unique<mmltk::backend::media::video::VideoFileSource>(path, mmltk::backend::media::video::VideoFrameCapacity{4U}, 0,
                                                                                      reinterpret_cast<std::uintptr_t>(stream), std::stop_token{}, retirement);
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
    // The foreign context has now retired; explicitly restore this fixture's
    // stream owner before constructing another decoder.
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    CHECK(retirement->admission_open());
    CHECK(retirement->fact().reservations == 0U);
    {
        mmltk::backend::media::video::VideoFileSource source(
            path, {4U}, 0, reinterpret_cast<std::uintptr_t>(stream), {}, retirement, +[](cudaStream_t value) -> cudaError_t {
                const auto status = cudaStreamSynchronize(value);
                return status == cudaSuccess ? cudaErrorUnknown : status;
            });
        REQUIRE(source.Next());
    }
    CHECK_FALSE(retirement->admission_open());
    CHECK(retirement->fact().occupancy == 1U);
    for (unsigned attempt = 0U; attempt < 4U; ++attempt)
        CHECK_THROWS(mmltk::backend::media::video::VideoFileSource(path, {4U}, 0, reinterpret_cast<std::uintptr_t>(stream), {}, retirement));
    CHECK(retirement->fact().occupancy == 1U);
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    std::stop_source stop;
    stop.request_stop();
    mmltk::backend::media::video::VideoFileSource cancelled(path, {4U}, 0, reinterpret_cast<std::uintptr_t>(stream), stop.get_token());
    CHECK_FALSE(cancelled.Next().has_value());
}
namespace {
void write_rotated_video(const std::filesystem::path& path, double rotation, int width = 64, int height = 32, const char* muxer = "mp4",
                         AVCodecID codec_id = AV_CODEC_ID_MPEG4) {
    AVFormatContext* output = nullptr;
    REQUIRE(avformat_alloc_output_context2(&output, nullptr, muxer, path.c_str()) >= 0);
    const mmltk::testsupport::ScopedTestCleanup release_output{[&] {
        if (output->pb) avio_closep(&output->pb);
        avformat_free_context(output);
    }};
    const auto* encoder = avcodec_find_encoder(codec_id);
    REQUIRE(encoder);
    auto* codec = avcodec_alloc_context3(encoder);
    auto* frame = av_frame_alloc();
    auto* packet = av_packet_alloc();
    const mmltk::testsupport::ScopedTestCleanup release_codec{[&] {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
    }};
    REQUIRE(codec);
    REQUIRE(frame);
    REQUIRE(packet);
    const bool rgb = codec_id == AV_CODEC_ID_PNG;
    codec->width = width;
    codec->height = height;
    codec->pix_fmt = rgb ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_YUV420P;
    const int rate = codec_id == AV_CODEC_ID_MPEG2VIDEO ? 25 : 4;
    codec->time_base = {1, rate};
    codec->framerate = {rate, 1};
    codec->max_b_frames = rgb ? 0 : 2;
    if (output->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    REQUIRE(avcodec_open2(codec, encoder, nullptr) >= 0);
    auto* track = avformat_new_stream(output, nullptr);
    REQUIRE(track);
    track->time_base = codec->time_base;
    REQUIRE(avcodec_parameters_from_context(track->codecpar, codec) >= 0);
    auto* matrix = av_packet_side_data_new(&track->codecpar->coded_side_data, &track->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX,
                                           9U * sizeof(std::int32_t), 0);
    REQUIRE(matrix);
    av_display_rotation_set(reinterpret_cast<std::int32_t*>(matrix->data), rotation);
    REQUIRE(avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
    REQUIRE(avformat_write_header(output, nullptr) >= 0);
    frame->format = codec->pix_fmt;
    frame->width = codec->width;
    frame->height = codec->height;
    REQUIRE(av_frame_get_buffer(frame, 32) >= 0);
    const auto drain = [&] {
        while (true) {
            const auto status = avcodec_receive_packet(codec, packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
            REQUIRE(status >= 0);
            packet->duration = 1;
            av_packet_rescale_ts(packet, codec->time_base, track->time_base);
            packet->stream_index = track->index;
            REQUIRE(av_interleaved_write_frame(output, packet) >= 0);
            av_packet_unref(packet);
        }
    };
    for (int index = 0; index < 5; ++index) {
        REQUIRE(av_frame_make_writable(frame) >= 0);
        if (rgb) {
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) std::fill_n(frame->data[0] + y * frame->linesize[0] + x * 3, 3, x < width / 2 ? 0U : 255U);
        } else {
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) frame->data[0][y * frame->linesize[0] + x] = x < width / 2 ? 16U : 235U;
            for (int plane = 1; plane < 3; ++plane)
                for (int y = 0; y < height / 2; ++y) std::fill_n(frame->data[plane] + y * frame->linesize[plane], width / 2, 128U);
        }
        frame->pts = index;
        REQUIRE(avcodec_send_frame(codec, frame) >= 0);
        drain();
    }
    REQUIRE(avcodec_send_frame(codec, nullptr) >= 0);
    drain();
    REQUIRE(av_write_trailer(output) >= 0);
}
}  // namespace
namespace {
int isolated_process(const std::function<int()>& body) {
    const auto child = ::fork();
    if (child < 0) return -1;
    if (child == 0) {
        try {
            ::_exit(body());
        } catch (...) { ::_exit(127); }
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
struct LogDispatchCount final {
    const AVClass* av_class = nullptr;
    int calls = 0;
};
void count_log_dispatch(void* context, int, const char*, va_list) {
    if (context) ++static_cast<LogDispatchCount*>(context)->calls;
}
}  // namespace
TEST_CASE("video logging disables callback dispatch and preserves independent process state", "[video]") {
    using Access = mmltk::backend::media::video::test_support::VideoFileSourceTestAccess;
    const auto index = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U);
    const std::array levels{spdlog::level::off,  spdlog::level::trace, spdlog::level::debug,   spdlog::level::info,
                            spdlog::level::warn, spdlog::level::err,   spdlog::level::critical};
    const std::array admissions{AV_LOG_QUIET, AV_LOG_TRACE, AV_LOG_DEBUG, AV_LOG_INFO, AV_LOG_WARNING, AV_LOG_ERROR, AV_LOG_FATAL};
    const auto original_application = mmltk::common::logging::level();
    const auto original_ffmpeg = av_log_get_level();
    const auto log_path = std::filesystem::temp_directory_path() / ("mmltk-video-log-" + std::to_string(::getpid()) + "-" + std::to_string(index));
    const auto nested_log_path = log_path.string() + "-nested";
    const mmltk::testsupport::ScopedTestCleanup remove_logs{[&] {
        std::filesystem::remove(log_path);
        std::filesystem::remove(nested_log_path);
    }};
    // FFmpeg provides no callback getter. Process custody restores even an
    // independently installed callback without guessing/resetting parent state.
    // Invoke the private setup directly: a once_flag inherited from an earlier
    // decoder test must neither skip this case nor be reset in production.
    CHECK(isolated_process([&] {
              mmltk::common::logging::initialize({.app_name = "video-parent-log-test", .level = spdlog::level::warn, .log_file = log_path});
              av_log_set_level(AV_LOG_DEBUG);
              av_log_set_callback(&count_log_dispatch);
              const auto result = isolated_process([&] {
                  mmltk::common::logging::initialize({.app_name = "video-logging-test", .level = levels[index], .log_file = nested_log_path});
                  av_log_set_level(AV_LOG_VERBOSE);
                  av_log_set_callback(&count_log_dispatch);
                  bool rejected = false;
                  try {
                      mmltk::backend::media::video::VideoFileSource source("/nonexistent/video-logging", {4U}, 0, 1U, {});
                  } catch (const std::invalid_argument&) { rejected = true; }
                  if (!rejected || av_log_get_level() != AV_LOG_VERBOSE) return 1;
                  LogDispatchCount context;
                  av_log(&context, AV_LOG_TRACE, "invalid path retains callback");
                  if (context.calls != 1) return 2;
                  Access::ConfigureLogging(&count_log_dispatch);
                  if (av_log_get_level() != admissions[index]) return 3;
                  context.calls = 0;
                  av_log(&context, AV_LOG_TRACE, "actual callback dispatch");
                  av_log(&context, AV_LOG_PANIC, "actual callback dispatch");
                  if (context.calls != (levels[index] == spdlog::level::off ? 0 : 2)) return 4;
                  Access::ConfigureLogging();
                  for (int decoration : {0, AV_LOG_C(134)}) {
                      int suppressed = -1;
                      int admitted = -1;
                      const auto rejected_level = levels[index] == spdlog::level::off ? AV_LOG_PANIC : admissions[index] + 8;
                      av_log(nullptr, rejected_level | decoration, "hidden%n", &suppressed);
                      if (suppressed != -1) return 5;
                      if (levels[index] != spdlog::level::off) {
                          av_log(nullptr, admissions[index] | decoration, "visible%n", &admitted);
                          if (admitted != 7) return 6;
                      }
                  }
                  return 0;
              });
              if (result != 0) return result;
              if (mmltk::common::logging::level() != spdlog::level::warn || av_log_get_level() != AV_LOG_DEBUG) return 7;
              LogDispatchCount context;
              av_log(&context, AV_LOG_DEBUG, "independent callback retained");
              return context.calls == 1 ? 0 : 8;
          }) == 0);
    CHECK(mmltk::common::logging::level() == original_application);
    CHECK(av_log_get_level() == original_ffmpeg);
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
        // FFmpeg also applies max_pixels to padded MPEG-4 scratch storage.
        mmltk::backend::media::video::VideoFileSource source(path, {256U * 256U}, 0, reinterpret_cast<std::uintptr_t>(stream), {});
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
            CHECK(pixels[low] < 0.1F);
            CHECK(pixels[high] > 0.9F);
        }
        CHECK(count == 5U);
        CHECK_FALSE(source.Next());
    }
}
TEST_CASE("video resolution growth stays bounded and failed admission preserves prior pixels", "[video][gpu]") {
    namespace gpu = mmltk::frameworks::gpu;
    using Access = mmltk::backend::media::video::test_support::VideoFileSourceTestAccess;
    const auto base = "mmltk-growing-" + std::to_string(::getpid());
    const auto path = std::filesystem::temp_directory_path() / (base + ".png");
    const auto part = std::filesystem::temp_directory_path() / (base + "-part.png");
    const mmltk::testsupport::ScopedTestCleanup remove{[&] {
        std::filesystem::remove(path);
        std::filesystem::remove(part);
    }};
    {
        std::ofstream output(path, std::ios::binary);
        // PNG has no hardware decoder and admits dimension changes per image.
        // Cross a host page, reuse a smaller extent, then reject an oversized one.
        for (int width : {32, 64, 32, 96}) {
            write_rotated_video(part, 0.0, width, 32, "image2pipe", AV_CODEC_ID_PNG);
            std::ifstream input(part, std::ios::binary);
            output << input.rdbuf();
            REQUIRE(output.good());
        }
    }
    AVFormatContext* initial = nullptr;
    REQUIRE(avformat_open_input(&initial, path.c_str(), nullptr, nullptr) >= 0);
    {
        const mmltk::testsupport::ScopedTestCleanup close{[&] { avformat_close_input(&initial); }};
        REQUIRE(initial->nb_streams > 0U);
        CHECK(initial->streams[0]->codecpar->width == 0);
        CHECK(initial->streams[0]->codecpar->height == 0);
    }
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    mmltk::common::system::ScopedExecutionPolicy policy({execution.placement.cpus, "video-growth", 0, execution.placement.numa_node, -10, false});
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream{};
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    unsigned context_calls = 0U;
    gpu::CudaContextApi api{&context_calls,
                            [](void* count, CUcontext* context) noexcept {
                                ++*static_cast<unsigned*>(count);
                                return cuCtxGetCurrent(context);
                            },
                            [](void* count, CUcontext context) noexcept {
                                ++*static_cast<unsigned*>(count);
                                return cuCtxSetCurrent(context);
                            }};
    auto retirement = std::make_shared<gpu::TerminalCudaRetirementOwner>(1U);
    auto source = Access::Create(path, {64U * 32U}, 0, reinterpret_cast<std::uintptr_t>(stream), {}, retirement, api);
    std::optional<mmltk::backend::media::video::VideoFrame> previous;
    std::vector<float> completed;
    const float* high_water = nullptr;
    unsigned count = 0U;
    bool rejected = false;
    try {
        while (const auto frame = source->Next()) {
            REQUIRE(count < 15U);
            CHECK(frame->index == count);
            CHECK(frame->width == (count >= 5U && count < 10U ? 64U : 32U));
            CHECK(frame->height == 32U);
            if (count == 5U) high_water = frame->chw;
            if (count > 5U) CHECK(frame->chw == high_water);
            completed.resize(static_cast<std::size_t>(frame->width) * frame->height * 3U);
            REQUIRE(cudaMemcpyAsync(completed.data(), frame->chw, completed.size() * sizeof(float), cudaMemcpyDeviceToHost, stream) == cudaSuccess);
            REQUIRE(cudaStreamSynchronize(stream) == cudaSuccess);
            CHECK(completed[8U * frame->width + 8U] < 0.01F);
            CHECK(completed[8U * frame->width + frame->width - 8U] > 0.99F);
            previous = frame;
            ++count;
        }
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK((message.starts_with("decode prediction video:") || message.starts_with("submit prediction video packet:")));
        rejected = true;
    }
    REQUIRE(rejected);
    REQUIRE(count == 15U);
    REQUIRE(previous);
    const auto calls_after_failure = context_calls;
    for (unsigned attempt = 0; attempt < 4U; ++attempt) CHECK_THROWS_AS(source->Next(), std::runtime_error);
    CHECK(context_calls == calls_after_failure);
    std::vector<float> retained(completed.size());
    REQUIRE(cudaMemcpyAsync(retained.data(), previous->chw, retained.size() * sizeof(float), cudaMemcpyDeviceToHost, stream) == cudaSuccess);
    REQUIRE(cudaStreamSynchronize(stream) == cudaSuccess);
    CHECK(retained == completed);
    source.reset();
    CHECK(retirement->admission_open());
    CHECK(retirement->fact().occupancy == 0U);
    CHECK(retirement->fact().reservations == 0U);
}
TEST_CASE("video probing discovers late streams and preserves decoded timing facts", "[video][gpu]") {
    const auto path = std::filesystem::temp_directory_path() / ("mmltk-discovered-" + std::to_string(::getpid()) + ".mpg");
    const mmltk::testsupport::ScopedTestCleanup remove{[&] { std::filesystem::remove(path); }};
    write_rotated_video(path, 0.0, 64, 32, "mpeg", AV_CODEC_ID_MPEG2VIDEO);
    AVFormatContext* reference = nullptr;
    REQUIRE(avformat_open_input(&reference, path.c_str(), nullptr, nullptr) >= 0);
    const mmltk::testsupport::ScopedTestCleanup close{[&] { avformat_close_input(&reference); }};
    const auto before = reference->nb_streams;
    REQUIRE(avformat_find_stream_info(reference, nullptr) >= 0);
    REQUIRE(reference->nb_streams > before);
    const AVCodec* decoder = nullptr;
    const auto selected = av_find_best_stream(reference, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    REQUIRE(selected >= 0);
    const auto* track = reference->streams[selected];
    double rate = av_q2d(track->avg_frame_rate);
    if (!std::isfinite(rate) || rate <= 0.0) rate = av_q2d(track->r_frame_rate);
    REQUIRE(rate > 0.0);
    auto* codec = avcodec_alloc_context3(decoder);
    auto* decoded = av_frame_alloc();
    auto* packet = av_packet_alloc();
    const mmltk::testsupport::ScopedTestCleanup release_reference{[&] {
        av_packet_free(&packet);
        av_frame_free(&decoded);
        avcodec_free_context(&codec);
    }};
    REQUIRE(codec);
    REQUIRE(decoded);
    REQUIRE(packet);
    REQUIRE(avcodec_parameters_to_context(codec, track->codecpar) >= 0);
    REQUIRE(avcodec_open2(codec, decoder, nullptr) >= 0);
    std::vector<std::optional<double>> timestamps;
    const auto receive = [&] {
        while (true) {
            const auto status = avcodec_receive_frame(codec, decoded);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
            REQUIRE(status >= 0);
            REQUIRE(timestamps.size() < 5U);
            if (decoded->best_effort_timestamp == AV_NOPTS_VALUE)
                timestamps.emplace_back();
            else
                timestamps.emplace_back(static_cast<double>(decoded->best_effort_timestamp) * av_q2d(track->time_base));
            av_frame_unref(decoded);
        }
    };
    while (true) {
        const auto status = av_read_frame(reference, packet);
        if (status == AVERROR_EOF) break;
        REQUIRE(status >= 0);
        if (packet->stream_index == selected) {
            REQUIRE(avcodec_send_packet(codec, packet) >= 0);
            receive();
        }
        av_packet_unref(packet);
    }
    REQUIRE(avcodec_send_packet(codec, nullptr) >= 0);
    receive();
    REQUIRE(timestamps.size() == 5U);
    REQUIRE(timestamps.front());
    const auto execution = mmltk::frameworks::gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    mmltk::common::system::ScopedExecutionPolicy policy({execution.placement.cpus, "video-discovery", 0, execution.placement.numa_node, -10, false});
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream{};
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    mmltk::backend::media::video::VideoFileSource source(path, {64U * 32U}, 0, reinterpret_cast<std::uintptr_t>(stream), {});
    CHECK(source.frame_count() == static_cast<std::uint64_t>(std::max<std::int64_t>(0, track->nb_frames)));
    std::optional<double> previous;
    std::uint64_t count = 0U;
    while (const auto frame = source.Next()) {
        CHECK(frame->index == count++);
        CHECK(frame->width == 64U);
        CHECK(frame->height == 32U);
        // MPEG program streams may omit the final delayed frame's timestamp.
        // Preserve that source fact; playback owns the declared-FPS fallback.
        REQUIRE(frame->presentation_seconds.has_value() == timestamps.at(frame->index).has_value());
        if (frame->presentation_seconds) {
            CHECK(std::abs(*frame->presentation_seconds - *timestamps.at(frame->index)) < 0.001);
            if (previous) CHECK(std::abs(*frame->presentation_seconds - *previous - 1.0 / rate) < 0.001);
        }
        previous = frame->presentation_seconds;
        CHECK(std::abs(source.frames_per_second() - rate) < 0.001);
    }
    CHECK(count == 5U);
}
TEST_CASE("video conversion covers tall narrow frames beyond one CUDA grid", "[video][gpu]") {
    using namespace mmltk::backend::media::video;
    constexpr std::uint32_t grid_rows = 65535U * 16U;
    const auto height = GENERATE_COPY(grid_rows, grid_rows + 1U);
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    std::uint8_t* device = nullptr;
    float* output = nullptr;
    REQUIRE(cudaMalloc(reinterpret_cast<void**>(&device), static_cast<std::size_t>(height) * 3U) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release_input{[&] { static_cast<void>(cudaFree(device)); }};
    REQUIRE(cudaMalloc(reinterpret_cast<void**>(&output), static_cast<std::size_t>(height) * 3U * sizeof(float)) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release_output{[&] { static_cast<void>(cudaFree(output)); }};
    const std::array values{32U, 128U, 224U};
    VideoColorConversion conversion;
    conversion.rgb = true;
    conversion.chroma_x = 0;
    conversion.chroma_y = 0;
    for (unsigned channel = 0U; channel < values.size(); ++channel) {
        auto* plane = device + static_cast<std::size_t>(channel) * height;
        REQUIRE(cudaMemset(plane, static_cast<int>(values[channel]), height) == cudaSuccess);
        conversion.planes[channel] = {plane, 1U, 1, 0, 0, 8};
    }
    REQUIRE(cudaMemset(output, 0, static_cast<std::size_t>(height) * 3U * sizeof(float)) == cudaSuccess);
    REQUIRE(convert_video_chw(conversion, 1U, height, output, 0U) == cudaSuccess);
    REQUIRE(cudaStreamSynchronize(nullptr) == cudaSuccess);
    for (unsigned channel = 0U; channel < values.size(); ++channel) {
        for (auto row : {0U, grid_rows - 1U, height - 1U}) {
            float value = 0.0F;
            REQUIRE(cudaMemcpy(&value, output + static_cast<std::size_t>(channel) * height + row, sizeof(value), cudaMemcpyDeviceToHost) == cudaSuccess);
            CHECK(std::abs(value - static_cast<float>(values[channel]) / 255.0F) < 0.00001F);
        }
    }
}
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
TEST_CASE("video exact context transitions seal the existing owner before further decoding", "[video][gpu][context]") {
    namespace gpu = mmltk::frameworks::gpu;
    using mmltk::backend::media::video::VideoFileSource;
    enum class Stage { Construction, Next, Destruction };
    const auto stage = GENERATE(Stage::Construction, Stage::Next, Stage::Destruction);
    const bool query = GENERATE(false, true);
    const bool repeated = GENERATE(false, true);
    const auto path = std::filesystem::temp_directory_path() / ("mmltk-video-context-" + std::to_string(::getpid()) + ".y4m");
    const mmltk::testsupport::ScopedTestCleanup remove{[&] { std::filesystem::remove(path); }};
    {
        std::ofstream file(path, std::ios::binary);
        file << "YUV4MPEG2 W2 H2 F4:1 Ip A1:1 C444\nFRAME\n";
        for (auto value : {16, 128, 128})
            for (unsigned pixel = 0; pixel < 4U; ++pixel) file.put(static_cast<char>(value));
    }
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    mmltk::common::system::ScopedExecutionPolicy policy({execution.placement.cpus, "video-context", 0, execution.placement.numa_node, -10, false});
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream{};
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    struct Driver final {
        bool armed;
        bool query;
        bool repeated;
        unsigned failures = 0U;
        unsigned calls = 0U;
        unsigned sets = 0U;
        CUcontext caller = nullptr;
    } driver{stage == Stage::Construction, query, repeated};
    gpu::CudaContextApi api{
        &driver,
        [](void* opaque, CUcontext* value) noexcept {
            auto& injected = *static_cast<Driver*>(opaque);
            ++injected.calls;
            if (injected.armed && injected.query) return CUDA_ERROR_INVALID_CONTEXT;
            return cuCtxGetCurrent(value);
        },
        [](void* opaque, CUcontext value) noexcept {
            auto& injected = *static_cast<Driver*>(opaque);
            ++injected.calls;
            const auto result = cuCtxSetCurrent(value);
            if (++injected.sets > 1U && injected.armed && !injected.query && value == injected.caller && (injected.repeated || injected.failures == 0U)) {
                ++injected.failures;
                return CUDA_ERROR_INVALID_CONTEXT;
            }
            return result;
        }};
    auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(1U);
    std::unique_ptr<VideoFileSource> source;
    const auto create = [&] {
        source = mmltk::backend::media::video::test_support::VideoFileSourceTestAccess::Create(path, {4U}, 0, reinterpret_cast<std::uintptr_t>(stream),
                                                                                               std::stop_token{}, authority, api);
    };
    const bool terminal = query || repeated;
    if (stage == Stage::Construction) {
        // Initial construction also restores the already-current source context.
        REQUIRE(cuCtxGetCurrent(&driver.caller) == CUDA_SUCCESS);
        CHECK_THROWS(create());
    } else {
        create();
        // An unrelated isolated caller on the same device must be restored exactly.
        gpu::DeviceContext caller(0, gpu::cuda_image_copy_backend());
        caller.Bind();
        REQUIRE(cuCtxGetCurrent(&driver.caller) == CUDA_SUCCESS);
        driver.armed = true;
        if (stage == Stage::Next) {
            CHECK_THROWS_AS(source->Next(), gpu::CudaContextFailure);
            if (terminal) {
                const auto calls = driver.calls;
                CHECK_THROWS_AS(source->Next(), gpu::CudaContextFailure);
                CHECK(driver.calls == calls);
            }
            driver.armed = false;
        }
        source.reset();
        CUcontext restored{};
        REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
        CHECK(restored == driver.caller);
    }
    CHECK(authority->admission_open() == !terminal);
    CHECK(authority->fact().occupancy == (terminal ? 1U : 0U));
    CHECK(authority->fact().reservations == 0U);
}
