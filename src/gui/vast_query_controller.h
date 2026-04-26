#pragma once

#include "vast_runtime.h"

#include "mmltk/runtime/async_runtime.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::gui {

using VastOfferQueryFn =
    std::function<std::vector<VastOfferSummary>(const VastQueryConfig&, const std::vector<RemoteGpuFamily>&)>;

struct VastQueryState {
    bool running = false;
    std::string last_summary;
    std::string last_error;
    std::vector<VastOfferSummary> results;
};

class VastQueryController {
   public:
    VastQueryController(mmltk::runtime::BackgroundExecutor& background_executor,
                        mmltk::runtime::UiCallbackQueue& ui_callbacks);
    VastQueryController(mmltk::runtime::BackgroundExecutor& background_executor,
                        mmltk::runtime::UiCallbackQueue& ui_callbacks, VastOfferQueryFn query_offers_fn);

    void launch(const std::string& api_key, const std::vector<RemoteGpuFamily>& families,
                std::function<void(const std::string&)> on_error = {});
    void arm_offer(const VastOfferSummary& offer);
    void clear_armed_offer();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const VastQueryState& state() const noexcept;
    [[nodiscard]] const std::optional<VastOfferSummary>& armed_offer() const noexcept;
    [[nodiscard]] std::string armed_offer_summary() const;

   private:
    mmltk::runtime::BackgroundExecutor& background_executor_;
    mmltk::runtime::UiCallbackQueue& ui_callbacks_;
    VastOfferQueryFn query_offers_fn_;
    VastQueryState state_{};
    std::optional<VastOfferSummary> armed_offer_;
};

}  // namespace mmltk::gui
