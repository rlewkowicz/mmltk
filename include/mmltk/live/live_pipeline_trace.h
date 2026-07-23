#pragma once

#include "mmltk/live/live_frame_id.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mmltk::live {

enum class LivePipelineStage : std::uint8_t {
    IngressDequeue = 0,
    IngressH2dReady,
    FanoutAcquireSource,
    FanoutPublishOutput,
    FanoutReleaseSource,
    CompositorAcquireOutput,
    CompositorPublishWorkspace,
    CompositorReleaseWorkspace,
};

inline constexpr std::size_t kLivePipelineStageCount = 8U;
inline constexpr std::uint32_t kLivePipelineUnknownCudaDevice = 0xffffffffU;

[[nodiscard]] const char* live_pipeline_stage_name(LivePipelineStage stage) noexcept;
[[nodiscard]] std::size_t live_pipeline_stage_index(LivePipelineStage stage) noexcept;

struct LivePipelineTraceEvent {
    LivePipelineStage stage = LivePipelineStage::IngressDequeue;
    const char* stage_name = "ingress.dequeue";
    LiveFrameId frame_id{};
    std::uint64_t event_count = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t latency_us = 0;
    std::uint32_t cuda_device_index = kLivePipelineUnknownCudaDevice;
    std::uint64_t surface_revision = 0;
};

struct LivePipelineTraceSnapshot {
    std::array<std::uint64_t, kLivePipelineStageCount> stage_counters{};
    LivePipelineTraceEvent last_event{};
    std::uint64_t release_backlog = 0;
    std::uint64_t acquire_misses = 0;
    std::uint64_t startup_acquire_misses = 0;
    std::uint64_t post_startup_acquire_misses = 0;
    bool first_workspace_publication_ready = false;
};

class LivePipelineTrace {
   public:
    LivePipelineTrace() = default;

    LivePipelineTrace(const LivePipelineTrace&) = delete;
    LivePipelineTrace& operator=(const LivePipelineTrace&) = delete;

    void reset() noexcept;
    LivePipelineTraceEvent record(LivePipelineStage stage, const LiveFrameId& frame_id, std::uint64_t timestamp_ns,
                                  std::uint64_t latency_base_ns = 0,
                                  std::uint32_t cuda_device_index = kLivePipelineUnknownCudaDevice,
                                  std::uint64_t surface_revision = 0) noexcept;
    void note_release_backlog(std::uint64_t value) noexcept;
    void note_acquire_miss() noexcept;
    void mark_first_workspace_publication_ready() noexcept;
    [[nodiscard]] bool first_workspace_publication_ready() const noexcept;
    [[nodiscard]] LivePipelineTraceSnapshot snapshot() const noexcept;

   private:
    std::array<std::atomic<std::uint64_t>, kLivePipelineStageCount> stage_counters_{};
    std::atomic<std::uint32_t> last_stage_index_{0};
    std::atomic<std::uint64_t> last_session_nonce_{0};
    std::atomic<std::uint64_t> last_sequence_{0};
    std::atomic<std::uint64_t> last_event_count_{0};
    std::atomic<std::uint64_t> last_timestamp_ns_{0};
    std::atomic<std::uint64_t> last_latency_us_{0};
    std::atomic<std::uint32_t> last_cuda_device_index_{kLivePipelineUnknownCudaDevice};
    std::atomic<std::uint64_t> last_surface_revision_{0};
    std::atomic<std::uint64_t> release_backlog_{0};
    std::atomic<std::uint64_t> acquire_misses_{0};
    std::atomic<std::uint64_t> startup_acquire_misses_{0};
    std::atomic<std::uint64_t> post_startup_acquire_misses_{0};
    std::atomic<bool> first_workspace_publication_ready_{false};
};

}  
