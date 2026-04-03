#pragma once

#include "train_command.h"
#include "train_process_runtime.h"
#include "view_state.h"

#include "mmltk/runtime/async_runtime.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::gui {

using LocalGpuEnumerator = std::function<std::vector<LocalGpuInfo>(std::string*)>;

class LocalTrainController {
public:
    LocalTrainController(mmltk::runtime::BackgroundExecutor& background_executor,
                         mmltk::runtime::UiCallbackQueue& ui_callbacks);
    LocalTrainController(mmltk::runtime::BackgroundExecutor& background_executor,
                         mmltk::runtime::UiCallbackQueue& ui_callbacks,
                         LocalGpuEnumerator gpu_enumerator);

    LocalTrainController(const LocalTrainController&) = delete;
    LocalTrainController& operator=(const LocalTrainController&) = delete;

    void initialize(const std::vector<int>& preferred_device_ids);
    void poll();
    void refresh_visible_gpus(const std::vector<int>& preferred_device_ids);
    void set_device_selected(std::size_t index, bool selected);
    void start(const TrainViewState& state, std::string_view preset_name);
    void request_stop(bool force);
    void shutdown();

    [[nodiscard]] const LocalTrainSessionState& session_state() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool gpu_refresh_running() const noexcept;
    [[nodiscard]] const std::vector<LocalGpuInfo>& gpus() const noexcept;
    [[nodiscard]] const std::vector<bool>& gpu_selection() const noexcept;
    [[nodiscard]] const std::string& gpu_error() const noexcept;
    [[nodiscard]] std::vector<int> selected_device_ids() const;

private:
    mmltk::runtime::BackgroundExecutor& background_executor_;
    mmltk::runtime::UiCallbackQueue& ui_callbacks_;
    LocalGpuEnumerator gpu_enumerator_;
    std::vector<LocalGpuInfo> gpus_;
    std::vector<bool> gpu_selection_;
    std::string gpu_error_;
    bool gpu_refresh_running_ = false;
    LocalTrainSessionState state_{};
    std::unique_ptr<LocalTrainSession> session_;
};

} // namespace mmltk::gui
