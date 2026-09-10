#pragma once

#include "src/controller/presentation/visual_source_projection.h"

#include <memory>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <array>
#include <inplace_vector>
#include <string>
#include <string_view>
#include <variant>

#include "src/controller/contracts/annotation.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/frameworks/gpu/image_types.h"
#include "src/frameworks/gpu/system_image_model.h"
#include "src/controller/presentation/visual_document.h"

namespace mmltk::controller {

struct AnnotationPointer final {
    contracts::AnnotationPointerPhase phase = contracts::AnnotationPointerPhase::Begin;
    std::uint64_t interaction_id = 0U;
    std::uint64_t sequence = 0U;
    contracts::AnnotationPointerTarget target{};
    contracts::AnnotationPoint point{};
    [[= mmltk::frameworks::reflection::Minimum<std::uint16_t>{contracts::kMinAnnotationBrushRadius}]]
        [[= mmltk::frameworks::reflection::Maximum<std::uint16_t>{contracts::kMaxAnnotationBrushRadius}]] std::uint16_t brush_radius =
            contracts::kDefaultAnnotationBrushRadius;
    [[nodiscard]] bool valid() const noexcept {
        return mmltk::frameworks::reflection::enum_contains(phase) && interaction_id != 0U && sequence != 0U && target.valid() &&
               point.finite() && brush_radius >= contracts::kMinAnnotationBrushRadius &&
               brush_radius <= contracts::kMaxAnnotationBrushRadius;
    }
};
inline constexpr std::size_t kAnnotationInputBatchCapacity = 32U;
inline constexpr std::size_t kAnnotationInputAdmissionSlots = 2U;
struct AnnotationInputBatch final {
    std::uint64_t epoch = 0U;
    std::uint64_t document_epoch = 0U;
    std::uint64_t sequence = 0U;
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationInputBatchCapacity}]]
    std::inplace_vector<AnnotationPointer, kAnnotationInputBatchCapacity> samples{};
};
struct AnnotationInputProgress final {
    bool operator==(const AnnotationInputProgress&) const = default;
    std::uint64_t epoch = 0U;
    std::uint64_t consumed_sequence = 0U;
    [[= mmltk::frameworks::reflection::MaxBytes{kVisualFailureByteCapacity}]] std::optional<std::string> rejection{};
};
struct AnnotationOpen final {
    VisualFrame source{};
    bool original_content = false;
};
struct AnnotationSave final {
    [[= mmltk::frameworks::reflection::MaxBytes{4096U}]] std::string destination;
};
struct AnnotationToolEdit final {
    contracts::AnnotationTool tool = contracts::AnnotationTool::Select;
};
struct AnnotationSetupEdit final {
    contracts::AnnotationSetupAction action = contracts::AnnotationSetupAction::ReloadFrame;
};
struct AnnotationHoldEdit final {
    bool enabled = false;
};
struct AnnotationSidebarEdit final {
    contracts::AnnotationSidebarCommand command = contracts::AnnotationSidebarCommand::Assist;
};
struct AnnotationObjectEdit final {
    std::uint16_t object = 0U;
};
struct AnnotationCategoryEdit final {
    contracts::AnnotationText category{};
};
struct AnnotationClassEdit final {
    std::uint16_t category = 0U;
};
struct AnnotationSelectedObjectEdit final {
    std::uint16_t category = 0U;
    bool enabled = true;
};
struct AnnotationSplineEdit final {
    std::uint16_t segment = 0U;
};
struct AnnotationSplineHandleEdit final {
    contracts::AnnotationHandleRole handle = contracts::AnnotationHandleRole::SplineInHandle;
    contracts::AnnotationSplineHandleMode mode = contracts::AnnotationSplineHandleMode::Corner;
    contracts::AnnotationPoint point{};
};
struct AnnotationSkeletonEdit final {
    std::uint16_t joint = 0U;
};
struct AnnotationMaskCleanupEdit final {
    contracts::AnnotationMaskCleanup operation = contracts::AnnotationMaskCleanup::LargestComponent;
    [[= mmltk::frameworks::reflection::Minimum<std::uint16_t>{contracts::kMinAnnotationMaskCleanupRadius}]]
        [[= mmltk::frameworks::reflection::Maximum<std::uint16_t>{contracts::kMaxAnnotationMaskCleanupRadius}]] std::uint16_t radius =
            contracts::kDefaultAnnotationMaskCleanupRadius;
};
struct AnnotationMaskColorsEdit final {
    contracts::AnnotationColorRange sup{};
    contracts::AnnotationColorRange nosup{};
};
struct AnnotationSceneEdit final {};
struct AnnotationUndoEdit final {};
struct AnnotationRedoEdit final {};
struct AnnotationEdit final {
    using variant_type =
        std::variant<AnnotationToolEdit, AnnotationSetupEdit, AnnotationHoldEdit, AnnotationSidebarEdit, AnnotationObjectEdit,
                     AnnotationCategoryEdit, AnnotationSelectedObjectEdit, AnnotationSplineEdit, AnnotationSplineHandleEdit,
                     AnnotationSkeletonEdit, AnnotationMaskCleanupEdit, AnnotationMaskColorsEdit, AnnotationSceneEdit, AnnotationUndoEdit,
                     AnnotationRedoEdit, AnnotationClassEdit>;
    variant_type value{};
};
struct AnnotationEditRequest final {
    AnnotationEdit edit{};
};
enum class AnnotationOperationOutcome : std::uint8_t { Applied, Rejected };
struct AnnotationOperationResult final {
    contracts::AnnotationUiState ui{};
    std::string detail;
    AnnotationOperationOutcome outcome = AnnotationOperationOutcome::Rejected;
};

struct AnnotationPointerResult final {
    std::string detail;
    AnnotationOperationOutcome outcome = AnnotationOperationOutcome::Rejected;
    bool ui_changed = false;
};

class AnnotationAlgorithm : public mmltk::frameworks::gpu::SystemImageModel {
   public:
    ~AnnotationAlgorithm() override = default;
    [[nodiscard]] virtual AnnotationOperationResult Open(mmltk::frameworks::gpu::ImagePlaneView source, contracts::AnnotationSceneContent,
                                                         VisualRegion) = 0;
    [[nodiscard]] virtual AnnotationPointerResult Pointer(const AnnotationPointer&) = 0;
    [[nodiscard]] virtual const contracts::AnnotationUiState& Ui() const noexcept = 0;
    virtual void PeerClosed() noexcept = 0;
    [[nodiscard]] virtual AnnotationOperationResult Edit(const AnnotationEdit&) = 0;
    [[nodiscard]] virtual AnnotationOperationResult Save(std::string_view destination) = 0;
    // A missing source preserves the initialized clean plane; semantics are replaced completely.
    virtual void Render(mmltk::frameworks::gpu::ImagePlaneView source, mmltk::frameworks::gpu::ImagePlaneView clean,
                        mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t stream) const = 0;
};
struct AnnotationSnapshot final {
    std::uint64_t revision = 0U;
    std::uint64_t ui_revision = 0U;
    std::uint64_t input_document_epoch = 0U;
    bool busy = false;
    bool cancellation_requested = false;
    // CLEANUP-IGNORE: Annotation readiness begins a domain-specific reflected snapshot tail, not shared state.
    bool ready = false;
    contracts::AnnotationUiState ui{};
    VisualFrame frame{};
    // CLEANUP-IGNORE: The Annotation snapshot terminator precedes domain-specific transient and critical events.
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::LatestState}]] AnnotationChanged final {
    AnnotationSnapshot snapshot{};
};
// A frame may advance without changing UI. The matching full-state identity also
// orders command admission/settlement when socket records arrive in either order.
struct AnnotationFrameState final {
    std::uint64_t revision = 0U;
    std::uint64_t ui_revision = 0U;
    VisualFrame frame{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::LatestState}]] AnnotationFrameChanged final {
    AnnotationFrameState snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] AnnotationFailed final {
    AnnotationSnapshot snapshot{};
    // CLEANUP-IGNORE: This critical Annotation detail is a distinct reflected event boundary.
    [[= mmltk::frameworks::reflection::MaxBytes{kVisualFailureByteCapacity}]] std::string detail;
};

class AnnotationSystem final {
   public:
    using visual_source = VisualSourceProjection<AnnotationSnapshot, PresentationSourceKind::Annotation,
                                                 mmltk::frameworks::reflection::member_path<&AnnotationSnapshot::frame>,
                                                 mmltk::frameworks::reflection::member_path<&AnnotationSnapshot::revision>>;
    using event_type = std::variant<AnnotationChanged, AnnotationFrameChanged, AnnotationFailed>;
    AnnotationSystem(VisualDeviceSettings, VisualRuntimeFactory, ExactVisualDocumentBorrower, SystemEventSink<event_type> = {},
                     VisualDiagnosticSink = {});
    ~AnnotationSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] AnnotationSnapshot Open(AnnotationOpen);
    // CLEANUP-IGNORE: Annotation's pointer/edit/save endpoints are a distinct reflected domain interface.
    [[= contracts::reflection::direct::InteractionEndpoint{}]] void Input(AnnotationInputBatch);
    void SetInputPeer(std::uint64_t, SystemEventSink<AnnotationInputProgress>);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] AnnotationSnapshot Edit(AnnotationEditRequest);
    // CLEANUP-IGNORE: Annotation persistence and lifecycle methods do not duplicate Explore navigation ownership.
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] AnnotationSnapshot Save(AnnotationSave);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] AnnotationSnapshot Stop() noexcept;
    void PeerClosed() noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[= contracts::reflection::Snapshot{contracts::kAnnotationUiStateByteBudget}]] [[nodiscard]] AnnotationSnapshot snapshot() const;
    [[nodiscard]] VisualSourceObservation ObserveSource() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// CLEANUP-IGNORE: The native factory and following reflection inventory are Annotation's canonical owner-colocated API.
[[nodiscard]] VisualRuntimeFactory make_native_annotation_runtime_factory(VisualDeviceSettings);
// CLEANUP-IGNORE: Annotation's canonical field registrations are distinct stable schema identities.
MMLTK_REFLECT_FIELDS(AnnotationPointer)
MMLTK_REFLECT_FIELDS(AnnotationInputBatch)
MMLTK_REFLECT_FIELDS(AnnotationInputProgress)
MMLTK_REFLECT_FIELDS(AnnotationOpen)
MMLTK_REFLECT_FIELDS(AnnotationSave)
MMLTK_REFLECT_FIELDS(AnnotationToolEdit)
MMLTK_REFLECT_FIELDS(AnnotationSetupEdit)
MMLTK_REFLECT_FIELDS(AnnotationHoldEdit)
// CLEANUP-IGNORE: The remainder of Annotation's field inventory cannot share registrations with Explore types.
MMLTK_REFLECT_FIELDS(AnnotationSidebarEdit)
// CLEANUP-IGNORE: These registrations identify distinct Annotation types; their projections already share reflection.
MMLTK_REFLECT_FIELDS(AnnotationObjectEdit)
MMLTK_REFLECT_FIELDS(AnnotationCategoryEdit)
MMLTK_REFLECT_FIELDS(AnnotationClassEdit)
MMLTK_REFLECT_FIELDS(AnnotationSelectedObjectEdit)
MMLTK_REFLECT_FIELDS(AnnotationSplineEdit)
MMLTK_REFLECT_FIELDS(AnnotationSplineHandleEdit)
MMLTK_REFLECT_FIELDS(AnnotationSkeletonEdit)
MMLTK_REFLECT_FIELDS(AnnotationMaskCleanupEdit)
MMLTK_REFLECT_FIELDS(AnnotationMaskColorsEdit)
MMLTK_REFLECT_FIELDS(AnnotationSceneEdit)
MMLTK_REFLECT_FIELDS(AnnotationUndoEdit)
MMLTK_REFLECT_FIELDS(AnnotationRedoEdit)
MMLTK_REFLECT_FIELDS(AnnotationEdit)
MMLTK_REFLECT_FIELDS(AnnotationEditRequest)
MMLTK_REFLECT_FIELDS(AnnotationSnapshot)
MMLTK_REFLECT_FIELDS(AnnotationChanged)
MMLTK_REFLECT_FIELDS(AnnotationFrameState)
MMLTK_REFLECT_FIELDS(AnnotationFrameChanged)
MMLTK_REFLECT_FIELDS(AnnotationFailed)

}  // namespace mmltk::controller
