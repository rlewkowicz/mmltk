#pragma once

#include "src/controller/presentation/visual_source_projection.h"
#include "src/controller/contracts/workspace_input.h"

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

struct AnnotationRenderState;

struct AnnotationPointer final {
    contracts::AnnotationPointerPhase phase = contracts::AnnotationPointerPhase::Begin;
    std::uint64_t interaction_id = 0U;
    std::uint64_t sequence = 0U;
    contracts::AnnotationPointerTarget target{};
    contracts::AnnotationTargetIdentity identity{};
    // CLEANUP-IGNORE: The resolved native pointer and transport mouse enforce the same canonical brush bounds but retain distinct
    // lifetimes.
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
class AnnotationAlgorithm : public mmltk::frameworks::gpu::SystemImageModel {
   public:
    [[nodiscard]] virtual mmltk::frameworks::gpu::ImageWorkspaceCoverage WorkspaceCoverage(
        const mmltk::frameworks::gpu::ImageWorkspaceObservation&) const {
        return {};
    }
    ~AnnotationAlgorithm() override = default;
    virtual void Open(mmltk::frameworks::gpu::ImagePlaneView source, VisualRegion) = 0;
    [[nodiscard]] virtual contracts::AnnotationColor Sample(contracts::AnnotationPoint) = 0;
    // Source is the retained immutable document baseline. The algorithm owns
    // allocation-local initialization, damage and reusable raster inputs.
    virtual void Render(const AnnotationRenderState&, mmltk::frameworks::gpu::ImagePlaneView source,
                        mmltk::frameworks::gpu::ImagePlaneView clean, mmltk::frameworks::gpu::ImagePlaneView semantic,
                        std::uintptr_t stream) const = 0;
};
struct AnnotationRenderedFacts final {
    bool operator==(const AnnotationRenderedFacts&) const = default;
    std::uint64_t generation = 0U;
    std::uint64_t document_epoch = 0U;
    std::uint64_t scene_revision = 0U;
    contracts::AnnotationEditorFacts editor{};
};
struct AnnotationImageMetadata final {
    VisualFrame frame{};
    // Opt-in evidence for the exact rendered image; never authorizes input.
    std::optional<AnnotationRenderedFacts> diagnostics{};
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
    // CLEANUP-IGNORE: Compact frame identity is distinct from Live's full lifecycle snapshot.
    std::uint64_t revision = 0U;
    std::uint64_t ui_revision = 0U;
    // CLEANUP-IGNORE: Annotation UI, compact frame notifications, and Upscale events have distinct canonical types and delivery meanings.
    VisualFrame frame{};
};
// CLEANUP-IGNORE: Annotation frame state and Presentation capability events belong to different canonical owners.
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
    using visual_source =
        VisualSourceProjection<AnnotationSnapshot, PresentationSourceKind::Annotation,
                               mmltk::frameworks::reflection::member_path<&AnnotationSnapshot::frame>,
                               mmltk::frameworks::reflection::member_path<&AnnotationSnapshot::revision>, AnnotationImageMetadata>;
    using event_type = std::variant<AnnotationChanged, AnnotationFrameChanged, AnnotationFailed>;
    AnnotationSystem(VisualDeviceSettings, VisualRuntimeFactory, ExactVisualDocumentBorrower, SystemEventSink<event_type> = {},
                     VisualDiagnosticSink = {});
    ~AnnotationSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] AnnotationSnapshot Open(AnnotationOpen);
    // CLEANUP-IGNORE: Annotation's pointer/edit/save endpoints are a distinct reflected domain interface.
    [[= contracts::reflection::direct::InteractionEndpoint{}]] void Input(WorkspaceMouse);
    // CLEANUP-IGNORE: Annotation document mutation and Explore navigation are separate direct typed domain endpoints.
    void SetInputPeer(std::uint64_t);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] AnnotationSnapshot Edit(AnnotationEditRequest);
    // CLEANUP-IGNORE: Annotation persistence and lifecycle methods do not duplicate Explore navigation ownership.
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] AnnotationSnapshot Save(AnnotationSave);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] AnnotationSnapshot Stop() noexcept;
    void PeerClosed() noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[= contracts::reflection::Snapshot{contracts::kAnnotationUiStateByteBudget}]] [[nodiscard]] AnnotationSnapshot snapshot() const;
    [[nodiscard]] std::optional<AnnotationImageMetadata> ImageSnapshot(const VisualFrame&) const;
    // CLEANUP-IGNORE: Annotation exposes its sealed source and workspace API; shared runtime behavior remains private.
    [[nodiscard]] VisualSourceObservation ObserveSource() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageWorkspace BorrowWorkspace() const;
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceObservation ObserveWorkspace() const;
    void RequestWorkspace(VisualWorkspaceRequest);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// CLEANUP-IGNORE: The native factory and following reflection inventory are Annotation's canonical owner-colocated API.
[[nodiscard]] VisualRuntimeFactory make_native_annotation_runtime_factory(VisualDeviceSettings);
// CLEANUP-IGNORE: Annotation's canonical field registrations are distinct stable schema identities.
MMLTK_REFLECT_FIELDS(AnnotationPointer)
// CLEANUP-IGNORE: Each canonical Annotation type requires its own registration; projection already shares reflected field machinery.
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
MMLTK_REFLECT_FIELDS(AnnotationClassEdit)  // CLEANUP-IGNORE: Distinct canonical Annotation types require distinct reflection registrations.
MMLTK_REFLECT_FIELDS(AnnotationSelectedObjectEdit)
// CLEANUP-IGNORE: Annotation edit and publication types have their own schema identities, independent of Explore's registered types.
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
MMLTK_REFLECT_FIELDS(AnnotationRenderedFacts)
MMLTK_REFLECT_FIELDS(AnnotationImageMetadata)
MMLTK_REFLECT_FIELDS(AnnotationSnapshot)
MMLTK_REFLECT_FIELDS(AnnotationChanged)
MMLTK_REFLECT_FIELDS(AnnotationFrameState)
MMLTK_REFLECT_FIELDS(AnnotationFrameChanged)
MMLTK_REFLECT_FIELDS(AnnotationFailed)

}  // namespace mmltk::controller
