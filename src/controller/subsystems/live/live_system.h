#pragma once

#include "src/controller/presentation/visual_source_projection.h"

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <variant>

#include "src/controller/contracts/application_boundary.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/frameworks/gpu/image_types.h"
#include "src/frameworks/gpu/system_image_model.h"

namespace mmltk::controller {

struct LiveStart final {
    VisualExtent extent{};
    std::uint32_t frames_per_second = 30U;
};
struct LiveNativeConfiguration final {
    std::string capture_device{"/dev/video0"};
    std::uint32_t capture_width = 1920U;
    std::uint32_t capture_height = 1080U;
    std::uint32_t capture_fps = 120U;
    std::uint32_t capture_buffers = 4U;
    std::uint32_t ingress_slots = 3U;
    std::uint32_t fanout_slots = 3U;
    std::uint32_t analysis_slots = 2U;
    std::uint32_t composite_slots = 3U;
    std::uint32_t manual_overlay_slots = 2U;
    std::uint32_t maximum_manual_instances = 256U;
};
class LiveAlgorithm : public mmltk::frameworks::gpu::SystemImageModel {
   public:
    ~LiveAlgorithm() override = default;
    virtual void Start(const LiveStart&) = 0;
    virtual void SetOutputAvailableSink(std::function<void()>) = 0;
    [[nodiscard]] virtual bool AcquireOutput() = 0;
    [[nodiscard]] virtual bool Capture(mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t stream, std::stop_token) = 0;
    virtual void Stop() noexcept = 0;
};
struct LiveSnapshot final {
    std::uint64_t revision = 0U;
    bool running = false;
    bool cancellation_requested = false;
    std::uint64_t completed_frames = 0U;
    VisualFrame frame{};
    // CLEANUP-IGNORE: LiveSnapshot is a distinct reflected snapshot even where its closing shape matches peers.
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] LiveFrameCompleted final {
    // CLEANUP-IGNORE: Frame completion is a distinct transient event in the generated Live vocabulary.
    LiveSnapshot snapshot{};
    // CLEANUP-IGNORE: The transient Live event terminator intentionally mirrors other one-snapshot events.
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] LiveChanged final {
    LiveSnapshot snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] LiveFailed final {
    LiveSnapshot snapshot{};
    [[= mmltk::frameworks::reflection::MaxBytes{kVisualFailureByteCapacity}]] std::string detail;
};

[[nodiscard]] VisualRuntimeFactory make_native_live_runtime_factory(VisualDeviceSettings, LiveNativeConfiguration = {});

class LiveSystem final {
   public:
    using visual_source =
        VisualSourceProjection<LiveSnapshot, PresentationSourceKind::Live, mmltk::frameworks::reflection::member_path<&LiveSnapshot::frame>,
                               mmltk::frameworks::reflection::member_path<&LiveSnapshot::revision>>;
    using event_type = std::variant<LiveFrameCompleted, LiveChanged, LiveFailed>;
    LiveSystem(VisualDeviceSettings, VisualRuntimeFactory, SystemEventSink<event_type> = {}, VisualDiagnosticSink = {});
    ~LiveSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] LiveSnapshot Start(LiveStart);
    // CLEANUP-IGNORE: Live Stop is a distinct reflected endpoint with Live-specific cancellation semantics.
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] LiveSnapshot Stop() noexcept;
    void Shutdown() noexcept;
    // CLEANUP-IGNORE: The sealed Live facade exposes ordinary lifecycle observation without sharing implementation.
    [[nodiscard]] bool stopped() const noexcept;
    // CLEANUP-IGNORE: The Live snapshot and adjacent read API belong to this sealed domain, with shared execution in VisualRuntimeOwner.
    [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] LiveSnapshot snapshot() const;
    [[nodiscard]] std::optional<VisualImageMetadata> ImageSnapshot(const VisualFrame&) const;
    // CLEANUP-IGNORE: Live exposes raw and display reads through its own public API; shared implementation remains private.
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageWorkspace BorrowWorkspace() const;
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceObservation ObserveWorkspace() const;
    void RequestWorkspace(VisualWorkspaceRequest);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

MMLTK_REFLECT_FIELDS(LiveStart)
MMLTK_REFLECT_FIELDS(LiveSnapshot)
MMLTK_REFLECT_FIELDS(LiveFrameCompleted)
MMLTK_REFLECT_FIELDS(LiveChanged)
MMLTK_REFLECT_FIELDS(LiveFailed)

}  // namespace mmltk::controller
