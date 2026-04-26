#include "local_train_controller.h"

#include "rfdetr_workflows.h"

#include <algorithm>
#include <utility>

namespace mmltk::gui {

LocalTrainController::LocalTrainController(mmltk::runtime::BackgroundExecutor& background_executor,
                                           mmltk::runtime::UiCallbackQueue& ui_callbacks)
    : LocalTrainController(background_executor, ui_callbacks, enumerate_local_gpus) {}

LocalTrainController::LocalTrainController(mmltk::runtime::BackgroundExecutor& background_executor,
                                           mmltk::runtime::UiCallbackQueue& ui_callbacks,
                                           LocalGpuEnumerator gpu_enumerator)
    : background_executor_(background_executor),
      ui_callbacks_(ui_callbacks),
      gpu_enumerator_(std::move(gpu_enumerator)),
      session_(std::make_unique<LocalTrainSession>()) {
    if (!gpu_enumerator_) {
        gpu_enumerator_ = enumerate_local_gpus;
    }
}

void LocalTrainController::initialize(const std::vector<int>& preferred_device_ids) {
    refresh_visible_gpus(preferred_device_ids);
}

void LocalTrainController::poll() {
    if (!session_) {
        return;
    }
    state_ = session_->snapshot();
}

void LocalTrainController::refresh_visible_gpus(const std::vector<int>& preferred_device_ids) {
    if (gpu_refresh_running_) {
        return;
    }
    gpu_refresh_running_ = true;
    gpu_error_.clear();
    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [gpu_enumerator = gpu_enumerator_]() {
            std::string error;
            std::vector<LocalGpuInfo> refreshed = gpu_enumerator(&error);
            return std::make_pair(std::move(refreshed), std::move(error));
        },
        [this, preferred_device_ids](std::pair<std::vector<LocalGpuInfo>, std::string> result) {
            std::vector<LocalGpuInfo> refreshed = std::move(result.first);
            gpu_error_ = std::move(result.second);

            std::vector<bool> selected(refreshed.size(), true);
            for (std::size_t index = 0; index < refreshed.size(); ++index) {
                if (!preferred_device_ids.empty()) {
                    selected[index] = std::ranges::find(preferred_device_ids, refreshed[index].device_id) !=
                                      preferred_device_ids.end();
                    continue;
                }
                const auto found = std::ranges::find_if(
                    gpus_, [&](const LocalGpuInfo& info) { return info.device_id == refreshed[index].device_id; });
                if (found == gpus_.end()) {
                    continue;
                }
                const auto previous_index = static_cast<std::size_t>(std::distance(gpus_.begin(), found));
                if (previous_index < gpu_selection_.size()) {
                    selected[index] = gpu_selection_[previous_index];
                }
            }
            gpus_ = std::move(refreshed);
            gpu_selection_ = std::move(selected);
            gpu_refresh_running_ = false;
        },
        [this](const std::string& error) {
            gpu_error_ = error;
            gpu_refresh_running_ = false;
        });
}

void LocalTrainController::set_device_selected(const std::size_t index, const bool selected) {
    if (index >= gpu_selection_.size()) {
        return;
    }
    gpu_selection_[index] = selected;
}

void LocalTrainController::set_selected_device_ids(const std::vector<int>& device_ids) {
    gpu_selection_.assign(gpus_.size(), false);
    for (std::size_t index = 0; index < gpus_.size(); ++index) {
        gpu_selection_[index] = std::ranges::find(device_ids, gpus_[index].device_id) != device_ids.end();
    }
}

void LocalTrainController::start(const TrainViewState& state, std::string_view preset_name) {
    if (!session_) {
        session_ = std::make_unique<LocalTrainSession>();
    }
    if (session_->running()) {
        return;
    }

    const mmltk::rfdetr::TrainRequest request = rfdetr_workflows::build_train_request(state, selected_device_ids());
    const std::filesystem::path current_executable = current_executable_path();
    const std::filesystem::path cli_path = resolve_sibling_mmltk_cli(current_executable);
    session_->start(request, cli_path, std::string(preset_name));
    state_ = session_->snapshot();
}

void LocalTrainController::request_stop(const bool force) {
    if (!session_ || !session_->running()) {
        return;
    }
    session_->request_stop(force);
    state_ = session_->snapshot();
}

void LocalTrainController::shutdown() {
    if (!session_) {
        state_ = {};
        return;
    }
    session_->shutdown();
    state_ = session_->snapshot();
}

const LocalTrainSessionState& LocalTrainController::session_state() const noexcept {
    return state_;
}

bool LocalTrainController::running() const noexcept {
    return state_.running;
}

bool LocalTrainController::gpu_refresh_running() const noexcept {
    return gpu_refresh_running_;
}

const std::vector<LocalGpuInfo>& LocalTrainController::gpus() const noexcept {
    return gpus_;
}

const std::vector<bool>& LocalTrainController::gpu_selection() const noexcept {
    return gpu_selection_;
}

const std::string& LocalTrainController::gpu_error() const noexcept {
    return gpu_error_;
}

std::vector<int> LocalTrainController::selected_device_ids() const {
    std::vector<int> device_ids;
    for (std::size_t index = 0; index < std::min(gpus_.size(), gpu_selection_.size()); ++index) {
        if (gpu_selection_[index]) {
            device_ids.push_back(gpus_[index].device_id);
        }
    }
    return device_ids;
}

}  // namespace mmltk::gui
