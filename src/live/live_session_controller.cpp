#include "mmltk/live/live_session_controller.h"

#include "mmltk_logging.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace mmltk::live {

namespace {

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

} // namespace

LiveSessionController::LiveSessionController(LiveSessionConfig config)
    : config_(std::move(config)) {
    config_.capture.cuda_device_index = config_.cuda_device_index;
    ingress_ = std::make_unique<LiveVideoIngress>(config_.capture);
    fanout_ = std::make_unique<LiveFrameFanout>(*ingress_,
                                                ui_crop_state_,
                                                config_.detect_slot_count,
                                                config_.output_slot_count);
    analyzer_worker_ = std::make_unique<LiveAnalyzerWorker>(*fanout_,
                                                            config_.cuda_device_index,
                                                            config_.capture.width,
                                                            config_.capture.height,
                                                            config_.analyzer_result_slot_count,
                                                            config_.analyzer_overlay_slot_count);
    manual_overlay_worker_ = std::make_unique<LiveManualOverlayWorker>(manual_overlay_document_,
                                                                       config_.cuda_device_index,
                                                                       config_.capture.width,
                                                                       config_.capture.height,
                                                                       config_.manual_overlay_slot_count);
    compositor_ = std::make_unique<LiveCompositor>(*fanout_,
                                                   analyzer_worker_.get(),
                                                   manual_overlay_worker_.get(),
                                                   config_.cuda_device_index,
                                                   config_.capture.width,
                                                   config_.capture.height,
                                                   config_.output_slot_count,
                                                   config_.raw_base_cache_slot_count);
    compositor_->set_persistent_analysis_overlay(config_.persistent_analysis_overlay);
}

LiveSessionController::~LiveSessionController() {
    try {
        stop();
    } catch (const std::exception& error) {
        log_live_controller_message("error", error.what());
    }
}

void LiveSessionController::start() {
    if (running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("live session controller is already running");
    }

    ingress_->start();
    try {
        fanout_->start();
        analyzer_worker_->start();
        manual_overlay_worker_->start();
        compositor_->start();
        compositor_->set_persistent_analysis_overlay(config_.persistent_analysis_overlay);
    } catch (...) {
        stop_component_safely(compositor_.get());
        stop_component_safely(manual_overlay_worker_.get());
        stop_component_safely(analyzer_worker_.get());
        stop_component_safely(fanout_.get());
        stop_component_safely(ingress_.get());
        throw;
    }

    running_.store(true, std::memory_order_release);
    log_live_controller_message("info", "controller started");
}

void LiveSessionController::stop() {
    std::string stop_error;

    stop_component_safely(compositor_.get(), &stop_error);
    stop_component_safely(manual_overlay_worker_.get(), &stop_error);
    stop_component_safely(analyzer_worker_.get(), &stop_error);
    stop_component_safely(fanout_.get(), &stop_error);
    stop_component_safely(ingress_.get(), &stop_error);

    running_.store(false, std::memory_order_release);
    log_live_controller_message("info", "controller stopped");
    if (!stop_error.empty()) {
        throw std::runtime_error(stop_error);
    }
}

bool LiveSessionController::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::string LiveSessionController::last_error() const {
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
    return ingress_ != nullptr ? ingress_->last_error() : std::string{};
}

LiveSessionStatus LiveSessionController::snapshot_status() const {
    LiveSessionStatus status;
    status.running = running();
    status.last_error = last_error();
    if (analyzer_worker_ != nullptr) {
        status.analyzer = analyzer_worker_->snapshot_status();
    }
    if (manual_overlay_worker_ != nullptr) {
        status.manual = manual_overlay_worker_->snapshot_status();
    }
    if (compositor_ != nullptr) {
        status.compositor = compositor_->snapshot_status();
    }
    return status;
}

bool LiveSessionController::try_acquire_latest_output(OutputBundle* out) {
    return compositor_ != nullptr && compositor_->try_acquire_latest_output(out);
}

void LiveSessionController::release_output(std::uint32_t slot_index) {
    if (!compositor_) {
        throw std::runtime_error("live session controller has no fanout to release output from");
    }
    compositor_->release_output(slot_index);
}

bool LiveSessionController::try_acquire_latest_detect(DetectBundle* out) {
    return fanout_ != nullptr && fanout_->try_acquire_detect(out);
}

void LiveSessionController::release_detect(std::uint32_t slot_index) {
    if (!fanout_) {
        throw std::runtime_error("live session controller has no fanout to release detect output from");
    }
    fanout_->release_detect(slot_index);
}

UiCropState& LiveSessionController::ui_crop_state() noexcept {
    return ui_crop_state_;
}

const UiCropState& LiveSessionController::ui_crop_state() const noexcept {
    return ui_crop_state_;
}

ManualOverlayDocument& LiveSessionController::manual_overlay_document() noexcept {
    return manual_overlay_document_;
}

const ManualOverlayDocument& LiveSessionController::manual_overlay_document() const noexcept {
    return manual_overlay_document_;
}

void LiveSessionController::attach_analyzer(std::unique_ptr<FrameAnalyzer> analyzer) {
    if (!analyzer_worker_) {
        throw std::runtime_error("live session controller has no analyzer worker");
    }
    analyzer_worker_->set_analyzer(std::move(analyzer));
}

void LiveSessionController::clear_analyzer() {
    if (analyzer_worker_) {
        analyzer_worker_->set_analyzer(nullptr);
    }
}

void LiveSessionController::set_persistent_analysis_overlay(const bool enabled) {
    config_.persistent_analysis_overlay = enabled;
    if (compositor_) {
        compositor_->set_persistent_analysis_overlay(enabled);
    }
}

bool LiveSessionController::persistent_analysis_overlay() const noexcept {
    return compositor_ != nullptr && compositor_->persistent_analysis_overlay();
}

bool LiveSessionController::readback_raw_base(const LiveFrameId& frame_id,
                                              std::vector<std::uint8_t>* pixels_bgr,
                                              std::uint32_t* width,
                                              std::uint32_t* height,
                                              LiveCaptureRegion* region,
                                              std::string* error_message) {
    if (!compositor_) {
        if (error_message != nullptr) {
            *error_message = "live session controller has no compositor for raw-base readback";
        }
        return false;
    }
    return compositor_->readback_raw_base(frame_id, pixels_bgr, width, height, region, error_message);
}

LiveVideoIngress& LiveSessionController::ingress() noexcept {
    return *ingress_;
}

const LiveVideoIngress& LiveSessionController::ingress() const noexcept {
    return *ingress_;
}

LiveFrameFanout& LiveSessionController::fanout() noexcept {
    return *fanout_;
}

const LiveFrameFanout& LiveSessionController::fanout() const noexcept {
    return *fanout_;
}

LiveAnalyzerWorker* LiveSessionController::analyzer_worker() noexcept {
    return analyzer_worker_.get();
}

const LiveAnalyzerWorker* LiveSessionController::analyzer_worker() const noexcept {
    return analyzer_worker_.get();
}

LiveManualOverlayWorker* LiveSessionController::manual_overlay_worker() noexcept {
    return manual_overlay_worker_.get();
}

const LiveManualOverlayWorker* LiveSessionController::manual_overlay_worker() const noexcept {
    return manual_overlay_worker_.get();
}

LiveCompositor& LiveSessionController::compositor() noexcept {
    return *compositor_;
}

const LiveCompositor& LiveSessionController::compositor() const noexcept {
    return *compositor_;
}

} // namespace mmltk::live
