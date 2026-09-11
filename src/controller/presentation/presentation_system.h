#pragma once

#include <sys/types.h>

#include <cstdint>
#include <cstddef>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>

#include "src/controller/contracts/application_boundary.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/frameworks/gpu/image_buffer.h"

namespace mmltk::controller {

struct VisualSourceReader final {
    PresentationSourceIdentity source{};
    std::function<VisualSourceObservation()> observe;
    std::function<mmltk::frameworks::gpu::BorrowedImageProductReadView()> borrow;
    std::function<mmltk::frameworks::gpu::ImageWorkspaceObservation()> observe_workspace;
    std::function<mmltk::frameworks::gpu::BorrowedImageWorkspace()> borrow_workspace;
    std::function<void(VisualWorkspaceRequest)> request_workspace;
};
struct RendererObservation final {
    std::uint64_t completed_sample = 0U;
    bool redraw_requested = false;
};
enum class PresentationCapabilityCondition : std::uint8_t {
    Unavailable,
    Admitted,
    Ready,
};
MMLTK_REFLECT_ENUM(PresentationCapabilityCondition)
struct PresentationCapability final {
    std::uint64_t surface_high = 0U;
    std::uint64_t surface_low = 0U;
    VisualExtent extent{};
    std::uint64_t generation = 0U;
    PresentationCapabilityCondition condition = PresentationCapabilityCondition::Unavailable;

    [[nodiscard]] bool valid() const noexcept {
        return (surface_high != 0U || surface_low != 0U) && extent.valid() && generation != 0U &&
               condition != PresentationCapabilityCondition::Unavailable;
    }
    bool operator==(const PresentationCapability&) const = default;
};
struct PresentationSnapshot final {
    std::uint64_t revision = 0U;
    PresentationSourceIdentity selected{};
    VisualFrame completed{};
    std::uint64_t completed_source_revision = 0U;
    std::uint64_t timeline_ready = 0U;
    std::uint64_t presentation_revision = 0U;
    PresentationCapability capability{};
    std::uint64_t browser_completed_sample = 0U;
};
struct PresentationPublication final {
    PresentationCapability capability{};
    std::uint64_t timeline_ready = 0U;
    std::uint64_t presentation_revision = 0U;
    std::uint64_t transfer_sequence = 0U;

    [[nodiscard]] bool valid() const noexcept {
        return capability.valid() && capability.condition == PresentationCapabilityCondition::Ready && timeline_ready != 0U &&
               presentation_revision != 0U;
    }
};
struct PresentationSubmittedSource final {
    VisualSourceObservation observation{};
    std::uint64_t selection_generation = 0U;

    [[nodiscard]] bool valid() const noexcept { return observation.valid() && selection_generation != 0U; }
    bool operator==(const PresentationSubmittedSource&) const = default;
};
// One immutable operation projection. An unissued copy carries only capability
// and submission; a physical publication also carries its exact transfer.
struct PresentationDiagnosticRecord final {
    PresentationSubmittedSource submitted{};
    PresentationPublication publication{};
    contracts::DiagnosticLink link{};
};
[[nodiscard]] VisualDiagnosticFact presentation_diagnostic_fact(VisualDiagnosticOperation, const PresentationDiagnosticRecord&, int device,
                                                                std::uint64_t outcome = 0U) noexcept;
enum class PresentationNativeProgress : std::uint8_t {
    Waiting,
    Superseded,
    Published,
};
struct PresentationNativeOutcome final {
    PresentationNativeProgress progress = PresentationNativeProgress::Waiting;
    PresentationSubmittedSource submitted{};
    PresentationPublication publication{};
    PresentationCapability capability{};
    contracts::DiagnosticLink diagnostic_link{};
};
enum class PresentationShutdownResult : std::uint8_t {
    Stopped,
    BrowserTerminalRequired,
};
class PresentationNativeWriter {
   public:
    struct Retirement final {
        bool all_released = true;
        bool safe_to_destroy = true;
    };
    virtual ~PresentationNativeWriter() = default;
    virtual void Submit(PresentationSubmittedSource, const VisualSourceReader&) = 0;
    [[nodiscard]] virtual PresentationNativeOutcome Pump(std::uint64_t current_selection_generation) = 0;
    [[nodiscard]] virtual int poll_fd() const noexcept = 0;
    [[nodiscard]] virtual int completion_fd() const noexcept = 0;
    [[nodiscard]] virtual bool wants_write() const noexcept = 0;
    virtual void SetApplicationPeerConnected(bool) noexcept = 0;
    virtual void SetExpectedBrowserProcessGroup(pid_t) = 0;
    [[nodiscard]] virtual Retirement BrowserPeerLost() noexcept = 0;
};
using PresentationNativeWriterFactory = std::function<std::unique_ptr<PresentationNativeWriter>()>;
struct PresentationNativeConfiguration final {
    std::filesystem::path import_socket;
    std::size_t minimum_allocation_bytes = 0U;
    bool pending_supersession_acceptance = false;
};
[[nodiscard]] PresentationNativeWriterFactory make_native_presentation_writer_factory(VisualDeviceSettings, PresentationNativeConfiguration,
                                                                                      VisualDiagnosticSink = {});
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::LatestState}]] PresentationCompleted final {
    // CLEANUP-IGNORE: Presentation completion has its own reflected identity and durable state-delivery contract.
    PresentationSnapshot snapshot{};
};  // CLEANUP-IGNORE: Presentation event identities are distinct from Annotation's frame and failure events.
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] PresentationCapabilityChanged final {
    PresentationSnapshot snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] PresentationFailed final {
    PresentationSnapshot snapshot{};
    [[= mmltk::frameworks::reflection::MaxBytes{kVisualFailureByteCapacity}]] std::string detail;
};

class PresentationSystem final {
   public:
    using event_type = std::variant<PresentationCompleted, PresentationCapabilityChanged, PresentationFailed>;
    PresentationSystem(VisualDeviceSettings, PresentationNativeWriterFactory, std::span<const VisualSourceReader>,
                       SystemEventSink<event_type> = {}, VisualDiagnosticSink = {});
    ~PresentationSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] PresentationSnapshot Select(PresentationSourceIdentity);
    void Observe(RendererObservation);
    void SourceChanged(PresentationSourceIdentity) noexcept;
    void SetApplicationPeerConnected(bool) noexcept;
    void SetExpectedBrowserProcessGroup(pid_t);
    void BrowserPeerLost() noexcept;
    void CloseAdmission() noexcept;
    [[nodiscard]] PresentationShutdownResult Stop() noexcept;
    [[nodiscard]] PresentationShutdownResult Shutdown() noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] PresentationSnapshot snapshot() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

MMLTK_REFLECT_FIELDS(RendererObservation)
MMLTK_REFLECT_FIELDS(PresentationCapability)
MMLTK_REFLECT_FIELDS(PresentationSnapshot)
MMLTK_REFLECT_FIELDS(PresentationCompleted)
MMLTK_REFLECT_FIELDS(PresentationCapabilityChanged)
MMLTK_REFLECT_FIELDS(PresentationFailed)

}  // namespace mmltk::controller
