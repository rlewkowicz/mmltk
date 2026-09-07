#pragma once
#include "src/frameworks/gpu/device_execution.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "src/controller/contracts/diagnostic_context.h"
#include "src/controller/contracts/visual_source.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/frameworks/gpu/system_image_worker.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

namespace mmltk::controller {

struct VisualDeviceSettings final {
    int device = -1;
    std::uint32_t maximum_width = 0U;
    std::uint32_t maximum_height = 0U;
    int numa_node = -1;
    [[nodiscard]] constexpr bool valid() const noexcept {
        return device >= 0 && numa_node >= -1 && maximum_width != 0U && maximum_height != 0U;
    }
};

[[nodiscard]] inline mmltk::frameworks::gpu::DeviceExecution resolve_visual_device_execution(const VisualDeviceSettings& settings) {
    return mmltk::frameworks::gpu::resolve_device_execution(settings.device, mmltk::common::system::NumaTopology::Capture(),
                                                            settings.numa_node);
}

struct VisualExtent final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    [[nodiscard]] constexpr bool valid() const noexcept { return width != 0U && height != 0U; }
    // CLEANUP-IGNORE: Visual geometry has controller sampling semantics, independent of model-analysis regions.
    bool operator==(const VisualExtent&) const = default;
};

struct VisualRegion final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    [[nodiscard]] constexpr bool valid() const noexcept { return width != 0U && height != 0U; }
    bool operator==(const VisualRegion&) const = default;
};
struct VisualFrame final {
    PresentationSourceIdentity source{};
    VisualExtent extent{};
    std::uint64_t revision = 0U;
    VisualRegion content{};
    std::uint64_t clean_revision = 0U;
    bool operator==(const VisualFrame&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return source.valid() && extent.valid() && revision != 0U; }
};
[[nodiscard]] bool visual_product_matches_frame(const VisualFrame&, const mmltk::frameworks::gpu::BorrowedImageProductReadView&) noexcept;
[[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(
    const VisualFrame&, mmltk::frameworks::gpu::BorrowedImageProductReadView);
[[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(const VisualFrame&,
                                                                                                  const detail::VisualRuntimeOwner&);
enum class VisualSystemKind : std::uint8_t {
    Explore,
    Annotation,
    Upscale,
    Live,
    Presentation,
};
namespace detail {

struct VisualDiagnosticName final {
    char value[97]{};

    template <std::size_t Extent>
    consteval explicit VisualDiagnosticName(const char (&name)[Extent]) {
        static_assert(Extent > 0U);
        if (Extent - 1U >= sizeof(value)) throw "visual diagnostic names allow at most 96 bytes";
        if (name[Extent - 1U] != '\0') throw "visual diagnostic names require a final null terminator";
        for (std::size_t index = 0U; index < Extent - 1U; ++index) {
            if (name[index] == '\0') throw "visual diagnostic names cannot contain embedded null bytes";
            value[index] = name[index];
        }
    }
};

}  // namespace detail

enum class VisualDiagnosticOperation : std::uint8_t {
    // CLEANUP-IGNORE: These distinct enum declarations and diagnostic aliases are the canonical reflected vocabulary,
    // not repeated mapping logic.
    WorkerFailure[[= detail::VisualDiagnosticName{"worker.failure"}]],
    RenderCompleted[[= detail::VisualDiagnosticName{"render.completed"}]],
    DocumentOpened[[= detail::VisualDiagnosticName{"document.opened"}]],
    DocumentEdited[[= detail::VisualDiagnosticName{"document.edited"}]],
    ModelCompleted[[= detail::VisualDiagnosticName{"model.completed"}]],
    FrameCompleted[[= detail::VisualDiagnosticName{"frame.completed"}]],
    TimelineReady[[= detail::VisualDiagnosticName{"timeline.ready"}]],
    CopyCompleted[[= detail::VisualDiagnosticName{"copy.completed"}]],
    PlaceholderPublished[[= detail::VisualDiagnosticName{"placeholder.published"}]],
    TileBatchPublishStarted[[= detail::VisualDiagnosticName{"tile.batch.publish.started"}]],
    TileBatchComposeStarted[[= detail::VisualDiagnosticName{"tile.batch.compose.started"}]],
    TileBatchComposeCompleted[[= detail::VisualDiagnosticName{"tile.batch.compose.completed"}]],
    GalleryGpuCompleted[[= detail::VisualDiagnosticName{"gallery.gpu.completed"}]],
    GalleryGpuFailed[[= detail::VisualDiagnosticName{"gallery.gpu.failed"}]],
    GalleryReadStarted[[= detail::VisualDiagnosticName{"gallery.read.started"}]],
    GalleryReadCompleted[[= detail::VisualDiagnosticName{"gallery.read.completed"}]],
    GalleryTransferCompleted[[= detail::VisualDiagnosticName{"gallery.transfer.completed"}]],
    TileBatchPublished[[= detail::VisualDiagnosticName{"tile.batch.published"}]],
    PresentationAllocationCreated[[= detail::VisualDiagnosticName{"presentation.allocation.created"}]],
    PresentationAdmissionEnqueued[[= detail::VisualDiagnosticName{"presentation.admission.enqueued"}]],
    PresentationAdmissionWritten[[= detail::VisualDiagnosticName{"presentation.admission.written"}]],
    PresentationImportOutcome[[= detail::VisualDiagnosticName{"presentation.import.outcome"}]],
    PresentationActiveWithdrawal[[= detail::VisualDiagnosticName{"presentation.active.withdrawal"}]],
    PresentationCandidateWithdrawal[[= detail::VisualDiagnosticName{"presentation.candidate.withdrawal"}]],
    PresentationSourceCopy[[= detail::VisualDiagnosticName{"presentation.source.copy"}]],
    PresentationFrameEdge[[= detail::VisualDiagnosticName{"presentation.frame.edge"}]],
    PresentationReplacement[[= detail::VisualDiagnosticName{"presentation.replacement"}]],
    PresentationRetirement[[= detail::VisualDiagnosticName{"presentation.retirement"}]],
    PresentationPumpStarted[[= detail::VisualDiagnosticName{"presentation.pump.started"}]],
    PresentationPumpCompleted[[= detail::VisualDiagnosticName{"presentation.pump.completed"}]],
    PresentationCapabilityPublished[[= detail::VisualDiagnosticName{"presentation.capability.published"}]],
    PresentationReleaseWaitStarted[[= detail::VisualDiagnosticName{"presentation.release_wait.started"}]],
    PresentationReleaseWaitCompleted[[= detail::VisualDiagnosticName{"presentation.release_wait.completed"}]],
    PresentationSourceBorrowStarted[[= detail::VisualDiagnosticName{"presentation.source_borrow.started"}]],
    PresentationSourceBorrowCompleted[[= detail::VisualDiagnosticName{"presentation.source_borrow.completed"}]],
    PresentationReadySyncStarted[[= detail::VisualDiagnosticName{"presentation.ready_sync.started"}]],
    PresentationReadySyncCompleted[[= detail::VisualDiagnosticName{"presentation.ready_sync.completed"}]],
    StaleThumbnailDiscarded[[= detail::VisualDiagnosticName{"thumbnail.stale.discarded"}]],
    AcceptanceLaneStarted[[= detail::VisualDiagnosticName{"acceptance.lane.started"}]],
    AcceptanceCompiledRead[[= detail::VisualDiagnosticName{"acceptance.compiled.read.started"}]],
    AcceptanceCompiledReadCompleted[[= detail::VisualDiagnosticName{"acceptance.compiled.read.completed"}]],
    AcceptanceReadyStateStarted[[= detail::VisualDiagnosticName{"acceptance.ready_state.started"}]],
    AcceptanceReadyStateCompleted[[= detail::VisualDiagnosticName{"acceptance.ready_state.completed"}]],
    AcceptanceReadySinkStarted[[= detail::VisualDiagnosticName{"acceptance.ready_sink.started"}]],
    AcceptanceReadySinkCompleted[[= detail::VisualDiagnosticName{"acceptance.ready_sink.completed"}]],
    AcceptanceCompletionHeld[[= detail::VisualDiagnosticName{"acceptance.completion.held"}]],
    AcceptanceCompletionReleased[[= detail::VisualDiagnosticName{"acceptance.completion.released"}]],
    AcceptanceGateTerminal[[= detail::VisualDiagnosticName{"acceptance.gate.terminal"}]],
    AcceptanceStaleReadDiscarded[[= detail::VisualDiagnosticName{"acceptance.stale.read.discarded"}]],
    AcceptancePlaceholderSlot[[= detail::VisualDiagnosticName{"acceptance.placeholder.slot"}]],
    AcceptancePlaceholderComplete[[= detail::VisualDiagnosticName{"acceptance.placeholder.complete"}]],
    AcceptanceSlotPatched[[= detail::VisualDiagnosticName{"acceptance.slot.patched"}]],
    ExploreAugmentationBatchPrepared[[= detail::VisualDiagnosticName{"explore.augmentation.batch.prepared"}]],
    ExploreOverlayDescriptorsPrepared[[= detail::VisualDiagnosticName{"explore.overlay.descriptors.prepared"}]],
    ExploreTransformedBounds[[= detail::VisualDiagnosticName{"explore.overlay.transformed_bounds"}]],
    ExploreSemanticPixels[[= detail::VisualDiagnosticName{"explore.semantic.nonzero_pixels"}]],
    ExploreImagePixels[[= detail::VisualDiagnosticName{"explore.image.pixel_checksum"}]],
    ExploreCardGeometryProbe[[= detail::VisualDiagnosticName{"explore.card.geometry_probe"}]],
    ExploreOverlaySelectionProbe[[= detail::VisualDiagnosticName{"explore.overlay.selection_probe"}]],
    ExploreRenderedCardProbe[[= detail::VisualDiagnosticName{"explore.card.rendered_probe"}]],
    ExploreRenderedTransitionProbe[[= detail::VisualDiagnosticName{"explore.card.transition_probe"}]],
    ExploreFramePublished[[= detail::VisualDiagnosticName{"explore.frame.published"}]],
    ExploreDonorDescriptorsPrepared[[= detail::VisualDiagnosticName{"explore.donor.descriptors.prepared"}]],
    ExploreContinuationSubmitted[[= detail::VisualDiagnosticName{"explore.continuation.submitted"}]],
    ExploreContinuationStarted[[= detail::VisualDiagnosticName{"explore.continuation.started"}]],
    UpscaleWarmAdmissionStarted[[= detail::VisualDiagnosticName{"upscale.warm.admission.started"}]],
    UpscaleWarmRuntimeStarted[[= detail::VisualDiagnosticName{"upscale.warm.runtime.started"}]],
    UpscaleWarmRuntimeCompleted[[= detail::VisualDiagnosticName{"upscale.warm.runtime.completed"}]],
    ExplorePrefetchReady[[= detail::VisualDiagnosticName{"explore.prefetch.ready"}]],
    UpscaleRequestAdmitted[[= detail::VisualDiagnosticName{"upscale.request.admitted"}]],
    UpscaleWorkerStarted[[= detail::VisualDiagnosticName{"upscale.worker.started"}]],
    UpscaleResultReused[[= detail::VisualDiagnosticName{"upscale.result.reused"}]],
    UpscaleModelSubmitted[[= detail::VisualDiagnosticName{"upscale.model.submitted"}]],
    UpscaleCopyStarted[[= detail::VisualDiagnosticName{"upscale.copy.started"}]],
    UpscaleInputGeometry[[= detail::VisualDiagnosticName{"upscale.input.geometry"}]],
    UpscaleInputAllocation[[= detail::VisualDiagnosticName{"upscale.input.allocation"}]],
    UpscaleOutputAllocation[[= detail::VisualDiagnosticName{"upscale.output.allocation"}]],
    PresentationSourceWaitSubmitted[[= detail::VisualDiagnosticName{"presentation.source.wait_submitted"}]],
};

namespace detail {

struct VisualDiagnosticNameMaterializer final {
    template <class Enum, class Reflection>
        requires std::is_same_v<Enum, VisualDiagnosticOperation>
    [[nodiscard]] consteval auto operator()() const {
        std::array<std::string_view, Reflection::enumerators.size()> names{};
        std::size_t index = 0U;
        template for (constexpr auto enumerator : Reflection::enumerators) {
            static_assert(mmltk::frameworks::reflection::reflected_annotation_count<enumerator>(
                              []<class Annotation> { return std::is_same_v<Annotation, VisualDiagnosticName>; }) == 1U,
                          "each visual diagnostic operation requires exactly one name annotation");
            if (static_cast<std::size_t>([:enumerator:]) != index)
                throw "visual diagnostic operations require dense unique values starting at zero";
            template for (constexpr auto annotation : mmltk::frameworks::reflection::reflected_annotations<enumerator>()) {
                using Annotation = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
                if constexpr (std::is_same_v<Annotation, VisualDiagnosticName>) {
                    constexpr auto name = std::meta::extract<Annotation>(annotation);
                    const std::string_view alias{name.value};
                    if (alias.empty() || !std::all_of(alias.begin(), alias.end(), [](const char character) {
                            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                                   (character >= '0' && character <= '9') || character == '.' || character == '_';
                        }))
                        throw "visual diagnostic names require nonempty alphanumeric, dot, or underscore text";
                    names[index] = std::define_static_string(std::string_view{name.value});
                }
            }
            ++index;
        }
        auto sorted_names = names;
        std::sort(sorted_names.begin(), sorted_names.end());
        if (std::adjacent_find(sorted_names.begin(), sorted_names.end()) != sorted_names.end())
            throw "visual diagnostic names must be unique";
        return names;
    }
};

inline constexpr auto kVisualDiagnosticNames =
    mmltk::frameworks::reflection::materialize<VisualDiagnosticOperation>(VisualDiagnosticNameMaterializer{});

}  // namespace detail

[[nodiscard]] constexpr std::string_view visual_diagnostic_event_name(const VisualDiagnosticOperation operation) noexcept {
    const auto index = static_cast<std::size_t>(operation);
    return index < detail::kVisualDiagnosticNames.size() ? detail::kVisualDiagnosticNames[index] : std::string_view{};
}
struct VisualDiagnosticFact final {
    VisualSystemKind system = VisualSystemKind::Explore;
    VisualDiagnosticOperation operation = VisualDiagnosticOperation::WorkerFailure;
    int device = -1;
    std::uint64_t generation = 0U;
    std::uint64_t value = 0U;
    std::uint64_t detail = 0U;
    mmltk::frameworks::gpu::ImageCopyPath copy_path = mmltk::frameworks::gpu::ImageCopyPath::SameDevice;
    contracts::DiagnosticContext context{};
    std::string_view failure_detail{};
};
MMLTK_REFLECT_ENUM(VisualSystemKind)
MMLTK_REFLECT_ENUM(VisualDiagnosticOperation)
struct VisualDiagnosticSink final {
    void* context = nullptr;
    void (*write)(void*, VisualDiagnosticFact) noexcept = nullptr;

    [[nodiscard]] bool valid() const noexcept { return context != nullptr && write != nullptr; }
    void operator()(const VisualDiagnosticFact fact) const noexcept {
        if (valid()) write(context, fact);
    }
};
void report_visual_worker_failure(VisualDiagnosticSink, VisualSystemKind, int, std::string_view, std::uint64_t generation = 0U) noexcept;

using VisualRuntimeFactory = detail::VisualRuntimeOwner::RuntimeFactory;

inline constexpr std::size_t kVisualFailureByteCapacity = 1024U;

[[nodiscard]] inline std::string visual_failure_detail(const std::exception_ptr failure, const std::string_view fallback) {
    std::string_view detail = fallback;
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const std::exception& error) { detail = error.what(); } catch (...) {
    }
    return std::string{detail.substr(0U, std::min(detail.size(), kVisualFailureByteCapacity))};
}

template <class Sink, class Event>
void publish_visual_event_noexcept(Sink& sink, Event event) noexcept {
    if (!sink) return;
    try {
        sink(std::move(event));
    } catch (...) { sink = {}; }
}

[[nodiscard]] constexpr VisualFrame visual_frame(const PresentationSourceIdentity source, const VisualExtent extent,
                                                 const std::uint64_t revision) noexcept {
    return {
        .source = source,
        .extent = extent,
        .revision = revision,
    };
}

MMLTK_REFLECT_FIELDS(VisualDeviceSettings)
MMLTK_REFLECT_FIELDS(VisualExtent)
MMLTK_REFLECT_FIELDS(VisualRegion)
MMLTK_REFLECT_FIELDS(VisualFrame)

}  // namespace mmltk::controller
