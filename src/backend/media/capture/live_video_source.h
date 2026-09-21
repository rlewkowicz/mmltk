#pragma once
#include <cstdint>
#include <string>
#include "src/backend/media/capture/capture_types.h"
namespace mmltk::backend::media::capture {
struct LiveVideoSourceOptions {
 std::string device_path = "/dev/video0";
 std::uint32_t width = 1920U;
 std::uint32_t height = 1080U;
 std::uint32_t fps = 120U;
 std::uint32_t v4l2_buffer_count = 4U;
 CaptureRegion initial_region{};
};
}  // namespace mmltk::backend::media::capture
