#pragma once

#include "src/controller/contracts/workspace_input.h"
#include "src/controller/contracts/annotation.h"

#include "src/controller/presentation/visual_source_projection.h"

#include <functional>
#include <expected>
#include "src/frameworks/gpu/image_product_pool.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include <memory>
#include <mutex>
#include <optional>
#include "src/frameworks/gpu/device_execution.h"
#include <stop_token>
#include <variant>
#include <vector>

#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/local_run.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/system/system_events.h"

namespace mmltk::controller {

struct DirectComputeConfiguration final {
    std::optional<mmltk::frameworks::gpu::DeviceExecution> execution;
    [[nodiscard]] bool valid() const noexcept {
        return execution && execution->device >= 0 && execution->placement.numa_node >= 0 && !execution->placement.cpus.empty();
    }
};

using ComputeProgressSink = std::function<void(const contracts::ComputeProgress&)>;

class ValidationRuntime {
   public:
    virtual ~ValidationRuntime() = default;
    [[nodiscard]] virtual contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token,
                                                         const ComputeProgressSink&) = 0;
};
class ExportRuntime {
   public:
    virtual ~ExportRuntime() = default;
    [[nodiscard]] virtual contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ModelExportRequest, std::stop_token,
                                                         const ComputeProgressSink&) = 0;
};
struct PredictLabel final {
    contracts::AnnotationBox box{};
    int category = 0;
    float confidence = 0.0F;
    contracts::AnnotationColor color{};
    [[= mmltk::frameworks::reflection::MaxBytes{256U}]] std::string name;
};
namespace detail { class PredictionPreviewFrame; }
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
    [[nodiscard]] virtual contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token,
                                                         const ComputeProgressSink&, const ProductSink&, const PlaybackGate&, VisualExtent maximum, const ContextProvider&, const PreviewRetirement&) = 0;
};

class CudaValidationRuntime final : public ValidationRuntime {
   public:
    explicit CudaValidationRuntime(DirectComputeConfiguration);
    ~CudaValidationRuntime() override;
    [[nodiscard]] contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token,
                                                 const ComputeProgressSink&) override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
class CudaExportRuntime final : public ExportRuntime {
   public:
    explicit CudaExportRuntime(DirectComputeConfiguration);
    ~CudaExportRuntime() override;
    [[nodiscard]] contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ModelExportRequest, std::stop_token,
                                                 const ComputeProgressSink&) override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
class CudaPredictRuntime final : public PredictRuntime {
   public:
    explicit CudaPredictRuntime(DirectComputeConfiguration);
    ~CudaPredictRuntime() override;
    void Close() noexcept override;
    [[nodiscard]] bool HasUnsafeCustody() const noexcept override;
    [[nodiscard]] contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token, const ComputeProgressSink&, const ProductSink&, const PlaybackGate&, VisualExtent maximum, const ContextProvider&, const PreviewRetirement&) override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] ComputeProgressEvent final {
    contracts::ComputeUiState snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] ComputeChanged final {
    contracts::ComputeUiState snapshot{};
};
using ComputeSystemEvent = std::variant<ComputeProgressEvent, ComputeChanged>;

using ValidationRuntimeFactory = std::function<std::unique_ptr<ValidationRuntime>()>;
using ExportRuntimeFactory = std::function<std::unique_ptr<ExportRuntime>()>;
using PredictRuntimeFactory = std::function<std::unique_ptr<PredictRuntime>()>;

class ValidationSystem final {
   public:
    using event_type = ComputeSystemEvent;
    ValidationSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, ValidationRuntimeFactory, SystemEventSink<event_type> = {},
                     std::optional<mmltk::frameworks::gpu::DeviceExecution> = {});
    ~ValidationSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ComputeUiState Start(contracts::ValidateWorkflowIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ComputeUiState Stop() noexcept;
    void Shutdown() noexcept;
    [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] contracts::ComputeUiState snapshot() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
class ExportSystem final {
   public:
    using event_type = ComputeSystemEvent;
    ExportSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, ExportRuntimeFactory, SystemEventSink<event_type> = {},
                 std::optional<mmltk::frameworks::gpu::DeviceExecution> = {});
    ~ExportSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ComputeUiState Start(contracts::ExportWorkflowIntent);
    // CLEANUP-IGNORE: Export exposes its own reflected typed action surface; Validation remains an independently sealed
    // system.
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ComputeUiState Stop() noexcept;
    void Shutdown() noexcept;
    [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] contracts::ComputeUiState snapshot() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
struct PredictPauseIntent final { bool paused = false; };
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

    using visual_source = VisualSourceProjection<PredictSnapshot, PresentationSourceKind::Predict,
                                                 mmltk::frameworks::reflection::member_path<&PredictSnapshot::frame>,
                                                 // Logical progress does not invalidate committed pixels or metadata.
                                                 mmltk::frameworks::reflection::member_path<&PredictSnapshot::frame, &VisualFrame::revision>, PredictImageMetadata>;
    using progress_type = PredictProgressState;
    using event_type = std::variant<PredictProgress, PredictChanged, PredictFailed>;
    PredictSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, VisualDeviceSettings, PredictRuntimeFactory,
                  SystemEventSink<event_type> = {});
    ~PredictSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] PredictSnapshot Start(contracts::PredictWorkflowIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] PredictSnapshot Pause(PredictPauseIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] PredictSnapshot Stop(contracts::PredictWorkflowIntent) noexcept;
    // CLEANUP-IGNORE: Predict shutdown is ordinary facade forwarding for this sealed system.
    void Shutdown() noexcept;
    // CLEANUP-IGNORE: Predict owns its reflected snapshot and public read surface independently of Live and Annotation.
    [[= contracts::reflection::Snapshot{contracts::kAnnotationUiStateByteBudget}]] [[nodiscard]] PredictSnapshot snapshot() const;
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

MMLTK_REFLECT_FIELDS(ComputeProgressEvent)
MMLTK_REFLECT_FIELDS(ComputeChanged)
MMLTK_REFLECT_FIELDS(PredictPauseIntent)
MMLTK_REFLECT_FIELDS(PredictLabel)
MMLTK_REFLECT_FIELDS(PredictImageMetadata)
MMLTK_REFLECT_FIELDS(PredictSnapshot)
MMLTK_REFLECT_FIELDS(PredictProgressState)
MMLTK_REFLECT_FIELDS(PredictProgress)
MMLTK_REFLECT_FIELDS(PredictChanged)
MMLTK_REFLECT_FIELDS(PredictFailed)

}  // namespace mmltk::controller
