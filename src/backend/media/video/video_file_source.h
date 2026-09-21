#pragma once
#include <cstddef>
#include <cstdarg>
#include <cuda_runtime_api.h>
#include <cstdint>
#include <filesystem>
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
struct VideoFrame final {
 const float* chw = nullptr;
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 std::uint64_t index = 0U;
 std::optional<double> presentation_seconds;
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
                 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement = {},
                 decltype(&cudaStreamSynchronize) settle = &cudaStreamSynchronize);
 ~VideoFileSource();
 VideoFileSource(const VideoFileSource&) = delete;
 VideoFileSource& operator=(const VideoFileSource&) = delete;
 [[nodiscard]] std::optional<VideoFrame> Next();
 [[nodiscard]] double frames_per_second() const noexcept;
 [[nodiscard]] std::uint64_t frame_count() const noexcept;

private:
 VideoFileSource(const std::filesystem::path&, VideoFrameCapacity, int device, std::uintptr_t stream, std::stop_token,
                 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement, decltype(&cudaStreamSynchronize) settle,
                 mmltk::frameworks::gpu::CudaContextApi context_api);
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
