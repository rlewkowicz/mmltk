#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <cstddef>
#include "video_media.h"
#include "src/frameworks/gpu/image/image_types.h"
namespace mmltk::backend::media::video {
// Synchronous worker-side backpressure. Each call settles its borrowed device
// read; encoder and mux packet custody remains entirely inside the media owner.
class VideoFileSink final {
public:
 using FileWrite = std::function<std::ptrdiff_t(int, const void*, std::size_t)>;
 VideoFileSink(const std::filesystem::path& partial, const std::filesystem::path& completed, const VideoMediaInfo&, int device, bool software_only = false, FileWrite = {});
 ~VideoFileSink();
 VideoFileSink(const VideoFileSink&) = delete;
 VideoFileSink& operator=(const VideoFileSink&) = delete;
 void Write(mmltk::frameworks::gpu::ImagePlaneView, VideoTiming, std::uintptr_t stream);
 void Audio(const VideoAudioPacket&);
 void Complete();
 void ClosePartial();

private:
 struct State;
 std::unique_ptr<State> state_;
};
}  // namespace mmltk::backend::media::video
