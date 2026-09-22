#pragma once
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/contracts/annotation.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/contracts/workspace_input.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_source_projection.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/image_product_pool.h"
#include "src/frameworks/gpu/image_workspace.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
namespace mmltk::controller {
struct PredictLabel final {
 contracts::AnnotationBox box{};
 int class_reference = 0;
 mmltk::backend::data::catalog::ClassReferenceDomain class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::Foreground;
 float confidence = 0.0F;
 contracts::AnnotationColor color{};
 [[= mmltk::frameworks::reflection::MaxBytes{256U}]] std::string name;
};
namespace detail {
class PredictionPreviewFrame;
}
class PredictRuntime {
public:
 struct Product final {
  VisualExtent extent{};
  std::shared_ptr<const detail::PredictionPreviewFrame> raw;
  std::int64_t image_id = 0;
  std::int64_t source_index = 0;
 };
 using ProductSink = std::function<void(std::expected<Product, std::string>)>;
 using ContextProvider = std::function<std::optional<mmltk::frameworks::gpu::DeviceContext>()>;
 using PreviewRetirement = std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>;
 using PlaybackGate = std::function<bool(std::optional<double>, double)>;
 virtual ~PredictRuntime() = default;
 virtual void Close() noexcept {}
 [[nodiscard]] virtual bool HasUnsafeCustody() const noexcept { return false; }
 [[nodiscard]] virtual contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token, const ComputeProgressSink&, const ProductSink&, const PlaybackGate&,
  VisualExtent maximum, const ContextProvider&, const PreviewRetirement&) = 0;
};
class CudaPredictRuntime final : public PredictRuntime {
public:
 explicit CudaPredictRuntime(DirectComputeConfiguration);
 ~CudaPredictRuntime() override;
 void Close() noexcept override;
 [[nodiscard]] bool HasUnsafeCustody() const noexcept override;
 [[nodiscard]] contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token, const ComputeProgressSink&, const ProductSink&, const PlaybackGate&,
  VisualExtent maximum, const ContextProvider&, const PreviewRetirement&) override;

private:
 class Impl;
 std::unique_ptr<Impl> impl_;
};
using PredictRuntimeFactory = std::function<std::unique_ptr<PredictRuntime>()>;
struct PredictPauseIntent final {
 bool paused = false;
};
struct PredictImageMetadata final {
 std::uint64_t content_identity = 0U;
 VisualFrame frame{};
 [[= mmltk::frameworks::reflection::MaxItems{contracts::kAnnotationObjectCapacity}]] std::vector<PredictLabel> labels{};
 std::int64_t image_id = 0;
};
struct PredictSnapshot final {
 std::uint64_t content_identity = 0U;
 std::uint64_t revision = 0U;
 // CLEANUP-IGNORE: Predict composes compute facts with its private visual frame in one canonical snapshot.
 contracts::ComputeUiState operation{};
 bool paused = false;
 bool video = false;
 VisualFrame frame{};
 [[= mmltk::frameworks::reflection::MaxItems{contracts::kAnnotationObjectCapacity}]] std::vector<PredictLabel> labels{};
 std::int64_t image_id = 0;
 // CLEANUP-IGNORE: PredictSnapshot is a distinct reflected boundary type.
};
// Scalar observation excludes immutable image labels and metadata.
struct PredictProgressState final {
 std::uint64_t revision = 0U;
 contracts::ComputeUiState operation{};
 bool paused = false;
 bool video = false;
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] PredictProgress final {
 PredictProgressState snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] PredictChanged final {
 PredictSnapshot snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] PredictFailed final {
 PredictSnapshot snapshot{};
 // CLEANUP-IGNORE: Predict failure fields and source registration have independent canonical domain identities.
 [[= mmltk::frameworks::reflection::MaxBytes{kVisualFailureByteCapacity}]] std::string detail;
 // CLEANUP-IGNORE: Predict direct input and source declarations are not a duplicate input or rendering implementation.
};
class PredictSystem final {
public:
 [[= contracts::reflection::direct::InteractionEndpoint{}]] void Input(WorkspaceMouse);
 void SetInputPeer(std::uint64_t);
 using visual_source = VisualSourceProjection<PredictSnapshot, PresentationSourceKind::Predict, mmltk::frameworks::reflection::member_path<&PredictSnapshot::frame>,
  // Logical progress does not invalidate committed pixels or metadata.
  mmltk::frameworks::reflection::member_path<&PredictSnapshot::frame, &VisualFrame::revision>, PredictImageMetadata>;
 using progress_type = PredictProgressState;
 using event_type = std::variant<PredictProgress, PredictChanged, PredictFailed>;
 PredictSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, VisualDeviceSettings, PredictRuntimeFactory, SystemEventSink<event_type> = {});
 ~PredictSystem();
 [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] PredictSnapshot Start(contracts::PredictWorkflowIntent);
 [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] PredictSnapshot Pause(PredictPauseIntent);
 [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] PredictSnapshot Stop(contracts::PredictWorkflowIntent) noexcept;
 // CLEANUP-IGNORE: Predict shutdown is ordinary facade forwarding for this sealed system.
 void Shutdown() noexcept;
 // CLEANUP-IGNORE: Predict owns its reflected snapshot and public read surface independently of Live and Annotation.
 [[= contracts::reflection::Snapshot{2U * 1024U * 1024U}]] [[nodiscard]] PredictSnapshot snapshot() const;
 // CLEANUP-IGNORE: Predict exposes its sealed image and workspace API through the existing private shared runtime.
 [[nodiscard]] std::optional<PredictImageMetadata> ImageSnapshot(const VisualFrame&) const;
 [[nodiscard]] VisualSourceObservation ObserveSource() const;
 // CLEANUP-IGNORE: Predict retains a sealed raw/display read API backed by the shared private VisualRuntimeOwner.
 [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
 [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageWorkspace BorrowWorkspace() const;
 [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceObservation ObserveWorkspace() const;
 void RequestWorkspace(VisualWorkspaceRequest);

private:
 class Impl;
 std::unique_ptr<Impl> impl_;
};
MMLTK_REFLECT_FIELDS(PredictPauseIntent)
MMLTK_REFLECT_FIELDS(PredictLabel)
MMLTK_REFLECT_FIELDS(PredictImageMetadata)
MMLTK_REFLECT_FIELDS(PredictSnapshot)
MMLTK_REFLECT_FIELDS(PredictProgressState)
MMLTK_REFLECT_FIELDS(PredictProgress)
MMLTK_REFLECT_FIELDS(PredictChanged)
MMLTK_REFLECT_FIELDS(PredictFailed)
}  // namespace mmltk::controller
