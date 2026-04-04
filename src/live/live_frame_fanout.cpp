#include "mmltk/live/live_frame_fanout.h"

#include "live/live_helpers.h"
#include "mmltk/live/live_worker_runtime.h"
#include "mmltk_logging.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace mmltk::live {

namespace {

using Clock = std::chrono::steady_clock;

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

std::size_t clamp_copy_offset_bytes(const LiveCaptureRegion& source_region,
                                    const LiveCaptureRegion& detect_region,
                                    std::size_t pitch_bytes) {
    const std::uint32_t row_offset = detect_region.y > source_region.y ? detect_region.y - source_region.y : 0U;
    const std::uint32_t col_offset = detect_region.x > source_region.x ? detect_region.x - source_region.x : 0U;
    return static_cast<std::size_t>(row_offset) * pitch_bytes + static_cast<std::size_t>(col_offset) * 3U;
}

} // namespace

LiveFrameFanout::LiveFrameFanout(LiveVideoIngress& ingress,
                                 const UiCropState& ui_crop_state,
                                 std::uint32_t detect_slot_count,
                                 std::uint32_t output_slot_count)
    : ingress_(ingress),
      ui_crop_state_(ui_crop_state),
      max_capture_width_(std::max<std::uint32_t>(1U, ingress.config().width)),
      max_capture_height_(std::max<std::uint32_t>(1U, ingress.config().height)) {
    if (detect_slot_count == 0U) {
        throw std::runtime_error("live fanout requires at least one detect slot");
    }
    if (output_slot_count == 0U) {
        throw std::runtime_error("live fanout requires at least one output slot");
    }
    allocate_resources(detect_slot_count, output_slot_count);
}

LiveFrameFanout::~LiveFrameFanout() {
    try {
        stop();
    } catch (const std::exception& error) {
        log_live_fanout_message("error", error.what());
    }
    destroy_resources();
}

void LiveFrameFanout::start() {
    if (running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("live fanout is already running");
    }
    if (thread_.joinable()) {
        throw std::runtime_error("live fanout must be stopped before it can be restarted");
    }

    publish_error("");
    worker_runtime::publish_status(&status_, Status{});
    ensure_cuda_ok(cudaSetDevice(ingress_.config().cuda_device_index), "cudaSetDevice for live fanout start");
    worker_runtime::reset_slot_views_for_restart(detect_slots_,
                                                 latest_detect_index_,
                                                 "cudaEventSynchronize for live detect slot restart");
    worker_runtime::reset_slot_views_for_restart(output_slots_,
                                                 latest_output_index_,
                                                 "cudaEventSynchronize for live output slot restart");
    worker_runtime::start_worker_thread(
        thread_,
        stop_requested_,
        running_,
        [this]() { fanout_thread_main(); },
        "live fanout is already running",
        "live fanout must be stopped before it can be restarted");
    log_live_fanout_message("info", "fanout thread started");
}

void LiveFrameFanout::stop() {
    worker_runtime::stop_worker_thread(thread_, stop_requested_, running_);
}

bool LiveFrameFanout::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::string LiveFrameFanout::last_error() const {
    return snapshot_status().last_error;
}

LiveFrameFanout::Status LiveFrameFanout::snapshot_status() const {
    return worker_runtime::snapshot_status(status_);
}

bool LiveFrameFanout::try_acquire_detect(DetectBundle* out) {
    if (out == nullptr) {
        throw std::runtime_error("live fanout requires a detect bundle output");
    }
    *out = {};
    if (!running()) {
        return false;
    }
    return try_acquire_live_frame_slot(detect_slots_, latest_detect_index_, out);
}

void LiveFrameFanout::release_detect(std::uint32_t slot_index) {
    if (slot_index >= detect_slots_.size()) {
        throw std::runtime_error("live fanout detect release index out of range");
    }
    DetectSlot& slot = *detect_slots_[slot_index];
    release_live_frame_slot(slot, "live fanout detect slot release called for a slot that is not acquired");
}

bool LiveFrameFanout::try_acquire_output(OutputBundle* out) {
    if (out == nullptr) {
        throw std::runtime_error("live fanout requires an output bundle output");
    }
    *out = {};
    if (!running()) {
        return false;
    }
    return try_acquire_live_frame_slot(output_slots_, latest_output_index_, out);
}

void LiveFrameFanout::release_output(std::uint32_t slot_index) {
    if (slot_index >= output_slots_.size()) {
        throw std::runtime_error("live fanout output release index out of range");
    }
    OutputSlot& slot = *output_slots_[slot_index];
    release_live_frame_slot(slot, "live fanout output slot release called for a slot that is not acquired");
}

void LiveFrameFanout::allocate_resources(const std::uint32_t detect_slot_count,
                                         const std::uint32_t output_slot_count) {
    ensure_cuda_ok(cudaSetDevice(ingress_.config().cuda_device_index), "cudaSetDevice for live fanout resource allocation");

    try {
        allocate_live_frame_slots(detect_slots_, detect_slot_count, max_capture_width_, max_capture_height_,
                                  "cudaMallocPitch for live detect bundle",
                                  "cudaStreamCreateWithPriority for live detect bundle",
                                  "cudaEventCreateWithFlags for live detect bundle");
        allocate_live_frame_slots(output_slots_, output_slot_count, max_capture_width_, max_capture_height_,
                                  "cudaMallocPitch for live output bundle",
                                  "cudaStreamCreateWithPriority for live output bundle",
                                  "cudaEventCreateWithFlags for live output bundle");

        release_stream_.create_with_highest_priority("cudaStreamCreateWithPriority for live source release stream");
        release_events_.resize(std::max<std::uint32_t>(1U, ingress_.max_inflight_sources()));
        for (ReleaseEvent& release_event : release_events_) {
            release_event.event.create(cudaEventDisableTiming,
                                       "cudaEventCreateWithFlags for live source release event");
        }
    } catch (...) {
        destroy_resources();
        throw;
    }
}

void LiveFrameFanout::destroy_resources() noexcept {
    (void)cudaSetDevice(ingress_.config().cuda_device_index);
    release_stream_.reset();
    for (ReleaseEvent& release_event : release_events_) {
        release_event.in_use = false;
    }
    release_events_.clear();
    latest_detect_index_.store(-1, std::memory_order_release);
    latest_output_index_.store(-1, std::memory_order_release);
    detect_slots_.clear();
    output_slots_.clear();
}

void LiveFrameFanout::fanout_thread_main() {
    worker_runtime::run_worker_thread_main(
        running_,
        stop_requested_,
        status_,
        ingress_.config().cuda_device_index,
        "cudaSetDevice for live fanout thread",
        "live.fanout",
        [this](Status& status) {
            drain_pending_source_releases(false);

            SourceFrameView source{};
            if (!ingress_.try_acquire_latest_source(&source)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return;
            }

            runtime_crop_.sync_from(ui_crop_state_);
            const LiveCaptureRegion detect_region = resolve_detect_region(source);

            DetectSlot* detect_slot = reserve_detect_slot();
            if (detect_slot != nullptr) {
                if (source.ready_event != nullptr) {
                    ensure_cuda_ok(cudaStreamWaitEvent(detect_slot->stream.get(), source.ready_event, 0),
                                   "cudaStreamWaitEvent for live detect bundle source readiness");
                }
                const CUdeviceptr source_ptr =
                    source.data + clamp_copy_offset_bytes(source.region, detect_region, source.pitch_bytes);
                ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(detect_slot->device_buffer.data()),
                                                 detect_slot->device_buffer.pitch_bytes(),
                                                 device_ptr_as_const_void(source_ptr),
                                                 source.pitch_bytes,
                                                 static_cast<std::size_t>(detect_region.width) * 3U,
                                                 detect_region.height,
                                                 cudaMemcpyDeviceToDevice,
                                                 detect_slot->stream.get()),
                               "cudaMemcpy2DAsync for live detect bundle");
                ensure_cuda_ok(cudaEventRecord(detect_slot->ready_event.get(), detect_slot->stream.get()),
                               "cudaEventRecord for live detect bundle");
                publish_live_frame_slot(*detect_slot, latest_detect_index_, source, detect_region, now_ns());
            }

            OutputSlot* output_slot = reserve_output_slot();
            if (output_slot != nullptr) {
                if (source.ready_event != nullptr) {
                    ensure_cuda_ok(cudaStreamWaitEvent(output_slot->stream.get(), source.ready_event, 0),
                                   "cudaStreamWaitEvent for live output bundle source readiness");
                }
                ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(output_slot->device_buffer.data()),
                                                 output_slot->device_buffer.pitch_bytes(),
                                                 device_ptr_as_const_void(source.data),
                                                 source.pitch_bytes,
                                                 static_cast<std::size_t>(source.region.width) * 3U,
                                                 source.region.height,
                                                 cudaMemcpyDeviceToDevice,
                                                 output_slot->stream.get()),
                               "cudaMemcpy2DAsync for live output bundle");
                ensure_cuda_ok(cudaEventRecord(output_slot->ready_event.get(), output_slot->stream.get()),
                               "cudaEventRecord for live output bundle");
                publish_live_frame_slot(*output_slot, latest_output_index_, source, source.region, now_ns());
            }

            if (detect_slot == nullptr && output_slot == nullptr) {
                ingress_.release_source(source.buffer_index);
                return;
            }

            const std::size_t release_event_index = acquire_release_event_index();
            if (detect_slot != nullptr) {
                ensure_cuda_ok(cudaStreamWaitEvent(release_stream_.get(), detect_slot->ready_event.get(), 0),
                               "cudaStreamWaitEvent for deferred detect source release");
            }
            if (output_slot != nullptr) {
                ensure_cuda_ok(cudaStreamWaitEvent(release_stream_.get(), output_slot->ready_event.get(), 0),
                               "cudaStreamWaitEvent for deferred output source release");
            }
            ensure_cuda_ok(cudaEventRecord(release_events_[release_event_index].event.get(), release_stream_.get()),
                           "cudaEventRecord for deferred live source release");
            release_events_[release_event_index].in_use = true;
            pending_source_releases_.push_back(PendingSourceRelease{source.buffer_index, release_event_index});
        },
        [this](const char* error_message) {
            publish_error(error_message);
            try {
                drain_pending_source_releases(true);
            } catch (const std::exception& release_error) {
                Status s = snapshot_status();
                s.last_error = std::string(error_message) + " | cleanup: " + release_error.what();
                worker_runtime::publish_status(&status_, s);
                log_live_fanout_message("error", release_error.what());
            }
        });
    log_live_fanout_message("info", "fanout thread stopped");
}

void LiveFrameFanout::publish_error(std::string error_message) {
    Status s = snapshot_status();
    s.last_error = std::move(error_message);
    worker_runtime::publish_status(&status_, s);
}

void LiveFrameFanout::drain_pending_source_releases(bool wait) {
    std::vector<PendingSourceRelease> remaining;
    remaining.reserve(pending_source_releases_.size());

    for (const PendingSourceRelease& pending : pending_source_releases_) {
        if (pending.event_index >= release_events_.size()) {
            throw std::runtime_error("live fanout source release event index out of range");
        }
        ReleaseEvent& release_event = release_events_[pending.event_index];
        if (release_event.event.empty()) {
            throw std::runtime_error("live fanout source release event is not initialized");
        }

        bool ready = wait;
        if (!ready) {
            ready = event_ready(release_event.event.get());
        }
        if (!ready) {
            remaining.push_back(pending);
            continue;
        }
        if (wait) {
            ensure_cuda_ok(cudaEventSynchronize(release_event.event.get()),
                           "cudaEventSynchronize for deferred live source release");
        }
        ingress_.release_source(pending.buffer_index);
        release_event.in_use = false;
    }

    pending_source_releases_.swap(remaining);
}

LiveFrameFanout::DetectSlot* LiveFrameFanout::reserve_detect_slot() {
    return reserve_live_frame_slot(detect_slots_, latest_detect_index_);
}

LiveFrameFanout::OutputSlot* LiveFrameFanout::reserve_output_slot() {
    return reserve_live_frame_slot(output_slots_, latest_output_index_);
}

std::size_t LiveFrameFanout::acquire_release_event_index() {
    for (std::size_t index = 0; index < release_events_.size(); ++index) {
        ReleaseEvent& release_event = release_events_[index];
        if (!release_event.in_use) {
            return index;
        }
        if (event_ready(release_event.event.get())) {
            release_event.in_use = false;
            return index;
        }
    }
    throw std::runtime_error("live fanout ran out of deferred source release events");
}

LiveCaptureRegion LiveFrameFanout::resolve_detect_region(const SourceFrameView& source) const {
    const UiCropSnapshot crop = runtime_crop_.snapshot();
    if (!crop.has_crop || crop.region.width == 0U || crop.region.height == 0U) {
        return source.region;
    }

    const std::uint32_t source_x2 = source.region.x + source.region.width;
    const std::uint32_t source_y2 = source.region.y + source.region.height;
    const std::uint32_t crop_x1 = std::clamp(crop.region.x, source.region.x, source_x2 - 1U);
    const std::uint32_t crop_y1 = std::clamp(crop.region.y, source.region.y, source_y2 - 1U);
    const std::uint32_t crop_x2 = std::clamp(crop.region.x + std::max<std::uint32_t>(1U, crop.region.width),
                                             crop_x1 + 1U,
                                             source_x2);
    const std::uint32_t crop_y2 = std::clamp(crop.region.y + std::max<std::uint32_t>(1U, crop.region.height),
                                             crop_y1 + 1U,
                                             source_y2);
    return LiveCaptureRegion{crop_x1, crop_y1, crop_x2 - crop_x1, crop_y2 - crop_y1};
}

} // namespace mmltk::live
