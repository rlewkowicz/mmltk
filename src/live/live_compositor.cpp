#include "mmltk/live/live_compositor.h"

#include "live/live_helpers.h"
#include "live/live_worker_logging.h"
#include "mmltk/live/live_analyzer_worker.h"
#include "mmltk/live/live_frame_fanout.h"
#include "mmltk/live/live_manual_overlay_worker.h"
#include "mmltk/live/live_worker_runtime.h"
#include "mmltk/live/workspace_surface_pool.h"
#include "mmltk/live/workspace_trace.h"
#include "mmltk/rfdetr/draw_cuda.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace mmltk::live {

namespace {

[[nodiscard]] mmltk::rfdetr::draw_launch::WorkspaceRgbaTarget workspace_target(const WorkspaceSurfaceWriteLease& lease,
                                                                               const int width, const int height) {
    if (lease.pitched) {
        return mmltk::rfdetr::draw_launch::make_workspace_rgba_pitched_target(device_ptr_as_bytes(lease.data),
                                                                              lease.pitch_bytes, width, height);
    }
    if (lease.array) {
        return mmltk::rfdetr::draw_launch::make_workspace_rgba_surface_target(lease.surface_object, width, height);
    }
    throw std::runtime_error("workspace surface has no CUDA write target");
}

void composite_rgba(cudaStream_t stream, const mmltk::rfdetr::draw_launch::WorkspaceRgbaTarget& destination,
                    const CUdeviceptr source, const std::size_t source_pitch, const int width, const int height,
                    const char* context) {
    mmltk::rfdetr::launch_composite_rgba_over_workspace_rgba(destination, device_ptr_as_bytes(source), source_pitch,
                                                             width, height, stream);
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
                                std::string("failed to release ") + worker_name + " overlay slot " +
                                    std::to_string(slot_index) + ": " + error.what());
    }
}

bool enqueue_input_release_barrier(const cudaEvent_t consumed_event, const cudaStream_t consumer_stream,
                                   const cudaStream_t producer_stream, const char* input_name) noexcept {
    if (consumed_event == nullptr || consumer_stream == nullptr || producer_stream == nullptr) {
        log_live_worker_message("live.compositor", "error",
                                std::string("cannot release consumed ") + input_name +
                                    " slot without consumer, producer, and completion handles");
        return false;
    }
    cudaError_t result = cudaEventRecord(consumed_event, consumer_stream);
    if (result == cudaSuccess) {
        result = cudaStreamWaitEvent(producer_stream, consumed_event, 0U);
    }
    if (result != cudaSuccess) {
        log_live_worker_message(
            "live.compositor", "error",
            std::string("failed to enqueue ") + input_name + " input-release barrier: " + cudaGetErrorString(result));
        return false;
    }
    return true;
}

struct RetainedAnalysisState {
    void initialize(const std::uint32_t width, const std::uint32_t height) {
        device_buffer.ensure_dimensions(width, height, "cudaMallocPitch for retained analysis overlay");
        stream.create_with_highest_priority("cudaStreamCreateWithPriority for retained analysis overlay");
        ready_event.create(cudaEventDisableTiming, "cudaEventCreateWithFlags for retained analysis overlay");
        read_complete_event.create(cudaEventDisableTiming,
                                   "cudaEventCreateWithFlags for retained analysis read completion");
        input_consumed_event.create(cudaEventDisableTiming,
                                    "cudaEventCreateWithFlags for retained analysis input consumption");
    }

    void reset() noexcept {
        if (stream.get() != nullptr) {
            (void)cudaStreamSynchronize(stream.get());
        }
        input_consumed_event.reset();
        read_complete_event.reset();
        ready_event.reset();
        stream.reset();
        device_buffer.reset();
        has_content = false;
    }

    void clear() noexcept {
        has_content = false;
    }

    bool composite_into(cudaStream_t destination_stream,
                        const mmltk::rfdetr::draw_launch::WorkspaceRgbaTarget& destination, const OutputBundle& base) {
        if (!has_content) {
            return false;
        }
        ensure_cuda_ok(cudaStreamWaitEvent(destination_stream, ready_event.get(), 0),
                       "cudaStreamWaitEvent for retained analysis overlay");
        try {
            composite_rgba(destination_stream, destination, device_buffer.data(), device_buffer.pitch_bytes(),
                           static_cast<int>(base.dims.width), static_cast<int>(base.dims.height),
                           "launch_composite_rgba_over_workspace_rgba retained analysis");
        } catch (...) {
            enqueue_read_completion(destination_stream);
            throw;
        }
        enqueue_read_completion(destination_stream);
        return true;
    }

    void update(const LiveAnalyzerWorker::AnalysisOverlayView& analysis, bool* release_barrier_installed) {
        if (release_barrier_installed == nullptr) {
            throw std::runtime_error("retained analysis update requires a release-barrier result");
        }
        *release_barrier_installed = false;
        if (!analysis.has_content) {
            has_content = false;
            return;
        }
        if (analysis.producer_stream == nullptr) {
            throw std::runtime_error("retained analysis input is missing its producer stream");
        }
        bool input_copy_enqueued = false;
        try {
            if (analysis.ready_event != nullptr) {
                ensure_cuda_ok(cudaStreamWaitEvent(stream.get(), analysis.ready_event, 0),
                               "cudaStreamWaitEvent for retained analysis update");
            }
            ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(device_buffer.data()), device_buffer.pitch_bytes(),
                                             device_ptr_as_const_void(analysis.data), analysis.pitch_bytes,
                                             static_cast<std::size_t>(analysis.width) * 4U, analysis.height,
                                             cudaMemcpyDeviceToDevice, stream.get()),
                           "cudaMemcpy2DAsync for retained analysis update");
            input_copy_enqueued = true;
            ensure_cuda_ok(cudaEventRecord(ready_event.get(), stream.get()),
                           "cudaEventRecord for retained analysis update");
            enqueue_input_completion(analysis.producer_stream);
            *release_barrier_installed = true;
            has_content = true;
        } catch (...) {
            if (input_copy_enqueued) {
                const cudaError_t record_result = cudaEventRecord(input_consumed_event.get(), stream.get());
                const cudaError_t wait_result =
                    record_result == cudaSuccess
                        ? cudaStreamWaitEvent(analysis.producer_stream, input_consumed_event.get(), 0U)
                        : record_result;
                *release_barrier_installed = wait_result == cudaSuccess;
            }
            throw;
        }
    }

    void enqueue_input_completion(const cudaStream_t producer_stream) {
        ensure_cuda_ok(cudaEventRecord(input_consumed_event.get(), stream.get()),
                       "cudaEventRecord for retained analysis input consumption");
        ensure_cuda_ok(cudaStreamWaitEvent(producer_stream, input_consumed_event.get(), 0U),
                       "cudaStreamWaitEvent for retained analysis input release");
    }

    void enqueue_read_completion(const cudaStream_t destination_stream) {
        ensure_cuda_ok(cudaEventRecord(read_complete_event.get(), destination_stream),
                       "cudaEventRecord for retained analysis read completion");
        ensure_cuda_ok(cudaStreamWaitEvent(stream.get(), read_complete_event.get(), 0U),
                       "cudaStreamWaitEvent for retained analysis read completion");
    }

    RgbaPitchedDeviceBuffer device_buffer;
    CudaStreamHandle stream;
    CudaEventHandle ready_event;
    CudaEventHandle read_complete_event;
    CudaEventHandle input_consumed_event;
    bool has_content = false;
};

}  

struct LiveCompositor::Impl {
    Impl(LiveFrameFanout& fanout_in, LiveAnalyzerWorker* analyzer_worker_in, LiveManualOverlayWorker* manual_worker_in,
         const int cuda_device_index_in, const std::uint32_t max_capture_width_in,
         const std::uint32_t max_capture_height_in, std::shared_ptr<WorkspaceSurfacePool> workspace_pool_in,
         std::shared_ptr<LivePipelineTrace> pipeline_trace_in)
        : fanout(fanout_in),
          analyzer_worker(analyzer_worker_in),
          manual_worker(manual_worker_in),
          cuda_device_index(cuda_device_index_in),
          max_capture_width(std::max(1U, max_capture_width_in)),
          max_capture_height(std::max(1U, max_capture_height_in)),
          workspace_pool(std::move(workspace_pool_in)),
          pipeline_trace(std::move(pipeline_trace_in)) {
        if (workspace_pool == nullptr) {
            throw std::runtime_error("live compositor requires an application-owned workspace surface pool");
        }
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for live compositor allocation");
        retained_analysis.initialize(max_capture_width, max_capture_height);
        base_consumed.create(cudaEventDisableTiming, "cudaEventCreateWithFlags for compositor base consumption");
        analysis_consumed.create(cudaEventDisableTiming,
                                 "cudaEventCreateWithFlags for compositor analysis consumption");
        manual_consumed.create(cudaEventDisableTiming, "cudaEventCreateWithFlags for compositor manual consumption");
    }

    ~Impl() {
        try {
            stop();
        } catch (const std::exception& error) {
            log_live_worker_message("live.compositor", "error", error.what());
        }
        (void)cudaSetDevice(cuda_device_index);
        manual_consumed.reset();
        analysis_consumed.reset();
        base_consumed.reset();
        retained_analysis.reset();
    }

    void start() {
        if (running.load(std::memory_order_acquire) || thread.joinable()) {
            throw std::runtime_error("live compositor must be stopped before it can start");
        }
        publish_error({});
        worker_runtime::publish_status(&status, Status{});
        reset_local_status();
        worker_runtime::start_worker_thread(
            thread, stop_requested, running, [this]() { thread_main(); }, "live compositor is already running",
            "live compositor must be stopped before restart");
    }

    void stop() {
        stop_requested.store(true, std::memory_order_release);
        fanout.notify_output_waiters();
        workspace_pool->notify_waiters();
        if (thread.joinable()) {
            thread.join();
        }
        running.store(false, std::memory_order_release);
        retained_analysis.clear();
        workspace_pool->reset_for_producer_restart();
    }

    [[nodiscard]] bool running_state() const noexcept {
        return running.load(std::memory_order_acquire);
    }

    [[nodiscard]] Status snapshot_status() const {
        Status result = worker_runtime::snapshot_status(status);
        const WorkspaceSurfacePoolStatus pool_status = workspace_pool->snapshot_status();
        result.running = running_state();
        result.frames_composited = frames_composited.load(std::memory_order_acquire);
        result.frames_composited_after_startup = frames_after_startup.load(std::memory_order_acquire);
        result.frames_dropped = frames_dropped.load(std::memory_order_acquire);
        result.skipped_compositor_presents = skipped_presents.load(std::memory_order_acquire);
        result.source_acquire_waits = pool_status.source_acquire_waits;
        result.source_leases_acquired = pool_status.source_leases_acquired;
        result.source_leases_released = pool_status.source_leases_released;
        result.source_stale_releases = pool_status.source_stale_releases;
        result.source_skipped_stale_frames = pool_status.source_skipped_stale_frames;
        result.source_slot_pressure = pool_status.source_slot_pressure;
        result.source_release_latency_ns = pool_status.source_release_latency_ns;
        result.front_slot_index = pool_status.front_slot_index;
        result.front_slot_revision = pool_status.front_slot_revision;
        result.last_frame_id = LiveFrameId{last_frame_session.load(std::memory_order_acquire),
                                           last_frame_sequence.load(std::memory_order_acquire)};
        result.analysis_overlay_active = analysis_active.load(std::memory_order_acquire);
        result.manual_overlay_active = manual_active.load(std::memory_order_acquire);
        const std::shared_ptr<const std::string> error = last_error.load(std::memory_order_acquire);
        result.last_error = error != nullptr ? *error : std::string{};
        if (pipeline_trace != nullptr) {
            attach_live_pipeline_status(result, *pipeline_trace);
        }
        return result;
    }

    void thread_main() {
        worker_runtime::run_worker_thread_main(
            running, stop_requested, status, cuda_device_index, "cudaSetDevice for live compositor thread",
            "live.compositor",
            [this](Status&) {
                OutputBundle base{};
                if (!fanout.wait_acquire_output(&base, stop_requested)) {
                    return;
                }
                WorkspaceSurfaceWriteLease write{};
                cudaStream_t consumer_stream = nullptr;
                bool base_was_consumed = false;
                auto base_release = worker_runtime::make_scoped_rollback(
                    [this, &base, &consumer_stream, &base_was_consumed]() noexcept {
                        if (base_was_consumed &&
                            !enqueue_input_release_barrier(base_consumed.get(), consumer_stream, base.stream, "base")) {
                            return;
                        }
                        fanout.release_output(base.slot_index);
                    });

                if (!workspace_pool->try_reserve_write(&write)) {
                    base_release.run_and_dismiss();
                    frames_dropped.fetch_add(1U, std::memory_order_relaxed);
                    skipped_presents.fetch_add(1U, std::memory_order_relaxed);
                    if (pipeline_trace != nullptr) {
                        pipeline_trace->note_acquire_miss();
                    }
                    trace_workspace("native", "workspace.live_frame_dropped", [&] {
                        return nlohmann::json{{"frame_session", base.frame_id.session_nonce},
                                              {"frame_sequence", base.frame_id.sequence},
                                              {"reason", "surface_pressure"}};
                    });
                    return;
                }
                auto write_cancel = worker_runtime::make_scoped_rollback(
                    [this, &write]() noexcept { workspace_pool->cancel_write(&write); });
                consumer_stream = write.stream;

                if (base.ready_event != nullptr) {
                    ensure_cuda_ok(cudaStreamWaitEvent(write.stream, base.ready_event, 0),
                                   "cudaStreamWaitEvent for live compositor base");
                }
                base_was_consumed = true;
                workspace_pool->cache_semantic_source(write.stream, base);

                const int frame_width = static_cast<int>(base.dims.width);
                const int frame_height = static_cast<int>(base.dims.height);
                const auto destination = workspace_target(write, frame_width, frame_height);
                mmltk::rfdetr::launch_copy_bgr_to_workspace_rgba(device_ptr_as_bytes(base.data), base.dims.pitch_bytes,
                                                                 destination, frame_width, frame_height, 255U,
                                                                 write.stream);
                ensure_cuda_ok(cudaPeekAtLastError(), "launch_copy_bgr_to_workspace_rgba for live compositor");

                bool has_analysis = false;
                const auto composite_retained = [&]() {
                    has_analysis = retained_analysis.composite_into(write.stream, destination, base) || has_analysis;
                };
                LiveAnalyzerWorker::AnalysisOverlayView analysis{};
                const bool analysis_acquired =
                    analyzer_worker != nullptr && analyzer_worker->try_acquire_latest_overlay(&analysis);
                bool analysis_was_consumed = false;
                bool retained_analysis_consumed = false;
                bool retained_analysis_release_ready = false;
                auto analysis_release = worker_runtime::make_scoped_rollback(
                    [this, &analysis, &consumer_stream, analysis_acquired, &analysis_was_consumed,
                     &retained_analysis_consumed, &retained_analysis_release_ready]() noexcept {
                        if (!analysis_acquired) {
                            return;
                        }
                        if (retained_analysis_consumed && !retained_analysis_release_ready) {
                            return;
                        }
                        if (analysis_was_consumed &&
                            !enqueue_input_release_barrier(analysis_consumed.get(), consumer_stream,
                                                           analysis.producer_stream, "analysis")) {
                            return;
                        }
                        release_overlay_silently(analyzer_worker, analysis.slot_index, "analysis");
                    });
                if (analysis_acquired) {
                    if (analysis.frame_id == base.frame_id) {
                        if (analysis.has_content) {
                            if (analysis.ready_event != nullptr) {
                                ensure_cuda_ok(cudaStreamWaitEvent(write.stream, analysis.ready_event, 0),
                                               "cudaStreamWaitEvent for live analysis overlay");
                            }
                            analysis_was_consumed = true;
                            composite_rgba(write.stream, destination, analysis.data, analysis.pitch_bytes, frame_width,
                                           frame_height, "launch_composite_rgba_over_workspace_rgba analysis");
                            has_analysis = true;
                        }
                        if (persistent_analysis.load(std::memory_order_acquire)) {
                            retained_analysis_consumed = analysis.has_content;
                            retained_analysis.update(analysis, &retained_analysis_release_ready);
                        }
                    } else if (persistent_analysis.load(std::memory_order_acquire)) {
                        composite_retained();
                    }
                } else if (persistent_analysis.load(std::memory_order_acquire)) {
                    composite_retained();
                }

                bool has_manual = false;
                LiveManualOverlayWorker::OverlayView manual{};
                const bool manual_acquired =
                    manual_worker != nullptr && manual_worker->try_acquire_latest_overlay(&manual);
                bool manual_was_consumed = false;
                auto manual_release = worker_runtime::make_scoped_rollback(
                    [this, &manual, &consumer_stream, manual_acquired, &manual_was_consumed]() noexcept {
                        if (!manual_acquired) {
                            return;
                        }
                        if (manual_was_consumed &&
                            !enqueue_input_release_barrier(manual_consumed.get(), consumer_stream,
                                                           manual.producer_stream, "manual")) {
                            return;
                        }
                        release_overlay_silently(manual_worker, manual.slot_index, "manual");
                    });
                if (manual_acquired && manual.has_content) {
                    if (manual.ready_event != nullptr) {
                        ensure_cuda_ok(cudaStreamWaitEvent(write.stream, manual.ready_event, 0),
                                       "cudaStreamWaitEvent for live manual overlay");
                    }
                    manual_was_consumed = true;
                    const std::uint64_t manual_right = static_cast<std::uint64_t>(base.region.x) + base.dims.width;
                    const std::uint64_t manual_bottom = static_cast<std::uint64_t>(base.region.y) + base.dims.height;
                    const bool region_fits = manual_right <= manual.width && manual_bottom <= manual.height;
                    if (!region_fits) {
                        throw std::runtime_error("manual overlay does not cover the live capture source region");
                    }
                    const CUdeviceptr manual_region = manual.data +
                                                      static_cast<std::size_t>(base.region.y) * manual.pitch_bytes +
                                                      static_cast<std::size_t>(base.region.x) * 4U;
                    composite_rgba(write.stream, destination, manual_region, manual.pitch_bytes, frame_width,
                                   frame_height, "launch_composite_rgba_over_workspace_rgba manual");
                    has_manual = true;
                }

                const std::uint64_t ready_ns = live_steady_clock_now_ns();
                const WorkspacePresentSnapshot published = workspace_pool->publish_write(
                    &write, WorkspaceSurfacePublishInfo{
                                base.frame_id,
                                base.dims.width,
                                base.dims.height,
                                base.region,
                                LiveCaptureRegion{0U, 0U, base.dims.width, base.dims.height},
                                base.capture_ns,
                                ready_ns,
                                base.short_frame,
                            });
                write_cancel.dismiss();
                trace_workspace("native", "workspace.live_composited", [&] {
                    return nlohmann::json{{"generation", published.swapchain_generation},
                                          {"slot", published.front_slot_index},
                                          {"revision", published.revision},
                                          {"analysis", has_analysis},
                                          {"manual", has_manual},
                                          {"capture_ns", base.capture_ns},
                                          {"ready_ns", ready_ns}};
                });

                if (pipeline_trace != nullptr) {
                    pipeline_trace->record(LivePipelineStage::CompositorPublishWorkspace, base.frame_id, ready_ns,
                                           base.capture_ns, trace_cuda_device_index(), published.revision);
                    pipeline_trace->mark_first_workspace_publication_ready();
                }
                const bool already_started = first_frame_published;
                first_frame_published = true;
                frames_composited.fetch_add(1U, std::memory_order_relaxed);
                if (already_started) {
                    frames_after_startup.fetch_add(1U, std::memory_order_relaxed);
                }
                last_frame_session.store(base.frame_id.session_nonce, std::memory_order_release);
                last_frame_sequence.store(base.frame_id.sequence, std::memory_order_release);
                analysis_active.store(has_analysis, std::memory_order_release);
                manual_active.store(has_manual, std::memory_order_release);
                clear_error_after_success();
            },
            [this](const char* error_message) { publish_error(error_message); });
    }

    void reset_local_status() noexcept {
        frames_composited.store(0U, std::memory_order_release);
        frames_after_startup.store(0U, std::memory_order_release);
        frames_dropped.store(0U, std::memory_order_release);
        skipped_presents.store(0U, std::memory_order_release);
        last_frame_session.store(0U, std::memory_order_release);
        last_frame_sequence.store(0U, std::memory_order_release);
        analysis_active.store(false, std::memory_order_release);
        manual_active.store(false, std::memory_order_release);
        first_frame_published = false;
        workspace_pool->reset_status();
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

    [[nodiscard]] std::uint32_t trace_cuda_device_index() const noexcept {
        return cuda_device_index >= 0 ? static_cast<std::uint32_t>(cuda_device_index) : kLivePipelineUnknownCudaDevice;
    }

    LiveFrameFanout& fanout;
    LiveAnalyzerWorker* analyzer_worker = nullptr;
    LiveManualOverlayWorker* manual_worker = nullptr;
    int cuda_device_index = 0;
    std::uint32_t max_capture_width = 0;
    std::uint32_t max_capture_height = 0;
    std::shared_ptr<WorkspaceSurfacePool> workspace_pool;
    std::shared_ptr<LivePipelineTrace> pipeline_trace;
    RetainedAnalysisState retained_analysis;
    CudaEventHandle base_consumed;
    CudaEventHandle analysis_consumed;
    CudaEventHandle manual_consumed;
    std::thread thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<bool> persistent_analysis{false};
    std::atomic<std::uint64_t> frames_composited{0U};
    std::atomic<std::uint64_t> frames_after_startup{0U};
    std::atomic<std::uint64_t> frames_dropped{0U};
    std::atomic<std::uint64_t> skipped_presents{0U};
    std::atomic<std::uint64_t> last_frame_session{0U};
    std::atomic<std::uint64_t> last_frame_sequence{0U};
    std::atomic<bool> analysis_active{false};
    std::atomic<bool> manual_active{false};
    std::atomic<std::shared_ptr<const Status>> status{std::make_shared<Status>()};
    std::atomic<std::shared_ptr<const std::string>> last_error{std::make_shared<std::string>()};
    bool first_frame_published = false;
};

LiveCompositor::LiveCompositor(LiveFrameFanout& fanout, LiveAnalyzerWorker* analyzer_worker,
                               LiveManualOverlayWorker* manual_worker, const int cuda_device_index,
                               const std::uint32_t max_capture_width, const std::uint32_t max_capture_height,
                               std::shared_ptr<WorkspaceSurfacePool> workspace_pool,
                               std::shared_ptr<LivePipelineTrace> pipeline_trace)
    : impl_(std::make_unique<Impl>(fanout, analyzer_worker, manual_worker, cuda_device_index, max_capture_width,
                                   max_capture_height, std::move(workspace_pool), std::move(pipeline_trace))) {}

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
    return impl_->workspace_pool->descriptor();
}

WorkspacePresentSnapshot LiveCompositor::latest_workspace_present() const noexcept {
    return impl_->workspace_pool->latest_present();
}

bool LiveCompositor::request_next_workspace_source_frame(const std::uint64_t after_revision,
                                                         const std::atomic<bool>& caller_stop_requested,
                                                         WorkspaceSourceFrameLease* out) {
    return impl_->workspace_pool->request_next_source_frame(after_revision, caller_stop_requested,
                                                            impl_->stop_requested, impl_->running, out);
}

void LiveCompositor::release_workspace_source_frame(const WorkspaceSourceFrameLease& lease) noexcept {
    impl_->workspace_pool->release_source_frame(lease);
}

bool LiveCompositor::try_acquire_latest_workspace(WorkspaceOutputBundle* out) {
    return impl_->running_state() && impl_->workspace_pool->try_acquire_latest(out);
}

void LiveCompositor::release_workspace(const std::uint32_t slot_index) {
    impl_->workspace_pool->release_acquired(slot_index);
}

bool LiveCompositor::acquire_workspace_slot_for_import(const std::uint32_t slot_index) {
    return impl_->workspace_pool->acquire_slot_for_import(slot_index);
}

bool LiveCompositor::mark_workspace_slot_in_flight(const std::uint32_t slot_index, const bool in_flight) {
    return impl_->workspace_pool->mark_slot_in_flight(slot_index, in_flight);
}

bool LiveCompositor::import_workspace_timeline_semaphore(const std::uint64_t generation, const std::uint32_t slot_index,
                                                         const int semaphore_fd, std::string* error_message) {
    return impl_->workspace_pool->import_timeline_semaphore(generation, slot_index, semaphore_fd, error_message);
}

bool LiveCompositor::workspace_timeline_sync_ready(const std::uint64_t generation) const noexcept {
    return impl_->workspace_pool->timeline_sync_ready(generation);
}

void LiveCompositor::set_persistent_analysis_overlay(const bool enabled) {
    impl_->persistent_analysis.store(enabled, std::memory_order_release);
}

bool LiveCompositor::persistent_analysis_overlay() const noexcept {
    return impl_->persistent_analysis.load(std::memory_order_acquire);
}

LiveCompositor::Status LiveCompositor::snapshot_status() const {
    return impl_->snapshot_status();
}

bool LiveCompositor::readback_raw_base(const LiveFrameId& frame_id, std::vector<std::uint8_t>* pixels_bgr,
                                       std::uint32_t* width, std::uint32_t* height, LiveCaptureRegion* region,
                                       std::string* error_message) {
    return impl_->workspace_pool->readback_semantic_source(frame_id, pixels_bgr, width, height, region, error_message);
}

}  
