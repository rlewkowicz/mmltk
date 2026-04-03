#include "mmltk/live/live_frame_fanout.h"

#include "live/live_helpers.h"
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
      detect_slot_count_(detect_slot_count),
      output_slot_count_(output_slot_count),
      max_capture_width_(std::max<std::uint32_t>(1U, ingress.config().width)),
      max_capture_height_(std::max<std::uint32_t>(1U, ingress.config().height)) {
    if (detect_slot_count == 0U) {
        throw std::runtime_error("live fanout requires at least one detect slot");
    }
    if (output_slot_count == 0U) {
        throw std::runtime_error("live fanout requires at least one output slot");
    }

    detect_slots_.reserve(detect_slot_count);
    output_slots_.reserve(output_slot_count);
    allocate_resources();
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
    if (running()) {
        throw std::runtime_error("live fanout is already running");
    }
    if (thread_.joinable()) {
        throw std::runtime_error("live fanout must be stopped before it can be restarted");
    }
    publish_error({});
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&LiveFrameFanout::fanout_thread_main, this);
    log_live_fanout_message("info", "fanout thread started");
}

void LiveFrameFanout::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

bool LiveFrameFanout::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::string LiveFrameFanout::last_error() const {
    const std::shared_ptr<const std::string> current =
        std::atomic_load_explicit(&last_error_, std::memory_order_acquire);
    return current ? *current : std::string{};
}

bool LiveFrameFanout::try_acquire_detect(DetectBundle* out) {
    if (out == nullptr) {
        throw std::runtime_error("live fanout requires a detect bundle output");
    }
    *out = {};
    return try_acquire_latest_published_slot(detect_slots_, latest_detect_index_, out, [](DetectSlot& slot) {
        return DetectBundle{
            slot.slot_index,
            slot.frame_id,
            slot.device_buffer.data(),
            DetectDimensions{slot.region.width, slot.region.height, slot.device_buffer.pitch_bytes()},
            slot.ready_event.get(),
            slot.stream.get(),
            slot.region,
            slot.capture_ns,
            slot.ready_ns,
            slot.short_frame,
        };
    });
}

void LiveFrameFanout::release_detect(std::uint32_t slot_index) {
    if (slot_index >= detect_slots_.size()) {
        throw std::runtime_error("live fanout detect release index out of range");
    }
    DetectSlot& slot = *detect_slots_[slot_index];
    release_acquired_slot(slot, "live fanout detect slot release called for a slot that is not acquired");
}

bool LiveFrameFanout::try_acquire_output(OutputBundle* out) {
    if (out == nullptr) {
        throw std::runtime_error("live fanout requires an output bundle output");
    }
    *out = {};
    return try_acquire_latest_published_slot(output_slots_, latest_output_index_, out, [](OutputSlot& slot) {
        return OutputBundle{
            slot.slot_index,
            slot.frame_id,
            slot.device_buffer.data(),
            OutputDimensions{slot.region.width, slot.region.height, slot.device_buffer.pitch_bytes()},
            slot.ready_event.get(),
            slot.stream.get(),
            slot.region,
            slot.capture_ns,
            slot.ready_ns,
            slot.short_frame,
        };
    });
}

void LiveFrameFanout::release_output(std::uint32_t slot_index) {
    if (slot_index >= output_slots_.size()) {
        throw std::runtime_error("live fanout output release index out of range");
    }
    OutputSlot& slot = *output_slots_[slot_index];
    release_acquired_slot(slot, "live fanout output slot release called for a slot that is not acquired");
}

void LiveFrameFanout::allocate_resources() {
    ensure_cuda_ok(cudaSetDevice(ingress_.config().cuda_device_index), "cudaSetDevice for live fanout resource allocation");

    try {
        for (std::uint32_t slot_index = 0; slot_index < detect_slot_count_; ++slot_index) {
            auto slot = std::make_unique<DetectSlot>();
            slot->slot_index = slot_index;
            slot->device_buffer.ensure_dimensions(max_capture_width_, max_capture_height_,
                                                  "cudaMallocPitch for live detect bundle");
            slot->stream.create_with_highest_priority("cudaStreamCreateWithPriority for live detect bundle");
            slot->ready_event.create(cudaEventDisableTiming,
                                     "cudaEventCreateWithFlags for live detect bundle");
            detect_slots_.push_back(std::move(slot));
        }

        for (std::uint32_t slot_index = 0; slot_index < output_slot_count_; ++slot_index) {
            auto slot = std::make_unique<OutputSlot>();
            slot->slot_index = slot_index;
            slot->device_buffer.ensure_dimensions(max_capture_width_, max_capture_height_,
                                                  "cudaMallocPitch for live output bundle");
            slot->stream.create_with_highest_priority("cudaStreamCreateWithPriority for live output bundle");
            slot->ready_event.create(cudaEventDisableTiming,
                                     "cudaEventCreateWithFlags for live output bundle");
            output_slots_.push_back(std::move(slot));
        }

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
    detect_slots_.clear();
    output_slots_.clear();
}

void LiveFrameFanout::fanout_thread_main() {
    try {
        ensure_cuda_ok(cudaSetDevice(ingress_.config().cuda_device_index), "cudaSetDevice for live fanout thread");
        while (!stop_requested_.load(std::memory_order_acquire)) {
            drain_pending_source_releases(false);

            SourceFrameView source{};
            if (!ingress_.try_acquire_latest_source(&source)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
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
                detect_slot->frame_id = source.frame_id;
                detect_slot->region = detect_region;
                detect_slot->capture_ns = source.capture_ns;
                detect_slot->ready_ns = now_ns();
                detect_slot->short_frame = source.short_frame;
                detect_slot->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
                latest_detect_index_.store(static_cast<int>(detect_slot->slot_index), std::memory_order_release);
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
                output_slot->frame_id = source.frame_id;
                output_slot->region = source.region;
                output_slot->capture_ns = source.capture_ns;
                output_slot->ready_ns = now_ns();
                output_slot->short_frame = source.short_frame;
                output_slot->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
                latest_output_index_.store(static_cast<int>(output_slot->slot_index), std::memory_order_release);
            }

            if (detect_slot == nullptr && output_slot == nullptr) {
                ingress_.release_source(source.buffer_index);
                continue;
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
        }

        drain_pending_source_releases(true);
    } catch (const std::exception& error) {
        publish_error(error.what());
        log_live_fanout_message("error", error.what());
        try {
            drain_pending_source_releases(true);
        } catch (const std::exception& release_error) {
            publish_error(std::string(error.what()) + " | cleanup: " + release_error.what());
            log_live_fanout_message("error", release_error.what());
        }
    }

    running_.store(false, std::memory_order_release);
    log_live_fanout_message("info", "fanout thread stopped");
}

void LiveFrameFanout::publish_error(std::string error_message) {
    store_error_message(&last_error_, std::move(error_message));
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
    return reserve_writable_slot(
        detect_slots_,
        latest_detect_index_,
        [](DetectSlot&) {},
        [](DetectSlot& slot) { return slot.ready_event.get(); },
        "cudaEventQuery for live fanout slot reuse");
}

LiveFrameFanout::OutputSlot* LiveFrameFanout::reserve_output_slot() {
    return reserve_writable_slot(
        output_slots_,
        latest_output_index_,
        [](OutputSlot&) {},
        [](OutputSlot& slot) { return slot.ready_event.get(); },
        "cudaEventQuery for live fanout slot reuse");
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

bool LiveFrameFanout::try_acquire_detect_slot(DetectSlot& slot, DetectBundle* out) {
    return try_acquire_published_slot(slot, out, [](DetectSlot& published) {
        return DetectBundle{
            published.slot_index,
            published.frame_id,
            published.device_buffer.data(),
            DetectDimensions{published.region.width, published.region.height, published.device_buffer.pitch_bytes()},
            published.ready_event.get(),
            published.stream.get(),
            published.region,
            published.capture_ns,
            published.ready_ns,
            published.short_frame,
        };
    });
}

bool LiveFrameFanout::try_acquire_output_slot(OutputSlot& slot, OutputBundle* out) {
    return try_acquire_published_slot(slot, out, [](OutputSlot& published) {
        return OutputBundle{
            published.slot_index,
            published.frame_id,
            published.device_buffer.data(),
            OutputDimensions{published.region.width, published.region.height, published.device_buffer.pitch_bytes()},
            published.ready_event.get(),
            published.stream.get(),
            published.region,
            published.capture_ns,
            published.ready_ns,
            published.short_frame,
        };
    });
}

} // namespace mmltk::live
