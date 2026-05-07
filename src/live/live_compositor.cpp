#include "mmltk/live/live_compositor.h"

#include "live/live_helpers.h"
#include "live/live_worker_logging.h"
#include "mmltk/live/live_analyzer_worker.h"
#include "mmltk/live/live_frame_fanout.h"
#include "mmltk/live/live_manual_overlay_worker.h"
#include "mmltk/live/live_worker_runtime.h"
#include "mmltk/rfdetr/draw_cuda.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mmltk::live {

namespace {

constexpr auto kCompositorNoWorkBackoff = std::chrono::microseconds(250);

using WorkspaceSlot =
    worker_runtime::LiveFrameSlotCommon<WorkspaceDimensions, WorkspaceOutputBundle, DmaBufCudaRgbaSurface>;

[[nodiscard]] WorkspaceSwapchainSlotDescriptor workspace_swapchain_slot_descriptor(
    const WorkspaceSlot& slot, const std::uint32_t width, const std::uint32_t height,
    const std::uint64_t revision) {
    return WorkspaceSwapchainSlotDescriptor{
        slot.slot_index,
        WorkspaceDimensions{width, height, slot.device_buffer.pitch_bytes()},
        slot.device_buffer.dmabuf_image(width, height),
        reinterpret_cast<std::uintptr_t>(slot.ready_event.get()),
        reinterpret_cast<std::uintptr_t>(slot.stream.get()),
        revision,
    };
}

[[nodiscard]] mmltk::rfdetr::draw_launch::WorkspaceRgbaTarget workspace_target_for_surface(
    const DmaBufCudaRgbaSurface& surface, const int width, const int height, const char* context) {
    if (surface.is_pitch_frame()) {
        return mmltk::rfdetr::draw_launch::make_workspace_rgba_pitched_target(device_ptr_as_bytes(surface.data()),
                                                                              surface.pitch_bytes(), width, height);
    }
    if (surface.is_array_frame()) {
        return mmltk::rfdetr::draw_launch::make_workspace_rgba_surface_target(surface.surface_object(), width, height);
    }
    throw std::runtime_error(context);
}

void composite_rgba_over_workspace(cudaStream_t stream, const mmltk::rfdetr::draw_launch::WorkspaceRgbaTarget& dst,
                                   const CUdeviceptr src, const std::size_t src_pitch_bytes, const int width,
                                   const int height, const char* context) {
    mmltk::rfdetr::launch_composite_rgba_over_workspace_rgba(dst, device_ptr_as_bytes(src), src_pitch_bytes, width,
                                                             height, stream);
    ensure_cuda_ok(cudaPeekAtLastError(), context);
}

template <typename OverlayWorker>
void release_overlay_silently(OverlayWorker* worker, const std::uint32_t slot_index, const char* worker_name) noexcept {
    if (worker == nullptr) {
        return;
    }
    try {
        worker->release_overlay(slot_index);
    } catch (const std::exception& error) {
        log_live_worker_message("live.compositor", "error",
                                std::string("mmltk live compositor release error: failed to release ") + worker_name +
                                    " overlay slot " + std::to_string(slot_index) + ": " + error.what());
    }
}

template <typename OverlayWorker, typename OverlayView, typename UseFn>
bool with_latest_overlay(OverlayWorker* worker, const char* worker_name, UseFn&& use_fn) {
    if (worker == nullptr) {
        return false;
    }

    OverlayView overlay{};
    if (!worker->try_acquire_latest_overlay(&overlay)) {
        return false;
    }

    auto overlay_release =
        worker_runtime::make_scoped_rollback([worker, slot_index = overlay.slot_index, worker_name]() noexcept {
            release_overlay_silently(worker, slot_index, worker_name);
        });
    use_fn(overlay);
    return true;
}

}  // namespace

struct LiveCompositor::Impl {
    struct RawBaseCacheState {
        struct Slot {
            BgrPitchedDeviceBuffer device_buffer;
            CudaEventHandle ready_event;
            LiveFrameId frame_id{};
            LiveCaptureRegion region{};
            bool occupied = false;
        };

        void initialize(const std::uint32_t max_capture_width, const std::uint32_t max_capture_height) {
            std::lock_guard<std::mutex> lock(mutex);
            slots.resize(slot_count);
            worker_runtime::initialize_slot_array(slots, [&](Slot& slot, const std::uint32_t) {
                worker_runtime::initialize_pitched_event_resource(slot, max_capture_width, max_capture_height,
                                                                  "cudaMallocPitch for live raw-base cache",
                                                                  "cudaEventCreateWithFlags for live raw-base cache");
            });
            next_index = 0;
        }

        void reset() noexcept {
            std::lock_guard<std::mutex> lock(mutex);
            for (Slot& slot : slots) {
                slot.occupied = false;
                worker_runtime::reset_pitched_event_resource(slot);
            }
            slots.clear();
            next_index = 0;
        }

        void clear_for_restart() noexcept {
            std::lock_guard<std::mutex> lock(mutex);
            for (Slot& slot : slots) {
                slot.occupied = false;
                slot.frame_id = {};
                slot.region = {};
            }
            next_index = 0;
        }

        void store(cudaStream_t source_stream, const OutputBundle& base, const std::size_t copy_width_bytes) {
            std::lock_guard<std::mutex> lock(mutex);
            if (slots.empty()) {
                throw std::runtime_error("live compositor raw-base cache index out of range");
            }
            Slot& cache = slots[next_index];
            ensure_cuda_ok(
                cudaMemcpy2DAsync(device_ptr_as_void(cache.device_buffer.data()), cache.device_buffer.pitch_bytes(),
                                  device_ptr_as_const_void(base.data), base.dims.pitch_bytes, copy_width_bytes,
                                  base.dims.height, cudaMemcpyDeviceToDevice, source_stream),
                "cudaMemcpy2DAsync for raw-base cache");
            ensure_cuda_ok(cudaEventRecord(cache.ready_event.get(), source_stream),
                           "cudaEventRecord for raw-base cache");
            cache.frame_id = base.frame_id;
            cache.region = base.region;
            cache.occupied = true;
            next_index = (next_index + 1U) % slots.size();
        }

        template <typename ReadbackFn>
        bool with_slot(const LiveFrameId& frame_id, ReadbackFn&& readback_fn, std::string* error_message) const {
            std::lock_guard<std::mutex> lock(mutex);
            for (const Slot& slot : slots) {
                if (slot.occupied && slot.frame_id == frame_id) {
                    return readback_fn(slot);
                }
            }
            if (error_message != nullptr) {
                *error_message = "raw-base readback failed because the requested live frame is no longer cached";
            }
            return false;
        }

        mutable std::mutex mutex;
        std::uint32_t slot_count = 0;
        std::vector<Slot> slots;
        std::size_t next_index = 0;
    };

    struct RetainedAnalysisState {
        void initialize(const std::uint32_t max_capture_width, const std::uint32_t max_capture_height) {
            worker_runtime::initialize_pitched_device_resource(
                *this, max_capture_width, max_capture_height, "cudaMallocPitch for retained analysis overlay",
                "cudaStreamCreateWithPriority for retained analysis overlay",
                "cudaEventCreateWithFlags for retained analysis overlay");
        }

        void reset() noexcept {
            worker_runtime::reset_pitched_device_resource(*this);
            has_content = false;
        }

        void clear_for_restart() noexcept {
            has_content = false;
        }

        bool composite_into(cudaStream_t stream_in, const mmltk::rfdetr::draw_launch::WorkspaceRgbaTarget& dst,
                            const OutputBundle& base, const char* context) const {
            if (!has_content) {
                return false;
            }
            if (ready_event.get() != nullptr) {
                ensure_cuda_ok(cudaStreamWaitEvent(stream_in, ready_event.get(), 0), context);
            }
            composite_rgba_over_workspace(stream_in, dst, device_buffer.data(), device_buffer.pitch_bytes(),
                                          static_cast<int>(base.dims.width), static_cast<int>(base.dims.height),
                                          context);
            return true;
        }

        void update_from_analysis(const LiveAnalyzerWorker::AnalysisOverlayView& analysis) {
            has_content = analysis.has_content;
            if (!analysis.has_content) {
                return;
            }
            if (analysis.ready_event != nullptr) {
                ensure_cuda_ok(cudaStreamWaitEvent(stream.get(), analysis.ready_event, 0),
                               "cudaStreamWaitEvent for retained analysis overlay");
            }
            ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(device_buffer.data()), device_buffer.pitch_bytes(),
                                             device_ptr_as_const_void(analysis.data), analysis.pitch_bytes,
                                             static_cast<std::size_t>(analysis.width) * 4U, analysis.height,
                                             cudaMemcpyDeviceToDevice, stream.get()),
                           "cudaMemcpy2DAsync for retained analysis overlay");
            ensure_cuda_ok(cudaEventRecord(ready_event.get(), stream.get()),
                           "cudaEventRecord for retained analysis overlay");
        }

        RgbaPitchedDeviceBuffer device_buffer;
        CudaEventHandle ready_event;
        CudaStreamHandle stream;
        bool has_content = false;
    };

    struct ReadbackState {
        void initialize() {
            worker_runtime::initialize_stream_event_resource(*this,
                                                             "cudaStreamCreateWithPriority for compositor readback",
                                                             "cudaEventCreateWithFlags for compositor readback");
        }

        void reset() noexcept {
            host_buffer.reset();
            worker_runtime::reset_stream_event_resource(*this);
        }

        bool copy_slot(const RawBaseCacheState::Slot& slot, const int cuda_device_index,
                       std::vector<std::uint8_t>* pixels_bgr, std::uint32_t* width, std::uint32_t* height,
                       LiveCaptureRegion* region, std::string* error_message) {
            std::lock_guard<std::mutex> lock(mutex);
            ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for compositor readback");

            const std::size_t row_bytes = static_cast<std::size_t>(slot.region.width) * 3U;
            const std::size_t required_bytes = row_bytes * static_cast<std::size_t>(slot.region.height);
            host_buffer.ensure_capacity(required_bytes, "cudaHostAlloc for compositor readback");
            if (slot.ready_event.get() != nullptr) {
                ensure_cuda_ok(cudaStreamWaitEvent(stream.get(), slot.ready_event.get(), 0),
                               "cudaStreamWaitEvent for compositor readback");
            }
            ensure_cuda_ok(
                cudaMemcpy2DAsync(host_buffer.data(), row_bytes, device_ptr_as_const_void(slot.device_buffer.data()),
                                  slot.device_buffer.pitch_bytes(), row_bytes, slot.region.height,
                                  cudaMemcpyDeviceToHost, stream.get()),
                "cudaMemcpy2DAsync for compositor readback");
            ensure_cuda_ok(cudaEventRecord(ready_event.get(), stream.get()), "cudaEventRecord for compositor readback");
            ensure_cuda_ok(cudaEventSynchronize(ready_event.get()), "cudaEventSynchronize for compositor readback");

            pixels_bgr->assign(host_buffer.data(), host_buffer.data() + required_bytes);
            *width = slot.region.width;
            *height = slot.region.height;
            *region = slot.region;
            if (error_message != nullptr) {
                error_message->clear();
            }
            return true;
        }

        std::mutex mutex;
        CudaStreamHandle stream;
        CudaEventHandle ready_event;
        PinnedUploadBuffer<std::uint8_t> host_buffer;
    };

    Impl(LiveFrameFanout& fanout_in, LiveAnalyzerWorker* analyzer_worker_in, LiveManualOverlayWorker* manual_worker_in,
         const int cuda_device_index_in, const std::uint32_t max_capture_width_in,
         const std::uint32_t max_capture_height_in, const std::uint32_t output_slot_count,
         const std::uint32_t raw_base_cache_slot_count, std::shared_ptr<LivePipelineTrace> pipeline_trace_in)
        : fanout(fanout_in),
          analyzer_worker(analyzer_worker_in),
          manual_worker(manual_worker_in),
          cuda_device_index(cuda_device_index_in),
          pipeline_trace(std::move(pipeline_trace_in)),
          max_capture_width(std::max<std::uint32_t>(1U, max_capture_width_in)),
          max_capture_height(std::max<std::uint32_t>(1U, max_capture_height_in)),
          raw_base_cache(std::make_unique<RawBaseCacheState>()),
          retained_analysis(std::make_unique<RetainedAnalysisState>()),
          readback(std::make_unique<ReadbackState>()) {
        if (output_slot_count == 0U || raw_base_cache_slot_count == 0U) {
            throw std::runtime_error("live compositor requires output slots and raw-base cache slots");
        }
        workspace_slots.reserve(output_slot_count);
        raw_base_cache->slot_count = raw_base_cache_slot_count;
        allocate_resources();
    }

    ~Impl() {
        try {
            stop();
        } catch (const std::exception& error) {
            log_live_worker_message("live.compositor", "error", error.what());
        }
        reset_runtime_state();
    }

    void start() {
        if (running.load(std::memory_order_acquire)) {
            throw std::runtime_error("live compositor is already running");
        }
        if (thread.joinable()) {
            throw std::runtime_error("live compositor must be stopped before restart");
        }

        publish_error({});
        worker_runtime::publish_status(&status, Status{});
        reset_local_status_for_start();
        if (workspace_slots.empty()) {
            allocate_resources();
        }
        worker_runtime::start_worker_thread(
            thread, stop_requested, running, [this]() { compositor_thread_main(); },
            "live compositor is already running", "live compositor must be stopped before restart");
    }

    void stop() {
        stop_requested.store(true, std::memory_order_release);
        fanout.notify_output_waiters();
        if (thread.joinable()) {
            thread.join();
        }
        running.store(false, std::memory_order_release);
        reset_runtime_state_for_restart();
    }

    [[nodiscard]] bool running_state() const noexcept {
        return running.load(std::memory_order_acquire);
    }

    [[nodiscard]] WorkspaceSwapchainDescriptor workspace_swapchain_descriptor() const {
        WorkspaceSwapchainDescriptor descriptor;
        descriptor.width = max_capture_width;
        descriptor.height = max_capture_height;
        descriptor.generation = workspace_swapchain_generation.load(std::memory_order_acquire);
        descriptor.slots.reserve(workspace_slots.size());
        for (const auto& slot : workspace_slots) {
            const std::uint32_t slot_index = slot->slot_index;
            const std::uint64_t revision =
                slot_index < workspace_slot_revisions.size()
                    ? workspace_slot_revisions[slot_index]->load(std::memory_order_acquire)
                    : 0U;
            descriptor.slots.push_back(
                workspace_swapchain_slot_descriptor(*slot, max_capture_width, max_capture_height, revision));
        }
        return descriptor;
    }

    [[nodiscard]] WorkspacePresentSnapshot latest_workspace_present() const noexcept {
        for (std::uint32_t attempt = 0; attempt < 4U; ++attempt) {
            const std::uint64_t begin_generation = present_snapshot_generation.load(std::memory_order_acquire);
            if ((begin_generation & 1U) != 0U) {
                continue;
            }

            WorkspacePresentSnapshot snapshot;
            snapshot.valid = present_valid.load(std::memory_order_relaxed);
            snapshot.front_slot_index = present_front_slot_index.load(std::memory_order_relaxed);
            snapshot.revision = present_revision.load(std::memory_order_relaxed);
            snapshot.swapchain_generation = present_swapchain_generation.load(std::memory_order_relaxed);
            snapshot.frame_id = LiveFrameId{present_frame_session_nonce.load(std::memory_order_relaxed),
                                            present_frame_sequence.load(std::memory_order_relaxed)};
            snapshot.dims = WorkspaceDimensions{
                present_width.load(std::memory_order_relaxed),
                present_height.load(std::memory_order_relaxed),
                static_cast<std::size_t>(present_pitch_bytes.load(std::memory_order_relaxed)),
            };
            snapshot.dmabuf_image = WorkspaceDmaBufImage{
                present_dmabuf_fd.load(std::memory_order_relaxed),
                present_dmabuf_width.load(std::memory_order_relaxed),
                present_dmabuf_height.load(std::memory_order_relaxed),
                present_dmabuf_stride_bytes.load(std::memory_order_relaxed),
                present_dmabuf_offset.load(std::memory_order_relaxed),
                present_dmabuf_allocation_size.load(std::memory_order_relaxed),
                present_dmabuf_drm_format.load(std::memory_order_relaxed),
                present_dmabuf_drm_modifier.load(std::memory_order_relaxed),
                static_cast<DmaBufModifierMode>(present_dmabuf_modifier_mode.load(std::memory_order_relaxed)),
            };
            snapshot.source_region = LiveCaptureRegion{
                present_source_x.load(std::memory_order_relaxed),
                present_source_y.load(std::memory_order_relaxed),
                present_source_width.load(std::memory_order_relaxed),
                present_source_height.load(std::memory_order_relaxed),
            };
            snapshot.dirty_rect = LiveCaptureRegion{
                present_dirty_x.load(std::memory_order_relaxed),
                present_dirty_y.load(std::memory_order_relaxed),
                present_dirty_width.load(std::memory_order_relaxed),
                present_dirty_height.load(std::memory_order_relaxed),
            };
            snapshot.ready_event_handle = present_ready_event_handle.load(std::memory_order_relaxed);
            snapshot.producer_stream_handle = present_producer_stream_handle.load(std::memory_order_relaxed);
            snapshot.capture_ns = present_capture_ns.load(std::memory_order_relaxed);
            snapshot.ready_ns = present_ready_ns.load(std::memory_order_relaxed);
            snapshot.short_frame = present_short_frame.load(std::memory_order_relaxed);

            const std::uint64_t end_generation = present_snapshot_generation.load(std::memory_order_acquire);
            if (begin_generation == end_generation && (end_generation & 1U) == 0U) {
                return snapshot;
            }
        }
        return {};
    }

    [[nodiscard]] bool try_acquire_latest_workspace(WorkspaceOutputBundle* out) {
        if (out == nullptr) {
            throw std::runtime_error("live compositor requires a workspace output bundle output");
        }
        *out = {};
        if (!running_state()) {
            return false;
        }
        const bool acquired = worker_runtime::try_acquire_latest_ready_published_slot_view(
            workspace_slots, latest_workspace_index, out, "cudaEventQuery for live compositor workspace acquisition");
        if (!acquired && pipeline_trace != nullptr) {
            pipeline_trace->note_acquire_miss();
        }
        return acquired;
    }

    void release_workspace(const std::uint32_t slot_index) {
        LiveFrameId released_frame_id{};
        if (slot_index < workspace_slots.size()) {
            released_frame_id = workspace_slots[slot_index]->frame_id;
        }
        worker_runtime::release_slot_by_index(
            workspace_slots, slot_index, "live compositor workspace release index out of range",
            "live compositor workspace slot release called for a slot that is not acquired");
        if (pipeline_trace != nullptr) {
            pipeline_trace->record(LivePipelineStage::BrowserReleaseSurface, released_frame_id,
                                   live_steady_clock_now_ns(), 0U, trace_cuda_device_index(),
                                   released_frame_id.sequence);
        }
    }

    void set_workspace_ready_callback(WorkspaceReadyCallback callback) {
        if (!callback) {
            workspace_ready_callback.store(nullptr, std::memory_order_release);
            return;
        }
        workspace_ready_callback.store(std::make_shared<const WorkspaceReadyCallback>(std::move(callback)),
                                       std::memory_order_release);
    }

    void set_workspace_present_callback(WorkspacePresentCallback callback) {
        if (!callback) {
            workspace_present_callback.store(nullptr, std::memory_order_release);
            return;
        }
        workspace_present_callback.store(std::make_shared<const WorkspacePresentCallback>(std::move(callback)),
                                         std::memory_order_release);
    }

    void mark_workspace_slot_in_flight(const std::uint32_t slot_index, const bool in_flight) {
        if (slot_index >= workspace_slot_in_flight.size()) {
            throw std::runtime_error("live compositor workspace in-flight index out of range");
        }
        workspace_slot_in_flight[slot_index]->store(in_flight, std::memory_order_release);
    }

    void set_persistent_analysis_overlay(const bool enabled) {
        persistent_analysis_overlay.store(enabled, std::memory_order_release);
        if (!enabled) {
            retained_analysis->has_content = false;
        }
    }

    [[nodiscard]] bool persistent_analysis_overlay_enabled() const noexcept {
        return persistent_analysis_overlay.load(std::memory_order_acquire);
    }

    [[nodiscard]] Status snapshot_status() const {
        Status s = worker_runtime::snapshot_status(status);
        s.running = running_state();
        s.frames_composited = frames_composited_count.load(std::memory_order_acquire);
        s.frames_composited_after_startup = frames_composited_after_startup_count.load(std::memory_order_acquire);
        s.frames_dropped = frames_dropped_count.load(std::memory_order_acquire);
        s.skipped_compositor_presents = skipped_compositor_presents_count.load(std::memory_order_acquire);
        s.front_slot_index = front_workspace_slot_index.load(std::memory_order_acquire);
        s.front_slot_revision = front_workspace_revision.load(std::memory_order_acquire);
        s.last_frame_id = LiveFrameId{last_frame_session_nonce.load(std::memory_order_acquire),
                                      last_frame_sequence.load(std::memory_order_acquire)};
        s.analysis_overlay_active = analysis_overlay_active.load(std::memory_order_acquire);
        s.manual_overlay_active = manual_overlay_active.load(std::memory_order_acquire);
        const std::shared_ptr<const std::string> error = last_error.load(std::memory_order_acquire);
        s.last_error = error != nullptr ? *error : std::string{};
        attach_pipeline_status(s);
        return s;
    }

    [[nodiscard]] bool readback_raw_base(const LiveFrameId& frame_id, std::vector<std::uint8_t>* pixels_bgr,
                                         std::uint32_t* width, std::uint32_t* height, LiveCaptureRegion* region,
                                         std::string* error_message) {
        if (pixels_bgr == nullptr || width == nullptr || height == nullptr || region == nullptr) {
            throw std::runtime_error("live compositor readback requires non-null outputs");
        }
        return raw_base_cache->with_slot(
            frame_id,
            [&](const RawBaseCacheState::Slot& slot) {
                return readback->copy_slot(slot, cuda_device_index, pixels_bgr, width, height, region, error_message);
            },
            error_message);
    }

    void allocate_resources() {
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for live compositor resource allocation");

        try {
            workspace_interop_device.ensure_open(cuda_device_index,
                                                 "GBM/EGL render node open for live compositor workspace output");
            worker_runtime::allocate_dmabuf_cuda_rgba_slots(
                workspace_slots, static_cast<std::uint32_t>(workspace_slots.capacity()), workspace_interop_device,
                cuda_device_index, max_capture_width, max_capture_height,
                "GBM/CUDA DMA-BUF allocation for live compositor workspace output",
                "cudaStreamCreateWithPriority for live compositor workspace output",
                "cudaEventCreateWithFlags for live compositor workspace output");
            initialize_workspace_slot_metadata();
            workspace_swapchain_generation.fetch_add(1U, std::memory_order_acq_rel);
            clear_latest_present_snapshot();

            raw_base_cache->initialize(max_capture_width, max_capture_height);
            retained_analysis->initialize(max_capture_width, max_capture_height);
            readback->initialize();
        } catch (...) {
            reset_runtime_state();
            throw;
        }
    }

    void reset_runtime_state_for_restart() noexcept {
        (void)cudaSetDevice(cuda_device_index);
        try {
            worker_runtime::reset_slot_views_for_restart(workspace_slots, latest_workspace_index,
                                                         "cudaEventSynchronize for live compositor workspace restart");
        } catch (const std::exception& error) {
            log_live_worker_message("live.compositor", "error", error.what());
        }
        raw_base_cache->clear_for_restart();
        retained_analysis->clear_for_restart();
        reset_workspace_slot_metadata_for_restart();
        clear_latest_present_snapshot();
    }

    void reset_runtime_state() noexcept {
        (void)cudaSetDevice(cuda_device_index);
        workspace_slots.clear();
        workspace_slot_revisions.clear();
        workspace_slot_in_flight.clear();
        latest_workspace_index.store(-1, std::memory_order_release);
        front_workspace_slot_index.store(-1, std::memory_order_release);
        retired_front_workspace_slot_index.store(-1, std::memory_order_release);
        front_workspace_revision.store(0U, std::memory_order_release);
        clear_latest_present_snapshot();
        raw_base_cache->reset();
        retained_analysis->reset();
        readback->reset();
        workspace_interop_device.reset();
    }

    void initialize_workspace_slot_metadata() {
        workspace_slot_revisions.clear();
        workspace_slot_in_flight.clear();
        workspace_slot_revisions.reserve(workspace_slots.size());
        workspace_slot_in_flight.reserve(workspace_slots.size());
        for (std::size_t slot_index = 0; slot_index < workspace_slots.size(); ++slot_index) {
            workspace_slot_revisions.push_back(std::make_unique<std::atomic<std::uint64_t>>(0U));
            workspace_slot_in_flight.push_back(std::make_unique<std::atomic<bool>>(false));
        }
        front_workspace_slot_index.store(-1, std::memory_order_release);
        retired_front_workspace_slot_index.store(-1, std::memory_order_release);
        front_workspace_revision.store(0U, std::memory_order_release);
    }

    void reset_local_status_for_start() noexcept {
        frames_composited_count.store(0U, std::memory_order_release);
        frames_composited_after_startup_count.store(0U, std::memory_order_release);
        frames_dropped_count.store(0U, std::memory_order_release);
        skipped_compositor_presents_count.store(0U, std::memory_order_release);
        last_frame_session_nonce.store(0U, std::memory_order_release);
        last_frame_sequence.store(0U, std::memory_order_release);
        analysis_overlay_active.store(false, std::memory_order_release);
        manual_overlay_active.store(false, std::memory_order_release);
        first_workspace_frame_logged = false;
        workspace_frame_log_count = 0U;
    }

    void reset_workspace_slot_metadata_for_restart() noexcept {
        for (auto& revision : workspace_slot_revisions) {
            revision->store(0U, std::memory_order_release);
        }
        for (auto& in_flight : workspace_slot_in_flight) {
            in_flight->store(false, std::memory_order_release);
        }
        front_workspace_slot_index.store(-1, std::memory_order_release);
        retired_front_workspace_slot_index.store(-1, std::memory_order_release);
        front_workspace_revision.store(0U, std::memory_order_release);
    }

    void publish_front_workspace_slot(const std::uint32_t slot_index, const std::uint64_t revision) noexcept {
        if (slot_index < workspace_slot_in_flight.size()) {
            workspace_slot_in_flight[slot_index]->store(false, std::memory_order_release);
        }
        const int next_front = static_cast<int>(slot_index);
        const int old_front = front_workspace_slot_index.exchange(next_front, std::memory_order_acq_rel);
        front_workspace_revision.store(revision, std::memory_order_release);
        if (old_front < 0 || old_front == next_front) {
            return;
        }

        const int old_retired = retired_front_workspace_slot_index.exchange(old_front, std::memory_order_acq_rel);
        if (old_front < static_cast<int>(workspace_slot_in_flight.size())) {
            workspace_slot_in_flight[static_cast<std::size_t>(old_front)]->store(true, std::memory_order_release);
        }
        if (old_retired >= 0 && old_retired != old_front && old_retired != next_front &&
            old_retired < static_cast<int>(workspace_slot_in_flight.size())) {
            workspace_slot_in_flight[static_cast<std::size_t>(old_retired)]->store(false, std::memory_order_release);
        }
    }

    void clear_latest_present_snapshot() noexcept {
        const std::uint64_t begin_generation = present_snapshot_generation.fetch_add(1U, std::memory_order_acq_rel);
        (void)begin_generation;
        present_valid.store(false, std::memory_order_relaxed);
        present_front_slot_index.store(0U, std::memory_order_relaxed);
        present_revision.store(0U, std::memory_order_relaxed);
        present_swapchain_generation.store(0U, std::memory_order_relaxed);
        present_frame_session_nonce.store(0U, std::memory_order_relaxed);
        present_frame_sequence.store(0U, std::memory_order_relaxed);
        present_width.store(0U, std::memory_order_relaxed);
        present_height.store(0U, std::memory_order_relaxed);
        present_pitch_bytes.store(0U, std::memory_order_relaxed);
        present_dmabuf_fd.store(-1, std::memory_order_relaxed);
        present_dmabuf_width.store(0U, std::memory_order_relaxed);
        present_dmabuf_height.store(0U, std::memory_order_relaxed);
        present_dmabuf_stride_bytes.store(0U, std::memory_order_relaxed);
        present_dmabuf_offset.store(0U, std::memory_order_relaxed);
        present_dmabuf_allocation_size.store(0U, std::memory_order_relaxed);
        present_dmabuf_drm_format.store(0U, std::memory_order_relaxed);
        present_dmabuf_drm_modifier.store(0U, std::memory_order_relaxed);
        present_dmabuf_modifier_mode.store(static_cast<std::uint8_t>(DmaBufModifierMode::Unknown),
                                           std::memory_order_relaxed);
        present_source_x.store(0U, std::memory_order_relaxed);
        present_source_y.store(0U, std::memory_order_relaxed);
        present_source_width.store(0U, std::memory_order_relaxed);
        present_source_height.store(0U, std::memory_order_relaxed);
        present_dirty_x.store(0U, std::memory_order_relaxed);
        present_dirty_y.store(0U, std::memory_order_relaxed);
        present_dirty_width.store(0U, std::memory_order_relaxed);
        present_dirty_height.store(0U, std::memory_order_relaxed);
        present_ready_event_handle.store(0U, std::memory_order_relaxed);
        present_producer_stream_handle.store(0U, std::memory_order_relaxed);
        present_capture_ns.store(0U, std::memory_order_relaxed);
        present_ready_ns.store(0U, std::memory_order_relaxed);
        present_short_frame.store(false, std::memory_order_relaxed);
        present_snapshot_generation.fetch_add(1U, std::memory_order_release);
    }

    void store_latest_present_snapshot(const WorkspacePresentSnapshot& snapshot) noexcept {
        present_snapshot_generation.fetch_add(1U, std::memory_order_acq_rel);
        present_valid.store(snapshot.valid, std::memory_order_relaxed);
        present_front_slot_index.store(snapshot.front_slot_index, std::memory_order_relaxed);
        present_revision.store(snapshot.revision, std::memory_order_relaxed);
        present_swapchain_generation.store(snapshot.swapchain_generation, std::memory_order_relaxed);
        present_frame_session_nonce.store(snapshot.frame_id.session_nonce, std::memory_order_relaxed);
        present_frame_sequence.store(snapshot.frame_id.sequence, std::memory_order_relaxed);
        present_width.store(snapshot.dims.width, std::memory_order_relaxed);
        present_height.store(snapshot.dims.height, std::memory_order_relaxed);
        present_pitch_bytes.store(static_cast<std::uint64_t>(snapshot.dims.pitch_bytes), std::memory_order_relaxed);
        present_dmabuf_fd.store(snapshot.dmabuf_image.fd, std::memory_order_relaxed);
        present_dmabuf_width.store(snapshot.dmabuf_image.width, std::memory_order_relaxed);
        present_dmabuf_height.store(snapshot.dmabuf_image.height, std::memory_order_relaxed);
        present_dmabuf_stride_bytes.store(snapshot.dmabuf_image.stride_bytes, std::memory_order_relaxed);
        present_dmabuf_offset.store(snapshot.dmabuf_image.offset, std::memory_order_relaxed);
        present_dmabuf_allocation_size.store(snapshot.dmabuf_image.allocation_size, std::memory_order_relaxed);
        present_dmabuf_drm_format.store(snapshot.dmabuf_image.drm_format, std::memory_order_relaxed);
        present_dmabuf_drm_modifier.store(snapshot.dmabuf_image.drm_modifier, std::memory_order_relaxed);
        present_dmabuf_modifier_mode.store(static_cast<std::uint8_t>(snapshot.dmabuf_image.modifier_mode),
                                           std::memory_order_relaxed);
        present_source_x.store(snapshot.source_region.x, std::memory_order_relaxed);
        present_source_y.store(snapshot.source_region.y, std::memory_order_relaxed);
        present_source_width.store(snapshot.source_region.width, std::memory_order_relaxed);
        present_source_height.store(snapshot.source_region.height, std::memory_order_relaxed);
        present_dirty_x.store(snapshot.dirty_rect.x, std::memory_order_relaxed);
        present_dirty_y.store(snapshot.dirty_rect.y, std::memory_order_relaxed);
        present_dirty_width.store(snapshot.dirty_rect.width, std::memory_order_relaxed);
        present_dirty_height.store(snapshot.dirty_rect.height, std::memory_order_relaxed);
        present_ready_event_handle.store(snapshot.ready_event_handle, std::memory_order_relaxed);
        present_producer_stream_handle.store(snapshot.producer_stream_handle, std::memory_order_relaxed);
        present_capture_ns.store(snapshot.capture_ns, std::memory_order_relaxed);
        present_ready_ns.store(snapshot.ready_ns, std::memory_order_relaxed);
        present_short_frame.store(snapshot.short_frame, std::memory_order_relaxed);
        present_snapshot_generation.fetch_add(1U, std::memory_order_release);
    }

    void compositor_thread_main() {
        worker_runtime::run_worker_thread_main(
            running, stop_requested, status, cuda_device_index, "cudaSetDevice for live compositor thread",
            "live.compositor",
            [this](Status&) {
                OutputBundle base{};
                if (!fanout.try_acquire_output(&base)) {
                    if (!stop_requested.load(std::memory_order_acquire) && pipeline_trace != nullptr) {
                        pipeline_trace->note_acquire_miss();
                        pipeline_trace->record(LivePipelineStage::CompositorAcquireOutput, last_workspace_frame_id,
                                               live_steady_clock_now_ns(), 0U, trace_cuda_device_index(),
                                               last_workspace_frame_id.sequence);
                    }
                    if (!stop_requested.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(kCompositorNoWorkBackoff);
                    }
                    return;
                }
                if (pipeline_trace != nullptr) {
                    const std::uint64_t acquired_ns = live_steady_clock_now_ns();
                    pipeline_trace->record(LivePipelineStage::CompositorAcquireOutput, base.frame_id, acquired_ns,
                                           base.capture_ns, trace_cuda_device_index(), base.frame_id.sequence);
                    const std::uint64_t acquire_latency_ns =
                        base.capture_ns != 0U && acquired_ns >= base.capture_ns ? acquired_ns - base.capture_ns : 0U;
                    warn_if_over_budget("compositor.acquire_output", base.frame_id, acquire_latency_ns);
                }
                auto base_release = worker_runtime::make_scoped_rollback(
                    [this, slot_index = base.slot_index]() noexcept { fanout.release_output(slot_index); });

                WorkspaceSlot* slot = reserve_workspace_slot();
                auto slot_release = worker_runtime::make_scoped_rollback([slot]() noexcept {
                    if (slot != nullptr) {
                        worker_runtime::release_writing_slot(*slot);
                    }
                });
                if (slot == nullptr) {
                    if (pipeline_trace != nullptr) {
                        pipeline_trace->note_acquire_miss();
                        pipeline_trace->record(LivePipelineStage::CompositorPublishWorkspace, base.frame_id,
                                               live_steady_clock_now_ns(), base.capture_ns, trace_cuda_device_index(),
                                               base.frame_id.sequence);
                    }
                    base_release.run_and_dismiss();
                    frames_dropped_count.fetch_add(1U, std::memory_order_relaxed);
                    const std::uint64_t skipped_count =
                        skipped_compositor_presents_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
                    if (live_should_log_periodic_frame(skipped_count)) {
                        log_live_worker_message(
                            "live.compositor", "warn",
                            "live compositor dropped workspace frame because no slot was available: frame=" +
                                std::to_string(base.frame_id.sequence) + " skipped_count=" +
                                std::to_string(skipped_count));
                    }
                    return;
                }

                if (base.ready_event != nullptr) {
                    ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), base.ready_event, 0),
                                   "cudaStreamWaitEvent for live compositor base");
                }

                const int frame_w = static_cast<int>(base.dims.width);
                const int frame_h = static_cast<int>(base.dims.height);
                const mmltk::rfdetr::draw_launch::WorkspaceRgbaTarget dst =
                    workspace_target_for_surface(slot->device_buffer, frame_w, frame_h,
                                                 "live compositor workspace surface has no CUDA write target");

                mmltk::rfdetr::launch_copy_bgr_to_workspace_rgba(device_ptr_as_bytes(base.data), base.dims.pitch_bytes,
                                                                 dst, frame_w, frame_h, 255U, slot->stream.get());
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_copy_bgr_to_workspace_rgba for live compositor base");

                bool analysis_active = false;
                auto composite_retained_analysis = [&](const char* context) {
                    analysis_active =
                        retained_analysis->composite_into(slot->stream.get(), dst, base, context) || analysis_active;
                };
                if (!with_latest_overlay<LiveAnalyzerWorker, LiveAnalyzerWorker::AnalysisOverlayView>(
                        analyzer_worker, "analysis", [&](const LiveAnalyzerWorker::AnalysisOverlayView& analysis) {
                            if (analysis.frame_id == base.frame_id) {
                                if (analysis.has_content) {
                                    if (analysis.ready_event != nullptr) {
                                        ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), analysis.ready_event, 0),
                                                       "cudaStreamWaitEvent for live compositor analysis");
                                    }
                                    composite_rgba_over_workspace(slot->stream.get(), dst, analysis.data,
                                                                  analysis.pitch_bytes, frame_w, frame_h,
                                                                  "launch_composite_rgba_over_workspace_rgba analysis");
                                    analysis_active = true;
                                }
                                if (persistent_analysis_overlay_enabled()) {
                                    retained_analysis->update_from_analysis(analysis);
                                }
                            } else if (persistent_analysis_overlay_enabled() && retained_analysis->has_content) {
                                composite_retained_analysis(
                                    "launch_composite_rgba_over_workspace_rgba retained analysis");
                            }
                        })) {
                    if (persistent_analysis_overlay_enabled() && retained_analysis->has_content) {
                        composite_retained_analysis("launch_composite_rgba_over_workspace_rgba retained analysis");
                    }
                }

                bool manual_active = false;
                (void)with_latest_overlay<LiveManualOverlayWorker, LiveManualOverlayWorker::OverlayView>(
                    manual_worker, "manual", [&](const LiveManualOverlayWorker::OverlayView& manual) {
                        if (manual.has_content) {
                            if (manual.ready_event != nullptr) {
                                ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), manual.ready_event, 0),
                                               "cudaStreamWaitEvent for live compositor manual overlay");
                            }
                            composite_rgba_over_workspace(slot->stream.get(), dst, manual.data, manual.pitch_bytes,
                                                          frame_w, frame_h,
                                                          "launch_composite_rgba_over_workspace_rgba manual");
                            manual_active = true;
                        }
                    });

                ensure_cuda_ok(cudaEventRecord(slot->ready_event.get(), slot->stream.get()),
                               "cudaEventRecord for live compositor workspace output");
                slot->frame_id = base.frame_id;
                slot->region = base.region;
                slot->capture_ns = base.capture_ns;
                slot->ready_ns = live_steady_clock_now_ns();
                slot->short_frame = base.short_frame;
                worker_runtime::publish_latest_slot(*slot, latest_workspace_index);
                const WorkspacePresentSnapshot present_snapshot{
                    true,
                    slot->slot_index,
                    base.frame_id.sequence,
                    workspace_swapchain_generation.load(std::memory_order_acquire),
                    base.frame_id,
                    WorkspaceDimensions{base.dims.width, base.dims.height, slot->device_buffer.pitch_bytes()},
                    slot->device_buffer.dmabuf_image(base.dims.width, base.dims.height),
                    base.region,
                    LiveCaptureRegion{0U, 0U, base.dims.width, base.dims.height},
                    reinterpret_cast<std::uintptr_t>(slot->ready_event.get()),
                    reinterpret_cast<std::uintptr_t>(slot->stream.get()),
                    base.capture_ns,
                    slot->ready_ns,
                    base.short_frame,
                };
                if (slot->slot_index < workspace_slot_revisions.size()) {
                    workspace_slot_revisions[slot->slot_index]->store(present_snapshot.revision,
                                                                      std::memory_order_release);
                }
                publish_front_workspace_slot(slot->slot_index, present_snapshot.revision);
                store_latest_present_snapshot(present_snapshot);
                if (pipeline_trace != nullptr) {
                    pipeline_trace->record(LivePipelineStage::CompositorPublishWorkspace, base.frame_id, slot->ready_ns,
                                           base.capture_ns, trace_cuda_device_index(), base.frame_id.sequence);
                    pipeline_trace->mark_first_workspace_publication_ready();
                }
                const std::uint64_t publish_latency_ns =
                    base.capture_ns != 0U && slot->ready_ns >= base.capture_ns ? slot->ready_ns - base.capture_ns : 0U;
                const bool startup_frame_already_published = first_workspace_frame_logged;
                workspace_frame_log_count += 1U;
                if (!first_workspace_frame_logged) {
                    first_workspace_frame_logged = true;
                    log_live_worker_message(
                        "live.compositor", "info",
                        "first live workspace output published: frame=" + std::to_string(base.frame_id.sequence) +
                            " revision=" + std::to_string(base.frame_id.sequence));
                } else if (live_should_log_periodic_frame(workspace_frame_log_count)) {
                    log_live_worker_message(
                        "live.compositor", "info",
                        "live workspace output published: frame=" + std::to_string(base.frame_id.sequence) +
                            " revision=" + std::to_string(base.frame_id.sequence) +
                            " count=" + std::to_string(workspace_frame_log_count));
                }
                warn_if_over_budget("compositor.publish_workspace", base.frame_id, publish_latency_ns);
                slot_release.dismiss();

                base_release.run_and_dismiss();

                frames_composited_count.fetch_add(1U, std::memory_order_relaxed);
                if (startup_frame_already_published) {
                    frames_composited_after_startup_count.fetch_add(1U, std::memory_order_relaxed);
                }
                last_workspace_frame_id = base.frame_id;
                last_frame_session_nonce.store(base.frame_id.session_nonce, std::memory_order_release);
                last_frame_sequence.store(base.frame_id.sequence, std::memory_order_release);
                analysis_overlay_active.store(analysis_active, std::memory_order_release);
                manual_overlay_active.store(manual_active, std::memory_order_release);
                clear_error_after_success();
                notify_workspace_present(present_snapshot);
            },
            [this](const char* error_message) {
                if (pipeline_trace != nullptr) {
                    pipeline_trace->note_acquire_miss();
                    pipeline_trace->record(LivePipelineStage::CompositorPublishWorkspace, last_workspace_frame_id,
                                           live_steady_clock_now_ns(), 0U, trace_cuda_device_index(),
                                           last_workspace_frame_id.sequence);
                }
                publish_error(error_message);
            });
    }

    [[nodiscard]] WorkspaceSlot* reserve_workspace_slot() {
        const int front_slot = front_workspace_slot_index.load(std::memory_order_acquire);
        for (auto& slot : workspace_slots) {
            if (static_cast<int>(slot->slot_index) == front_slot) {
                continue;
            }
            if (slot->slot_index < workspace_slot_in_flight.size() &&
                workspace_slot_in_flight[slot->slot_index]->load(std::memory_order_acquire)) {
                continue;
            }

            std::uint32_t expected = to_slot_state_value(SlotState::kFree);
            if (slot->state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kWriting),
                                                    std::memory_order_acq_rel)) {
                return slot.get();
            }

            if (expected != to_slot_state_value(SlotState::kPublished) ||
                !event_ready(slot->ready_event_handle(), "cudaEventQuery for live compositor workspace slot reuse")) {
                continue;
            }
            expected = to_slot_state_value(SlotState::kPublished);
            if (slot->state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kWriting),
                                                    std::memory_order_acq_rel)) {
                slot->reset_for_reuse();
                return slot.get();
            }
        }
        return nullptr;
    }

    void publish_error(std::string error_message) {
        worker_runtime::publish_error(&last_error, std::move(error_message));
    }

    void clear_error_after_success() noexcept {
        const std::shared_ptr<const std::string> error = last_error.load(std::memory_order_acquire);
        if (error != nullptr && !error->empty()) {
            worker_runtime::publish_error(&last_error, {});
        }
    }

    void record_callback_failure() noexcept {
        if (pipeline_trace == nullptr) {
            return;
        }
        pipeline_trace->note_acquire_miss();
        pipeline_trace->record(LivePipelineStage::CompositorPublishWorkspace, last_workspace_frame_id,
                               live_steady_clock_now_ns(), 0U, trace_cuda_device_index(),
                               last_workspace_frame_id.sequence);
    }

    void notify_workspace_ready() noexcept {
        const std::shared_ptr<const WorkspaceReadyCallback> callback =
            workspace_ready_callback.load(std::memory_order_acquire);
        if (!callback || !*callback) {
            return;
        }
        try {
            (*callback)();
        } catch (const std::exception& error) {
            record_callback_failure();
            log_live_worker_message("live.compositor", "error",
                                    std::string("mmltk live compositor workspace ready callback "
                                                "failed: ") +
                                        error.what());
        } catch (...) {
            record_callback_failure();
            log_live_worker_message("live.compositor", "error",
                                    "mmltk live compositor workspace ready callback failed");
        }
    }

    void notify_workspace_present(const WorkspacePresentSnapshot& snapshot) noexcept {
        const std::shared_ptr<const WorkspacePresentCallback> callback =
            workspace_present_callback.load(std::memory_order_acquire);
        if (callback && *callback) {
            try {
                (*callback)(snapshot);
            } catch (const std::exception& error) {
                record_callback_failure();
                log_live_worker_message("live.compositor", "error",
                                        std::string("mmltk live compositor workspace present callback failed: ") +
                                            error.what());
            } catch (...) {
                record_callback_failure();
                log_live_worker_message("live.compositor", "error",
                                        "mmltk live compositor workspace present callback failed");
            }
        }
        notify_workspace_ready();
    }

    LiveFrameFanout& fanout;
    LiveAnalyzerWorker* analyzer_worker = nullptr;
    LiveManualOverlayWorker* manual_worker = nullptr;
    int cuda_device_index = 0;
    std::shared_ptr<LivePipelineTrace> pipeline_trace;
    std::uint32_t max_capture_width = 0;
    std::uint32_t max_capture_height = 0;
    std::thread thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<bool> persistent_analysis_overlay{false};
    std::atomic<std::uint64_t> frames_composited_count{0};
    std::atomic<std::uint64_t> frames_composited_after_startup_count{0};
    std::atomic<std::uint64_t> frames_dropped_count{0};
    std::atomic<std::uint64_t> skipped_compositor_presents_count{0};
    std::atomic<std::uint64_t> last_frame_session_nonce{0};
    std::atomic<std::uint64_t> last_frame_sequence{0};
    std::atomic<bool> analysis_overlay_active{false};
    std::atomic<bool> manual_overlay_active{false};
    std::atomic<std::shared_ptr<const Status>> status{std::make_shared<Status>()};
    std::atomic<std::shared_ptr<const std::string>> last_error{std::make_shared<std::string>()};
    std::atomic<std::shared_ptr<const WorkspaceReadyCallback>> workspace_ready_callback{};
    std::atomic<std::shared_ptr<const WorkspacePresentCallback>> workspace_present_callback{};
    LinuxGpuInteropDevice workspace_interop_device;
    std::vector<std::unique_ptr<WorkspaceSlot>> workspace_slots;
    std::vector<std::unique_ptr<std::atomic<std::uint64_t>>> workspace_slot_revisions;
    std::vector<std::unique_ptr<std::atomic<bool>>> workspace_slot_in_flight;
    std::atomic<int> latest_workspace_index{-1};
    std::atomic<int> front_workspace_slot_index{-1};
    std::atomic<int> retired_front_workspace_slot_index{-1};
    std::atomic<std::uint64_t> front_workspace_revision{0};
    std::atomic<std::uint64_t> workspace_swapchain_generation{0};
    std::atomic<std::uint64_t> present_snapshot_generation{0};
    std::atomic<bool> present_valid{false};
    std::atomic<std::uint32_t> present_front_slot_index{0};
    std::atomic<std::uint64_t> present_revision{0};
    std::atomic<std::uint64_t> present_swapchain_generation{0};
    std::atomic<std::uint64_t> present_frame_session_nonce{0};
    std::atomic<std::uint64_t> present_frame_sequence{0};
    std::atomic<std::uint32_t> present_width{0};
    std::atomic<std::uint32_t> present_height{0};
    std::atomic<std::uint64_t> present_pitch_bytes{0};
    std::atomic<int> present_dmabuf_fd{-1};
    std::atomic<std::uint32_t> present_dmabuf_width{0};
    std::atomic<std::uint32_t> present_dmabuf_height{0};
    std::atomic<std::uint64_t> present_dmabuf_stride_bytes{0};
    std::atomic<std::uint64_t> present_dmabuf_offset{0};
    std::atomic<std::uint64_t> present_dmabuf_allocation_size{0};
    std::atomic<std::uint32_t> present_dmabuf_drm_format{0};
    std::atomic<std::uint64_t> present_dmabuf_drm_modifier{0};
    std::atomic<std::uint8_t> present_dmabuf_modifier_mode{static_cast<std::uint8_t>(DmaBufModifierMode::Unknown)};
    std::atomic<std::uint32_t> present_source_x{0};
    std::atomic<std::uint32_t> present_source_y{0};
    std::atomic<std::uint32_t> present_source_width{0};
    std::atomic<std::uint32_t> present_source_height{0};
    std::atomic<std::uint32_t> present_dirty_x{0};
    std::atomic<std::uint32_t> present_dirty_y{0};
    std::atomic<std::uint32_t> present_dirty_width{0};
    std::atomic<std::uint32_t> present_dirty_height{0};
    std::atomic<std::uintptr_t> present_ready_event_handle{0};
    std::atomic<std::uintptr_t> present_producer_stream_handle{0};
    std::atomic<std::uint64_t> present_capture_ns{0};
    std::atomic<std::uint64_t> present_ready_ns{0};
    std::atomic<bool> present_short_frame{false};
    std::unique_ptr<RawBaseCacheState> raw_base_cache;
    std::unique_ptr<RetainedAnalysisState> retained_analysis;
    std::unique_ptr<ReadbackState> readback;
    bool first_workspace_frame_logged = false;
    std::uint64_t workspace_frame_log_count = 0;
    LiveFrameId last_workspace_frame_id{};

    [[nodiscard]] std::uint32_t trace_cuda_device_index() const noexcept {
        return cuda_device_index >= 0 ? static_cast<std::uint32_t>(cuda_device_index) : kLivePipelineUnknownCudaDevice;
    }

    [[nodiscard]] std::uint64_t frame_budget_ns() const noexcept {
        return 16666666ULL;
    }

    void warn_if_over_budget(const char* stage, const LiveFrameId& frame_id, const std::uint64_t latency_ns) const {
        const std::uint64_t budget_ns = frame_budget_ns();
        if (latency_ns <= budget_ns) {
            return;
        }
        log_live_worker_message("live.compositor", "warn",
                                std::string(stage) +
                                    " exceeded frame interval: frame=" + std::to_string(frame_id.sequence) +
                                    " latency_us=" + std::to_string(latency_ns / 1000U) +
                                    " budget_us=" + std::to_string(budget_ns / 1000U));
    }

    void attach_pipeline_status(Status& s) const noexcept {
        if (pipeline_trace == nullptr) {
            return;
        }
        attach_live_pipeline_status(s, *pipeline_trace);
    }
};

LiveCompositor::LiveCompositor(LiveFrameFanout& fanout, LiveAnalyzerWorker* analyzer_worker,
                               LiveManualOverlayWorker* manual_worker, const int cuda_device_index,
                               const std::uint32_t max_capture_width, const std::uint32_t max_capture_height,
                               const std::uint32_t output_slot_count, const std::uint32_t raw_base_cache_slot_count,
                               std::shared_ptr<LivePipelineTrace> pipeline_trace)
    : impl_(std::make_unique<Impl>(fanout, analyzer_worker, manual_worker, cuda_device_index, max_capture_width,
                                   max_capture_height, output_slot_count, raw_base_cache_slot_count,
                                   std::move(pipeline_trace))) {}

LiveCompositor::~LiveCompositor() = default;

void LiveCompositor::start() {
    impl_->start();
}

void LiveCompositor::stop() {
    impl_->stop();
}

bool LiveCompositor::running() const noexcept {
    return impl_->running_state();
}

WorkspaceSwapchainDescriptor LiveCompositor::workspace_swapchain_descriptor() const {
    return impl_->workspace_swapchain_descriptor();
}

WorkspacePresentSnapshot LiveCompositor::latest_workspace_present() const noexcept {
    return impl_->latest_workspace_present();
}

bool LiveCompositor::try_acquire_latest_workspace(WorkspaceOutputBundle* out) {
    return impl_->try_acquire_latest_workspace(out);
}

void LiveCompositor::release_workspace(const std::uint32_t slot_index) {
    impl_->release_workspace(slot_index);
}

void LiveCompositor::set_workspace_ready_callback(WorkspaceReadyCallback callback) {
    impl_->set_workspace_ready_callback(std::move(callback));
}

void LiveCompositor::set_workspace_present_callback(WorkspacePresentCallback callback) {
    impl_->set_workspace_present_callback(std::move(callback));
}

void LiveCompositor::mark_workspace_slot_in_flight(const std::uint32_t slot_index, const bool in_flight) {
    impl_->mark_workspace_slot_in_flight(slot_index, in_flight);
}

void LiveCompositor::set_persistent_analysis_overlay(const bool enabled) {
    impl_->set_persistent_analysis_overlay(enabled);
}

bool LiveCompositor::persistent_analysis_overlay() const noexcept {
    return impl_->persistent_analysis_overlay_enabled();
}

LiveCompositor::Status LiveCompositor::snapshot_status() const {
    return impl_->snapshot_status();
}

bool LiveCompositor::readback_raw_base(const LiveFrameId& frame_id, std::vector<std::uint8_t>* pixels_bgr,
                                       std::uint32_t* width, std::uint32_t* height, LiveCaptureRegion* region,
                                       std::string* error_message) {
    return impl_->readback_raw_base(frame_id, pixels_bgr, width, height, region, error_message);
}

}  // namespace mmltk::live
