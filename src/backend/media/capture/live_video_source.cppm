module;
#include <cstdint>
#include <string>

export module mmltk.backend.media.capture.live_video_source;

import mmltk.backend.media.capture.capture_types;

export namespace mmltk::backend::media::capture {

struct LiveVideoSourceOptions {
    std::string device_path = "/dev/video0";
    std::uint32_t width = 1920U;
    std::uint32_t height = 1080U;
    std::uint32_t fps = 120U;
    std::uint32_t v4l2_buffer_count = 4U;
    CaptureRegion initial_region{};
};

[[nodiscard]] constexpr bool live_capture_supported() noexcept { return true; }

}  // namespace mmltk::backend::media::capture
