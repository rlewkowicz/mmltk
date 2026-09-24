#pragma once
#include <cstddef>
#include <cstdarg>
#include <cuda_runtime_api.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
namespace mmltk::frameworks::gpu {
class TerminalCudaRetirementOwner;
struct CudaContextApi;
}  // namespace mmltk::frameworks::gpu
namespace mmltk::backend::media::video {
namespace test_support {
struct VideoFileSourceTestAccess;
}
struct VideoTiming final {
 std::optional<std::int64_t> pts;
 std::int64_t duration = 0;
 int time_base_numerator = 0, time_base_denominator = 1;
};
class VideoFileSink;
class VideoMediaInfo final {
public:
 struct State;
 std::uint32_t width = 0, height = 0;
 int rate_numerator = 0, rate_denominator = 1;
private:
 std::shared_ptr<const State> state_;
 friend class VideoFileSource;
 friend class VideoFileSink;
};
class VideoAudioPacket final {
public:
 struct State;
private:
 std::shared_ptr<const State> state_;
 friend class VideoFileSource;
 friend class VideoFileSink;
};
struct VideoFrame final {
 const float* chw = nullptr;
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 std::uint64_t index = 0U;
 std::optional<double> presentation_seconds;
 VideoTiming timing{};
};
// Product policy is supplied by the caller; media owns storage admission.
struct VideoFrameCapacity final {
 std::size_t maximum_pixels;
};
// Owns the decoder, frame references and reusable GPU/pinned transfer storage.
// Next settles decoder input reads before releasing AVFrame/pinned storage.
// The returned CHW view is valid until Next; GPU readers must use the supplied
// stream or enqueue their completion wait on it before Next. The supplied
// stream and its context must outlive this source. CHW reuse is
// stream-ordered; destruction settles all consumers before releasing storage.
// An escaping source-work failure requires a fresh source for retry.
class VideoFileSource final {
public:
 VideoFileSource(const std::filesystem::path&, VideoFrameCapacity, int device, std::uintptr_t stream, std::stop_token,
  std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement = {}, decltype(&cudaStreamSynchronize) settle = &cudaStreamSynchronize);
 ~VideoFileSource();
 VideoFileSource(const VideoFileSource&) = delete;
 VideoFileSource& operator=(const VideoFileSource&) = delete;
 [[nodiscard]] std::optional<VideoFrame> Next();
 [[nodiscard]] VideoMediaInfo media_info() const;
 void SetAudioConsumer(std::function<void(const VideoAudioPacket&)>);
 [[nodiscard]] double frames_per_second() const noexcept;
 [[nodiscard]] std::uint64_t frame_count() const noexcept;

private:
 VideoFileSource(const std::filesystem::path&, VideoFrameCapacity, int device, std::uintptr_t stream, std::stop_token, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement,
  decltype(&cudaStreamSynchronize) settle, mmltk::frameworks::gpu::CudaContextApi context_api);
 struct StorageLimits final {
  explicit StorageLimits(VideoFrameCapacity);
  [[nodiscard]] std::size_t Pixels(int width, int height) const;
  void Declared(int width, int height) const;
  std::size_t maximum_pixels;
  std::size_t chw_bytes;
  std::size_t rgb_bytes;
  std::size_t pinned_bytes;
  std::size_t owned_bytes;
 };
 using LogCallback = void (*)(void*, int, const char*, va_list);
 static void ConfigureLogging(LogCallback = nullptr);
 struct State;
 struct Owner;
 std::unique_ptr<Owner> owner_;
 friend struct test_support::VideoFileSourceTestAccess;
};
}  // namespace mmltk::backend::media::video
