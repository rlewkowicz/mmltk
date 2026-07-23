#include "mmltk/live/live_analyzer_worker.h"

#include "live/live_helpers.h"
#include "live/live_worker_logging.h"
#include "mmltk/live/frame_analyzer.h"
#include "mmltk/live/live_frame_fanout.h"
#include "mmltk/live/live_worker_runtime.h"
#include "mmltk/rfdetr/draw_cuda.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mmltk::live {

namespace {

using Clock = std::chrono::steady_clock;

}

struct LiveAnalyzerWorker::Impl {
    struct ResultSlot {
        std::uint32_t slot_index = 0;
        AnalyzerResult result{};
        std::vector<torch::Tensor> retained_tensors;
        std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};

        [[nodiscard]] LiveAnalyzerWorker::AnalyzerResultView make_view() const noexcept {
            return LiveAnalyzerWorker::AnalyzerResultView{slot_index, &result};
        }

        void reset_for_reuse() noexcept {
            retained_tensors.clear();
            result = {};
        }

        [[nodiscard]] cudaEvent_t ready_event_handle() const noexcept {
            return result.ready_event;
        }
    };

    struct OverlaySlot {
        std::uint32_t slot_index = 0;
        RgbaPitchedDeviceBuffer device_buffer;
        CudaStreamHandle stream;
        CudaEventHandle ready_event;
        std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
        LiveFrameId frame_id{};
        bool has_content = false;

        [[nodiscard]] LiveAnalyzerWorker::AnalysisOverlayView make_view() const noexcept {
            return LiveAnalyzerWorker::AnalysisOverlayView{
                slot_index,
                frame_id,
                device_buffer.data(),
                device_buffer.pitch_bytes(),
                device_buffer.width(),
                device_buffer.height(),
                ready_event.get(),
                stream.get(),
                has_content,
            };
        }

        void reset_for_reuse() noexcept {
            frame_id = {};
            has_content = false;
        }

        [[nodiscard]] cudaEvent_t ready_event_handle() const noexcept {
            return ready_event.get();
        }
    };

    Impl(LiveFrameFanout& fanout_in, const int cuda_device_index_in, const std::uint32_t max_capture_width_in,
         const std::uint32_t max_capture_height_in, const std::uint32_t result_slot_count,
         const std::uint32_t overlay_slot_count)
        : fanout(fanout_in),
          cuda_device_index(cuda_device_index_in),
          max_capture_width(std::max<std::uint32_t>(1U, max_capture_width_in)),
          max_capture_height(std::max<std::uint32_t>(1U, max_capture_height_in)) {
        if (result_slot_count == 0U || overlay_slot_count == 0U) {
            throw std::runtime_error("live analyzer worker requires at least one result slot and one overlay slot");
        }
        result_slots.reserve(result_slot_count);
        overlay_slots.reserve(overlay_slot_count);
        allocate_resources();
    }

    ~Impl() {
        try {
            stop();
        } catch (const std::exception& error) {
            log_live_worker_message("live.analyzer", "error", error.what());
        }
        destroy_resources();
    }

    void start() {
        if (running.load(std::memory_order_acquire)) {
            throw std::runtime_error("live analyzer worker is already running");
        }
        if (thread.joinable()) {
            throw std::runtime_error("live analyzer worker must be stopped before restart");
        }

        publish_error({});
        worker_runtime::publish_status(&status, Status{});
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for live analyzer start");
        worker_runtime::reset_slot_views_for_restart(result_slots, latest_result_index,
                                                     "cudaEventSynchronize for live analyzer result slot restart");
        worker_runtime::reset_slot_views_for_restart(overlay_slots, latest_overlay_index,
                                                     "cudaEventSynchronize for live analyzer overlay slot restart");
        worker_runtime::start_worker_thread(
            thread, stop_requested, running, [this]() { worker_thread_main(); },
            "live analyzer worker is already running", "live analyzer worker must be stopped before restart");
    }

    void stop() {
        stop_requested.store(true, std::memory_order_release);
        fanout.notify_detect_waiters();
        if (thread.joinable()) {
            thread.join();
        }
        running.store(false, std::memory_order_release);
    }

    void set_analyzer(std::unique_ptr<FrameAnalyzer> analyzer_in) {
        std::shared_ptr<FrameAnalyzer> shared = std::move(analyzer_in);
        analyzer.store(std::move(shared), std::memory_order_release);
    }

    [[nodiscard]] bool running_state() const noexcept {
        return running.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool try_acquire_latest_result(LiveAnalyzerWorker::AnalyzerResultView* out) {
        return worker_runtime::try_acquire_latest_running_slot_view(result_slots, latest_result_index, running, out,
                                                                    "live analyzer worker requires a result output");
    }

    void release_result(const std::uint32_t slot_index) {
        worker_runtime::release_slot_by_index(
            result_slots, slot_index, "live analyzer worker result release index out of range",
            "live analyzer worker result slot release called for a slot that is not acquired");
    }

    [[nodiscard]] bool try_acquire_latest_overlay(LiveAnalyzerWorker::AnalysisOverlayView* out) {
        return worker_runtime::try_acquire_latest_running_slot_view(overlay_slots, latest_overlay_index, running, out,
                                                                    "live analyzer worker requires an overlay output");
    }

    void release_overlay(const std::uint32_t slot_index) {
        worker_runtime::release_slot_by_index(
            overlay_slots, slot_index, "live analyzer worker overlay release index out of range",
            "live analyzer worker overlay slot release called for a slot that is not acquired");
    }

    [[nodiscard]] Status snapshot_status() const {
        return worker_runtime::snapshot_status(status);
    }

    void allocate_resources() {
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for live analyzer resource allocation");

        try {
            worker_runtime::allocate_unique_slots(
                result_slots, static_cast<std::uint32_t>(result_slots.capacity()),
                [](ResultSlot& slot, const std::uint32_t slot_index) { slot.slot_index = slot_index; });
            worker_runtime::allocate_pitched_device_slots(
                overlay_slots, static_cast<std::uint32_t>(overlay_slots.capacity()), max_capture_width,
                max_capture_height, "cudaMallocPitch for live analyzer overlay",
                "cudaStreamCreateWithPriority for live analyzer overlay",
                "cudaEventCreateWithFlags for live analyzer overlay");
        } catch (...) {
            destroy_resources();
            throw;
        }
    }

    void destroy_resources() noexcept {
        (void)cudaSetDevice(cuda_device_index);
        overlay_slots.clear();
        result_slots.clear();
    }

    void worker_thread_main() {
        worker_runtime::run_worker_thread_main(
            running, stop_requested, status, cuda_device_index, "cudaSetDevice for live analyzer thread",
            "live.analyzer",
            [this](Status& current_status) {
                DetectBundle bundle{};
                if (!fanout.wait_acquire_detect(&bundle, stop_requested)) {
                    return;
                }
                auto detect_release = worker_runtime::make_scoped_rollback(
                    [this, slot_index = bundle.slot_index]() noexcept { fanout.release_detect(slot_index); });

                const std::shared_ptr<FrameAnalyzer> current_analyzer = analyzer.load(std::memory_order_acquire);
                current_status.analyzer_attached = static_cast<bool>(current_analyzer);
                current_status.model_hot = static_cast<bool>(current_analyzer);
                current_status.backend_name = current_analyzer ? current_analyzer->backend_name() : std::string{};
                if (!current_analyzer) {
                    detect_release.run_and_dismiss();
                    current_status.frames_skipped += 1U;
                    current_status.last_error.clear();
                    worker_runtime::publish_status(&status, current_status);
                    return;
                }

                const auto started_at = Clock::now();
                AnalyzerResult result;
                try {
                    result = current_analyzer->analyze(bundle);
                } catch (const std::exception& error) {
                    detect_release.run_and_dismiss();
                    current_status.last_error = error.what();
                    worker_runtime::publish_status(&status, current_status);
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
                        ensure_cuda_ok(
                            cudaMemset2DAsync(device_ptr_as_void(overlay_slot->device_buffer.data()),
                                              overlay_slot->device_buffer.pitch_bytes(), 0,
                                              static_cast<std::size_t>(overlay_slot->device_buffer.width()) * 4U,
                                              overlay_slot->device_buffer.height(), overlay_slot->stream.get()),
                            "cudaMemset2DAsync for live analyzer overlay clear");
                        if (result.ready_event != nullptr) {
                            ensure_cuda_ok(cudaStreamWaitEvent(overlay_slot->stream.get(), result.ready_event, 0),
                                           "cudaStreamWaitEvent for live analyzer overlay");
                        }

                        for (const AnalyzerSplitResult& split : result.splits) {
                            if (!split.boxes_xyxy.defined() || !split.labels_zero_based.defined() ||
                                !split.colors_rgb.defined() || split.boxes_xyxy.numel() == 0 ||
                                split.labels_zero_based.numel() == 0) {
                                continue;
                            }
                            std::uint8_t* overlay_ptr = device_ptr_as_bytes(overlay_slot->device_buffer.data()) +
                                                        static_cast<std::size_t>(split.source_region.y) *
                                                            overlay_slot->device_buffer.pitch_bytes() +
                                                        static_cast<std::size_t>(split.source_region.x) * 4U;
                            const bool* masks_ptr = nullptr;
                            if (split.masks.defined() && split.masks.dim() == 3 &&
                                split.masks.size(0) == split.labels_zero_based.size(0) &&
                                split.masks.size(1) == static_cast<int64_t>(split.source_region.height) &&
                                split.masks.size(2) == static_cast<int64_t>(split.source_region.width)) {
                                masks_ptr = split.masks.data_ptr<bool>();
                            }
                            mmltk::rfdetr::launch_draw_analysis_overlay_rgba_pitched(
                                overlay_ptr, overlay_slot->device_buffer.pitch_bytes(),
                                static_cast<int>(split.source_region.width),
                                static_cast<int>(split.source_region.height), masks_ptr,
                                split.boxes_xyxy.data_ptr<float>(), split.colors_rgb.data_ptr<std::uint8_t>(),
                                split.labels_zero_based.data_ptr<int>(),
                                static_cast<int>(split.labels_zero_based.size(0)), static_cast<std::uint8_t>(115U), 2,
                                overlay_slot->stream.get());
                            ensure_cuda_ok(cudaPeekAtLastError(), "launch_draw_analysis_overlay_rgba_pitched");
                            overlay_has_content = true;
                        }

                        ensure_cuda_ok(cudaEventRecord(overlay_slot->ready_event.get(), overlay_slot->stream.get()),
                                       "cudaEventRecord for live analyzer overlay");
                        overlay_slot->frame_id = result.frame_id;
                        overlay_slot->has_content = overlay_has_content;
                        worker_runtime::publish_latest_slot(*overlay_slot, latest_overlay_index);
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
                    worker_runtime::publish_latest_slot(*result_slot, latest_result_index);
                    result_slot_release.dismiss();
                }

                detect_release.run_and_dismiss();

                current_status.frames_analyzed += 1U;
                current_status.last_completed_frame_id = result.frame_id;
                current_status.last_latency_ms =
                    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(Clock::now() - started_at)
                        .count();
                current_status.last_error.clear();
                worker_runtime::publish_status(&status, current_status);
            },
            [this](const char* error_message) { publish_error(error_message); });
    }

    [[nodiscard]] ResultSlot* reserve_result_slot() {
        return worker_runtime::reserve_writable_slot_view(result_slots, latest_result_index,
                                                          "cudaEventQuery for live analyzer slot reuse");
    }

    [[nodiscard]] OverlaySlot* reserve_overlay_slot() {
        return worker_runtime::reserve_writable_slot_view(overlay_slots, latest_overlay_index,
                                                          "cudaEventQuery for live analyzer slot reuse");
    }

    void publish_error(std::string error_message) {
        worker_runtime::publish_error(&last_error, std::move(error_message));
    }

    LiveFrameFanout& fanout;
    int cuda_device_index = 0;
    std::uint32_t max_capture_width = 0;
    std::uint32_t max_capture_height = 0;
    std::thread thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<std::shared_ptr<FrameAnalyzer>> analyzer{std::shared_ptr<FrameAnalyzer>{}};
    std::atomic<std::shared_ptr<const Status>> status{std::make_shared<Status>()};
    std::atomic<std::shared_ptr<const std::string>> last_error{std::make_shared<std::string>()};
    std::vector<std::unique_ptr<ResultSlot>> result_slots;
    std::vector<std::unique_ptr<OverlaySlot>> overlay_slots;
    std::atomic<int> latest_result_index{-1};
    std::atomic<int> latest_overlay_index{-1};
};

LiveAnalyzerWorker::LiveAnalyzerWorker(LiveFrameFanout& fanout, const int cuda_device_index,
                                       const std::uint32_t max_capture_width, const std::uint32_t max_capture_height,
                                       const std::uint32_t result_slot_count, const std::uint32_t overlay_slot_count)
    : impl_(std::make_unique<Impl>(fanout, cuda_device_index, max_capture_width, max_capture_height, result_slot_count,
                                   overlay_slot_count)) {}

LiveAnalyzerWorker::~LiveAnalyzerWorker() = default;

void LiveAnalyzerWorker::start() {
    impl_->start();
}

void LiveAnalyzerWorker::stop() {
    impl_->stop();
}

void LiveAnalyzerWorker::set_analyzer(std::unique_ptr<FrameAnalyzer> analyzer) {
    impl_->set_analyzer(std::move(analyzer));
}

bool LiveAnalyzerWorker::running() const noexcept {
    return impl_->running_state();
}

bool LiveAnalyzerWorker::try_acquire_latest_result(AnalyzerResultView* out) {
    return impl_->try_acquire_latest_result(out);
}

void LiveAnalyzerWorker::release_result(const std::uint32_t slot_index) {
    impl_->release_result(slot_index);
}

bool LiveAnalyzerWorker::try_acquire_latest_overlay(AnalysisOverlayView* out) {
    return impl_->try_acquire_latest_overlay(out);
}

void LiveAnalyzerWorker::release_overlay(const std::uint32_t slot_index) {
    impl_->release_overlay(slot_index);
}

LiveAnalyzerWorker::Status LiveAnalyzerWorker::snapshot_status() const {
    return impl_->snapshot_status();
}

}  
