#pragma once

#include "vast_runtime.h"

#include "mmltk/runtime/async_runtime.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mmltk::gui {

struct RemoteTrainInstanceState : VastInstanceStateFields {
    int offer_id = 0;
};

struct RemoteTrainSessionState {
    bool request_running = false;
    bool refresh_running = false;
    std::string action_label;
    std::string last_summary;
    std::string last_error;
    std::string output_tail;
    std::optional<RemoteTrainInstanceState> instance;
};

[[nodiscard]] bool remote_train_instance_running(const RemoteTrainInstanceState& state) noexcept;

struct RemoteRefreshPayload;

class RemoteTrainController {
   public:
    RemoteTrainController(mmltk::runtime::BackgroundExecutor& background_executor,
                          mmltk::runtime::UiCallbackQueue& ui_callbacks);

    RemoteTrainController(const RemoteTrainController&) = delete;
    RemoteTrainController& operator=(const RemoteTrainController&) = delete;

    void launch(const std::string& api_key, const VastOfferSummary& offer, std::string_view image,
                std::string_view launch_template, std::function<void(const std::string&)> on_error = {});
    void start(const std::string& api_key, std::function<void(const std::string&)> on_error = {});
    void stop(const std::string& api_key, std::function<void(const std::string&)> on_error = {});
    void poll(std::string_view api_key);

    [[nodiscard]] const RemoteTrainSessionState& state() const noexcept;

   private:
    void run_existing_instance_action(const std::string& api_key, std::function<void(const std::string&)> on_error,
                                      std::string_view action_label, std::string_view missing_instance_error,
                                      std::string_view summary_suffix, bool stop_instance);
    void complete_request(RemoteRefreshPayload payload, std::string_view summary_suffix);
    void fail_request(const std::string& error, const std::function<void(const std::string&)>& on_error);
    void complete_refresh(RemoteRefreshPayload payload);
    void fail_refresh(const std::string& error);

    mmltk::runtime::BackgroundExecutor& background_executor_;
    mmltk::runtime::UiCallbackQueue& ui_callbacks_;
    RemoteTrainSessionState state_{};
    std::chrono::steady_clock::time_point next_refresh_deadline_{};
};

}  // namespace mmltk::gui
