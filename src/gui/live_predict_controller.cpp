#include "live_predict_controller.h"

#include "live_session_utils.h"
#include "rfdetr_workflows.h"

#include "mmltk/rfdetr/live_predict.h"

#if MMLTK_RFDETR_LIVE_CAPTURE
#include <frameshow/capture_types.hpp>
#endif

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace mmltk::gui {

namespace {

#if MMLTK_RFDETR_LIVE_CAPTURE
mmltk::live::LiveSessionConfig
make_predict_live_session_config(const mmltk::rfdetr::LivePredictOptions& options) {
    mmltk::live::LiveSessionConfig config;
    config.capture.device_path = options.source.device_path;
    config.capture.cuda_device_index = options.device_id;
    config.capture.width = options.source.width;
    config.capture.height = options.source.height;
    config.capture.fps = options.source.fps;
    config.capture.v4l2_buffer_count = options.source.v4l2_buffer_count;
    config.capture.preview_buffer_count = 0U;
    config.capture.initial_region = frameshow::CaptureRegion{
        0U,
        0U,
        options.source.width,
        options.source.height,
    };
    config.detect_slot_count = 2U;
    config.output_slot_count =
        std::max<std::uint32_t>(2U, options.source.preview_buffer_count);
    config.cuda_device_index = options.device_id;
    return config;
}
#endif

} // namespace

void LivePredictController::poll_status() {
    if (controller_ == nullptr) {
        return;
    }
    if (!status_) {
        status_ = std::make_unique<mmltk::live::LiveSessionStatus>();
    }
    *status_ = controller_->snapshot_status();
}

void LivePredictController::clear_errors() {
    start_error_.clear();
    action_error_.clear();
}

void LivePredictController::clear_start_error() {
    start_error_.clear();
}

void LivePredictController::launch(
    mmltk::runtime::BackgroundExecutor& background_executor,
    mmltk::runtime::UiCallbackQueue& ui_callbacks,
    const PredictViewState& state,
    const std::string& preset_name,
    std::function<void(int)> on_started,
    std::function<void(const std::string&)> on_error) {
    if (running()) {
        return;
    }

    clear_errors();
    starting_ = true;
    start_task_ = mmltk::runtime::submit_background_task(
        background_executor,
        ui_callbacks,
        [state, preset_name]() {
            StartOutcome outcome;
            const mmltk::rfdetr::LivePredictOptions options =
                rfdetr_workflows::build_live_predict_options(state, preset_name);
            outcome.device_id = options.device_id;
#if !MMLTK_RFDETR_LIVE_CAPTURE
            throw std::runtime_error(
                "live prediction requires live capture support in this build");
#else
            outcome.controller =
                std::make_unique<mmltk::live::LiveSessionController>(
                    make_predict_live_session_config(options));
            seed_runtime_crop_from_source(outcome.controller->ui_crop_state(),
                                          state.source);
            outcome.controller->attach_analyzer(
                make_live_rfdetr_frame_analyzer(options));
            outcome.controller->start();
            outcome.status = outcome.controller->snapshot_status();
#endif
            return outcome;
        },
        [this, on_started = std::move(on_started)](StartOutcome outcome) mutable {
            starting_ = false;
            controller_ = std::move(outcome.controller);
            status_ = std::make_unique<mmltk::live::LiveSessionStatus>(
                std::move(outcome.status));
            clear_errors();
            if (on_started) {
                on_started(outcome.device_id);
            }
        },
        [this, on_error = std::move(on_error)](const std::string& error) {
            starting_ = false;
            start_error_ = error;
            if (on_error) {
                on_error(error);
            }
        });
}

void LivePredictController::stop(
    mmltk::runtime::BackgroundExecutor& background_executor,
    mmltk::runtime::UiCallbackQueue& ui_callbacks,
    SourceSelectionState* source,
    const std::function<void()>& before_async_stop,
    std::function<void()> on_stopped,
    std::function<void(const std::string&)> on_error) {
    if (starting_) {
        start_task_.cancel();
        starting_ = false;
        clear_start_error();
        return;
    }
    if (stopping_) {
        return;
    }
    if (controller_ == nullptr) {
        status_.reset();
        clear_errors();
        return;
    }

    stopping_ = true;
    status_.reset();
#if MMLTK_RFDETR_LIVE_CAPTURE
    if (source != nullptr) {
        mirror_runtime_crop_to_source(controller_->ui_crop_state(), source);
    }
#else
    (void)source;
#endif
    auto controller = std::move(controller_);
    if (before_async_stop) {
        before_async_stop();
    }
    mmltk::runtime::submit_background_task(
        background_executor,
        ui_callbacks,
        [controller = std::move(controller)]() mutable {
            if (controller) {
                controller->stop();
            }
        },
        [this, on_stopped = std::move(on_stopped)]() mutable {
            stopping_ = false;
            clear_errors();
            if (on_stopped) {
                on_stopped();
            }
        },
        [this, on_error = std::move(on_error)](const std::string& error) {
            stopping_ = false;
            start_error_.clear();
            action_error_ = error;
            if (on_error) {
                on_error(error);
            }
        });
}

bool LivePredictController::running() const noexcept {
    return controller_ != nullptr || starting_ || stopping_;
}

bool LivePredictController::active() const noexcept {
    if (starting_ || stopping_ || status_ == nullptr) {
        return starting_ || stopping_;
    }
    return status_->running ||
           status_->analyzer.running ||
           status_->analyzer.model_hot;
}

bool LivePredictController::starting() const noexcept {
    return starting_;
}

bool LivePredictController::stopping() const noexcept {
    return stopping_;
}

mmltk::live::LiveSessionController* LivePredictController::controller() noexcept {
    return controller_.get();
}

const mmltk::live::LiveSessionController* LivePredictController::controller() const noexcept {
    return controller_.get();
}

const mmltk::live::LiveSessionStatus* LivePredictController::status() const noexcept {
    return status_.get();
}

const std::string& LivePredictController::start_error() const noexcept {
    return start_error_;
}

const std::string& LivePredictController::action_error() const noexcept {
    return action_error_;
}

} // namespace mmltk::gui
