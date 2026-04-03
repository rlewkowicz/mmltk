#include "mmltk/live/live_analyzer_worker.h"

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

void log_live_analyzer_message(const char* level, const std::string& message) {
    if (message.empty()) {
        return;
    }
    auto logger = mmltk::logging::logger("live.analyzer");
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

} // namespace

LiveAnalyzerWorker::LiveAnalyzerWorker(LiveFrameFanout& fanout,
                                       const int cuda_device_index,
                                       const std::uint32_t max_capture_width,
                                       const std::uint32_t max_capture_height,
                                       const std::uint32_t result_slot_count,
                                       const std::uint32_t overlay_slot_count)
    : fanout_(fanout),
      cuda_device_index_(cuda_device_index),
      max_capture_width_(std::max<std::uint32_t>(1U, max_capture_width)),
      max_capture_height_(std::max<std::uint32_t>(1U, max_capture_height)) {
    if (result_slot_count == 0U || overlay_slot_count == 0U) {
        throw std::runtime_error("live analyzer worker requires at least one result slot and one overlay slot");
    }
    result_slots_.reserve(result_slot_count);
    overlay_slots_.reserve(overlay_slot_count);
    allocate_resources();
}

LiveAnalyzerWorker::~LiveAnalyzerWorker() {
    try {
        stop();
    } catch (const std::exception& error) {
        log_live_analyzer_message("error", error.what());
    }
    destroy_resources();
}

void LiveAnalyzerWorker::start() {
    if (running()) {
        throw std::runtime_error("live analyzer worker is already running");
    }
    if (thread_.joinable()) {
        throw std::runtime_error("live analyzer worker must be stopped before restart");
    }
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&LiveAnalyzerWorker::worker_thread_main, this);
}

void LiveAnalyzerWorker::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void LiveAnalyzerWorker::set_analyzer(std::unique_ptr<FrameAnalyzer> analyzer) {
    std::shared_ptr<FrameAnalyzer> shared = std::move(analyzer);
    std::atomic_store_explicit(&analyzer_, std::move(shared), std::memory_order_release);
}

bool LiveAnalyzerWorker::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool LiveAnalyzerWorker::try_acquire_latest_result(AnalyzerResultView* out) {
    if (out == nullptr) {
        throw std::runtime_error("live analyzer worker requires a result output");
    }
    *out = {};
    return try_acquire_latest_published_slot(result_slots_, latest_result_index_, out, [](ResultSlot& slot) {
        return AnalyzerResultView{slot.slot_index, &slot.result};
    });
}

void LiveAnalyzerWorker::release_result(const std::uint32_t slot_index) {
    if (slot_index >= result_slots_.size()) {
        throw std::runtime_error("live analyzer worker result release index out of range");
    }
    ResultSlot& slot = *result_slots_[slot_index];
    release_acquired_slot(slot, "live analyzer worker result slot release called for a slot that is not acquired");
}

bool LiveAnalyzerWorker::try_acquire_latest_overlay(AnalysisOverlayView* out) {
    if (out == nullptr) {
        throw std::runtime_error("live analyzer worker requires an overlay output");
    }
    *out = {};
    return try_acquire_latest_published_slot(overlay_slots_, latest_overlay_index_, out, [](OverlaySlot& slot) {
        return AnalysisOverlayView{
            slot.slot_index,
            slot.frame_id,
            slot.device_buffer.data(),
            slot.device_buffer.pitch_bytes(),
            slot.device_buffer.width(),
            slot.device_buffer.height(),
            slot.ready_event.get(),
            slot.has_content,
        };
    });
}

void LiveAnalyzerWorker::release_overlay(const std::uint32_t slot_index) {
    if (slot_index >= overlay_slots_.size()) {
        throw std::runtime_error("live analyzer worker overlay release index out of range");
    }
    OverlaySlot& slot = *overlay_slots_[slot_index];
    release_acquired_slot(slot, "live analyzer worker overlay slot release called for a slot that is not acquired");
}

LiveAnalyzerWorker::Status LiveAnalyzerWorker::snapshot_status() const {
    const std::shared_ptr<const Status> current =
        std::atomic_load_explicit(&status_, std::memory_order_acquire);
    return current ? *current : Status{};
}

void LiveAnalyzerWorker::allocate_resources() {
    ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live analyzer resource allocation");

    try {
        for (std::uint32_t slot_index = 0; slot_index < result_slots_.capacity(); ++slot_index) {
            auto slot = std::make_unique<ResultSlot>();
            slot->slot_index = slot_index;
            result_slots_.push_back(std::move(slot));
        }

        for (std::uint32_t slot_index = 0; slot_index < overlay_slots_.capacity(); ++slot_index) {
            auto slot = std::make_unique<OverlaySlot>();
            slot->slot_index = slot_index;
            slot->device_buffer.ensure_dimensions(max_capture_width_, max_capture_height_,
                                                  "cudaMallocPitch for live analyzer overlay");
            slot->stream.create_with_highest_priority("cudaStreamCreateWithPriority for live analyzer overlay");
            slot->ready_event.create(cudaEventDisableTiming,
                                     "cudaEventCreateWithFlags for live analyzer overlay");
            overlay_slots_.push_back(std::move(slot));
        }
    } catch (...) {
        destroy_resources();
        throw;
    }
}

void LiveAnalyzerWorker::destroy_resources() noexcept {
    (void)cudaSetDevice(cuda_device_index_);
    overlay_slots_.clear();
    result_slots_.clear();
}

void LiveAnalyzerWorker::worker_thread_main() {
    Status status;
    status.running = true;

    try {
        ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live analyzer thread");
        while (!stop_requested_.load(std::memory_order_acquire)) {
            DetectBundle bundle{};
            if (!fanout_.try_acquire_detect(&bundle)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            const std::shared_ptr<FrameAnalyzer> analyzer =
                std::atomic_load_explicit(&analyzer_, std::memory_order_acquire);
            status.analyzer_attached = static_cast<bool>(analyzer);
            status.model_hot = static_cast<bool>(analyzer);
            status.backend_name = analyzer ? analyzer->backend_name() : std::string{};
            if (!analyzer) {
                fanout_.release_detect(bundle.slot_index);
                status.frames_skipped += 1U;
                status.last_error.clear();
                std::shared_ptr<const Status> immutable = std::make_shared<Status>(status);
                std::atomic_store_explicit(&status_, std::move(immutable), std::memory_order_release);
                continue;
            }

            const auto started_at = Clock::now();
            AnalyzerResult result;
            try {
                result = analyzer->analyze(bundle);
            } catch (const std::exception& error) {
                fanout_.release_detect(bundle.slot_index);
                status.last_error = error.what();
                std::shared_ptr<const Status> immutable = std::make_shared<Status>(status);
                std::atomic_store_explicit(&status_, std::move(immutable), std::memory_order_release);
                continue;
            }

            ResultSlot* result_slot = reserve_result_slot();
            OverlaySlot* overlay_slot = reserve_overlay_slot();
            bool overlay_has_content = false;
            if (overlay_slot != nullptr) {
                try {
                    ensure_cuda_ok(cudaMemset2DAsync(device_ptr_as_void(overlay_slot->device_buffer.data()),
                                                     overlay_slot->device_buffer.pitch_bytes(),
                                                     0,
                                                     static_cast<std::size_t>(overlay_slot->device_buffer.width()) * 4U,
                                                     overlay_slot->device_buffer.height(),
                                                     overlay_slot->stream.get()),
                                   "cudaMemset2DAsync for live analyzer overlay clear");
                    if (result.ready_event != nullptr) {
                        ensure_cuda_ok(cudaStreamWaitEvent(overlay_slot->stream.get(), result.ready_event, 0),
                                       "cudaStreamWaitEvent for live analyzer overlay");
                    }

                    for (const AnalyzerSplitResult& split : result.splits) {
                        if (!split.boxes_xyxy.defined() ||
                            !split.labels_zero_based.defined() ||
                            !split.colors_rgb.defined() ||
                            split.boxes_xyxy.numel() == 0 ||
                            split.labels_zero_based.numel() == 0) {
                            continue;
                        }
                        std::uint8_t* overlay_ptr =
                            device_ptr_as_bytes(overlay_slot->device_buffer.data()) +
                            static_cast<std::size_t>(split.source_region.y) * overlay_slot->device_buffer.pitch_bytes() +
                            static_cast<std::size_t>(split.source_region.x) * 4U;
                        const bool* masks_ptr = nullptr;
                        if (split.masks.defined() &&
                            split.masks.dim() == 3 &&
                            split.masks.size(0) == split.labels_zero_based.size(0) &&
                            split.masks.size(1) == static_cast<int64_t>(split.source_region.height) &&
                            split.masks.size(2) == static_cast<int64_t>(split.source_region.width)) {
                            masks_ptr = split.masks.data_ptr<bool>();
                        }
                        mmltk::rfdetr::launch_draw_analysis_overlay_rgba_pitched(
                            overlay_ptr,
                            overlay_slot->device_buffer.pitch_bytes(),
                            static_cast<int>(split.source_region.width),
                            static_cast<int>(split.source_region.height),
                            masks_ptr,
                            split.boxes_xyxy.data_ptr<float>(),
                            split.colors_rgb.data_ptr<std::uint8_t>(),
                            split.labels_zero_based.data_ptr<int>(),
                            static_cast<int>(split.labels_zero_based.size(0)),
                            static_cast<std::uint8_t>(115U),
                            2,
                            overlay_slot->stream.get());
                        ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_analysis_overlay_rgba_pitched");
                        overlay_has_content = true;
                    }

                    ensure_cuda_ok(cudaEventRecord(overlay_slot->ready_event.get(), overlay_slot->stream.get()),
                                   "cudaEventRecord for live analyzer overlay");
                    overlay_slot->frame_id = result.frame_id;
                    overlay_slot->has_content = overlay_has_content;
                    overlay_slot->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
                    latest_overlay_index_.store(static_cast<int>(overlay_slot->slot_index), std::memory_order_release);
                } catch (...) {
                    overlay_slot->has_content = false;
                    overlay_slot->state.store(to_slot_state_value(SlotState::kFree), std::memory_order_release);
                    throw;
                }
            }

            if (result_slot != nullptr) {
                result_slot->result = result;
                result_slot->retained_tensors.clear();
                for (const AnalyzerSplitResult& split : result_slot->result.splits) {
                    if (split.boxes_xyxy.defined()) {
                        result_slot->retained_tensors.push_back(split.boxes_xyxy);
                    }
                    if (split.labels_zero_based.defined()) {
                        result_slot->retained_tensors.push_back(split.labels_zero_based);
                    }
                    if (split.scores.defined()) {
                        result_slot->retained_tensors.push_back(split.scores);
                    }
                    if (split.colors_rgb.defined()) {
                        result_slot->retained_tensors.push_back(split.colors_rgb);
                    }
                    if (split.masks.defined()) {
                        result_slot->retained_tensors.push_back(split.masks);
                    }
                }
                result_slot->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
                latest_result_index_.store(static_cast<int>(result_slot->slot_index), std::memory_order_release);
            }

            fanout_.release_detect(bundle.slot_index);

            status.frames_analyzed += 1U;
            status.last_completed_frame_id = result.frame_id;
            status.last_latency_ms =
                std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(Clock::now() - started_at).count();
            status.last_error.clear();
            std::shared_ptr<const Status> immutable = std::make_shared<Status>(status);
            std::atomic_store_explicit(&status_, std::move(immutable), std::memory_order_release);
        }
    } catch (const std::exception& error) {
        publish_error(error.what());
        status.last_error = error.what();
        log_live_analyzer_message("error", error.what());
    }

    status.running = false;
    std::shared_ptr<const Status> immutable = std::make_shared<Status>(status);
    std::atomic_store_explicit(&status_, std::move(immutable), std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

LiveAnalyzerWorker::ResultSlot* LiveAnalyzerWorker::reserve_result_slot() {
    return reserve_writable_slot(
        result_slots_,
        latest_result_index_,
        [](ResultSlot& slot) {
            slot.retained_tensors.clear();
            slot.result = {};
        },
        [](ResultSlot& slot) { return slot.result.ready_event; },
        "cudaEventQuery for live analyzer slot reuse");
}

LiveAnalyzerWorker::OverlaySlot* LiveAnalyzerWorker::reserve_overlay_slot() {
    return reserve_writable_slot(
        overlay_slots_,
        latest_overlay_index_,
        [](OverlaySlot& slot) {
            slot.frame_id = {};
            slot.has_content = false;
        },
        [](OverlaySlot& slot) { return slot.ready_event.get(); },
        "cudaEventQuery for live analyzer slot reuse");
}

bool LiveAnalyzerWorker::try_acquire_result_slot(ResultSlot& slot, AnalyzerResultView* out) {
    return try_acquire_published_slot(slot, out, [](ResultSlot& published) {
        return AnalyzerResultView{published.slot_index, &published.result};
    });
}

bool LiveAnalyzerWorker::try_acquire_overlay_slot(OverlaySlot& slot, AnalysisOverlayView* out) {
    return try_acquire_published_slot(slot, out, [](OverlaySlot& published) {
        return AnalysisOverlayView{
            published.slot_index,
            published.frame_id,
            published.device_buffer.data(),
            published.device_buffer.pitch_bytes(),
            published.device_buffer.width(),
            published.device_buffer.height(),
            published.ready_event.get(),
            published.has_content,
        };
    });
}

void LiveAnalyzerWorker::publish_error(std::string error_message) {
    store_error_message(&last_error_, std::move(error_message));
}

} // namespace mmltk::live
