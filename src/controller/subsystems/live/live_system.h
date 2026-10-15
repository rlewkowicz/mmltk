#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
#include "src/backend/media/live/live_analysis_product.h"
#include "src/controller/contracts/workspace_input.h"
#include "src/controller/presentation/visual_source_projection.h"
#include "src/controller/presentation/visual_document.h"
#include <chrono>
#include "src/backend/media/capture/capture_session.h"
#include <memory>
#include <stop_token>
#include <string>
#include <variant>
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/frameworks/gpu/image/image_types.h"
#include "src/frameworks/gpu/image/image_buffer.h"
#include "src/frameworks/gpu/runtime/system_image_model.h"
namespace mmltk::controller {
class SettingsSystem;
class ModelSystem;
struct LiveAnalysisConfiguration;
struct LiveStart final {
 VisualExtent extent{};
 std::uint32_t frames_per_second = 30U;
};
struct LiveNativeConfiguration final {
 std::string capture_device{"/dev/video0"};
 mmltk::backend::media::capture::CaptureDeviceApi capture_api{};
 std::shared_ptr<void> capture_custody;
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
 virtual void Start(const LiveStart&, std::shared_ptr<const LiveAnalysisConfiguration>, std::stop_token) = 0;
 virtual void SetOutputAvailableSink(std::function<void()>) = 0;
 [[nodiscard]] virtual bool AcquireOutput() = 0;
 // Success replaces all active pixels and analysis, and settles receiver reads before returning.
 [[nodiscard]] virtual bool Capture(
  mmltk::frameworks::gpu::ImagePlaneView target, mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t stream, mmltk::backend::media::live::LiveAnalysisProduct&, std::stop_token) = 0;
 virtual void RenderAnalysis(mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, const mmltk::backend::media::live::LiveAnalysisProduct&) {}
 [[nodiscard]] virtual mmltk::backend::media::capture::Status Stop() noexcept = 0;
};
// A read retains the exact image allocation and its immutable native meaning.
// The product cannot escape separately and cannot be recycled while this read lives.
class LiveAnalysisRead final {
public:
 LiveAnalysisRead() noexcept = default;
 LiveAnalysisRead(LiveAnalysisRead&&) noexcept = default;
 LiveAnalysisRead& operator=(LiveAnalysisRead&&) noexcept = default;
 [[nodiscard]] bool valid() const noexcept { return image_.valid() && analysis_ && analysis_->ready(); }
 [[nodiscard]] const VisualFrame& frame() const noexcept { return frame_; }
 [[nodiscard]] const mmltk::backend::media::live::LiveAnalysisProduct& analysis() const { return *analysis_; }
 [[nodiscard]] const mmltk::frameworks::gpu::BorrowedImageProductReadView& image() const noexcept { return image_; }

private:
 LiveAnalysisRead(VisualFrame, mmltk::frameworks::gpu::BorrowedImageProductReadView, std::shared_ptr<const mmltk::backend::media::live::LiveAnalysisProduct>) noexcept;
 VisualFrame frame_{};
 std::shared_ptr<const mmltk::backend::media::live::LiveAnalysisProduct> analysis_;
 mmltk::frameworks::gpu::BorrowedImageProductReadView image_;
 friend class LiveSystem;
};
struct LiveSnapshot final {
 std::uint64_t revision = 0U;
 bool running = false;
 bool cancellation_requested = false;
 std::uint64_t completed_frames = 0U;
 // CLEANUP-IGNORE: Live's frame and typed events are independent declarations, not shared execution.
 VisualFrame frame{};
 VisualDocumentFacts document{};
 // CLEANUP-IGNORE: LiveSnapshot is a distinct reflected snapshot even where its closing shape matches peers.
};
struct LiveImageMetadata final {
 VisualFrame frame{};
 VisualDocumentFacts document{};
 bool analyzed = false;
 MMLTK_MAX_ITEMS(contracts::kAnnotationObjectCapacity) std::vector<VisualObjectLabel> labels {};
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
 MMLTK_MAX_BYTES(kVisualFailureByteCapacity) std::string detail;
};
// CLEANUP-IGNORE: Live runtime construction, input endpoint, and source projection are independent canonical domain declarations.
[[nodiscard]] VisualRuntimeFactory make_native_live_runtime_factory(VisualDeviceSettings, LiveNativeConfiguration = {});
class LiveSystem final {
public:
 [[= contracts::reflection::direct::InteractionEndpoint{}]] void Input(WorkspaceMouse);
 void SetInputPeer(std::uint64_t);
 using visual_source = VisualSourceProjection<LiveSnapshot, PresentationSourceKind::Live, mmltk::frameworks::reflection::member_path<&LiveSnapshot::frame>,
  mmltk::frameworks::reflection::member_path<&LiveSnapshot::revision>, LiveImageMetadata>;
 using event_type = std::variant<LiveFrameCompleted, LiveChanged, LiveFailed>;
 LiveSystem(VisualDeviceSettings, VisualRuntimeFactory, SystemEventSink<event_type> = {}, VisualDiagnosticSink = {}, SettingsSystem* = nullptr, ModelSystem* = nullptr);
 ~LiveSystem();
 [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] LiveSnapshot Start(LiveStart);
 // CLEANUP-IGNORE: Live Stop is a distinct reflected endpoint with Live-specific cancellation semantics.
 [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] LiveSnapshot Stop() noexcept;
 void Shutdown() noexcept;
 // CLEANUP-IGNORE: The sealed Live facade exposes ordinary lifecycle observation without sharing implementation.
 [[nodiscard]] bool stopped() const noexcept;
 // CLEANUP-IGNORE: The Live snapshot and adjacent read API belong to this sealed domain, with shared execution in VisualRuntimeOwner.
 [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] LiveSnapshot snapshot() const;
 // CLEANUP-IGNORE: Live exposes a sealed image and workspace API; the existing private runtime already supplies shared behavior.
 [[nodiscard]] std::optional<LiveImageMetadata> ImageSnapshot(const VisualFrame&) const;
 [[nodiscard]] VisualDocumentRead BorrowDocument(const VisualFrame&) const;
 // CLEANUP-IGNORE: Live exposes raw and display reads through its own public API; shared implementation remains private.
 [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
 [[nodiscard]] LiveAnalysisRead ReadAnalysis(const VisualFrame&) const;
 [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageWorkspace BorrowWorkspace() const;
 [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceObservation ObserveWorkspace() const;
 void RequestWorkspace(VisualWorkspaceRequest);

private:
 class Impl;
 std::unique_ptr<Impl> impl_;
};
MMLTK_REFLECT_FIELDS(LiveStart)
MMLTK_REFLECT_FIELDS(LiveSnapshot)
MMLTK_REFLECT_FIELDS(LiveImageMetadata)
MMLTK_REFLECT_FIELDS(LiveFrameCompleted)
MMLTK_REFLECT_FIELDS(LiveChanged)
MMLTK_REFLECT_FIELDS(LiveFailed)
}  // namespace mmltk::controller
