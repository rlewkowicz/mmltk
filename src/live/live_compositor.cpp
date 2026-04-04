#include "mmltk/live/live_compositor.h"

#include "live/live_helpers.h"
#include "mmltk/live/live_worker_runtime.h"
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

struct DeviceBufferView {
    CUdeviceptr data;
    std::size_t pitch_bytes;
};

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
                                     const DeviceBufferView& dst,
                                     CUdeviceptr src,
                                     std::size_t src_pitch_bytes,
                                     int width,
                                     int height,
                                     const char* context) {
    mmltk::rfdetr::launch_composite_rgba_over_bgr_pitched(device_ptr_as_bytes(dst.data),
                                                          dst.pitch_bytes,
                                                          device_ptr_as_bytes(src),
                                                          src_pitch_bytes,
                                                          width,
                                                          height,
                                                          stream);
    ensure_cuda_ok(cudaPeekAtLastError(), context);
}

template <typename OverlayWorker>
void release_overlay_silently(OverlayWorker* worker,
                              const std::uint32_t slot_index,
                              const char* worker_name) noexcept {
    if (worker == nullptr) {
        return;
    }
    try {
        worker->release_overlay(slot_index);
    } catch (const std::exception& error) {
        log_live_compositor_message(
            "error",
            std::string("mmltk live compositor release error: failed to release ") +
                worker_name + " overlay slot " + std::to_string(slot_index) + ": " + error.what());
    }
}

template <typename OverlayWorker, typename OverlayView, typename UseFn>
bool with_latest_overlay(OverlayWorker* worker,
                         const char* worker_name,
                         UseFn&& use_fn) {
    if (worker == nullptr) {
        return false;
    }

    OverlayView overlay{};
    if (!worker->try_acquire_latest_overlay(&overlay)) {
        return false;
    }

    auto overlay_release = worker_runtime::make_scoped_rollback(
        [worker, slot_index = overlay.slot_index, worker_name]() noexcept {
            release_overlay_silently(worker, slot_index, worker_name);
        });
    use_fn(overlay);
    return true;
}

} // namespace

struct LiveCompositor::RawBaseCacheState {
    struct Slot {
        BgrPitchedDeviceBuffer device_buffer;
        CudaEventHandle ready_event;
        LiveFrameId frame_id{};
        LiveCaptureRegion region{};
        bool occupied = false;
    };

    void initialize(const std::uint32_t max_capture_width,
                    const std::uint32_t max_capture_height) {
        std::lock_guard<std::mutex> lock(mutex);
        slots.resize(slot_count);
        worker_runtime::initialize_slot_array(slots, [&](Slot& slot, std::uint32_t) {
            worker_runtime::initialize_pitched_event_resource(
                slot,
                max_capture_width,
                max_capture_height,
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

    void store(cudaStream_t source_stream, const OutputBundle& base, const std::size_t copy_width_bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        if (slots.empty()) {
            throw std::runtime_error("live compositor raw-base cache index out of range");
        }
        Slot& cache = slots[next_index];
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

struct LiveCompositor::RetainedAnalysisState {
    void initialize(const std::uint32_t max_capture_width, const std::uint32_t max_capture_height) {
        worker_runtime::initialize_pitched_device_resource(
            *this,
            max_capture_width,
            max_capture_height,
            "cudaMallocPitch for retained analysis overlay",
            "cudaStreamCreateWithPriority for retained analysis overlay",
            "cudaEventCreateWithFlags for retained analysis overlay");
    }

    void reset() noexcept {
        worker_runtime::reset_pitched_device_resource(*this);
        has_content = false;
    }

    bool composite_into(cudaStream_t stream,
                        const DeviceBufferView& dst,
                        const OutputBundle& base,
                        const char* context) const {
        if (!has_content) {
            return false;
        }
        if (ready_event.get() != nullptr) {
            ensure_cuda_ok(cudaStreamWaitEvent(stream, ready_event.get(), 0), context);
        }
        composite_rgba_over_bgr_pitched(stream,
                                        dst,
                                        device_buffer.data(),
                                        device_buffer.pitch_bytes(),
                                        static_cast<int>(base.dims.width),
                                        static_cast<int>(base.dims.height),
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
        ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(device_buffer.data()),
                                         device_buffer.pitch_bytes(),
                                         device_ptr_as_const_void(analysis.data),
                                         analysis.pitch_bytes,
                                         static_cast<std::size_t>(analysis.width) * 4U,
                                         analysis.height,
                                         cudaMemcpyDeviceToDevice,
                                         stream.get()),
                       "cudaMemcpy2DAsync for retained analysis overlay");
        ensure_cuda_ok(cudaEventRecord(ready_event.get(), stream.get()),
                       "cudaEventRecord for retained analysis overlay");
    }

    RgbaPitchedDeviceBuffer device_buffer;
    CudaEventHandle ready_event;
    CudaStreamHandle stream;
    bool has_content = false;
};

struct LiveCompositor::ReadbackState {
    void initialize() {
        worker_runtime::initialize_stream_event_resource(
            *this,
            "cudaStreamCreateWithPriority for compositor readback",
            "cudaEventCreateWithFlags for compositor readback");
    }

    void reset() noexcept {
        host_buffer.reset();
        worker_runtime::reset_stream_event_resource(*this);
    }

    bool copy_slot(const RawBaseCacheState::Slot& slot,
                   const int cuda_device_index,
                   std::vector<std::uint8_t>* pixels_bgr,
                   std::uint32_t* width,
                   std::uint32_t* height,
                   LiveCaptureRegion* region,
                   std::string* error_message) {
        std::lock_guard<std::mutex> lock(mutex);
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for compositor readback");

        const std::size_t row_bytes = static_cast<std::size_t>(slot.region.width) * 3U;
        const std::size_t required_bytes = row_bytes * static_cast<std::size_t>(slot.region.height);
        host_buffer.ensure_capacity(required_bytes, "cudaHostAlloc for compositor readback");
        if (slot.ready_event.get() != nullptr) {
            ensure_cuda_ok(cudaStreamWaitEvent(stream.get(), slot.ready_event.get(), 0),
                           "cudaStreamWaitEvent for compositor readback");
        }
        ensure_cuda_ok(cudaMemcpy2DAsync(host_buffer.data(),
                                         row_bytes,
                                         device_ptr_as_const_void(slot.device_buffer.data()),
                                         slot.device_buffer.pitch_bytes(),
                                         row_bytes,
                                         slot.region.height,
                                         cudaMemcpyDeviceToHost,
                                         stream.get()),
                       "cudaMemcpy2DAsync for compositor readback");
        ensure_cuda_ok(cudaEventRecord(ready_event.get(), stream.get()),
                       "cudaEventRecord for compositor readback");
        ensure_cuda_ok(cudaEventSynchronize(ready_event.get()),
                       "cudaEventSynchronize for compositor readback");

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
      max_capture_height_(std::max<std::uint32_t>(1U, max_capture_height)),
      raw_base_cache_(std::make_unique<RawBaseCacheState>()),
      retained_analysis_(std::make_unique<RetainedAnalysisState>()),
      readback_(std::make_unique<ReadbackState>()) {
    if (output_slot_count == 0U || raw_base_cache_slot_count == 0U) {
        throw std::runtime_error("live compositor requires output slots and raw-base cache slots");
    }
    output_slots_.reserve(output_slot_count);
    raw_base_cache_->slot_count = raw_base_cache_slot_count;
    allocate_resources();
}

LiveCompositor::~LiveCompositor() {
    try {
        stop();
    } catch (const std::exception& error) {
        log_live_compositor_message("error", error.what());
    }
    reset_runtime_state();
}

void LiveCompositor::start() {
    if (running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("live compositor is already running");
    }
    if (thread_.joinable()) {
        throw std::runtime_error("live compositor must be stopped before restart");
    }

    publish_error({});
    worker_runtime::publish_status(&status_, Status{});
    if (output_slots_.empty()) {
        allocate_resources();
    }
    worker_runtime::start_worker_thread(
        thread_,
        stop_requested_,
        running_,
        [this]() { compositor_thread_main(); },
        "live compositor is already running",
        "live compositor must be stopped before restart");
}

void LiveCompositor::stop() {
    worker_runtime::stop_worker_thread(thread_, stop_requested_, running_);
    reset_runtime_state();
}

bool LiveCompositor::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool LiveCompositor::try_acquire_latest_output(OutputBundle* out) {
    if (out == nullptr) {
        throw std::runtime_error("live compositor requires an output bundle output");
    }
    *out = {};
    if (!running()) {
        return false;
    }
    return worker_runtime::try_acquire_latest_published_slot_view(output_slots_,
                                                                   latest_output_index_,
                                                                   out);
}

void LiveCompositor::release_output(const std::uint32_t slot_index) {
    worker_runtime::release_slot_by_index(
        output_slots_,
        slot_index,
        "live compositor output release index out of range",
        "live compositor output slot release called for a slot that is not acquired");
}

void LiveCompositor::set_persistent_analysis_overlay(const bool enabled) {
    persistent_analysis_overlay_.store(enabled, std::memory_order_release);
    if (!enabled) {
        retained_analysis_->has_content = false;
    }
}

bool LiveCompositor::persistent_analysis_overlay() const noexcept {
    return persistent_analysis_overlay_.load(std::memory_order_acquire);
}

LiveCompositor::Status LiveCompositor::snapshot_status() const {
    return worker_runtime::snapshot_status(status_);
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
    return raw_base_cache_->with_slot(
        frame_id,
        [&](const RawBaseCacheState::Slot& slot) {
            return readback_->copy_slot(slot, cuda_device_index_, pixels_bgr, width, height, region, error_message);
        },
        error_message);
}

void LiveCompositor::allocate_resources() {
    ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live compositor resource allocation");

    try {
        worker_runtime::allocate_pitched_device_slots(
            output_slots_,
            static_cast<std::uint32_t>(output_slots_.capacity()),
            max_capture_width_,
            max_capture_height_,
            "cudaMallocPitch for live compositor output",
            "cudaStreamCreateWithPriority for live compositor output",
            "cudaEventCreateWithFlags for live compositor output");

        raw_base_cache_->initialize(max_capture_width_, max_capture_height_);
        retained_analysis_->initialize(max_capture_width_, max_capture_height_);
        readback_->initialize();
    } catch (...) {
        reset_runtime_state();
        throw;
    }
}

void LiveCompositor::reset_runtime_state() noexcept {
    (void)cudaSetDevice(cuda_device_index_);
    raw_base_cache_->reset();
    retained_analysis_->reset();
    readback_->reset();
    output_slots_.clear();
    latest_output_index_.store(-1, std::memory_order_release);
}

void LiveCompositor::compositor_thread_main() {
    worker_runtime::run_worker_thread_main(
        running_,
        stop_requested_,
        status_,
        cuda_device_index_,
        "cudaSetDevice for live compositor thread",
        "live.compositor",
        [this](Status& status) {
            OutputBundle base{};
            if (!fanout_.try_acquire_output(&base)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return;
            }
            auto base_release = worker_runtime::make_scoped_rollback([this, slot_index = base.slot_index]() noexcept {
                fanout_.release_output(slot_index);
            });

            CompositorSlot* slot = reserve_output_slot();
            auto slot_release = worker_runtime::make_scoped_rollback([slot]() noexcept {
                if (slot != nullptr) {
                    worker_runtime::release_writing_slot(*slot);
                }
            });
            if (slot == nullptr) {
                base_release.run_and_dismiss();
                status.frames_dropped += 1U;
                worker_runtime::publish_status(&status_, status);
                return;
            }

            if (base.ready_event != nullptr) {
                ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), base.ready_event, 0),
                               "cudaStreamWaitEvent for live compositor base");
            }

            const DeviceBufferView dst{slot->device_buffer.data(), slot->device_buffer.pitch_bytes()};
            const int frame_w = static_cast<int>(base.dims.width);
            const int frame_h = static_cast<int>(base.dims.height);

            const std::size_t copy_width_bytes = static_cast<std::size_t>(base.dims.width) * 3U;
            ensure_cuda_ok(cudaMemcpy2DAsync(device_ptr_as_void(dst.data),
                                             dst.pitch_bytes,
                                             device_ptr_as_const_void(base.data),
                                             base.dims.pitch_bytes,
                                             copy_width_bytes,
                                             base.dims.height,
                                             cudaMemcpyDeviceToDevice,
                                             slot->stream.get()),
                           "cudaMemcpy2DAsync for live compositor base");

            cache_raw_base(slot->stream.get(), base, copy_width_bytes);

            bool analysis_active = false;
            auto composite_retained_analysis = [&](const char* context) {
                analysis_active = retained_analysis_->composite_into(slot->stream.get(), dst, base, context) || analysis_active;
            };
            if (!with_latest_overlay<LiveAnalyzerWorker, LiveAnalyzerWorker::AnalysisOverlayView>(
                    analyzer_worker_,
                    "analysis",
                    [&](const LiveAnalyzerWorker::AnalysisOverlayView& analysis) {
                    if (analysis.frame_id == base.frame_id) {
                        if (analysis.has_content) {
                            if (analysis.ready_event != nullptr) {
                                ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), analysis.ready_event, 0),
                                               "cudaStreamWaitEvent for live compositor analysis");
                            }
                            composite_rgba_over_bgr_pitched(
                                slot->stream.get(),
                                dst,
                                analysis.data,
                                analysis.pitch_bytes,
                                frame_w,
                                frame_h,
                                "launch_composite_rgba_over_bgr_pitched analysis");
                            analysis_active = true;
                        }
                        if (persistent_analysis_overlay()) {
                            retained_analysis_->update_from_analysis(analysis);
                        }
                    } else if (persistent_analysis_overlay() && retained_analysis_->has_content) {
                        composite_retained_analysis("launch_composite_rgba_over_bgr_pitched retained analysis");
                    }
                })) {
                if (persistent_analysis_overlay() && retained_analysis_->has_content) {
                    composite_retained_analysis("launch_composite_rgba_over_bgr_pitched retained analysis");
                }
            }

            bool manual_active = false;
            (void)with_latest_overlay<LiveManualOverlayWorker, LiveManualOverlayWorker::OverlayView>(
                manual_worker_,
                "manual",
                [&](const LiveManualOverlayWorker::OverlayView& manual) {
                    if (manual.has_content) {
                        if (manual.ready_event != nullptr) {
                            ensure_cuda_ok(cudaStreamWaitEvent(slot->stream.get(), manual.ready_event, 0),
                                           "cudaStreamWaitEvent for live compositor manual overlay");
                        }
                        composite_rgba_over_bgr_pitched(
                            slot->stream.get(),
                            dst,
                            manual.data,
                            manual.pitch_bytes,
                            frame_w,
                            frame_h,
                            "launch_composite_rgba_over_bgr_pitched manual");
                        manual_active = true;
                    }
                });

            ensure_cuda_ok(cudaEventRecord(slot->ready_event.get(), slot->stream.get()),
                           "cudaEventRecord for live compositor output");
            slot->frame_id = base.frame_id;
            slot->region = base.region;
            slot->capture_ns = base.capture_ns;
            slot->ready_ns = now_ns();
            slot->short_frame = base.short_frame;
            worker_runtime::publish_latest_slot(*slot, latest_output_index_);
            slot_release.dismiss();

            base_release.run_and_dismiss();

            status.frames_composited += 1U;
            status.last_frame_id = base.frame_id;
            status.analysis_overlay_active = analysis_active;
            status.manual_overlay_active = manual_active;
            status.last_error.clear();
            worker_runtime::publish_status(&status_, status);
        },
        [this](const char* error_message) {
            publish_error(error_message);
        });
}

LiveCompositor::CompositorSlot* LiveCompositor::reserve_output_slot() {
    return worker_runtime::reserve_writable_slot_view(output_slots_,
                                                      latest_output_index_,
                                                      "cudaEventQuery for live compositor slot reuse");
}

void LiveCompositor::cache_raw_base(cudaStream_t source_stream,
                                    const OutputBundle& base,
                                    const std::size_t copy_width_bytes) {
    raw_base_cache_->store(source_stream, base, copy_width_bytes);
}

void LiveCompositor::publish_error(std::string error_message) {
    worker_runtime::publish_error(&last_error_, std::move(error_message));
}

} // namespace mmltk::live
