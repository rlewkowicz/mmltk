#include "remote_train_controller.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <optional>
#include <sstream>
#include <utility>

namespace mmltk::gui {

struct RemoteRefreshPayload {
    RemoteTrainInstanceState instance;
    std::string output_tail;
    std::string log_error;
};

namespace {

constexpr std::chrono::seconds kRemoteRefreshInterval{5};
constexpr std::chrono::seconds kRemoteRefreshRetryInterval{2};
constexpr std::size_t kRemoteLogTailLines = 80U;

std::string lower_copy(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const unsigned char ch : text) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

bool contains_status_token(const std::string& haystack, const std::string_view needle) {
    return haystack.find(std::string(needle)) != std::string::npos;
}

std::string instance_status_text(const RemoteTrainInstanceState& state) {
    for (const std::string_view candidate : std::array<std::string_view, 5>{
             state.current_state, state.actual_status, state.intended_status, state.next_state, state.status_message}) {
        if (!candidate.empty()) {
            return std::string(candidate);
        }
    }
    return {};
}

RemoteTrainInstanceState instance_state_from_info(const VastInstanceInfo& info, const int offer_id) {
    RemoteTrainInstanceState state;
    state.offer_id = offer_id;
    static_cast<VastInstanceStateFields&>(state) = static_cast<const VastInstanceStateFields&>(info);
    return state;
}

RemoteRefreshPayload fetch_remote_refresh_payload(const VastBridgeConfig& config, const int offer_id,
                                                  const int instance_id) {
    const VastInstanceInfo info = show_vast_instance(config, instance_id);
    RemoteRefreshPayload payload;
    payload.instance = instance_state_from_info(info, offer_id);
    try {
        payload.output_tail = fetch_vast_instance_logs(config, instance_id, kRemoteLogTailLines);
    } catch (const std::exception& error) {
        payload.log_error = error.what();
    }
    return payload;
}

std::string summarize_instance(const RemoteTrainInstanceState& state) {
    std::ostringstream stream;
    stream << "remote instance " << state.instance_id;
    const std::string status = instance_status_text(state);
    if (!status.empty()) {
        stream << " · " << status;
    }
    if (!state.gpu_name.empty()) {
        stream << " · " << state.gpu_name;
    }
    if (state.num_gpus > 0) {
        stream << " · " << state.num_gpus << " GPUs";
    }
    return stream.str();
}

void update_session_from_refresh(RemoteTrainSessionState& state, RemoteRefreshPayload payload, std::string summary) {
    state.instance = std::move(payload.instance);
    if (!payload.output_tail.empty()) {
        state.output_tail = std::move(payload.output_tail);
    }
    if (!summary.empty()) {
        state.last_summary = std::move(summary);
    } else if (state.instance.has_value()) {
        state.last_summary = summarize_instance(*state.instance);
    }
    state.last_error = std::move(payload.log_error);
}

}  // namespace

bool remote_train_instance_running(const RemoteTrainInstanceState& state) noexcept {
    const std::string normalized =
        lower_copy(state.current_state + " " + state.actual_status + " " + state.intended_status + " " +
                   state.next_state + " " + state.status_message);
    if (contains_status_token(normalized, "stopp") || contains_status_token(normalized, "offline") ||
        contains_status_token(normalized, "terminat") || contains_status_token(normalized, "exited") ||
        contains_status_token(normalized, "dead") || contains_status_token(normalized, "destroy")) {
        return false;
    }
    if (contains_status_token(normalized, "running") || contains_status_token(normalized, "started") ||
        contains_status_token(normalized, "active") || contains_status_token(normalized, "online")) {
        return true;
    }
    return state.instance_id > 0;
}

RemoteTrainController::RemoteTrainController(mmltk::runtime::BackgroundExecutor& background_executor,
                                             mmltk::runtime::UiCallbackQueue& ui_callbacks)
    : background_executor_(background_executor), ui_callbacks_(ui_callbacks) {}

void RemoteTrainController::launch(const std::string& api_key, const VastOfferSummary& offer, std::string_view image,
                                   std::string_view launch_template, std::function<void(const std::string&)> on_error) {
    if (state_.request_running || state_.refresh_running) {
        return;
    }

    state_.request_running = true;
    state_.action_label = "Launching Remote Instance";
    state_.last_error.clear();
    state_.last_summary = "launching remote Vast instance";
    state_.output_tail.clear();

    const VastBridgeConfig config = make_vast_bridge_config(api_key);
    const int offer_id = offer.offer_id;
    const std::string image_text(image);
    const std::string template_text(launch_template);
    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [config, offer_id, image_text, template_text]() {
            const VastLaunchTemplateOptions options = parse_vast_launch_template(template_text);
            const VastCreateInstanceResult created = create_vast_instance(config, offer_id, image_text, options);
            return fetch_remote_refresh_payload(config, offer_id, created.instance_id);
        },
        [this](RemoteRefreshPayload payload) { complete_request(std::move(payload), " launched"); },
        [this, on_error = std::move(on_error)](const std::string& error) { fail_request(error, on_error); });
}

void RemoteTrainController::start(const std::string& api_key, std::function<void(const std::string&)> on_error) {
    run_existing_instance_action(api_key, std::move(on_error), "Starting Remote Instance",
                                 "remote train start requires an existing Vast instance", " started", false);
}

void RemoteTrainController::stop(const std::string& api_key, std::function<void(const std::string&)> on_error) {
    run_existing_instance_action(api_key, std::move(on_error), "Stopping Remote Instance",
                                 "remote train stop requires an existing Vast instance", " stopped", true);
}

void RemoteTrainController::run_existing_instance_action(
    const std::string& api_key, std::function<void(const std::string&)> on_error, const std::string_view action_label,
    const std::string_view missing_instance_error, const std::string_view summary_suffix, const bool stop_instance) {
    if (state_.request_running || state_.refresh_running) {
        return;
    }
    if (!state_.instance.has_value()) {
        const std::string error(missing_instance_error);
        state_.last_error = error;
        if (on_error) {
            on_error(error);
        }
        return;
    }

    state_.request_running = true;
    state_.action_label = action_label;
    state_.last_error.clear();
    const int offer_id = state_.instance->offer_id;
    const int instance_id = state_.instance->instance_id;
    const VastBridgeConfig config = make_vast_bridge_config(api_key);
    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [config, offer_id, instance_id, stop_instance]() {
            if (stop_instance) {
                stop_vast_instance(config, instance_id);
            } else {
                start_vast_instance(config, instance_id);
            }
            return fetch_remote_refresh_payload(config, offer_id, instance_id);
        },
        [this, summary_suffix = std::string(summary_suffix)](RemoteRefreshPayload payload) {
            complete_request(std::move(payload), summary_suffix);
        },
        [this, on_error = std::move(on_error)](const std::string& error) { fail_request(error, on_error); });
}

void RemoteTrainController::poll(const std::string_view api_key) {
    if (state_.request_running || state_.refresh_running || !state_.instance.has_value() || api_key.empty() ||
        std::chrono::steady_clock::now() < next_refresh_deadline_) {
        return;
    }

    state_.refresh_running = true;
    const int offer_id = state_.instance->offer_id;
    const int instance_id = state_.instance->instance_id;
    const VastBridgeConfig config = make_vast_bridge_config(api_key);
    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [config, offer_id, instance_id]() { return fetch_remote_refresh_payload(config, offer_id, instance_id); },
        [this](RemoteRefreshPayload payload) { complete_refresh(std::move(payload)); },
        [this](const std::string& error) { fail_refresh(error); });
}

void RemoteTrainController::complete_request(RemoteRefreshPayload payload, const std::string_view summary_suffix) {
    update_session_from_refresh(state_, std::move(payload), {});
    state_.request_running = false;
    state_.action_label.clear();
    if (state_.instance.has_value()) {
        state_.last_summary = summarize_instance(*state_.instance) + std::string(summary_suffix);
    }
    next_refresh_deadline_ = std::chrono::steady_clock::now() + kRemoteRefreshInterval;
}

void RemoteTrainController::fail_request(const std::string& error,
                                         const std::function<void(const std::string&)>& on_error) {
    state_.request_running = false;
    state_.action_label.clear();
    state_.last_error = error;
    next_refresh_deadline_ = std::chrono::steady_clock::now() + kRemoteRefreshRetryInterval;
    if (on_error) {
        on_error(error);
    }
}

void RemoteTrainController::complete_refresh(RemoteRefreshPayload payload) {
    update_session_from_refresh(state_, std::move(payload), {});
    state_.refresh_running = false;
    next_refresh_deadline_ = std::chrono::steady_clock::now() + kRemoteRefreshInterval;
}

void RemoteTrainController::fail_refresh(const std::string& error) {
    state_.refresh_running = false;
    state_.last_error = error;
    next_refresh_deadline_ = std::chrono::steady_clock::now() + kRemoteRefreshRetryInterval;
}

const RemoteTrainSessionState& RemoteTrainController::state() const noexcept {
    return state_;
}

}  // namespace mmltk::gui
