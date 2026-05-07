#include "mmltk/live/live_pipeline_trace.h"

#include "mmltk_logging.h"

#include <algorithm>

namespace mmltk::live {

namespace {

constexpr std::array<const char*, kLivePipelineStageCount> kStageNames{
    "ingress.dequeue",          "ingress.h2d_ready",         "fanout.acquire_source",        "fanout.publish_output",
    "fanout.release_source",    "compositor.acquire_output", "compositor.publish_workspace", "browser.retain_surface",
    "cef.export_shared_image",  "cef.present_front_slot",    "renderer.receive_surface",     "renderer.import_texture",
    "webgpu.draw_surface",      "renderer.release_surface",  "browser.release_surface",
};

[[nodiscard]] bool stage_is_diagnostic_budgeted(const LivePipelineStage stage) noexcept {
    switch (stage) {
        case LivePipelineStage::FanoutReleaseSource:
        case LivePipelineStage::CompositorPublishWorkspace:
        case LivePipelineStage::CefExportSharedImage:
        case LivePipelineStage::CefPresentFrontSlot:
        case LivePipelineStage::RendererReleaseSurface:
        case LivePipelineStage::BrowserReleaseSurface:
            return true;
        case LivePipelineStage::IngressDequeue:
        case LivePipelineStage::IngressH2dReady:
        case LivePipelineStage::FanoutAcquireSource:
        case LivePipelineStage::FanoutPublishOutput:
        case LivePipelineStage::CompositorAcquireOutput:
        case LivePipelineStage::BrowserRetainSurface:
        case LivePipelineStage::RendererReceiveSurface:
        case LivePipelineStage::RendererImportTexture:
        case LivePipelineStage::WebGpuDrawSurface:
            return false;
    }
    return false;
}

}  // namespace

const char* live_pipeline_stage_name(const LivePipelineStage stage) noexcept {
    const std::size_t index = live_pipeline_stage_index(stage);
    return kStageNames[index];
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
    diagnostic_failed_.store(false, std::memory_order_release);
    diagnostic_drawn_frames_.store(0, std::memory_order_release);
    for (std::atomic<std::uint64_t>& miss_counter : diagnostic_stage_consecutive_misses_) {
        miss_counter.store(0, std::memory_order_release);
    }
    diagnostic_completed_.store(false, std::memory_order_release);
    diagnostic_failure_stage_index_.store(0, std::memory_order_release);
    diagnostic_failure_session_nonce_.store(0, std::memory_order_release);
    diagnostic_failure_sequence_.store(0, std::memory_order_release);
    diagnostic_failure_event_count_.store(0, std::memory_order_release);
    diagnostic_failure_timestamp_ns_.store(0, std::memory_order_release);
    diagnostic_failure_latency_us_.store(0, std::memory_order_release);
    diagnostic_failure_cuda_device_index_.store(kLivePipelineUnknownCudaDevice, std::memory_order_release);
    diagnostic_failure_surface_revision_.store(0, std::memory_order_release);
}

void LivePipelineTrace::configure_single_buffer_diagnostic(const std::uint64_t frame_budget_ns,
                                                           const std::uint32_t frame_limit,
                                                           const std::uint32_t consecutive_miss_limit) noexcept {
    diagnostic_enabled_.store(frame_budget_ns != 0U, std::memory_order_release);
    diagnostic_frame_budget_us_.store(frame_budget_ns / 1000U, std::memory_order_release);
    diagnostic_frame_limit_.store(frame_limit, std::memory_order_release);
    diagnostic_drawn_frames_.store(0, std::memory_order_release);
    diagnostic_consecutive_miss_limit_.store(std::max<std::uint32_t>(1U, consecutive_miss_limit),
                                             std::memory_order_release);
    diagnostic_failed_.store(false, std::memory_order_release);
    for (std::atomic<std::uint64_t>& miss_counter : diagnostic_stage_consecutive_misses_) {
        miss_counter.store(0, std::memory_order_release);
    }
    diagnostic_completed_.store(false, std::memory_order_release);
}

LivePipelineTraceEvent LivePipelineTrace::record(const LivePipelineStage stage, const LiveFrameId& frame_id,
                                                 const std::uint64_t timestamp_ns, const std::uint64_t latency_base_ns,
                                                 const std::uint32_t cuda_device_index,
                                                 const std::uint64_t surface_revision) noexcept {
    const std::size_t index = live_pipeline_stage_index(stage);
    const std::uint64_t event_count = stage_counters_[index].fetch_add(1, std::memory_order_acq_rel) + 1U;
    const std::uint64_t previous_session_nonce = last_session_nonce_.load(std::memory_order_acquire);
    const std::uint64_t previous_sequence = last_sequence_.load(std::memory_order_acquire);
    const std::uint64_t previous_timestamp_ns = last_timestamp_ns_.load(std::memory_order_acquire);
    const bool can_use_previous_event_latency =
        latency_base_ns == 0U && stage_is_diagnostic_budgeted(stage) && previous_timestamp_ns != 0U &&
        previous_session_nonce == frame_id.session_nonce && previous_sequence == frame_id.sequence &&
        timestamp_ns >= previous_timestamp_ns;
    const std::uint64_t latency_origin_ns = latency_base_ns != 0U            ? latency_base_ns
                                            : can_use_previous_event_latency ? previous_timestamp_ns
                                                                             : 0U;
    const std::uint64_t latency_us =
        latency_origin_ns != 0U && timestamp_ns >= latency_origin_ns ? (timestamp_ns - latency_origin_ns) / 1000U : 0U;

    last_stage_index_.store(static_cast<std::uint32_t>(index), std::memory_order_release);
    last_session_nonce_.store(frame_id.session_nonce, std::memory_order_release);
    last_sequence_.store(frame_id.sequence, std::memory_order_release);
    last_event_count_.store(event_count, std::memory_order_release);
    last_timestamp_ns_.store(timestamp_ns, std::memory_order_release);
    last_latency_us_.store(latency_us, std::memory_order_release);
    last_cuda_device_index_.store(cuda_device_index, std::memory_order_release);
    last_surface_revision_.store(surface_revision, std::memory_order_release);

    LivePipelineTraceEvent event{
        stage,
        live_pipeline_stage_name(stage),
        frame_id,
        event_count,
        timestamp_ns,
        latency_us,
        cuda_device_index,
        surface_revision,
    };
    if (diagnostic_enabled_.load(std::memory_order_acquire)) {
        const std::uint64_t budget_us = diagnostic_frame_budget_us_.load(std::memory_order_acquire);
        try {
            mmltk::logging::logger("live.pipeline")
                ->info("single-buffer diagnostic stage={} frame={} revision={} latency_us={} budget_us={}",
                       event.stage_name, frame_id.sequence, surface_revision, latency_us, budget_us);
        } catch (...) {
        }
        if (stage == LivePipelineStage::CefPresentFrontSlot) {
            const std::uint64_t drawn_frames = diagnostic_drawn_frames_.fetch_add(1, std::memory_order_acq_rel) + 1U;
            const std::uint32_t frame_limit = diagnostic_frame_limit_.load(std::memory_order_acquire);
            if (frame_limit != 0U && drawn_frames >= frame_limit) {
                diagnostic_completed_.store(true, std::memory_order_release);
            }
        }
        if (stage_is_diagnostic_budgeted(stage)) {
            const bool missed_budget = budget_us != 0U && latency_us > budget_us;
            const std::uint64_t consecutive_misses =
                missed_budget ? diagnostic_stage_consecutive_misses_[index].fetch_add(1, std::memory_order_acq_rel) + 1U
                              : 0U;
            if (!missed_budget) {
                diagnostic_stage_consecutive_misses_[index].store(0, std::memory_order_release);
            } else if (consecutive_misses >= diagnostic_consecutive_miss_limit_.load(std::memory_order_acquire) &&
                       !diagnostic_failed_.exchange(true, std::memory_order_acq_rel)) {
                diagnostic_failure_stage_index_.store(static_cast<std::uint32_t>(index), std::memory_order_release);
                diagnostic_failure_session_nonce_.store(frame_id.session_nonce, std::memory_order_release);
                diagnostic_failure_sequence_.store(frame_id.sequence, std::memory_order_release);
                diagnostic_failure_event_count_.store(event_count, std::memory_order_release);
                diagnostic_failure_timestamp_ns_.store(timestamp_ns, std::memory_order_release);
                diagnostic_failure_latency_us_.store(latency_us, std::memory_order_release);
                diagnostic_failure_cuda_device_index_.store(cuda_device_index, std::memory_order_release);
                diagnostic_failure_surface_revision_.store(surface_revision, std::memory_order_release);
            }
        }
    }
    return event;
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
    const std::uint32_t raw_stage = last_stage_index_.load(std::memory_order_acquire);
    const std::size_t stage_index = std::min<std::size_t>(raw_stage, kLivePipelineStageCount - 1U);
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
    out.diagnostic_enabled = diagnostic_enabled_.load(std::memory_order_acquire);
    out.diagnostic_failed = diagnostic_failed_.load(std::memory_order_acquire);
    out.diagnostic_frame_budget_us = diagnostic_frame_budget_us_.load(std::memory_order_acquire);
    out.diagnostic_frame_limit = diagnostic_frame_limit_.load(std::memory_order_acquire);
    out.diagnostic_drawn_frames = diagnostic_drawn_frames_.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < kLivePipelineStageCount; ++index) {
        out.diagnostic_consecutive_misses =
            std::max(out.diagnostic_consecutive_misses,
                     diagnostic_stage_consecutive_misses_[index].load(std::memory_order_acquire));
    }
    out.diagnostic_completed = diagnostic_completed_.load(std::memory_order_acquire);
    const std::uint32_t raw_failure_stage = diagnostic_failure_stage_index_.load(std::memory_order_acquire);
    const std::size_t failure_stage_index = std::min<std::size_t>(raw_failure_stage, kLivePipelineStageCount - 1U);
    out.diagnostic_failure_event.stage = static_cast<LivePipelineStage>(failure_stage_index);
    out.diagnostic_failure_event.stage_name = kStageNames[failure_stage_index];
    out.diagnostic_failure_event.frame_id = LiveFrameId{
        diagnostic_failure_session_nonce_.load(std::memory_order_acquire),
        diagnostic_failure_sequence_.load(std::memory_order_acquire),
    };
    out.diagnostic_failure_event.event_count = diagnostic_failure_event_count_.load(std::memory_order_acquire);
    out.diagnostic_failure_event.timestamp_ns = diagnostic_failure_timestamp_ns_.load(std::memory_order_acquire);
    out.diagnostic_failure_event.latency_us = diagnostic_failure_latency_us_.load(std::memory_order_acquire);
    out.diagnostic_failure_event.cuda_device_index =
        diagnostic_failure_cuda_device_index_.load(std::memory_order_acquire);
    out.diagnostic_failure_event.surface_revision =
        diagnostic_failure_surface_revision_.load(std::memory_order_acquire);
    return out;
}

}  // namespace mmltk::live
