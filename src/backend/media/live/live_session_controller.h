#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/frameworks/gpu/resource_owner_command_authority.h"
#include "src/backend/media/live/live_types.h"
#include "src/backend/media/live/manual_overlay_document.h"
#include "src/backend/media/capture/capture_session.h"
#include "src/backend/media/capture/capture_types.h"
#include "src/backend/media/capture/live_video_source.h"
#include "src/backend/media/capture/status.h"
namespace mmltk::backend::media::live {
struct LiveDataPlaneConfig final {
    mmltk::backend::media::capture::CaptureConfig capture{};
    mmltk::frameworks::gpu::ResourceOwnerWorkerCapability resource_worker{};
    std::uint32_t ingress_slots = 3U;
    std::uint32_t fanout_slots = 3U;
    std::uint32_t analysis_slots = 2U;
    std::uint32_t composite_slots = 3U;
    std::uint32_t analysis_regions = 1U;
    std::uint32_t manual_overlay_slots = 2U;
    std::uint32_t maximum_manual_instances = 256U;
    LiveManualOverlayUploadLimits manual_overlay_uploads{};
};
struct LiveRevisionListener final {
    void* context = nullptr;
    void (*notify)(void*) noexcept = nullptr;
    [[nodiscard]] bool valid() const noexcept { return context != nullptr && notify != nullptr; }
    void operator()() const noexcept {
        if (valid()) notify(context);
    }
};
class LivePhysicalTerminal final {
   public:
    LivePhysicalTerminal(std::shared_ptr<const mmltk::backend::media::capture::CaptureStopTerminal> capture,
                         mmltk::backend::media::capture::Status status) noexcept
        : capture_(std::move(capture)), status_(std::move(status)) {}
    [[nodiscard]] bool valid() const noexcept { return capture_ != nullptr && capture_->identity.valid(); }
    [[nodiscard]] bool fully_settled() const noexcept { return valid() && status_.ok(); }
    [[nodiscard]] const mmltk::backend::media::capture::CaptureStopTerminal& capture_terminal() const noexcept { return *capture_; }
    [[nodiscard]] const mmltk::backend::media::capture::Status& status() const noexcept { return status_; }

   private:
    const std::shared_ptr<const mmltk::backend::media::capture::CaptureStopTerminal> capture_;
    const mmltk::backend::media::capture::Status status_;
};
// Only a run that acquired capture custody can publish this asynchronous
// terminal. A no-custody start failure is returned synchronously by start().
struct LiveAdmittedRunTerminalListener final {
    void* context = nullptr;
    void (*notify)(void*, const LivePhysicalTerminal&) noexcept = nullptr;
    [[nodiscard]] bool valid() const noexcept { return context != nullptr && notify != nullptr; }
    void operator()(const LivePhysicalTerminal& terminal) const noexcept {
        if (valid()) notify(context, terminal);
    }
};
struct LiveAnalysisProviderFactory final {
    const void* context = nullptr;
    std::shared_ptr<mmltk::backend::ml::runtime::AnalysisProvider> (*create)(const void*, mmltk::backend::ml::runtime::BorrowedCommandStream) = nullptr;
    [[nodiscard]] bool valid() const noexcept { return context != nullptr && create != nullptr; }
    [[nodiscard]] std::shared_ptr<mmltk::backend::ml::runtime::AnalysisProvider> operator()(mmltk::backend::ml::runtime::BorrowedCommandStream stream) const {
        return valid() ? create(context, stream) : nullptr;
    }
};
// One physical Live data-plane owner. Logical application session facts and
// presentation selection remain outside this component.
class LiveMediaDataPlane final {
   public:
    explicit LiveMediaDataPlane(LiveDataPlaneConfig config);
    ~LiveMediaDataPlane();
    LiveMediaDataPlane(const LiveMediaDataPlane&) = delete;
    LiveMediaDataPlane& operator=(const LiveMediaDataPlane&) = delete;
    [[nodiscard]] mmltk::backend::media::capture::CaptureSessionStartResult start(LiveAnalysisProviderFactory provider_factory = {});
    [[nodiscard]] mmltk::backend::media::capture::Status stop() noexcept;
    [[nodiscard]] std::shared_ptr<const LivePhysicalTerminal> admitted_run_terminal() const noexcept;
    [[nodiscard]] mmltk::backend::media::capture::Status first_failure() const;
    [[nodiscard]] std::optional<PhysicalFrameRevision> newest_revision() const noexcept;
    [[nodiscard]] bool try_acquire_output(PhysicalFrameRevision revision, LiveCompositeOutputLease* output);
    [[nodiscard]] bool request_raw_readback(LiveRawFrameReadback request);
    void set_revision_listener(LiveRevisionListener listener);
    void set_admitted_run_terminal_listener(LiveAdmittedRunTerminalListener listener);
    void publish_manual_overlay(ManualOverlayDocumentSnapshot snapshot);
    void clear_manual_overlay(std::uint32_t width, std::uint32_t height);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::media::live
