#pragma once

#include <cstdint>

namespace mmltk::backend::media::live {

struct LiveCompositorTelemetry {
    bool running = false;
    std::uint64_t frames_composited = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t front_revision = 0;
};

}  // namespace mmltk::backend::media::live
