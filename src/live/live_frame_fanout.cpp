#include "mmltk/live/live_frame_fanout.h"

#include "live/live_helpers.h"
#include "live/live_worker_logging.h"
#include "mmltk/live/crop_state.h"
#include "mmltk/live/live_video_ingress.h"
#include "mmltk/live/live_worker_runtime.h"
#include "mmltk_logging.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace mmltk::live {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kFanoutNoWorkBackoff = std::chrono::microseconds(250);

template <typename Dimensions, typename Bundle>
struct LiveFrameFanoutSlot : worker_runtime::LiveFrameSlotCommon<Dimensions, Bundle> {
    void publish_from_source(const SourceFrameView& source, const LiveCaptureRegion& published_region,
                             const std::uint64_t published_ready_ns) noexcept {
        this->frame_id = source.frame_id;
        this->region = published_region;
        this->capture_ns = source.capture_ns;
        this->ready_ns = published_ready_ns;
        this->short_frame = source.short_frame;
        this->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    }
};

template <typename Slot>
void publish_live_frame_slot(Slot& slot, std::atomic<int>& latest_index, const SourceFrameView& source,
                             const LiveCaptureRegion& published_region,
                             const std::uint64_t published_ready_ns) noexcept {
    slot.publish_from_source(source, published_region, published_ready_ns);
    latest_index.store(static_cast<int>(slot.slot_index), std::memory_order_release);
}

template <typename Slot>
[[nodiscard]] Slot* reserve_live_frame_slot(std::vector<std::unique_ptr<Slot>>& slots, std::atomic<int>& latest_index) {
    return worker_runtime::reserve_writable_slot_view(slots, latest_index, "cudaEventQuery for live fanout slot reuse");
}

template <typename Slot>
void release_live_frame_slot(Slot& slot, const char* context) {
    release_acquired_slot(slot, context);
}

template <typename Slot>
void allocate_live_frame_slots(std::vector<std::unique_ptr<Slot>>& slots, const std::uint32_t slot_count,
                               const std::uint32_t max_capture_width, const std::uint32_t max_capture_height,
                               const char* buffer_context, const char* stream_context, const char* event_context) {
    worker_runtime::allocate_pitched_device_slots(slots, slot_count, max_capture_width, max_capture_height,
                                                  buffer_context, stream_context, event_context);
}

void log_live_fanout_message(const char* level, const std::string& message) {
    if (message.empty()) {
        return;
    }
    auto logger = mmltk::logging::logger("live.fanout");
    if (std::string_view(level) == "error") {
        logger->error("{}", message);
    } else if (std::string_view(level) == "warn") {
        logger->warn("{}", message);
    } else {
        logger->info("{}", message);
    }
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

std::size_t clamp_copy_offset_bytes(const LiveCaptureRegion& source_region, const LiveCaptureRegion& detect_region,
                                    const std::size_t pitch_bytes) {
    const std::uint32_t row_offset = detect_region.y > source_region.y ? detect_region.y - source_region.y : 0U;
    const std::uint32_t col_offset = detect_region.x > source_region.x ? detect_region.x - source_region.x : 0U;
    return static_cast<std::size_t>(row_offset) * pitch_bytes + static_cast<std::size_t>(col_offset) * 3U;
}

}  // namespace

struct LiveFrameFanout::Impl {
    using DetectSlot = LiveFrameFanoutSlot<DetectDimensions, DetectBundle>;
    using OutputSlot = LiveFrameFanoutSlot<OutputDimensions, OutputBundle>;

    struct ReleaseEvent {
        CudaEventHandle event;
        bool in_use = false;
    };

    struct PendingSourceRelease {
        std::uint32_t buffer_index = 0;
        std::size_t event_index = 0;
        LiveFrameId frame_id{};
        std::uint64_t capture_ns = 0;
    };

    struct PublicationSignal {
        void notify() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++generation;
            }
            cv.notify_all();
        }

        void wake() noexcept {
            cv.notify_all();
        }

        template <typename StopPredicate>
        void wait_for_next(std::uint64_t start_generation, StopPredicate&& stop_predicate) {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return stop_predicate() || generation != start_generation; });
        }

        [[nodiscard]] std::uint64_t snapshot_generation() const {
            std::lock_guard<std::mutex> lock(mutex);
            return generation;
        }

        mutable std::mutex mutex;
        std::condition_variable cv;
        std::uint64_t generation = 0;
    };

    Impl(LiveVideoIngress& ingress_in, const UiCropState& ui_crop_state_in, const std::uint32_t detect_slot_count,
         const std::uint32_t output_slot_count, std::shared_ptr<LivePipelineTrace> pipeline_trace_in)
        : ingress(ingress_in),
          ui_crop_state(ui_crop_state_in),
          pipeline_trace(std::move(pipeline_trace_in)),
          max_capture_width(std::max<std::uint32_t>(1U, ingress.config().width)),
          max_capture_height(std::max<std::uint32_t>(1U, ingress.config().height)) {
        if (detect_slot_count == 0U) {
            throw std::runtime_error("live fanout requires at least one detect slot");
        }
        if (output_slot_count == 0U) {
            throw std::runtime_error("live fanout requires at least one output slot");
        }
        allocate_resources(detect_slot_count, output_slot_count);
    }

    ~Impl() {
        try {
            stop();
        } catch (const std::exception& error) {
            log_live_fanout_message("error", error.what());
        }
        destroy_resources();
    }

    void start() {
        if (running.load(std::memory_order_acquire)) {
            throw std::runtime_error("live fanout is already running");
        }
        if (thread.joinable()) {
            throw std::runtime_error("live fanout must be stopped before it can be restarted");
        }

        publish_error("");
        worker_runtime::publish_status(&status, Status{});
        frames_fanned_out.store(0U, std::memory_order_release);
        skipped_detect_publishes.store(0U, std::memory_order_release);
        skipped_output_publishes.store(0U, std::memory_order_release);
        ensure_cuda_ok(cudaSetDevice(ingress.config().cuda_device_index), "cudaSetDevice for live fanout start");
        worker_runtime::reset_slot_views_for_restart(detect_slots, latest_detect_index,
                                                     "cudaEventSynchronize for live detect slot restart");
        worker_runtime::reset_slot_views_for_restart(output_slots, latest_output_index,
                                                     "cudaEventSynchronize for live output slot restart");
        worker_runtime::start_worker_thread(
            thread, stop_requested, running, [this]() { fanout_thread_main(); }, "live fanout is already running",
            "live fanout must be stopped before it can be restarted");
        log_live_fanout_message("info", "fanout thread started");
    }

    void stop() {
        stop_requested.store(true, std::memory_order_release);
        ingress.notify_source_waiters();
        detect_signal.wake();
        output_signal.wake();
        if (thread.joinable()) {
            thread.join();
        }
        running.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool running_state() const noexcept {
        return running.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::string last_error() const {
        return snapshot_status().last_error;
    }

    [[nodiscard]] Status snapshot_status() const {
        Status s = worker_runtime::snapshot_status(status);
        attach_local_status(s);
        attach_pipeline_status(s);
        return s;
    }

    [[nodiscard]] bool try_acquire_detect(DetectBundle* out) {
        const bool acquired = worker_runtime::try_acquire_latest_running_slot_view(
            detect_slots, latest_detect_index, running, out, "live fanout requires a detect bundle output");
        if (!acquired && running_state() && pipeline_trace != nullptr) {
            pipeline_trace->note_acquire_miss();
        }
        return acquired;
    }

    [[nodiscard]] bool wait_acquire_detect(DetectBundle* out, const std::atomic<bool>& stop) {
        while (!stop.load(std::memory_order_acquire) && running_state()) {
            if (try_acquire_detect(out)) {
                return true;
            }
            const std::uint64_t start_generation = detect_signal.snapshot_generation();
            if (try_acquire_detect(out)) {
                return true;
            }
            detect_signal.wait_for_next(start_generation,
                                        [&] { return stop.load(std::memory_order_acquire) || !running_state(); });
        }
        if (out != nullptr) {
            *out = {};
        }
        return false;
    }

    void release_detect(const std::uint32_t slot_index) {
        if (slot_index >= detect_slots.size()) {
            throw std::runtime_error("live fanout detect release index out of range");
        }
        DetectSlot& slot = *detect_slots[slot_index];
        release_live_frame_slot(slot, "live fanout detect slot release called for a slot that is not acquired");
    }

    [[nodiscard]] bool try_acquire_output(OutputBundle* out) {
        const bool acquired = worker_runtime::try_acquire_latest_running_slot_view(
            output_slots, latest_output_index, running, out, "live fanout requires an output bundle output");
        if (!acquired && running_state() && pipeline_trace != nullptr) {
            pipeline_trace->note_acquire_miss();
        }
        return acquired;
    }

    [[nodiscard]] bool wait_acquire_output(OutputBundle* out, const std::atomic<bool>& stop) {
        while (!stop.load(std::memory_order_acquire) && running_state()) {
            if (try_acquire_output(out)) {
                return true;
            }
            const std::uint64_t start_generation = output_signal.snapshot_generation();
            if (try_acquire_output(out)) {
                return true;
            }
            output_signal.wait_for_next(start_generation,
                                        [&] { return stop.load(std::memory_order_acquire) || !running_state(); });
        }
        if (out != nullptr) {
            *out = {};
        }
        return false;
    }

    void release_output(const std::uint32_t slot_index) {
        if (slot_index >= output_slots.size()) {
            throw std::runtime_error("live fanout output release index out of range");
        }
        OutputSlot& slot = *output_slots[slot_index];
        release_live_frame_slot(slot, "live fanout output slot release called for a slot that is not acquired");
    }

    void allocate_resources(const std::uint32_t detect_slot_count, const std::uint32_t output_slot_count) {
        ensure_cuda_ok(cudaSetDevice(ingress.config().cuda_device_index),
                       "cudaSetDevice for live fanout resource allocation");

        try {
            allocate_live_frame_slots(detect_slots, detect_slot_count, max_capture_width, max_capture_height,
                                      "cudaMallocPitch for live detect bundle",
                                      "cudaStreamCreateWithPriority for live detect bundle",
                                      "cudaEventCreateWithFlags for live detect bundle");
            allocate_live_frame_slots(output_slots, output_slot_count, max_capture_width, max_capture_height,
                                      "cudaMallocPitch for live output bundle",
                                      "cudaStreamCreateWithPriority for live output bundle",
                                      "cudaEventCreateWithFlags for live output bundle");

            release_stream.create_with_highest_priority("cudaStreamCreateWithPriority for live source release stream");
            release_events.resize(std::max<std::uint32_t>(1U, ingress.max_inflight_sources()));
            for (ReleaseEvent& release_event : release_events) {
                release_event.event.create(cudaEventDisableTiming,
                                           "cudaEventCreateWithFlags for live source release event");
            }
            pending_source_releases.reserve(release_events.size());
            pending_source_release_scratch.reserve(release_events.size());
        } catch (...) {
            destroy_resources();
            throw;
        }
    }

    void destroy_resources() noexcept {
        (void)cudaSetDevice(ingress.config().cuda_device_index);
        release_stream.reset();
        for (ReleaseEvent& release_event : release_events) {
            release_event.in_use = false;
        }
        release_events.clear();
        pending_source_releases.clear();
        pending_source_release_scratch.clear();
        latest_detect_index.store(-1, std::memory_order_release);
        latest_output_index.store(-1, std::memory_order_release);
        detect_slots.clear();
        output_slots.clear();
    }

    void fanout_thread_main() {
        worker_runtime::run_worker_thread_main(
            running, stop_requested, status, ingress.config().cuda_device_index, "cudaSetDevice for live fanout thread",
            "live.fanout",
            [this](Status&) {
                drain_pending_source_releases(false);

                SourceFrameView source{};
                if (!ingress.try_acquire_latest_source(&source)) {
                    if (!stop_requested.load(std::memory_order_acquire) && pipeline_trace != nullptr) {
                        pipeline_trace->note_acquire_miss();
                        pipeline_trace->record(LivePipelineStage::FanoutAcquireSource,
                                               LiveFrameId{ingress.session_nonce(), 0U}, now_ns(), 0U,
                                               trace_cuda_device_index());
                    }
                    if (!stop_requested.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(kFanoutNoWorkBackoff);
                    }
                    return;
                }
                if (pipeline_trace != nullptr) {
                    const std::uint64_t acquired_ns = now_ns();
                    pipeline_trace->record(LivePipelineStage::FanoutAcquireSource, source.frame_id, acquired_ns,
                                           source.capture_ns, trace_cuda_device_index());
                    const std::uint64_t acquire_latency_ns = source.capture_ns != 0U && acquired_ns >= source.capture_ns
                                                                 ? acquired_ns - source.capture_ns
                                                                 : 0U;
                    warn_if_over_budget("fanout.acquire_source", source.frame_id, acquire_latency_ns);
                }
                source_frame_log_count += 1U;
                if (!first_source_frame_logged) {
                    first_source_frame_logged = true;
                    log_live_fanout_message(
                        "info", "first live source frame acquired: frame=" + std::to_string(source.frame_id.sequence));
                } else if (live_should_log_periodic_frame(source_frame_log_count)) {
                    log_live_fanout_message(
                        "info", "live source frame acquired: frame=" + std::to_string(source.frame_id.sequence) +
                                    " count=" + std::to_string(source_frame_log_count));
                }

                runtime_crop.sync_from(ui_crop_state);
                const LiveCaptureRegion detect_region = resolve_detect_region(source);

                DetectSlot* detect_slot = reserve_detect_slot();
                if (detect_slot == nullptr) {
                    skipped_detect_publishes.fetch_add(1U, std::memory_order_relaxed);
                }
                if (detect_slot != nullptr) {
                    if (source.ready_event != nullptr) {
                        ensure_cuda_ok(cudaStreamWaitEvent(detect_slot->stream.get(), source.ready_event, 0),
                                       "cudaStreamWaitEvent for live detect bundle source readiness");
                    }
                    const CUdeviceptr source_ptr =
                        source.data + clamp_copy_offset_bytes(source.region, detect_region, source.pitch_bytes);
                    ensure_cuda_ok(cudaMemcpy2DAsync(
                                       device_ptr_as_void(detect_slot->device_buffer.data()),
                                       detect_slot->device_buffer.pitch_bytes(), device_ptr_as_const_void(source_ptr),
                                       source.pitch_bytes, static_cast<std::size_t>(detect_region.width) * 3U,
                                       detect_region.height, cudaMemcpyDeviceToDevice, detect_slot->stream.get()),
                                   "cudaMemcpy2DAsync for live detect bundle");
                    ensure_cuda_ok(cudaEventRecord(detect_slot->ready_event.get(), detect_slot->stream.get()),
                                   "cudaEventRecord for live detect bundle");
                    publish_live_frame_slot(*detect_slot, latest_detect_index, source, detect_region, now_ns());
                    if (pipeline_trace != nullptr) {
                        pipeline_trace->record(LivePipelineStage::FanoutPublishOutput, source.frame_id,
                                               detect_slot->ready_ns, source.capture_ns, trace_cuda_device_index());
                    }
                    const std::uint64_t detect_latency_ns =
                        source.capture_ns != 0U && detect_slot->ready_ns >= source.capture_ns
                            ? detect_slot->ready_ns - source.capture_ns
                            : 0U;
                    warn_if_over_budget("fanout.publish_detect", source.frame_id, detect_latency_ns);
                    detect_signal.notify();
                }

                OutputSlot* output_slot = reserve_output_slot();
                if (output_slot == nullptr) {
                    skipped_output_publishes.fetch_add(1U, std::memory_order_relaxed);
                }
                if (output_slot != nullptr) {
                    if (source.ready_event != nullptr) {
                        ensure_cuda_ok(cudaStreamWaitEvent(output_slot->stream.get(), source.ready_event, 0),
                                       "cudaStreamWaitEvent for live output bundle source readiness");
                    }
                    ensure_cuda_ok(cudaMemcpy2DAsync(
                                       device_ptr_as_void(output_slot->device_buffer.data()),
                                       output_slot->device_buffer.pitch_bytes(), device_ptr_as_const_void(source.data),
                                       source.pitch_bytes, static_cast<std::size_t>(source.region.width) * 3U,
                                       source.region.height, cudaMemcpyDeviceToDevice, output_slot->stream.get()),
                                   "cudaMemcpy2DAsync for live output bundle");
                    ensure_cuda_ok(cudaEventRecord(output_slot->ready_event.get(), output_slot->stream.get()),
                                   "cudaEventRecord for live output bundle");
                    publish_live_frame_slot(*output_slot, latest_output_index, source, source.region, now_ns());
                    if (pipeline_trace != nullptr) {
                        pipeline_trace->record(LivePipelineStage::FanoutPublishOutput, source.frame_id,
                                               output_slot->ready_ns, source.capture_ns, trace_cuda_device_index());
                    }
                    const std::uint64_t output_latency_ns =
                        source.capture_ns != 0U && output_slot->ready_ns >= source.capture_ns
                            ? output_slot->ready_ns - source.capture_ns
                            : 0U;
                    output_frame_log_count += 1U;
                    if (!first_output_frame_logged) {
                        first_output_frame_logged = true;
                        log_live_fanout_message("info", "first live fanout output published: frame=" +
                                                            std::to_string(source.frame_id.sequence));
                    } else if (live_should_log_periodic_frame(output_frame_log_count)) {
                        log_live_fanout_message(
                            "info", "live fanout output published: frame=" + std::to_string(source.frame_id.sequence) +
                                        " count=" + std::to_string(output_frame_log_count));
                    }
                    warn_if_over_budget("fanout.publish_output", source.frame_id, output_latency_ns);
                    output_signal.notify();
                }

                if (detect_slot == nullptr && output_slot == nullptr) {
                    if (pipeline_trace != nullptr) {
                        pipeline_trace->note_acquire_miss();
                        pipeline_trace->record(LivePipelineStage::FanoutPublishOutput, source.frame_id, now_ns(),
                                               source.capture_ns, trace_cuda_device_index());
                    }
                    log_live_fanout_message("warn",
                                            "live fanout dropped source frame because no output slots were "
                                            "available: frame=" +
                                                std::to_string(source.frame_id.sequence));
                    ingress.release_source(source.buffer_index, source.frame_id, source.capture_ns);
                    return;
                }
                frames_fanned_out.fetch_add(1U, std::memory_order_relaxed);

                const std::size_t release_event_index = acquire_release_event_index();
                if (detect_slot != nullptr) {
                    ensure_cuda_ok(cudaStreamWaitEvent(release_stream.get(), detect_slot->ready_event.get(), 0),
                                   "cudaStreamWaitEvent for deferred detect source release");
                }
                if (output_slot != nullptr) {
                    ensure_cuda_ok(cudaStreamWaitEvent(release_stream.get(), output_slot->ready_event.get(), 0),
                                   "cudaStreamWaitEvent for deferred output source release");
                }
                ensure_cuda_ok(cudaEventRecord(release_events[release_event_index].event.get(), release_stream.get()),
                               "cudaEventRecord for deferred live source release");
                release_events[release_event_index].in_use = true;
                pending_source_releases.push_back(
                    PendingSourceRelease{source.buffer_index, release_event_index, source.frame_id, source.capture_ns});
                note_release_backlog();
            },
            [this](const char* error_message) {
                publish_error(error_message);
                try {
                    drain_pending_source_releases(true);
                } catch (const std::exception& release_error) {
                    Status s = snapshot_status();
                    s.last_error = std::string(error_message) + " | cleanup: " + release_error.what();
                    worker_runtime::publish_status(&status, s);
                    log_live_fanout_message("error", release_error.what());
                }
            });
        log_live_fanout_message("info", "fanout thread stopped");
    }

    void publish_error(std::string error_message) {
        Status s = snapshot_status();
        s.last_error = std::move(error_message);
        worker_runtime::publish_status(&status, s);
    }

    void notify_detect_waiters() noexcept {
        detect_signal.wake();
    }

    void notify_output_waiters() noexcept {
        output_signal.wake();
    }

    void drain_pending_source_releases(const bool wait) {
        pending_source_release_scratch.clear();

        for (const PendingSourceRelease& pending : pending_source_releases) {
            if (pending.event_index >= release_events.size()) {
                throw std::runtime_error("live fanout source release event index out of range");
            }
            ReleaseEvent& release_event = release_events[pending.event_index];
            if (release_event.event.empty()) {
                throw std::runtime_error("live fanout source release event is not initialized");
            }

            bool ready = wait;
            if (!ready) {
                ready = event_ready(release_event.event.get());
            }
            if (!ready) {
                pending_source_release_scratch.push_back(pending);
                continue;
            }
            if (wait) {
                ensure_cuda_ok(cudaEventSynchronize(release_event.event.get()),
                               "cudaEventSynchronize for deferred live source release");
            }
            ingress.release_source(pending.buffer_index, pending.frame_id, pending.capture_ns);
            const std::uint64_t released_ns = now_ns();
            const std::uint64_t release_latency_ns =
                pending.capture_ns != 0U && released_ns >= pending.capture_ns ? released_ns - pending.capture_ns : 0U;
            source_release_log_count += 1U;
            if (!first_source_release_logged) {
                first_source_release_logged = true;
                log_live_fanout_message(
                    "info", "first live source release completed: frame=" + std::to_string(pending.frame_id.sequence) +
                                " latency_us=" + std::to_string(release_latency_ns / 1000U));
            } else if (live_should_log_periodic_frame(source_release_log_count)) {
                log_live_fanout_message(
                    "info", "live source release completed: frame=" + std::to_string(pending.frame_id.sequence) +
                                " count=" + std::to_string(source_release_log_count) +
                                " latency_us=" + std::to_string(release_latency_ns / 1000U));
            }
            warn_if_over_budget("fanout.release_source", pending.frame_id, release_latency_ns);
            release_event.in_use = false;
        }

        pending_source_releases.swap(pending_source_release_scratch);
        note_release_backlog();
    }

    [[nodiscard]] DetectSlot* reserve_detect_slot() {
        return reserve_live_frame_slot(detect_slots, latest_detect_index);
    }

    [[nodiscard]] OutputSlot* reserve_output_slot() {
        return reserve_live_frame_slot(output_slots, latest_output_index);
    }

    [[nodiscard]] std::size_t acquire_release_event_index() {
        for (std::size_t index = 0; index < release_events.size(); ++index) {
            ReleaseEvent& release_event = release_events[index];
            if (!release_event.in_use) {
                return index;
            }
        }
        drain_pending_source_releases(false);
        for (std::size_t index = 0; index < release_events.size(); ++index) {
            if (!release_events[index].in_use) {
                return index;
            }
        }
        if (pipeline_trace != nullptr) {
            pipeline_trace->note_acquire_miss();
            const LiveFrameId frame_id =
                pending_source_releases.empty() ? LiveFrameId{} : pending_source_releases.front().frame_id;
            const std::uint64_t capture_ns =
                pending_source_releases.empty() ? 0U : pending_source_releases.front().capture_ns;
            pipeline_trace->record(LivePipelineStage::FanoutReleaseSource, frame_id, now_ns(), capture_ns,
                                   trace_cuda_device_index());
        }
        throw std::runtime_error("live fanout ran out of deferred source release events");
    }

    [[nodiscard]] LiveCaptureRegion resolve_detect_region(const SourceFrameView& source) const {
        const UiCropSnapshot crop = runtime_crop.snapshot();
        if (!crop.has_crop || crop.region.width == 0U || crop.region.height == 0U) {
            return source.region;
        }

        const std::uint32_t source_x2 = source.region.x + source.region.width;
        const std::uint32_t source_y2 = source.region.y + source.region.height;
        const std::uint32_t crop_x1 = std::clamp(crop.region.x, source.region.x, source_x2 - 1U);
        const std::uint32_t crop_y1 = std::clamp(crop.region.y, source.region.y, source_y2 - 1U);
        const std::uint32_t crop_x2 =
            std::clamp(crop.region.x + std::max<std::uint32_t>(1U, crop.region.width), crop_x1 + 1U, source_x2);
        const std::uint32_t crop_y2 =
            std::clamp(crop.region.y + std::max<std::uint32_t>(1U, crop.region.height), crop_y1 + 1U, source_y2);
        return LiveCaptureRegion{crop_x1, crop_y1, crop_x2 - crop_x1, crop_y2 - crop_y1};
    }

    LiveVideoIngress& ingress;
    const UiCropState& ui_crop_state;
    std::shared_ptr<LivePipelineTrace> pipeline_trace;
    RuntimeCropState runtime_crop{};
    std::thread thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<std::shared_ptr<const Status>> status{std::make_shared<Status>()};

    std::vector<std::unique_ptr<DetectSlot>> detect_slots;
    std::vector<std::unique_ptr<OutputSlot>> output_slots;
    std::atomic<int> latest_detect_index{-1};
    std::atomic<int> latest_output_index{-1};
    PublicationSignal detect_signal;
    PublicationSignal output_signal;

    CudaStreamHandle release_stream;
    std::vector<ReleaseEvent> release_events;
    std::vector<PendingSourceRelease> pending_source_releases;
    std::vector<PendingSourceRelease> pending_source_release_scratch;
    bool first_source_frame_logged = false;
    bool first_output_frame_logged = false;
    bool first_source_release_logged = false;
    std::uint64_t source_frame_log_count = 0;
    std::uint64_t output_frame_log_count = 0;
    std::uint64_t source_release_log_count = 0;
    std::atomic<std::uint64_t> frames_fanned_out{0};
    std::atomic<std::uint64_t> skipped_detect_publishes{0};
    std::atomic<std::uint64_t> skipped_output_publishes{0};

    std::uint32_t max_capture_width = 0;
    std::uint32_t max_capture_height = 0;

    [[nodiscard]] std::uint32_t trace_cuda_device_index() const noexcept {
        return ingress.config().cuda_device_index >= 0 ? static_cast<std::uint32_t>(ingress.config().cuda_device_index)
                                                       : kLivePipelineUnknownCudaDevice;
    }

    [[nodiscard]] std::uint64_t frame_budget_ns() const noexcept {
        return ingress.config().fps > 0U ? 1000000000ULL / static_cast<std::uint64_t>(ingress.config().fps) : 0U;
    }

    void warn_if_over_budget(const char* stage, const LiveFrameId& frame_id, const std::uint64_t latency_ns) {
        const std::uint64_t budget_ns = frame_budget_ns();
        if (budget_ns == 0U || latency_ns <= budget_ns) {
            return;
        }
        log_live_fanout_message("warn", std::string(stage) +
                                            " exceeded frame interval: frame=" + std::to_string(frame_id.sequence) +
                                            " latency_us=" + std::to_string(latency_ns / 1000U) +
                                            " budget_us=" + std::to_string(budget_ns / 1000U));
    }

    void note_release_backlog() noexcept {
        if (pipeline_trace != nullptr) {
            pipeline_trace->note_release_backlog(static_cast<std::uint64_t>(pending_source_releases.size()));
        }
    }

    void attach_local_status(Status& s) const noexcept {
        s.running = running_state();
        s.frames_fanned_out = frames_fanned_out.load(std::memory_order_relaxed);
        s.skipped_detect_publishes = skipped_detect_publishes.load(std::memory_order_relaxed);
        s.skipped_output_publishes = skipped_output_publishes.load(std::memory_order_relaxed);
    }

    void attach_pipeline_status(Status& s) const noexcept {
        if (pipeline_trace == nullptr) {
            return;
        }
        attach_live_pipeline_status(s, *pipeline_trace);
    }
};

LiveFrameFanout::LiveFrameFanout(LiveVideoIngress& ingress, const UiCropState& ui_crop_state,
                                 const std::uint32_t detect_slot_count, const std::uint32_t output_slot_count,
                                 std::shared_ptr<LivePipelineTrace> pipeline_trace)
    : impl_(std::make_unique<Impl>(ingress, ui_crop_state, detect_slot_count, output_slot_count,
                                   std::move(pipeline_trace))) {}

LiveFrameFanout::~LiveFrameFanout() = default;

void LiveFrameFanout::start() {
    impl_->start();
}

void LiveFrameFanout::stop() {
    impl_->stop();
}

bool LiveFrameFanout::running() const noexcept {
    return impl_->running_state();
}

std::string LiveFrameFanout::last_error() const {
    return impl_->last_error();
}

LiveFrameFanout::Status LiveFrameFanout::snapshot_status() const {
    return impl_->snapshot_status();
}

bool LiveFrameFanout::try_acquire_detect(DetectBundle* out) {
    return impl_->try_acquire_detect(out);
}

bool LiveFrameFanout::wait_acquire_detect(DetectBundle* out, const std::atomic<bool>& stop_requested) {
    return impl_->wait_acquire_detect(out, stop_requested);
}

void LiveFrameFanout::release_detect(const std::uint32_t slot_index) {
    impl_->release_detect(slot_index);
}

bool LiveFrameFanout::try_acquire_output(OutputBundle* out) {
    return impl_->try_acquire_output(out);
}

bool LiveFrameFanout::wait_acquire_output(OutputBundle* out, const std::atomic<bool>& stop_requested) {
    return impl_->wait_acquire_output(out, stop_requested);
}

void LiveFrameFanout::release_output(const std::uint32_t slot_index) {
    impl_->release_output(slot_index);
}

void LiveFrameFanout::notify_detect_waiters() noexcept {
    impl_->notify_detect_waiters();
}

void LiveFrameFanout::notify_output_waiters() noexcept {
    impl_->notify_output_waiters();
}

}  // namespace mmltk::live
