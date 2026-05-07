#include "mmltk/live/live_session_controller.h"

#include "mmltk/live/frame_analyzer.h"
#include "mmltk/live/live_pipeline_trace.h"
#include "mmltk_logging.h"
#include "live/live_worker_logging.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace mmltk::live {

namespace {

constexpr auto kFirstWorkspaceStartupDeadline = std::chrono::milliseconds(2000);
constexpr auto kFirstWorkspaceStartupPoll = std::chrono::milliseconds(1);

void log_live_controller_message(const char* level, const std::string& message) {
    if (message.empty()) {
        return;
    }
    auto logger = mmltk::logging::logger("live.controller");
    if (std::string_view(level) == "error") {
        logger->error("{}", message);
    } else if (std::string_view(level) == "warn") {
        logger->warn("{}", message);
    } else {
        logger->info("{}", message);
    }
}

template <typename Component>
void stop_component_safely(Component* component, std::string* first_error = nullptr) {
    if (component == nullptr) {
        return;
    }
    try {
        component->stop();
    } catch (const std::exception& error) {
        if (first_error != nullptr && first_error->empty()) {
            *first_error = error.what();
        }
        log_live_controller_message("error", error.what());
    }
}

void store_controller_error(std::atomic<std::shared_ptr<const std::string>>* target, std::string error_message) {
    auto next = std::make_shared<std::string>(std::move(error_message));
    std::shared_ptr<const std::string> immutable = std::move(next);
    target->store(std::move(immutable), std::memory_order_release);
}

std::string load_controller_error(const std::atomic<std::shared_ptr<const std::string>>& source) {
    const std::shared_ptr<const std::string> current = source.load(std::memory_order_acquire);
    return current != nullptr ? *current : std::string{};
}

}  // namespace

struct LiveSessionController::Impl {
    explicit Impl(LiveSessionConfig config) : config_(std::move(config)) {
        if (config_.single_buffer_diagnostic.enabled) {
            config_.capture.v4l2_buffer_count = 1U;
            config_.capture.preview_buffer_count = std::min<std::uint32_t>(config_.capture.preview_buffer_count, 1U);
            config_.detect_slot_count = std::max<std::uint32_t>(1U, config_.detect_slot_count);
            config_.output_slot_count = std::max<std::uint32_t>(1U, config_.output_slot_count);
            config_.single_buffer_diagnostic.consecutive_miss_limit =
                std::max<std::uint32_t>(1U, config_.single_buffer_diagnostic.consecutive_miss_limit);
        }
        config_.capture.cuda_device_index = config_.cuda_device_index;
        pipeline_trace_ = std::make_shared<LivePipelineTrace>();
        if (config_.single_buffer_diagnostic.enabled) {
            pipeline_trace_->configure_single_buffer_diagnostic(
                config_.single_buffer_diagnostic.frame_budget_ns, config_.single_buffer_diagnostic.frame_count,
                config_.single_buffer_diagnostic.consecutive_miss_limit);
        }
        ingress_ = std::make_unique<LiveVideoIngress>(config_.capture, pipeline_trace_);
        fanout_ = std::make_unique<LiveFrameFanout>(*ingress_, ui_crop_state_, config_.detect_slot_count,
                                                    config_.output_slot_count, pipeline_trace_);
        if (!config_.single_buffer_diagnostic.enabled || !config_.single_buffer_diagnostic.disable_analyzer) {
            analyzer_worker_ = std::make_unique<LiveAnalyzerWorker>(
                *fanout_, config_.cuda_device_index, config_.capture.width, config_.capture.height,
                config_.analyzer_result_slot_count, config_.analyzer_overlay_slot_count);
        }
        manual_overlay_worker_ = std::make_unique<LiveManualOverlayWorker>(
            manual_overlay_document_, config_.cuda_device_index, config_.capture.width, config_.capture.height,
            config_.manual_overlay_slot_count);
        compositor_ = std::make_unique<LiveCompositor>(*fanout_, analyzer_worker_.get(), manual_overlay_worker_.get(),
                                                       config_.cuda_device_index, config_.capture.width,
                                                       config_.capture.height, config_.output_slot_count,
                                                       config_.raw_base_cache_slot_count, pipeline_trace_);
        compositor_->set_persistent_analysis_overlay(config_.persistent_analysis_overlay);
    }

    LiveSessionConfig config_{};
    UiCropState ui_crop_state_{};
    ManualOverlayDocument manual_overlay_document_{};
    std::unique_ptr<LiveVideoIngress> ingress_;
    std::shared_ptr<LivePipelineTrace> pipeline_trace_;
    std::unique_ptr<LiveFrameFanout> fanout_;
    std::unique_ptr<LiveAnalyzerWorker> analyzer_worker_;
    std::unique_ptr<LiveManualOverlayWorker> manual_overlay_worker_;
    std::unique_ptr<LiveCompositor> compositor_;
    std::atomic<bool> running_{false};
    std::atomic<bool> diagnostic_completion_stop_requested_{false};
    std::atomic<std::shared_ptr<const std::string>> startup_error_{std::make_shared<std::string>()};

    [[nodiscard]] std::string component_error(const bool trace_ingress_status_probe) const {
        const std::string startup_error = load_controller_error(startup_error_);
        if (!startup_error.empty()) {
            return startup_error;
        }
        if (pipeline_trace_ != nullptr) {
            const LivePipelineTraceSnapshot trace = pipeline_trace_->snapshot();
            if (trace.diagnostic_failed) {
                std::string message = "single-buffer live diagnostic missed frame budget: stage=";
                message += trace.diagnostic_failure_event.stage_name;
                message += " frame=";
                message += std::to_string(trace.diagnostic_failure_event.frame_id.sequence);
                message += " revision=";
                message += std::to_string(trace.diagnostic_failure_event.surface_revision);
                message += " latency_us=";
                message += std::to_string(trace.diagnostic_failure_event.latency_us);
                message += " budget_us=";
                message += std::to_string(trace.diagnostic_frame_budget_us);
                return message;
            }
        }
        if (compositor_ != nullptr) {
            const LiveCompositor::Status compositor_status = compositor_->snapshot_status();
            if (!compositor_status.last_error.empty()) {
                return compositor_status.last_error;
            }
        }
        if (analyzer_worker_ != nullptr) {
            const LiveAnalyzerWorker::Status analyzer_status = analyzer_worker_->snapshot_status();
            if (!analyzer_status.last_error.empty()) {
                return analyzer_status.last_error;
            }
        }
        if (manual_overlay_worker_ != nullptr) {
            const LiveManualOverlayWorker::Status manual_status = manual_overlay_worker_->snapshot_status();
            if (!manual_status.last_error.empty()) {
                return manual_status.last_error;
            }
        }
        if (fanout_ != nullptr) {
            const std::string fanout_error = fanout_->last_error();
            if (!fanout_error.empty()) {
                return fanout_error;
            }
        }
        if (ingress_ == nullptr) {
            return {};
        }
        return trace_ingress_status_probe ? ingress_->last_error() : ingress_->last_error_without_trace();
    }
};

LiveSessionController::LiveSessionController(LiveSessionConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

LiveSessionController::~LiveSessionController() {
    try {
        stop();
    } catch (const std::exception& error) {
        log_live_controller_message("error", error.what());
    }
}

void LiveSessionController::start() {
    if (impl_->running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("live session controller is already running");
    }

    if (impl_->pipeline_trace_ != nullptr) {
        impl_->pipeline_trace_->reset();
    }
    impl_->diagnostic_completion_stop_requested_.store(false, std::memory_order_release);
    store_controller_error(&impl_->startup_error_, {});
    impl_->ingress_->start();
    try {
        impl_->fanout_->start();
        if (impl_->analyzer_worker_ != nullptr) {
            impl_->analyzer_worker_->start();
        }
        impl_->manual_overlay_worker_->start();
        impl_->compositor_->start();
        impl_->compositor_->set_persistent_analysis_overlay(impl_->config_.persistent_analysis_overlay);
    } catch (...) {
        stop_component_safely(impl_->compositor_.get());
        stop_component_safely(impl_->manual_overlay_worker_.get());
        stop_component_safely(impl_->analyzer_worker_.get());
        stop_component_safely(impl_->fanout_.get());
        stop_component_safely(impl_->ingress_.get());
        throw;
    }

    impl_->running_.store(true, std::memory_order_release);
    const auto startup_deadline = std::chrono::steady_clock::now() + kFirstWorkspaceStartupDeadline;
    while (impl_->pipeline_trace_ != nullptr && !impl_->pipeline_trace_->first_workspace_publication_ready() &&
           std::chrono::steady_clock::now() < startup_deadline) {
        const std::string component_error = impl_->component_error(false);
        if (!component_error.empty()) {
            break;
        }
        std::this_thread::sleep_for(kFirstWorkspaceStartupPoll);
    }
    if (impl_->pipeline_trace_ != nullptr && !impl_->pipeline_trace_->first_workspace_publication_ready() &&
        impl_->component_error(false).empty()) {
        const LivePipelineTraceSnapshot trace = impl_->pipeline_trace_->snapshot();
        std::string message = "live pipeline failed to publish first workspace frame before startup deadline: stage=";
        message += trace.last_event.stage_name;
        message += " frame=";
        message += std::to_string(trace.last_event.frame_id.sequence);
        message += " cuda_device=";
        message += (trace.last_event.cuda_device_index == kLivePipelineUnknownCudaDevice
                        ? std::string("unknown")
                        : std::to_string(trace.last_event.cuda_device_index));
        message += " revision=";
        message += std::to_string(trace.last_event.surface_revision);
        message += " latency_us=";
        message += std::to_string(trace.last_event.latency_us);
        store_controller_error(&impl_->startup_error_, message);
        log_live_controller_message("error", message);
    }
    log_live_controller_message("info", "controller started");
}

void LiveSessionController::stop() {
    std::string stop_error;

    stop_component_safely(impl_->compositor_.get(), &stop_error);
    stop_component_safely(impl_->manual_overlay_worker_.get(), &stop_error);
    stop_component_safely(impl_->analyzer_worker_.get(), &stop_error);
    stop_component_safely(impl_->fanout_.get(), &stop_error);
    stop_component_safely(impl_->ingress_.get(), &stop_error);

    impl_->running_.store(false, std::memory_order_release);
    store_controller_error(&impl_->startup_error_, {});
    log_live_controller_message("info", "controller stopped");
    if (!stop_error.empty()) {
        throw std::runtime_error(stop_error);
    }
}

bool LiveSessionController::running() const noexcept {
    return impl_->running_.load(std::memory_order_acquire);
}

std::string LiveSessionController::last_error() const {
    return impl_->component_error(true);
}

LiveSessionStatus LiveSessionController::snapshot_status() const {
    LiveSessionStatus status;
    status.running = running();
    status.last_error = last_error();
    status.single_buffer_diagnostic = impl_->config_.single_buffer_diagnostic;
    if (impl_->analyzer_worker_ != nullptr) {
        status.analyzer = impl_->analyzer_worker_->snapshot_status();
    }
    if (impl_->manual_overlay_worker_ != nullptr) {
        status.manual = impl_->manual_overlay_worker_->snapshot_status();
    }
    if (impl_->fanout_ != nullptr) {
        status.fanout = impl_->fanout_->snapshot_status();
    }
    if (impl_->compositor_ != nullptr) {
        status.compositor = impl_->compositor_->snapshot_status();
    }
    if (impl_->pipeline_trace_ != nullptr) {
        attach_live_pipeline_status(status, *impl_->pipeline_trace_);
    }
    return status;
}

void LiveSessionController::record_pipeline_stage(const LivePipelineStage stage, const LiveFrameId& frame_id,
                                                  const std::uint64_t surface_revision,
                                                  const std::uint64_t latency_base_ns) noexcept {
    if (impl_->pipeline_trace_ == nullptr) {
        return;
    }
    impl_->pipeline_trace_->record(stage, frame_id, live_steady_clock_now_ns(), latency_base_ns,
                                   static_cast<std::uint32_t>(impl_->config_.cuda_device_index), surface_revision);
    if (stage != LivePipelineStage::CefPresentFrontSlot || !impl_->config_.single_buffer_diagnostic.enabled ||
        impl_->config_.single_buffer_diagnostic.frame_count == 0U || !impl_->running_.load(std::memory_order_acquire)) {
        return;
    }
    const LivePipelineTraceSnapshot trace = impl_->pipeline_trace_->snapshot();
    if (!trace.diagnostic_completed ||
        impl_->diagnostic_completion_stop_requested_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        log_live_controller_message("info", "single-buffer live diagnostic reached frame limit; stopping controller");
        stop();
    } catch (const std::exception& error) {
        try {
            store_controller_error(&impl_->startup_error_, error.what());
            log_live_controller_message("error", error.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            store_controller_error(&impl_->startup_error_, "unknown single-buffer diagnostic stop failure");
            log_live_controller_message("error", "unknown single-buffer diagnostic stop failure");
        } catch (...) {
        }
    }
}

WorkspaceSwapchainDescriptor LiveSessionController::workspace_swapchain_descriptor() const {
    return impl_->compositor_ != nullptr ? impl_->compositor_->workspace_swapchain_descriptor()
                                         : WorkspaceSwapchainDescriptor{};
}

WorkspacePresentSnapshot LiveSessionController::latest_workspace_present() const noexcept {
    return impl_->compositor_ != nullptr ? impl_->compositor_->latest_workspace_present() : WorkspacePresentSnapshot{};
}

bool LiveSessionController::try_acquire_latest_workspace(WorkspaceOutputBundle* out) {
    return impl_->compositor_ != nullptr && impl_->compositor_->try_acquire_latest_workspace(out);
}

void LiveSessionController::release_workspace(std::uint32_t slot_index) {
    if (!impl_->compositor_) {
        throw std::runtime_error("live session controller has no compositor to release workspace from");
    }
    impl_->compositor_->release_workspace(slot_index);
}

void LiveSessionController::set_workspace_ready_callback(LiveCompositor::WorkspaceReadyCallback callback) {
    if (impl_->compositor_) {
        impl_->compositor_->set_workspace_ready_callback(std::move(callback));
    }
}

void LiveSessionController::set_workspace_present_callback(LiveCompositor::WorkspacePresentCallback callback) {
    if (impl_->compositor_) {
        impl_->compositor_->set_workspace_present_callback(std::move(callback));
    }
}

void LiveSessionController::mark_workspace_slot_in_flight(const std::uint32_t slot_index, const bool in_flight) {
    if (!impl_->compositor_) {
        throw std::runtime_error("live session controller has no compositor to mark workspace in-flight");
    }
    impl_->compositor_->mark_workspace_slot_in_flight(slot_index, in_flight);
}

bool LiveSessionController::try_acquire_latest_detect(DetectBundle* out) {
    return impl_->fanout_ != nullptr && impl_->fanout_->try_acquire_detect(out);
}

void LiveSessionController::release_detect(std::uint32_t slot_index) {
    if (!impl_->fanout_) {
        throw std::runtime_error("live session controller has no fanout to release detect output from");
    }
    impl_->fanout_->release_detect(slot_index);
}

UiCropState& LiveSessionController::ui_crop_state() noexcept {
    return impl_->ui_crop_state_;
}

const UiCropState& LiveSessionController::ui_crop_state() const noexcept {
    return impl_->ui_crop_state_;
}

ManualOverlayDocument& LiveSessionController::manual_overlay_document() noexcept {
    return impl_->manual_overlay_document_;
}

const ManualOverlayDocument& LiveSessionController::manual_overlay_document() const noexcept {
    return impl_->manual_overlay_document_;
}

void LiveSessionController::attach_analyzer(std::unique_ptr<FrameAnalyzer> analyzer) {
    if (!impl_->analyzer_worker_) {
        throw std::runtime_error("live session controller has no analyzer worker");
    }
    impl_->analyzer_worker_->set_analyzer(std::move(analyzer));
}

void LiveSessionController::clear_analyzer() {
    if (impl_->analyzer_worker_) {
        impl_->analyzer_worker_->set_analyzer(nullptr);
    }
}

void LiveSessionController::set_persistent_analysis_overlay(const bool enabled) {
    impl_->config_.persistent_analysis_overlay = enabled;
    if (impl_->compositor_) {
        impl_->compositor_->set_persistent_analysis_overlay(enabled);
    }
}

bool LiveSessionController::persistent_analysis_overlay() const noexcept {
    return impl_->compositor_ != nullptr && impl_->compositor_->persistent_analysis_overlay();
}

bool LiveSessionController::readback_raw_base(const LiveFrameId& frame_id, std::vector<std::uint8_t>* pixels_bgr,
                                              std::uint32_t* width, std::uint32_t* height, LiveCaptureRegion* region,
                                              std::string* error_message) {
    if (!impl_->compositor_) {
        if (error_message != nullptr) {
            *error_message = "live session controller has no compositor for raw-base readback";
        }
        return false;
    }
    return impl_->compositor_->readback_raw_base(frame_id, pixels_bgr, width, height, region, error_message);
}

LiveVideoIngress& LiveSessionController::ingress() noexcept {
    return *impl_->ingress_;
}

const LiveVideoIngress& LiveSessionController::ingress() const noexcept {
    return *impl_->ingress_;
}

LiveFrameFanout& LiveSessionController::fanout() noexcept {
    return *impl_->fanout_;
}

const LiveFrameFanout& LiveSessionController::fanout() const noexcept {
    return *impl_->fanout_;
}

LiveAnalyzerWorker* LiveSessionController::analyzer_worker() noexcept {
    return impl_->analyzer_worker_.get();
}

const LiveAnalyzerWorker* LiveSessionController::analyzer_worker() const noexcept {
    return impl_->analyzer_worker_.get();
}

LiveManualOverlayWorker* LiveSessionController::manual_overlay_worker() noexcept {
    return impl_->manual_overlay_worker_.get();
}

const LiveManualOverlayWorker* LiveSessionController::manual_overlay_worker() const noexcept {
    return impl_->manual_overlay_worker_.get();
}

LiveCompositor& LiveSessionController::compositor() noexcept {
    return *impl_->compositor_;
}

const LiveCompositor& LiveSessionController::compositor() const noexcept {
    return *impl_->compositor_;
}

}  // namespace mmltk::live
