#pragma once

#include "src/controller/presentation/visual_source_projection.h"

#include <cstdint>
#include <functional>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "src/controller/contracts/application_boundary.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/frameworks/gpu/image_types.h"
#include "src/frameworks/gpu/system_image_model.h"
#include "src/controller/presentation/visual_document.h"
#include "src/backend/imaging/upscale/upscale_execution.h"

namespace mmltk::controller {

inline constexpr std::uint32_t kUpscaleOutputScale = 4U;

[[nodiscard]] VisualExtent checked_upscale_output_extent(VisualExtent);

enum class UpscaleKernel : std::uint8_t {
    Default,
    ShiftLut,
    RealPlksr,
};
struct UpscaleRequest final {
    // Full source revision identifies semantic pixels. Document facts identify
    // the immutable borrowed meaning independently of reusable clean pixels.
    VisualFrame source{};
    UpscaleKernel kernel = UpscaleKernel::Default;
    VisualDocumentFacts document{};
    bool operator==(const UpscaleRequest&) const = default;
};
enum class UpscaleFailureKind : std::uint8_t { Failed, Unavailable, Physical };
class UpscaleAlgorithm : public mmltk::frameworks::gpu::SystemImageModel {
   public:
    ~UpscaleAlgorithm() override = default;
    virtual void Warm() = 0;
    [[nodiscard]] virtual bool GraphReplay(UpscaleKernel) const { return false; }
    virtual void Run(UpscaleKernel, mmltk::frameworks::gpu::ImagePlaneView source, mmltk::frameworks::gpu::ImagePlaneView target,
                     std::uintptr_t stream, const std::function<bool()>& current = {}) = 0;
    virtual void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) = 0;
};
struct UpscaleMethodSnapshot final {
    bool available = false;
    bool warm = false;
    bool failed = false;
    bool initialization_failed = false;
    bool graph_replay = false;
    std::optional<UpscaleRequest> completed{};
    std::optional<UpscaleRequest> failure{};
    VisualFrame frame{};
};
struct UpscaleSnapshot final {
    std::uint64_t revision = 0U;
    bool busy = false;
    bool ready = false;
    std::optional<UpscaleRequest> pending{};
    std::array<UpscaleMethodSnapshot, 3U> methods{};
    // CLEANUP-IGNORE: Upscale kernel selection is a domain field in its canonical generated snapshot.
    UpscaleKernel kernel = UpscaleKernel::Default;
    // CLEANUP-IGNORE: Upscale owns a receiver-private visual frame with its own generated identity.
    VisualFrame frame{};
    VisualFrame input{};
    contracts::AnnotationSceneContent scene{};
    // CLEANUP-IGNORE: UpscaleSnapshot remains a distinct reflected application boundary.
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::LatestState}]] UpscaleChanged final {
    UpscaleSnapshot snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] UpscaleFailed final {
    UpscaleSnapshot snapshot{};
    [[= mmltk::frameworks::reflection::MaxBytes{kVisualFailureByteCapacity}]] std::string detail;
    std::optional<UpscaleRequest> request{};
    UpscaleFailureKind kind = UpscaleFailureKind::Failed;
};

class UpscaleSystem final {
   public:
    using visual_source = VisualSourceProjection<UpscaleSnapshot, PresentationSourceKind::Upscale,
        mmltk::frameworks::reflection::member_path<&UpscaleSnapshot::frame>,
        mmltk::frameworks::reflection::member_path<&UpscaleSnapshot::revision>>;
    using event_type = std::variant<UpscaleChanged, UpscaleFailed>;
    UpscaleSystem(VisualDeviceSettings, VisualRuntimeFactory, ExactVisualDocumentBorrower, SystemEventSink<event_type> = {},
                  VisualDiagnosticSink = {});
    ~UpscaleSystem();
    void Warm(VisualExtent) noexcept;
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] UpscaleSnapshot Start(UpscaleRequest);
    // CLEANUP-IGNORE: Upscale Stop is a distinct reflected endpoint with system-specific semantics.
    [[= contracts::reflection::direct::IntentEndpoint{}]] void Stop() noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    // CLEANUP-IGNORE: The sealed Upscale facade publishes its own reflected snapshot and frame borrow.
    [[= contracts::reflection::Snapshot{contracts::kAnnotationUiStateByteBudget}]] [[nodiscard]] UpscaleSnapshot snapshot() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
    [[nodiscard]] VisualDocumentRead BorrowDocument(const VisualFrame&) const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] VisualRuntimeFactory make_native_upscale_runtime_factory(
    VisualDeviceSettings, mmltk::backend::imaging::upscale::ImageUpscalerExecutionCheckpoint = {});

MMLTK_REFLECT_ENUM(UpscaleKernel)
MMLTK_REFLECT_ENUM(UpscaleFailureKind)
MMLTK_REFLECT_FIELDS(UpscaleRequest)
MMLTK_REFLECT_FIELDS(UpscaleMethodSnapshot)
MMLTK_REFLECT_FIELDS(UpscaleSnapshot)
MMLTK_REFLECT_FIELDS(UpscaleChanged)
MMLTK_REFLECT_FIELDS(UpscaleFailed)

}  // namespace mmltk::controller
