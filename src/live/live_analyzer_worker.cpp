#include "mmltk/live/live_analyzer_worker.h"

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
    if (running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("live analyzer worker is already running");
    }
    if (thread_.joinable()) {
        throw std::runtime_error("live analyzer worker must be stopped before restart");
    }

    publish_error({});
    worker_runtime::publish_status(&status_, Status{});
    ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live analyzer start");
    worker_runtime::reset_slot_views_for_restart(result_slots_,
                                                 latest_result_index_,
                                                 "cudaEventSynchronize for live analyzer result slot restart");
    worker_runtime::reset_slot_views_for_restart(overlay_slots_,
                                                 latest_overlay_index_,
                                                 "cudaEventSynchronize for live analyzer overlay slot restart");
    worker_runtime::start_worker_thread(
        thread_,
        stop_requested_,
        running_,
        [this]() { worker_thread_main(); },
        "live analyzer worker is already running",
        "live analyzer worker must be stopped before restart");
}

void LiveAnalyzerWorker::stop() {
    worker_runtime::stop_worker_thread(thread_, stop_requested_, running_);
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
    if (!running()) {
        return false;
    }
    return worker_runtime::try_acquire_latest_published_slot_view(result_slots_,
                                                                   latest_result_index_,
                                                                   out);
}

void LiveAnalyzerWorker::release_result(const std::uint32_t slot_index) {
    worker_runtime::release_slot_by_index(
        result_slots_,
        slot_index,
        "live analyzer worker result release index out of range",
        "live analyzer worker result slot release called for a slot that is not acquired");
}

bool LiveAnalyzerWorker::try_acquire_latest_overlay(AnalysisOverlayView* out) {
    if (out == nullptr) {
        throw std::runtime_error("live analyzer worker requires an overlay output");
    }
    *out = {};
    if (!running()) {
        return false;
    }
    return worker_runtime::try_acquire_latest_published_slot_view(overlay_slots_,
                                                                   latest_overlay_index_,
                                                                   out);
}

void LiveAnalyzerWorker::release_overlay(const std::uint32_t slot_index) {
    worker_runtime::release_slot_by_index(
        overlay_slots_,
        slot_index,
        "live analyzer worker overlay release index out of range",
        "live analyzer worker overlay slot release called for a slot that is not acquired");
}

LiveAnalyzerWorker::Status LiveAnalyzerWorker::snapshot_status() const {
    return worker_runtime::snapshot_status(status_);
}

void LiveAnalyzerWorker::allocate_resources() {
    ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice for live analyzer resource allocation");

    try {
        worker_runtime::allocate_unique_slots(result_slots_, static_cast<std::uint32_t>(result_slots_.capacity()),
                                              [](ResultSlot& slot, std::uint32_t slot_index) {
                                                  slot.slot_index = slot_index;
                                              });
        worker_runtime::allocate_pitched_device_slots(
            overlay_slots_,
            static_cast<std::uint32_t>(overlay_slots_.capacity()),
            max_capture_width_,
            max_capture_height_,
            "cudaMallocPitch for live analyzer overlay",
            "cudaStreamCreateWithPriority for live analyzer overlay",
            "cudaEventCreateWithFlags for live analyzer overlay");
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
    worker_runtime::run_worker_thread_main(
        running_,
        stop_requested_,
        status_,
        cuda_device_index_,
        "cudaSetDevice for live analyzer thread",
        "live.analyzer",
        [this](Status& status) {
            DetectBundle bundle{};
            if (!fanout_.try_acquire_detect(&bundle)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return;
            }
            auto detect_release = worker_runtime::make_scoped_rollback([this, slot_index = bundle.slot_index]() noexcept {
                fanout_.release_detect(slot_index);
            });

            const std::shared_ptr<FrameAnalyzer> analyzer =
                std::atomic_load_explicit(&analyzer_, std::memory_order_acquire);
            status.analyzer_attached = static_cast<bool>(analyzer);
            status.model_hot = static_cast<bool>(analyzer);
            status.backend_name = analyzer ? analyzer->backend_name() : std::string{};
            if (!analyzer) {
                detect_release.run_and_dismiss();
                status.frames_skipped += 1U;
                status.last_error.clear();
                worker_runtime::publish_status(&status_, status);
                return;
            }

            const auto started_at = Clock::now();
            AnalyzerResult result;
            try {
                result = analyzer->analyze(bundle);
            } catch (const std::exception& error) {
                detect_release.run_and_dismiss();
                status.last_error = error.what();
                worker_runtime::publish_status(&status_, status);
                return;
            }

            ResultSlot* result_slot = reserve_result_slot();
            OverlaySlot* overlay_slot = reserve_overlay_slot();
            auto result_slot_release = worker_runtime::make_scoped_rollback([result_slot]() noexcept {
                if (result_slot != nullptr) {
                    worker_runtime::release_writing_slot(*result_slot);
                }
            });
            bool overlay_has_content = false;
            if (overlay_slot != nullptr) {
                auto overlay_slot_release = worker_runtime::make_scoped_rollback([overlay_slot]() noexcept {
                    if (overlay_slot != nullptr) {
                        worker_runtime::release_writing_slot(*overlay_slot);
                    }
                });
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
                    worker_runtime::publish_latest_slot(*overlay_slot, latest_overlay_index_);
                    overlay_slot_release.dismiss();
                } catch (...) {
                    overlay_slot->has_content = false;
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
                worker_runtime::publish_latest_slot(*result_slot, latest_result_index_);
                result_slot_release.dismiss();
            }

            detect_release.run_and_dismiss();

            status.frames_analyzed += 1U;
            status.last_completed_frame_id = result.frame_id;
            status.last_latency_ms =
                std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(Clock::now() - started_at).count();
            status.last_error.clear();
            worker_runtime::publish_status(&status_, status);
        },
        [this](const char* error_message) {
            publish_error(error_message);
        });
}

LiveAnalyzerWorker::ResultSlot* LiveAnalyzerWorker::reserve_result_slot() {
    return worker_runtime::reserve_writable_slot_view(result_slots_,
                                                      latest_result_index_,
                                                      "cudaEventQuery for live analyzer slot reuse");
}

LiveAnalyzerWorker::OverlaySlot* LiveAnalyzerWorker::reserve_overlay_slot() {
    return worker_runtime::reserve_writable_slot_view(overlay_slots_,
                                                      latest_overlay_index_,
                                                      "cudaEventQuery for live analyzer slot reuse");
}

void LiveAnalyzerWorker::publish_error(std::string error_message) {
    worker_runtime::publish_error(&last_error_, std::move(error_message));
}

} // namespace mmltk::live
