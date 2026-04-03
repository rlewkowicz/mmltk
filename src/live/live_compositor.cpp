#include "mmltk/live/live_compositor.h"

#include "live/live_helpers.h"
#include "mmltk/rfdetr/draw_cuda.h"
#include "mmltk_logging.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace mmltk::live {

namespace {

using Clock = std::chrono::steady_clock;

void log_live_compositor_message(const char* level, const std::string& message) {
    if (message.empty()) {
        return;
    }
    auto logger = mmltk::logging::logger("live.compositor");
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

void composite_rgba_over_bgr_pitched(cudaStream_t stream,
                                     CUdeviceptr dst,
                                     std::size_t dst_pitch_bytes,
                                     CUdeviceptr src,
                                     std::size_t src_pitch_bytes,
                                     int width,
                                     int height,
                                     const char* context) {
    mmltk::rfdetr::launch_composite_rgba_over_bgr_pitched(device_ptr_as_bytes(dst),
                                                          dst_pitch_bytes,
                                                          device_ptr_as_bytes(src),
                                                          src_pitch_bytes,
                                                          width,
                                                          height,
                                                          stream);
    ensure_cuda_ok(cudaPeekAtLastError(), context);
}

} // namespace

LiveCompositor::LiveCompositor(LiveFrameFanout& fanout,
                               LiveAnalyzerWorker* analyzer_worker,
                               LiveManualOverlayWorker* manual_worker,
                               const int cuda_device_index,
                               const std::uint32_t max_capture_width,
                               const std::uint32_t max_capture_height,
                               const std::uint32_t output_slot_count,
                               const std::uint32_t raw_base_cache_slot_count)
    : fanout_(fanout),
      analyzer_worker_(analyzer_worker),
      manual_worker_(manual_worker),
      cuda_device_index_(cuda_device_index),
      max_capture_width_(std::max<std::uint32_t>(1U, max_capture_width)),
      max_capture_height_(std::max<std::uint32_t>(1U, max_capture_height)) {
    if (output_slot_count == 0U || raw_base_cache_slot_count == 0U) {
        throw std::runtime_error("live compositor requires output slots and raw-base cache slots");
    }
    output_slots_.reserve(output_slot_count);
    raw_base_cache_slots_.resize(raw_base_cache_slot_count);
    allocate_resources();
}

LiveCompositor::~LiveCompositor() {
    try {
        stop();
    } catch (const std::exception& error) {
        log_live_compositor_message("error", error.what());
    }
    destroy_resources();
}

void LiveCompositor::start() {
    if (running()) {
        throw std::runtime_error("live compositor is already running");
    }
    if (thread_.joinable()) {
        throw std::runtime_error("live compositor must be stopped before restart");
    }
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&LiveCompositor::compositor_thread_main, this);
}

void LiveCompositor::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

bool LiveCompositor::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool LiveCompositor::try_acquire_latest_output(OutputBundle* out) {
    if (out == nullptr) {
        throw std::runtime_error("live compositor requires an output bundle output");
    }
    *out = {};
    return try_acquire_latest_published_slot(output_slots_, latest_output_index_, out, [](CompositorSlot& slot) {
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

void LiveCompositor::release_output(const std::uint32_t slot_index) {
    if (slot_index >= output_slots_.size()) {
        throw std::runtime_error("live compositor output release index out of range");
    }
    CompositorSlot& slot = *output_slots_[slot_index];
    release_acquired_slot(slot, "live compositor output slot release called for a slot that is not acquired");
}

void LiveCompositor::set_persistent_analysis_overlay(const bool enabled) {
    persistent_analysis_overlay_.store(enabled, std::memory_order_release);
    if (!enabled) {
        retained_analysis_has_content_ = false;
    }
}

bool LiveCompositor::persistent_analysis_overlay() const noexcept {
    return persistent_analysis_overlay_.load(std::memory_order_acquire);
}

LiveCompositor::Status LiveCompositor::snapshot_status() const {
    const std::shared_ptr<const Status> current =
        std::atomic_load_explicit(&status_, std::memory_order_acquire);
    return current ? *current : Status{};
}

bool LiveCompositor::readback_raw_base(const LiveFrameId& frame_id,
                                       std::vector<std::uint8_t>* pixels_bgr,
                                       std::uint32_t* width,
                                       std::uint32_t* height,
                                       LiveCaptureRegion* region,
                                       std::string* error_message) {
    if (pixels_bgr == nullptr || width == nullptr || height == nullptr || region == nullptr) {
        throw std::runtime_error("live compositor readback requires non-null outputs");
    }
    std::lock_guard<std::mutex> lock(readback_mutex_);
    if (current_cuda_device_index_ != cuda_device_index_) {
        ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for compositor readback");
        current_cuda_device_index_ = cuda_device_index_;
    }

    const RawBaseCacheSlot* matched_slot = nullptr;
    for (const RawBaseCacheSlot& slot : raw_base_cache_slots_) {
        if (slot.occupied && slot.frame_id == frame_id) {
            matched_slot = &slot;
            break;
        }
    }
    if (matched_slot == nullptr) {
        if (error_message != nullptr) {
            *error_message = "raw-base readback failed because the requested live frame is no longer cached";
        }
        return false;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(matched_slot->region.width) * 3U;
    const std::size_t required_bytes = row_bytes * static_cast<std::size_t>(matched_slot->region.height);
    readback_host_buffer_.ensure_capacity(required_bytes, "cudaHostAlloc for compositor readback");

    if (matched_slot->ready_event.get() != nullptr) {
        ensure_cuda_ok(cudaStreamWaitEvent(readback_stream_.get(), matched_slot->ready_event.get(), 0),
                       "cudaStreamWaitEvent for compositor readback");
    }
    ensure_cuda_ok(cudaMemcpy2DAsync(readback_host_buffer_.data(),
                                     row_bytes,
                                     device_ptr_as_const_void(matched_slot->device_buffer.data()),
                                     matched_slot->device_buffer.pitch_bytes(),
                                     row_bytes,
                                     matched_slot->region.height,
                                     cudaMemcpyDeviceToHost,
                                     readback_stream_.get()),
                   "cudaMemcpy2DAsync for compositor readback");
    ensure_cuda_ok(cudaEventRecord(readback_ready_event_.get(), readback_stream_.get()),
                   "cudaEventRecord for compositor readback");
    ensure_cuda_ok(cudaEventSynchronize(readback_ready_event_.get()),
                   "cudaEventSynchronize for compositor readback");

    pixels_bgr->assign(readback_host_buffer_.data(), readback_host_buffer_.data() + required_bytes);
    *width = matched_slot->region.width;
    *height = matched_slot->region.height;
    *region = matched_slot->region;
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

void LiveCompositor::allocate_resources() {
    ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live compositor resource allocation");
    current_cuda_device_index_ = cuda_device_index_;

    try {
        for (std::uint32_t slot_index = 0; slot_index < output_slots_.capacity(); ++slot_index) {
            auto slot = std::make_unique<CompositorSlot>();
            slot->slot_index = slot_index;
            slot->device_buffer.ensure_dimensions(max_capture_width_, max_capture_height_,
                                                  "cudaMallocPitch for live compositor output");
            slot->stream.create_with_highest_priority("cudaStreamCreateWithPriority for live compositor output");
            slot->ready_event.create(cudaEventDisableTiming,
                                     "cudaEventCreateWithFlags for live compositor output");
            output_slots_.push_back(std::move(slot));
        }

        for (RawBaseCacheSlot& slot : raw_base_cache_slots_) {
            slot.device_buffer.ensure_dimensions(max_capture_width_, max_capture_height_,
                                                 "cudaMallocPitch for live raw-base cache");
            slot.ready_event.create(cudaEventDisableTiming,
                                    "cudaEventCreateWithFlags for live raw-base cache");
        }

        retained_analysis_overlay_.ensure_dimensions(max_capture_width_, max_capture_height_,
                                                     "cudaMallocPitch for retained analysis overlay");
        retained_analysis_stream_.create_with_highest_priority(
            "cudaStreamCreateWithPriority for retained analysis overlay");
        retained_analysis_ready_event_.create(cudaEventDisableTiming,
                                              "cudaEventCreateWithFlags for retained analysis overlay");

        readback_stream_.create_with_highest_priority("cudaStreamCreateWithPriority for compositor readback");
        readback_ready_event_.create(cudaEventDisableTiming,
                                     "cudaEventCreateWithFlags for compositor readback");
    } catch (...) {
        destroy_resources();
        throw;
    }
}

void LiveCompositor::destroy_resources() noexcept {
    (void)cudaSetDevice(cuda_device_index_);
    current_cuda_device_index_ = -1;
    readback_host_buffer_.reset();
    readback_ready_event_.reset();
    readback_stream_.reset();
    retained_analysis_ready_event_.reset();
    retained_analysis_stream_.reset();
    retained_analysis_overlay_.reset();
    retained_analysis_has_content_ = false;

    for (RawBaseCacheSlot& slot : raw_base_cache_slots_) {
        slot.occupied = false;
    }
    raw_base_cache_slots_.clear();
    output_slots_.clear();
}

void LiveCompositor::compositor_thread_main() {
    Status status;
    status.running = true;

    try {
        ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live compositor thread");
        current_cuda_device_index_ = cuda_device_index_;

        while (!stop_requested_.load(std::memory_order_acquire)) {
            OutputBundle base{};
            if (!fanout_.try_acquire_output(&base)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            CompositorSlot* slot = reserve_output_slot();
            if (slot == nullptr) {
                fanout_.release_output(base.slot_index);
                status.frames_dropped += 1U;
                std::shared_ptr<const Status> immutable = std::make_shared<Status>(status);
                std::atomic_store_explicit(&status_, std::move(immutable), std::memory_order_release);
                continue;
            }

            try {
                if (base.ready_event != nullptr) {
                    ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), base.ready_event, 0),
                                   "cudaStreamWaitEvent for live compositor base");
                }

                const std::size_t copy_width_bytes = static_cast<std::size_t>(base.dims.width) * 3U;
                ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(slot->device_buffer.data()),
                                                 slot->device_buffer.pitch_bytes(),
                                                 device_ptr_as_const_void(base.data),
                                                 base.dims.pitch_bytes,
                                                 copy_width_bytes,
                                                 base.dims.height,
                                                 cudaMemcpyDeviceToDevice,
                                                 slot->stream.get()),
                               "cudaMemcpy2DAsync for live compositor base");

                cache_raw_base(slot->stream.get(), base, copy_width_bytes, next_raw_base_cache_index_);
                next_raw_base_cache_index_ = (next_raw_base_cache_index_ + 1U) % raw_base_cache_slots_.size();

                bool analysis_active = false;
                auto composite_retained_analysis = [&](const char* context) {
                    if (retained_analysis_ready_event_.get() != nullptr) {
                        ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), retained_analysis_ready_event_.get(), 0),
                                       context);
                    }
                    composite_rgba_over_bgr_pitched(
                        slot->stream.get(),
                        slot->device_buffer.data(),
                        slot->device_buffer.pitch_bytes(),
                        retained_analysis_overlay_.data(),
                        retained_analysis_overlay_.pitch_bytes(),
                        static_cast<int>(base.dims.width),
                        static_cast<int>(base.dims.height),
                        context);
                    analysis_active = true;
                };
                if (analyzer_worker_ != nullptr) {
                    LiveAnalyzerWorker::AnalysisOverlayView analysis{};
                    const bool have_analysis = analyzer_worker_->try_acquire_latest_overlay(&analysis);
                    if (have_analysis) {
                        if (analysis.frame_id == base.frame_id) {
                            if (analysis.has_content) {
                                if (analysis.ready_event != nullptr) {
                                    ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), analysis.ready_event, 0),
                                                   "cudaStreamWaitEvent for live compositor analysis");
                                }
                                composite_rgba_over_bgr_pitched(
                                    slot->stream.get(),
                                    slot->device_buffer.data(),
                                    slot->device_buffer.pitch_bytes(),
                                    analysis.data,
                                    analysis.pitch_bytes,
                                    static_cast<int>(base.dims.width),
                                    static_cast<int>(base.dims.height),
                                    "launch_composite_rgba_over_bgr_pitched analysis");
                                analysis_active = true;
                            }
                            if (persistent_analysis_overlay()) {
                                retained_analysis_has_content_ = analysis.has_content;
                                if (analysis.has_content) {
                                    if (analysis.ready_event != nullptr) {
                                        ensure_cuda_ok(cudaStreamWaitEvent(retained_analysis_stream_.get(), analysis.ready_event, 0),
                                                       "cudaStreamWaitEvent for retained analysis overlay");
                                    }
                                    ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(retained_analysis_overlay_.data()),
                                                                     retained_analysis_overlay_.pitch_bytes(),
                                                                     device_ptr_as_const_void(analysis.data),
                                                                     analysis.pitch_bytes,
                                                                     static_cast<std::size_t>(analysis.width) * 4U,
                                                                     analysis.height,
                                                                     cudaMemcpyDeviceToDevice,
                                                                     retained_analysis_stream_.get()),
                                                   "cudaMemcpy2DAsync for retained analysis overlay");
                                    ensure_cuda_ok(cudaEventRecord(retained_analysis_ready_event_.get(), retained_analysis_stream_.get()),
                                                   "cudaEventRecord for retained analysis overlay");
                                }
                            }
                        } else if (persistent_analysis_overlay() && retained_analysis_has_content_) {
                            composite_retained_analysis(
                                "launch_composite_rgba_over_bgr_pitched retained analysis");
                        }
                        analyzer_worker_->release_overlay(analysis.slot_index);
                    } else if (persistent_analysis_overlay() && retained_analysis_has_content_) {
                        composite_retained_analysis("launch_composite_rgba_over_bgr_pitched retained analysis");
                    }
                }

                bool manual_active = false;
                if (manual_worker_ != nullptr) {
                    LiveManualOverlayWorker::OverlayView manual{};
                    if (manual_worker_->try_acquire_overlay(&manual)) {
                        if (manual.has_content) {
                            if (manual.ready_event != nullptr) {
                                ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), manual.ready_event, 0),
                                               "cudaStreamWaitEvent for live compositor manual overlay");
                            }
                            composite_rgba_over_bgr_pitched(
                                slot->stream.get(),
                                slot->device_buffer.data(),
                                slot->device_buffer.pitch_bytes(),
                                manual.data,
                                manual.pitch_bytes,
                                static_cast<int>(base.dims.width),
                                static_cast<int>(base.dims.height),
                                "launch_composite_rgba_over_bgr_pitched manual");
                            manual_active = true;
                        }
                        manual_worker_->release_overlay(manual.slot_index);
                    }
                }

                ensure_cuda_ok(cudaEventRecord(slot->ready_event.get(), slot->stream.get()),
                               "cudaEventRecord for live compositor output");
                slot->frame_id = base.frame_id;
                slot->region = base.region;
                slot->capture_ns = base.capture_ns;
                slot->ready_ns = now_ns();
                slot->short_frame = base.short_frame;
                slot->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
                latest_output_index_.store(static_cast<int>(slot->slot_index), std::memory_order_release);

                fanout_.release_output(base.slot_index);

                status.frames_composited += 1U;
                status.last_frame_id = base.frame_id;
                status.analysis_overlay_active = analysis_active;
                status.manual_overlay_active = manual_active;
                status.last_error.clear();
                std::shared_ptr<const Status> immutable = std::make_shared<Status>(status);
                std::atomic_store_explicit(&status_, std::move(immutable), std::memory_order_release);
            } catch (...) {
                slot->state.store(to_slot_state_value(SlotState::kFree), std::memory_order_release);
                fanout_.release_output(base.slot_index);
                throw;
            }
        }
    } catch (const std::exception& error) {
        publish_error(error.what());
        status.last_error = error.what();
        log_live_compositor_message("error", error.what());
    }

    status.running = false;
    std::shared_ptr<const Status> immutable = std::make_shared<Status>(status);
    std::atomic_store_explicit(&status_, std::move(immutable), std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

LiveCompositor::CompositorSlot* LiveCompositor::reserve_output_slot() {
    return reserve_writable_slot(
        output_slots_,
        latest_output_index_,
        [](CompositorSlot& slot) {
            slot.frame_id = {};
            slot.region = {};
        },
        [](CompositorSlot& slot) { return slot.ready_event.get(); },
        "cudaEventQuery for live compositor slot reuse");
}

bool LiveCompositor::try_acquire_output_slot(CompositorSlot& slot, OutputBundle* out) {
    return try_acquire_published_slot(slot, out, [](CompositorSlot& published) {
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

void LiveCompositor::cache_raw_base(cudaStream_t source_stream,
                                    const OutputBundle& base,
                                    const std::size_t copy_width_bytes,
                                    const std::size_t cache_index) {
    if (cache_index >= raw_base_cache_slots_.size()) {
        throw std::runtime_error("live compositor raw-base cache index out of range");
    }
    RawBaseCacheSlot& cache = raw_base_cache_slots_[cache_index];
    ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(cache.device_buffer.data()),
                                     cache.device_buffer.pitch_bytes(),
                                     device_ptr_as_const_void(base.data),
                                     base.dims.pitch_bytes,
                                     copy_width_bytes,
                                     base.dims.height,
                                     cudaMemcpyDeviceToDevice,
                                     source_stream),
                   "cudaMemcpy2DAsync for raw-base cache");
    ensure_cuda_ok(cudaEventRecord(cache.ready_event.get(), source_stream),
                   "cudaEventRecord for raw-base cache");
    cache.frame_id = base.frame_id;
    cache.region = base.region;
    cache.occupied = true;
}

void LiveCompositor::publish_error(std::string error_message) {
    store_error_message(&last_error_, std::move(error_message));
}

} // namespace mmltk::live
