#pragma once

#include "mmltk/live/live_session_controller.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mmltk::gui {

inline constexpr std::string_view kBrowserLiveNoWorkspaceOutputResultCode = "no_workspace_output";

enum class BrowserLivePublicationMode : std::uint8_t {
    None = 0,
    Predict = 1,
    Annotate = 2,
};

[[nodiscard]] inline int browser_live_publication_cuda_device_index(
    const BrowserLivePublicationMode mode, const int annotate_device_id, const int predict_device_id,
    const std::optional<int>& active_predict_device_id) {
    switch (mode) {
        case BrowserLivePublicationMode::Annotate:
            return annotate_device_id;
        case BrowserLivePublicationMode::Predict:
            return active_predict_device_id.value_or(predict_device_id);
        case BrowserLivePublicationMode::None:
            break;
    }
    throw std::runtime_error("browser live frame publication requires an active live mode");
}

struct BrowserLivePublicationFailureContext {
    mmltk::live::LivePipelineStage stage = mmltk::live::LivePipelineStage::CompositorPublishWorkspace;
    std::optional<mmltk::live::LiveFrameId> live_frame_id;
    std::uint32_t cuda_device_index = mmltk::live::kLivePipelineUnknownCudaDevice;
    std::string revision;
    std::string result_detail;
    bool transient_startup_miss = false;
    bool explicit_failure = false;
};

struct BrowserLiveWorkspaceAcquisitionMissDecision {
    BrowserLivePublicationFailureContext failure;
    bool record_failure = false;
};

[[nodiscard]] inline std::string browser_live_publication_cuda_device_label(const std::uint32_t cuda_device_index) {
    return cuda_device_index == mmltk::live::kLivePipelineUnknownCudaDevice ? std::string("unknown")
                                                                           : std::to_string(cuda_device_index);
}

[[nodiscard]] inline mmltk::live::LivePipelineTraceEvent browser_live_publication_failure_event(
    const mmltk::live::LiveSessionStatus& status) {
    if (status.pipeline.diagnostic_failed && status.pipeline.diagnostic_failure_event.event_count != 0U) {
        return status.pipeline.diagnostic_failure_event;
    }
    if (status.pipeline.last_event.event_count != 0U) {
        return status.pipeline.last_event;
    }
    mmltk::live::LivePipelineTraceEvent event;
    event.stage = mmltk::live::LivePipelineStage::CompositorPublishWorkspace;
    event.stage_name = mmltk::live::live_pipeline_stage_name(event.stage);
    event.frame_id = status.compositor.last_frame_id;
    event.cuda_device_index = mmltk::live::kLivePipelineUnknownCudaDevice;
    event.surface_revision = status.compositor.last_frame_id.sequence;
    return event;
}

[[nodiscard]] inline BrowserLivePublicationFailureContext browser_live_workspace_acquisition_failure_context(
    const mmltk::live::LiveSessionStatus& status, const std::uint32_t fallback_cuda_device_index,
    const std::string_view detail_override = {}) {
    const mmltk::live::LivePipelineTraceEvent event = browser_live_publication_failure_event(status);
    BrowserLivePublicationFailureContext context;
    context.stage = event.stage;
    context.cuda_device_index = event.cuda_device_index == mmltk::live::kLivePipelineUnknownCudaDevice
                                    ? fallback_cuda_device_index
                                    : event.cuda_device_index;
    if (event.frame_id.sequence != 0U || event.frame_id.session_nonce != 0U) {
        context.live_frame_id = event.frame_id;
    }
    if (event.surface_revision != 0U) {
        context.revision = std::to_string(event.surface_revision);
    } else if (context.live_frame_id.has_value() && context.live_frame_id->sequence != 0U) {
        context.revision = std::to_string(context.live_frame_id->sequence);
    }

    const bool has_failure_detail = !status.last_error.empty() || status.pipeline.diagnostic_failed ||
                                    !status.compositor.last_error.empty() || !detail_override.empty();
    const bool first_workspace_ready =
        status.first_workspace_publication_ready || status.pipeline.first_workspace_publication_ready;
    context.explicit_failure = has_failure_detail;
    context.transient_startup_miss = !first_workspace_ready && !has_failure_detail;

    std::string component_detail(detail_override);
    if (component_detail.empty()) {
        component_detail = !status.last_error.empty()       ? status.last_error
                           : !status.compositor.last_error.empty() ? status.compositor.last_error
                                                                   : std::string{};
    }

    context.result_detail = "stage=";
    context.result_detail +=
        event.stage_name != nullptr ? event.stage_name : mmltk::live::live_pipeline_stage_name(event.stage);
    context.result_detail += " frame=";
    context.result_detail += context.live_frame_id.has_value() ? std::to_string(context.live_frame_id->sequence)
                                                               : std::string("unknown");
    context.result_detail += " cuda_device=";
    context.result_detail += browser_live_publication_cuda_device_label(context.cuda_device_index);
    context.result_detail += " revision=";
    context.result_detail += context.revision.empty() ? std::string("unknown") : context.revision;
    context.result_detail += " latency_us=";
    context.result_detail += std::to_string(event.latency_us);
    if (!component_detail.empty()) {
        context.result_detail += " detail=";
        context.result_detail += component_detail;
    } else if (first_workspace_ready) {
        context.result_detail += " detail=workspace output acquisition missed after first publication";
    } else {
        context.result_detail += " detail=workspace output acquisition missed before first publication";
    }
    return context;
}

[[nodiscard]] inline BrowserLiveWorkspaceAcquisitionMissDecision browser_live_workspace_acquisition_miss_decision(
    const mmltk::live::LiveSessionStatus& status, const std::uint32_t fallback_cuda_device_index,
    const std::string_view detail_override = {}) {
    BrowserLiveWorkspaceAcquisitionMissDecision decision;
    decision.failure =
        browser_live_workspace_acquisition_failure_context(status, fallback_cuda_device_index, detail_override);
    decision.record_failure = decision.failure.explicit_failure;
    return decision;
}

}  // namespace mmltk::gui
