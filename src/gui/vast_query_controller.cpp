#include "vast_query_controller.h"

#include <sstream>
#include <utility>

namespace mmltk::gui {

namespace {

std::string format_decimal(const double value, const int precision) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(precision);
    stream << value;
    return stream.str();
}

std::string summarize_armed_offer(const VastOfferSummary& offer) {
    std::ostringstream stream;
    stream << remote_gpu_family_label(offer.family) << " · " << offer.gpu_name << " · " << offer.num_gpus << " GPUs"
           << " · DLPerf/$ " << format_decimal(offer.dlperf_usd, 2) << " · $" << format_decimal(offer.dph, 2) << "/hr";
    return stream.str();
}

}  // namespace

VastQueryController::VastQueryController(mmltk::runtime::BackgroundExecutor& background_executor,
                                         mmltk::runtime::UiCallbackQueue& ui_callbacks)
    : VastQueryController(background_executor, ui_callbacks, query_vast_offers) {}

VastQueryController::VastQueryController(mmltk::runtime::BackgroundExecutor& background_executor,
                                         mmltk::runtime::UiCallbackQueue& ui_callbacks,
                                         VastOfferQueryFn query_offers_fn)
    : background_executor_(background_executor),
      ui_callbacks_(ui_callbacks),
      query_offers_fn_(std::move(query_offers_fn)) {
    if (!query_offers_fn_) {
        query_offers_fn_ = query_vast_offers;
    }
}

void VastQueryController::launch(const std::string& api_key, const std::vector<RemoteGpuFamily>& families,
                                 std::function<void(const std::string&)> on_error) {
    if (state_.running) {
        return;
    }

    state_.running = true;
    state_.last_error.clear();
    state_.last_summary.clear();
    state_.results.clear();
    armed_offer_.reset();

    VastQueryConfig config;
    static_cast<VastBridgeConfig&>(config) = make_vast_bridge_config(api_key);
    config.min_gpus = 4;
    config.result_limit = 2;

    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [config, families, query_offers_fn = query_offers_fn_]() { return query_offers_fn(config, families); },
        [this](std::vector<VastOfferSummary> results) {
            state_.results = std::move(results);
            state_.last_error.clear();
            state_.last_summary = state_.results.empty()
                                      ? "query completed: no matching offers"
                                      : "query completed: " + std::to_string(state_.results.size()) + " offers";
            state_.running = false;
        },
        [this, on_error = std::move(on_error)](const std::string& error) {
            state_.last_error = error;
            state_.last_summary.clear();
            state_.running = false;
            if (on_error) {
                on_error(error);
            }
        });
}

void VastQueryController::arm_offer(const VastOfferSummary& offer) {
    armed_offer_ = offer;
}

void VastQueryController::clear_armed_offer() {
    armed_offer_.reset();
}

bool VastQueryController::running() const noexcept {
    return state_.running;
}

const VastQueryState& VastQueryController::state() const noexcept {
    return state_;
}

const std::optional<VastOfferSummary>& VastQueryController::armed_offer() const noexcept {
    return armed_offer_;
}

std::string VastQueryController::armed_offer_summary() const {
    return armed_offer_.has_value() ? summarize_armed_offer(*armed_offer_) : std::string{};
}

}  // namespace mmltk::gui
