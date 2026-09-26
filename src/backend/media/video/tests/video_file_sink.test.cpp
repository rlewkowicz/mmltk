#include "src/backend/media/video/video_file_sink.h"
#include "src/backend/media/video/video_file_source.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/cuda_test_utils.hpp"
#include "src/test_support/subprocess_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdlib>
#include <cerrno>
#include <array>
#include <vector>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
namespace {
namespace gpu = mmltk::frameworks::gpu;
namespace media = mmltk::backend::media::video;
void input_video(const std::filesystem::path& path) {
 std::ofstream input(path, std::ios::binary);
 input << "YUV4MPEG2 W5 H3 F4:1 Ip A1:1 C444\n";
 for (unsigned frame = 0; frame < 16; ++frame) {
  input << "FRAME\n";
  for (unsigned pixel = 0; pixel < 15; ++pixel) input.put(static_cast<char>(frame % 2 ? 235 : 16));
  for (unsigned pixel = 0; pixel < 30; ++pixel) input.put(static_cast<char>(128));
 }
 REQUIRE(input.good());
}
void encode(const std::filesystem::path& directory, bool complete, bool software, int ready = -1, bool obstruct_rename = false) {
 // CLEANUP-IGNORE: Device construction is shared; each media scenario owns its stream and source lifetimes.
 gpu::test_support::IsolatedTestDevice device(gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture()));
 const auto& execution = device.execution;
 auto& context = device.context;
 context.Bind();
 gpu::ImageStream stream(context);
 const auto input = directory / (std::filesystem::exists(directory / "source.mkv") ? "source.mkv" : "source.y4m");
 media::VideoFileSource source(input, {15U}, 0, stream.native_handle(), {});
 const auto info = [&] {
  media::VideoFileSource metadata_source(input, {15U}, 0, stream.native_handle(), {});
  return metadata_source.media_info();
 }();
 REQUIRE(info);  // The opaque audio/format facts outlive their decoder owner.
 media::VideoFileSink sink(directory / "recording.partial.mkv", directory / "recording.mkv", *info, 0, software);
 source.SetAudioConsumer([&](const auto& packet) { sink.Audio(packet); });
 gpu::SystemImageRuntime runtime({.device = 0, .numa_node = execution.placement.numa_node, .execution = execution, .adopted_context = context});
 while (auto frame = source.Next()) {
  auto candidate = runtime.AcquireOutput();
  runtime.PublishRetained(candidate, 5U, 3U, [&](auto image, auto, auto target_stream) {
   REQUIRE(cudaMemset2DAsync(reinterpret_cast<void*>(image.data), image.descriptor.pitch_bytes, frame->index % 2 ? 255 : 0, image.descriptor.row_bytes(), 3U,
            reinterpret_cast<cudaStream_t>(target_stream)) == cudaSuccess);
  });
  static_cast<void>(runtime.CommitOutput(std::move(candidate)));
  auto borrowed = runtime.Borrow();
  sink.Write(borrowed.plane(0).plane(), frame->timing, stream.native_handle());
 }
 if (ready >= 0) {
  const char byte = 'R';
  if (::write(ready, &byte, 1) != 1) ::_exit(125);
  for (;;) ::pause();
 }
 if (obstruct_rename) std::filesystem::create_directory(directory / "recording.mkv");
 if (complete) sink.Complete();
}
struct Decode final {
 AVFormatContext* format = nullptr;
 AVCodecContext* codec = nullptr;
 AVPacket* packet = av_packet_alloc();
 AVFrame* frame = av_frame_alloc();
 ~Decode() {
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
 }
 unsigned Run(const std::filesystem::path& path, bool exact) {
  REQUIRE(avformat_open_input(&format, path.c_str(), nullptr, nullptr) >= 0);
  REQUIRE(avformat_find_stream_info(format, nullptr) >= 0);
  const AVCodec* decoder = nullptr;
  const int track = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  REQUIRE(track >= 0);
  CHECK(format->streams[track]->codecpar->codec_id == AV_CODEC_ID_H264);
  CHECK(format->streams[track]->codecpar->width == 6);
  CHECK(format->streams[track]->codecpar->height == 4);
  codec = avcodec_alloc_context3(decoder);
  REQUIRE(codec);
  REQUIRE(avcodec_parameters_to_context(codec, format->streams[track]->codecpar) >= 0);
  REQUIRE(avcodec_open2(codec, decoder, nullptr) >= 0);
  unsigned count = 0;
  const auto receive = [&] {
   while (avcodec_receive_frame(codec, frame) == 0) {
    REQUIRE(frame->best_effort_timestamp != AV_NOPTS_VALUE);
    CHECK(av_compare_ts(frame->best_effort_timestamp, format->streams[track]->time_base, count, AVRational{1, 4}) == 0);
    // Interior luminance and padded edge retain the submitted frame, with
    // ordinary lossy H.264 tolerance and no geometric stretching.
    const int expected = count % 2 ? 235 : 16;
    CHECK(std::abs(int(frame->data[0][0]) - expected) < 8);
    CHECK(std::abs(int(frame->data[0][3 * frame->linesize[0] + 5]) - expected) < 8);
    ++count;
    av_frame_unref(frame);
   }
  };
  while (av_read_frame(format, packet) >= 0) {
   if (packet->stream_index == track) {
    REQUIRE(avcodec_send_packet(codec, packet) >= 0);
    receive();
   }
   av_packet_unref(packet);
  }
  REQUIRE(avcodec_send_packet(codec, nullptr) >= 0);
  receive();
  if (exact)
   CHECK(count == 16U);
  else
   CHECK(count >= 8U);
  return count;
 }
};
}  // namespace
TEST_CASE("native H264 output preserves frame timing odd geometry and recoverable stop", "[video][gpu][sink]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 const bool complete = GENERATE(false, true);
 const bool software = GENERATE(false, true);
 mmltk::testsupport::ScopedTempDir temporary("video-output");
 input_video(temporary.path() / "source.y4m");
 encode(temporary.path(), complete, software);
 CHECK(std::filesystem::exists(temporary.path() / "recording.partial.mkv") == !complete);
 CHECK(std::filesystem::exists(temporary.path() / "recording.mkv") == complete);
 Decode decoded;
 decoded.Run(temporary.path() / (complete ? "recording.mkv" : "recording.partial.mkv"), complete);
}
TEST_CASE("Matroska propagates full-disk write failure and retains its partial path", "[video][sink][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 gpu::test_support::IsolatedTestDevice device(gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture()));
 auto& context = device.context;
 context.Bind();
 gpu::ImageStream stream(context);
 mmltk::testsupport::ScopedTempDir temporary("video-full-disk");
 input_video(temporary.path() / "source.y4m");
 media::VideoFileSource source(temporary.path() / "source.y4m", {15U}, 0, stream.native_handle(), {});
 const auto info = source.media_info();
 REQUIRE(info);
 CHECK_THROWS(media::VideoFileSink(temporary.path() / "recording.partial.mkv", temporary.path() / "recording.mkv", *info, 0, true, [](int, const void*, std::size_t) -> std::ptrdiff_t {
  errno = ENOSPC;
  return -1;
 }));
 CHECK(std::filesystem::exists(temporary.path() / "recording.partial.mkv"));
 CHECK_FALSE(std::filesystem::exists(temporary.path() / "recording.mkv"));
}
TEST_CASE("video rename failure retains the completed partial file", "[video][sink][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 mmltk::testsupport::ScopedTempDir temporary("video-rename-failure");
 input_video(temporary.path() / "source.y4m");
 CHECK_THROWS(encode(temporary.path(), true, true, -1, true));
 CHECK(std::filesystem::is_directory(temporary.path() / "recording.mkv"));
 Decode decoded;
 decoded.Run(temporary.path() / "recording.partial.mkv", true);
}
TEST_CASE("packaged FFmpeg exposes both selected H264 encoders", "[video][sink]") {
 REQUIRE(avcodec_find_encoder_by_name("h264_nvenc") != nullptr);
 REQUIRE(avcodec_find_encoder_by_name("libx264") != nullptr);
}
TEST_CASE("annotated video retains variable source timestamp progression", "[video][sink][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 mmltk::testsupport::ScopedTempDir temporary("video-variable-time");
 input_video(temporary.path() / "source.y4m");
 const auto generated = mmltk::testsupport::run_subprocess_capture_output(
  {"ffmpeg", "-v", "error", "-i", (temporary.path() / "source.y4m").string(), "-vf", "setpts=N*(N+1)/(8*TB)", "-fps_mode", "vfr", "-c:v", "ffv1", (temporary.path() / "source.mkv").string()});
 REQUIRE(generated.exit_code == 0);
 encode(temporary.path(), true, true);
 const auto times = [](const std::filesystem::path& path) {
  AVFormatContext* format = nullptr;
  REQUIRE(avformat_open_input(&format, path.c_str(), nullptr, nullptr) >= 0);
  REQUIRE(avformat_find_stream_info(format, nullptr) >= 0);
  AVPacket* packet = av_packet_alloc();
  REQUIRE(packet);
  std::vector<std::pair<std::int64_t, std::int64_t>> result;
  while (av_read_frame(format, packet) >= 0) {
   if (packet->stream_index == 0)
    result.push_back({av_rescale_q(packet->pts, format->streams[0]->time_base, AVRational{1, 1000}), av_rescale_q(packet->duration, format->streams[0]->time_base, AVRational{1, 1000})});
   av_packet_unref(packet);
  }
  av_packet_free(&packet);
  avformat_close_input(&format);
  return result;
 };
 const auto original = times(temporary.path() / "source.mkv");
 REQUIRE(original.size() == 16U);
 CHECK(original[3].first - original[2].first != original[2].first - original[1].first);
 CHECK(times(temporary.path() / "recording.mkv") == original);
}
TEST_CASE("annotated Matroska copies multiple audio tracks and their dispositions", "[video][sink][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 mmltk::testsupport::ScopedTempDir temporary("video-audio");
 input_video(temporary.path() / "source.y4m");
 const auto generated = mmltk::testsupport::run_subprocess_capture_output(
  {"ffmpeg", "-v", "error", "-i", (temporary.path() / "source.y4m").string(), "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000:duration=4", "-f", "lavfi", "-i",
   "sine=frequency=880:sample_rate=8000:duration=4", "-map", "0:v", "-map", "1:a", "-map", "2:a", "-c:v", "ffv1", "-c:a", "pcm_s16le", "-disposition:a:0", "default", "-disposition:a:1", "comment",
   (temporary.path() / "source.mkv").string()});
 REQUIRE(generated.exit_code == 0);
 encode(temporary.path(), true, true);
 AVFormatContext* input = nullptr;
 AVFormatContext* output = nullptr;
 REQUIRE(avformat_open_input(&input, (temporary.path() / "source.mkv").c_str(), nullptr, nullptr) >= 0);
 REQUIRE(avformat_open_input(&output, (temporary.path() / "recording.mkv").c_str(), nullptr, nullptr) >= 0);
 struct Cleanup {
  AVFormatContext*& input;
  AVFormatContext*& output;
  ~Cleanup() {
   avformat_close_input(&input);
   avformat_close_input(&output);
  }
 } cleanup{input, output};
 REQUIRE(avformat_find_stream_info(input, nullptr) >= 0);
 REQUIRE(avformat_find_stream_info(output, nullptr) >= 0);
 REQUIRE(input->nb_streams == 3U);
 REQUIRE(output->nb_streams == 3U);
 for (unsigned track = 1; track < 3; ++track) {
  CHECK(input->streams[track]->codecpar->codec_id == output->streams[track]->codecpar->codec_id);
  CHECK(input->streams[track]->disposition == output->streams[track]->disposition);
 }
 const auto audio_packets = [](AVFormatContext* format) {
  std::array<std::vector<std::pair<std::int64_t, std::uint64_t>>, 2> result;
  AVPacket* packet = av_packet_alloc();
  REQUIRE(packet);
  while (av_read_frame(format, packet) >= 0) {
   if (packet->stream_index > 0 && packet->stream_index < 3) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (int index = 0; index < packet->size; ++index) hash = (hash ^ packet->data[index]) * 1099511628211ULL;
    result[packet->stream_index - 1].push_back({av_rescale_q(packet->pts, format->streams[packet->stream_index]->time_base, AVRational{1, 1000000}), hash});
   }
   av_packet_unref(packet);
  }
  av_packet_free(&packet);
  return result;
 };
 const auto original = audio_packets(input), saved = audio_packets(output);
 CHECK_FALSE(original[0].empty());
 CHECK_FALSE(original[1].empty());
 CHECK(saved == original);
}
TEST_CASE("video sink interrupted writer child", "[.video-sink-child]") {
 const char* directory = std::getenv("MMLTK_VIDEO_SINK_CHILD_DIRECTORY");
 const char* descriptor = std::getenv("MMLTK_VIDEO_SINK_CHILD_READY");
 if (!directory || !descriptor) SKIP("child process entry point");
 encode(directory, false, true, std::stoi(descriptor));
}
TEST_CASE("flushed Matroska prefix decodes after SIGKILL without a trailer", "[video][sink][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 mmltk::testsupport::ScopedTempDir temporary("video-interrupted");
 input_video(temporary.path() / "source.y4m");
 int descriptors[2];
 REQUIRE(::pipe(descriptors) == 0);
 const auto child = ::fork();
 REQUIRE(child >= 0);
 if (!child) {
  ::close(descriptors[0]);
  const auto descriptor = std::to_string(descriptors[1]);
  ::setenv("MMLTK_VIDEO_SINK_CHILD_DIRECTORY", temporary.path().c_str(), 1);
  ::setenv("MMLTK_VIDEO_SINK_CHILD_READY", descriptor.c_str(), 1);
  ::execl("/proc/self/exe", "video-sink-child", "video sink interrupted writer child", static_cast<char*>(nullptr));
  ::_exit(126);
 }
 ::close(descriptors[1]);
 pollfd readable{descriptors[0], POLLIN, 0};
 const auto observed = ::poll(&readable, 1, 60000);
 char ready = 0;
 const auto ready_bytes = observed > 0 ? ::read(descriptors[0], &ready, 1) : 0;
 ::close(descriptors[0]);
 ::kill(child, SIGKILL);
 int status = 0;
 REQUIRE(::waitpid(child, &status, 0) == child);
 REQUIRE(ready_bytes == 1);
 REQUIRE(ready == 'R');
 REQUIRE(WIFSIGNALED(status));
 Decode decoded;
 decoded.Run(temporary.path() / "recording.partial.mkv", false);
 CHECK_FALSE(std::filesystem::exists(temporary.path() / "recording.mkv"));
}
