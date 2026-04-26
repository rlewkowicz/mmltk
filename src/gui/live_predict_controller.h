#pragma once

#include "view_state.h"

#include "mmltk/live/live_session_controller.h"
#include "mmltk/runtime/async_runtime.h"

#include <functional>
#include <memory>
#include <string>

namespace mmltk::gui {

class LivePredictController {
   public:
    LivePredictController() = default;
    ~LivePredictController() = default;

    LivePredictController(const LivePredictController&) = delete;
    LivePredictController& operator=(const LivePredictController&) = delete;

    void poll_status();
    void clear_errors();
    void clear_start_error();

    void launch(mmltk::runtime::BackgroundExecutor& background_executor, mmltk::runtime::UiCallbackQueue& ui_callbacks,
                const PredictViewState& state, const std::string& preset_name, std::function<void()> on_workspace_ready,
                std::function<void(int)> on_started, std::function<void(const std::string&)> on_error = {});
    void stop(mmltk::runtime::BackgroundExecutor& background_executor, mmltk::runtime::UiCallbackQueue& ui_callbacks,
              SourceSelectionState* source, const std::function<void()>& before_async_stop = {},
              std::function<void()> on_stopped = {}, std::function<void(const std::string&)> on_error = {});

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool starting() const noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    [[nodiscard]] mmltk::live::LiveSessionController* controller() noexcept;
    [[nodiscard]] const mmltk::live::LiveSessionController* controller() const noexcept;
    [[nodiscard]] const mmltk::live::LiveSessionStatus* status() const noexcept;
    [[nodiscard]] const std::string& start_error() const noexcept;
    [[nodiscard]] const std::string& action_error() const noexcept;

   private:
    struct StartOutcome {
        std::unique_ptr<mmltk::live::LiveSessionController> controller;
        mmltk::live::LiveSessionStatus status;
        int device_id = 0;
    };

    std::unique_ptr<mmltk::live::LiveSessionController> controller_;
    std::unique_ptr<mmltk::live::LiveSessionStatus> status_;
    std::string start_error_;
    std::string action_error_;
    bool starting_ = false;
    bool stopping_ = false;
    mmltk::runtime::TaskCancellation start_task_{};
};

}  // namespace mmltk::gui
