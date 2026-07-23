#pragma once

#include "mmltk/live/live_pipeline_trace.h"
#include "mmltk/live/live_types.h"

#include <array>
#include <cstdint>

namespace mmltk::live {

struct LivePipelineStatus {
    LivePipelineTraceSnapshot pipeline{};
    std::array<std::uint64_t, kLivePipelineStageCount> stage_counters{};
    const char* last_stage_name = "ingress.dequeue";
    LiveFrameId last_stage_frame_id{};
    std::uint64_t last_stage_latency_us = 0;
    std::uint64_t release_backlog = 0;
    std::uint64_t acquire_misses = 0;
    std::uint64_t startup_acquire_misses = 0;
    std::uint64_t post_startup_acquire_misses = 0;
    bool first_workspace_publication_ready = false;
};

template <typename Status>
void attach_live_pipeline_status(Status& status, const LivePipelineTrace& trace) noexcept {
    status.pipeline = trace.snapshot();
    status.stage_counters = status.pipeline.stage_counters;
    status.last_stage_name = status.pipeline.last_event.stage_name;
    status.last_stage_frame_id = status.pipeline.last_event.frame_id;
    status.last_stage_latency_us = status.pipeline.last_event.latency_us;
    status.release_backlog = status.pipeline.release_backlog;
    status.acquire_misses = status.pipeline.acquire_misses;
    status.startup_acquire_misses = status.pipeline.startup_acquire_misses;
    status.post_startup_acquire_misses = status.pipeline.post_startup_acquire_misses;
    status.first_workspace_publication_ready = status.pipeline.first_workspace_publication_ready;
}

}  
