#include "mmltk/live/live_pipeline_trace.h"

#include <algorithm>

namespace mmltk::live {

namespace {

constexpr std::array<const char*, kLivePipelineStageCount> kStageNames{
    "ingress.dequeue",
    "ingress.h2d_ready",
    "fanout.acquire_source",
    "fanout.publish_output",
    "fanout.release_source",
    "compositor.acquire_output",
    "compositor.publish_workspace",
    "compositor.release_workspace",
};

}  

const char* live_pipeline_stage_name(const LivePipelineStage stage) noexcept {
    return kStageNames[live_pipeline_stage_index(stage)];
}

std::size_t live_pipeline_stage_index(const LivePipelineStage stage) noexcept {
    const auto raw_index = static_cast<std::size_t>(stage);
    return raw_index < kLivePipelineStageCount ? raw_index : 0U;
}

void LivePipelineTrace::reset() noexcept {
    for (std::atomic<std::uint64_t>& counter : stage_counters_) {
        counter.store(0, std::memory_order_release);
    }
    last_stage_index_.store(0, std::memory_order_release);
    last_session_nonce_.store(0, std::memory_order_release);
    last_sequence_.store(0, std::memory_order_release);
    last_event_count_.store(0, std::memory_order_release);
    last_timestamp_ns_.store(0, std::memory_order_release);
    last_latency_us_.store(0, std::memory_order_release);
    last_cuda_device_index_.store(kLivePipelineUnknownCudaDevice, std::memory_order_release);
    last_surface_revision_.store(0, std::memory_order_release);
    release_backlog_.store(0, std::memory_order_release);
    acquire_misses_.store(0, std::memory_order_release);
    startup_acquire_misses_.store(0, std::memory_order_release);
    post_startup_acquire_misses_.store(0, std::memory_order_release);
    first_workspace_publication_ready_.store(false, std::memory_order_release);
}

LivePipelineTraceEvent LivePipelineTrace::record(const LivePipelineStage stage, const LiveFrameId& frame_id,
                                                 const std::uint64_t timestamp_ns, const std::uint64_t latency_base_ns,
                                                 const std::uint32_t cuda_device_index,
                                                 const std::uint64_t surface_revision) noexcept {
    const std::size_t index = live_pipeline_stage_index(stage);
    const std::uint64_t event_count = stage_counters_[index].fetch_add(1, std::memory_order_acq_rel) + 1U;
    const std::uint64_t latency_us =
        latency_base_ns != 0U && timestamp_ns >= latency_base_ns ? (timestamp_ns - latency_base_ns) / 1000U : 0U;

    last_stage_index_.store(static_cast<std::uint32_t>(index), std::memory_order_release);
    last_session_nonce_.store(frame_id.session_nonce, std::memory_order_release);
    last_sequence_.store(frame_id.sequence, std::memory_order_release);
    last_event_count_.store(event_count, std::memory_order_release);
    last_timestamp_ns_.store(timestamp_ns, std::memory_order_release);
    last_latency_us_.store(latency_us, std::memory_order_release);
    last_cuda_device_index_.store(cuda_device_index, std::memory_order_release);
    last_surface_revision_.store(surface_revision, std::memory_order_release);

    return LivePipelineTraceEvent{
        stage,
        live_pipeline_stage_name(stage),
        frame_id,
        event_count,
        timestamp_ns,
        latency_us,
        cuda_device_index,
        surface_revision,
    };
}

void LivePipelineTrace::note_release_backlog(const std::uint64_t value) noexcept {
    release_backlog_.store(value, std::memory_order_release);
}

void LivePipelineTrace::note_acquire_miss() noexcept {
    acquire_misses_.fetch_add(1, std::memory_order_acq_rel);
    if (first_workspace_publication_ready_.load(std::memory_order_acquire)) {
        post_startup_acquire_misses_.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    startup_acquire_misses_.fetch_add(1, std::memory_order_acq_rel);
}

void LivePipelineTrace::mark_first_workspace_publication_ready() noexcept {
    first_workspace_publication_ready_.store(true, std::memory_order_release);
}

bool LivePipelineTrace::first_workspace_publication_ready() const noexcept {
    return first_workspace_publication_ready_.load(std::memory_order_acquire);
}

LivePipelineTraceSnapshot LivePipelineTrace::snapshot() const noexcept {
    LivePipelineTraceSnapshot out;
    for (std::size_t index = 0; index < kLivePipelineStageCount; ++index) {
        out.stage_counters[index] = stage_counters_[index].load(std::memory_order_acquire);
    }
    const std::size_t stage_index =
        std::min<std::size_t>(last_stage_index_.load(std::memory_order_acquire), kLivePipelineStageCount - 1U);
    out.last_event.stage = static_cast<LivePipelineStage>(stage_index);
    out.last_event.stage_name = kStageNames[stage_index];
    out.last_event.frame_id = LiveFrameId{
        last_session_nonce_.load(std::memory_order_acquire),
        last_sequence_.load(std::memory_order_acquire),
    };
    out.last_event.event_count = last_event_count_.load(std::memory_order_acquire);
    out.last_event.timestamp_ns = last_timestamp_ns_.load(std::memory_order_acquire);
    out.last_event.latency_us = last_latency_us_.load(std::memory_order_acquire);
    out.last_event.cuda_device_index = last_cuda_device_index_.load(std::memory_order_acquire);
    out.last_event.surface_revision = last_surface_revision_.load(std::memory_order_acquire);
    out.release_backlog = release_backlog_.load(std::memory_order_acquire);
    out.acquire_misses = acquire_misses_.load(std::memory_order_acquire);
    out.startup_acquire_misses = startup_acquire_misses_.load(std::memory_order_acquire);
    out.post_startup_acquire_misses = post_startup_acquire_misses_.load(std::memory_order_acquire);
    out.first_workspace_publication_ready = first_workspace_publication_ready_.load(std::memory_order_acquire);
    return out;
}

}  
