#pragma once

#include <cstdint>

namespace mmltk::live {

struct LiveCompositorTelemetry {
    bool running = false;
    std::uint64_t frames_composited = 0;
    std::uint64_t frames_composited_after_startup = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t skipped_compositor_presents = 0;
    std::uint64_t source_acquire_waits = 0;
    std::uint64_t source_leases_acquired = 0;
    std::uint64_t source_leases_released = 0;
    std::uint64_t source_stale_releases = 0;
    std::uint64_t source_skipped_stale_frames = 0;
    std::uint64_t source_slot_pressure = 0;
    std::uint64_t source_release_latency_ns = 0;
    int front_slot_index = -1;
    std::uint64_t front_slot_revision = 0;
};

}  
