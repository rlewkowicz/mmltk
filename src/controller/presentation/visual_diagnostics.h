#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include "src/controller/contracts/diagnostic_context.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/controller/services/runtime_diagnostic_span.h"
#include "src/frameworks/gpu/image_types.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::frameworks::gpu {
class ImageWorkspace;
}
namespace mmltk::controller {
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
    GalleryReadScheduled[[= detail::VisualDiagnosticName{"gallery.read.scheduled"}]],
    GalleryReadCompleted[[= detail::VisualDiagnosticName{"gallery.read.completed"}]],
    GalleryTransferCompleted[[= detail::VisualDiagnosticName{"gallery.transfer.completed"}]],
    TileBatchPublished[[= detail::VisualDiagnosticName{"tile.batch.published"}]],
    PresentationArenaAdvertised[[= detail::VisualDiagnosticName{"presentation.arena.advertised"}]],
    PresentationAdmissionEnqueued[[= detail::VisualDiagnosticName{"presentation.admission.enqueued"}]],
    PresentationAdmissionWritten[[= detail::VisualDiagnosticName{"presentation.admission.written"}]],
    PresentationImportOutcome[[= detail::VisualDiagnosticName{"presentation.import.outcome"}]],
    PresentationActiveWithdrawal[[= detail::VisualDiagnosticName{"presentation.active.withdrawal"}]],
    PresentationCandidateWithdrawal[[= detail::VisualDiagnosticName{"presentation.candidate.withdrawal"}]],
    PresentationSourceReadSubmitted[[= detail::VisualDiagnosticName{"presentation.source.read_submitted"}]],
    PresentationSourceAdmissionEnqueued[[= detail::VisualDiagnosticName{"presentation.source.admission.enqueued"}]],
    PresentationSourceAdmissionWritten[[= detail::VisualDiagnosticName{"presentation.source.admission.written"}]],
    PresentationSourceTimelineImportStarted[[= detail::VisualDiagnosticName{"presentation.source.timeline_import.started"}]],
    PresentationSourceReady[[= detail::VisualDiagnosticName{"presentation.source.ready"}]],
    PresentationSourceWithdrawal[[= detail::VisualDiagnosticName{"presentation.source.withdrawal"}]],
    PresentationSourceRetirementStarted[[= detail::VisualDiagnosticName{"presentation.source.retirement.started"}]],
    PresentationSourceRetirement[[= detail::VisualDiagnosticName{"presentation.source.retirement"}]],
    PresentationSourceStreamSettlementStarted[[= detail::VisualDiagnosticName{"presentation.source.stream_settlement.started"}]],
    PresentationSourceStreamSettlementCompleted[[= detail::VisualDiagnosticName{"presentation.source.stream_settlement.completed"}]],
    PresentationSourceProbeDeviceReleaseStarted[[= detail::VisualDiagnosticName{"presentation.source.probe_device_release.started"}]],
    PresentationSourceProbeDeviceReleaseCompleted[[= detail::VisualDiagnosticName{"presentation.source.probe_device_release.completed"}]],
    PresentationSourceProbeHostReleaseStarted[[= detail::VisualDiagnosticName{"presentation.source.probe_host_release.started"}]],
    PresentationSourceProbeHostReleaseCompleted[[= detail::VisualDiagnosticName{"presentation.source.probe_host_release.completed"}]],
    PresentationSourceStreamDestructionStarted[[= detail::VisualDiagnosticName{"presentation.source.stream_destruction.started"}]],
    PresentationSourceStreamDestructionCompleted[[= detail::VisualDiagnosticName{"presentation.source.stream_destruction.completed"}]],
    PresentationSourceSignalReleaseStarted[[= detail::VisualDiagnosticName{"presentation.source.signal_release.started"}]],
    PresentationSourceSignalReleaseCompleted[[= detail::VisualDiagnosticName{"presentation.source.signal_release.completed"}]],
    PresentationSourceTimelineDestructionStarted[[= detail::VisualDiagnosticName{"presentation.source.timeline_destruction.started"}]],
    PresentationSourceTimelineDestructionCompleted[[= detail::VisualDiagnosticName{"presentation.source.timeline_destruction.completed"}]],
    PresentationSourceWorkspaceReleaseStarted[[= detail::VisualDiagnosticName{"presentation.source.workspace_release.started"}]],
    PresentationSourceWorkspaceReleaseCompleted[[= detail::VisualDiagnosticName{"presentation.source.workspace_release.completed"}]],
    PresentationFrameEdge[[= detail::VisualDiagnosticName{"presentation.frame.edge"}]],
    PresentationPixel[[= detail::VisualDiagnosticName{"presentation.pixel"}]],
    PresentationPixelProbeStarted[[= detail::VisualDiagnosticName{"presentation.pixel_probe.started"}]],
    PresentationPixelProbeSubmitted[[= detail::VisualDiagnosticName{"presentation.pixel_probe.submitted"}]],
    PresentationPixelAfterRelease[[= detail::VisualDiagnosticName{"presentation.pixel_after_release"}]],
    PresentationPixelSourceCopyBeforeReady[[= detail::VisualDiagnosticName{"presentation.pixel_source_copy_before_ready"}]],
    PresentationPixelSourceCopyAfterRelease[[= detail::VisualDiagnosticName{"presentation.pixel_source_copy_after_release"}]],
    UpscaleStopRequested[[= detail::VisualDiagnosticName{"upscale.stop.requested"}]],
    PresentationReplacement[[= detail::VisualDiagnosticName{"presentation.replacement"}]],
    PresentationRetirement[[= detail::VisualDiagnosticName{"presentation.retirement"}]],
    PresentationPumpStarted[[= detail::VisualDiagnosticName{"presentation.pump.started"}]],
    PresentationPumpCompleted[[= detail::VisualDiagnosticName{"presentation.pump.completed"}]],
    PresentationChannelPumpStarted[[= detail::VisualDiagnosticName{"presentation.channel.pump.started"}]],
    PresentationChannelPumpCompleted[[= detail::VisualDiagnosticName{"presentation.channel.pump.completed"}]],
    PresentationCapabilityPublished[[= detail::VisualDiagnosticName{"presentation.capability.published"}]],
    PresentationReleaseWaitStarted[[= detail::VisualDiagnosticName{"presentation.release_wait.started"}]],
    PresentationReleaseWaitCompleted[[= detail::VisualDiagnosticName{"presentation.release_wait.completed"}]],
    PresentationTerminalReadCompleted[[= detail::VisualDiagnosticName{"presentation.terminal_read.completed"}]],
    PresentationTerminalReadRetained[[= detail::VisualDiagnosticName{"presentation.terminal_read.retained"}]],
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
    ExploreAugmentationImagePrepared[[= detail::VisualDiagnosticName{"explore.augmentation.image.prepared"}]],
    ExploreOverlayDescriptorsPrepared[[= detail::VisualDiagnosticName{"explore.overlay.descriptors.prepared"}]],
    ExploreTransformedBounds[[= detail::VisualDiagnosticName{"explore.overlay.transformed_bounds"}]],
    ExploreSemanticPixels[[= detail::VisualDiagnosticName{"explore.semantic.nonzero_pixels"}]],
    ExploreImagePixels[[= detail::VisualDiagnosticName{"explore.image.pixel_checksum"}]],
    ExploreCardGeometryProbe[[= detail::VisualDiagnosticName{"explore.card.geometry_probe"}]],
    ExploreCardPixelSamples[[= detail::VisualDiagnosticName{"explore.card.pixel_samples"}]],
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
    ExploreCacheStorage[[= detail::VisualDiagnosticName{"explore.cache.storage"}]],
    ExploreStorageGrown[[= detail::VisualDiagnosticName{"explore.storage.grown"}]],
    ExploreCacheTransfer[[= detail::VisualDiagnosticName{"explore.cache.transfer"}]],
    ExploreRenderSubmitted[[= detail::VisualDiagnosticName{"explore.render.submitted"}]],
    ExploreProbeFailed[[= detail::VisualDiagnosticName{"explore.probe.failed"}]],
    ExploreProbeBatchSubmitted[[= detail::VisualDiagnosticName{"explore.probe.batch_submitted"}]],
    ExploreProbeResourcesReleased[[= detail::VisualDiagnosticName{"explore.probe.resources_released"}]],
    UpscaleRequestAdmitted[[= detail::VisualDiagnosticName{"upscale.request.admitted"}]],
    UpscaleWorkerStarted[[= detail::VisualDiagnosticName{"upscale.worker.started"}]],
    UpscaleResultReused[[= detail::VisualDiagnosticName{"upscale.result.reused"}]],
    UpscaleModelSubmitted[[= detail::VisualDiagnosticName{"upscale.model.submitted"}]],
    UpscaleCopyStarted[[= detail::VisualDiagnosticName{"upscale.copy.started"}]],
    UpscaleInputGeometry[[= detail::VisualDiagnosticName{"upscale.input.geometry"}]],
    UpscaleInputAllocation[[= detail::VisualDiagnosticName{"upscale.input.allocation"}]],
    UpscaleOutputAllocation[[= detail::VisualDiagnosticName{"upscale.output.allocation"}]],
    UpscaleOutputAdmissionStarted[[= detail::VisualDiagnosticName{"upscale.output.admission.started"}]],
    UpscaleOutputAdmissionCompleted[[= detail::VisualDiagnosticName{"upscale.output.admission.completed"}]],
    PresentationSourceWaitSubmitted[[= detail::VisualDiagnosticName{"presentation.source.wait_submitted"}]],
    PresentationWorkspaceWait[[= detail::VisualDiagnosticName{"presentation.workspace.wait"}]],
    PresentationWorkspaceService[[= detail::VisualDiagnosticName{"presentation.workspace.service"}]],
    AnnotationRenderQueued[[= detail::VisualDiagnosticName{"annotation.render.queued"}]],
    AnnotationOutputAcquireStarted[[= detail::VisualDiagnosticName{"annotation.output.acquire.started"}]],
    AnnotationOutputUnavailable[[= detail::VisualDiagnosticName{"annotation.output.unavailable"}]],
    AnnotationOutputAcquired[[= detail::VisualDiagnosticName{"annotation.output.acquired"}]],
    AnnotationRenderPublished[[= detail::VisualDiagnosticName{"annotation.render.published"}]],
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
            if (static_cast<std::size_t>([:enumerator:]) != index) throw "visual diagnostic operations require dense unique values starting at zero";
            template for (constexpr auto annotation : mmltk::frameworks::reflection::reflected_annotations<enumerator>()) {
                using Annotation = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
                if constexpr (std::is_same_v<Annotation, VisualDiagnosticName>) {
                    constexpr auto name = std::meta::extract<Annotation>(annotation);
                    const std::string_view alias{name.value};
                    if (alias.empty() || !std::all_of(alias.begin(), alias.end(), [](const char character) {
                            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
                                   character == '.' || character == '_';
                        }))
                        throw "visual diagnostic names require nonempty alphanumeric, dot, or underscore text";
                    names[index] = std::define_static_string(std::string_view{name.value});
                }
            }
            ++index;
        }
        auto sorted_names = names;
        std::sort(sorted_names.begin(), sorted_names.end());
        if (std::adjacent_find(sorted_names.begin(), sorted_names.end()) != sorted_names.end()) throw "visual diagnostic names must be unique";
        return names;
    }
};
inline constexpr auto kVisualDiagnosticNames = mmltk::frameworks::reflection::materialize<VisualDiagnosticOperation>(VisualDiagnosticNameMaterializer{});
}  // namespace detail
[[nodiscard]] constexpr std::string_view visual_diagnostic_event_name(const VisualDiagnosticOperation operation) noexcept {
    const auto index = static_cast<std::size_t>(operation);
    return index < detail::kVisualDiagnosticNames.size() ? detail::kVisualDiagnosticNames[index] : std::string_view{};
}
struct VisualDiagnosticFact final {
    contracts::DiagnosticOwner system = contracts::DiagnosticOwner::Explore;
    VisualDiagnosticOperation operation = VisualDiagnosticOperation::WorkerFailure;
    int device = -1;
    std::uint64_t generation = 0U;
    std::uint64_t value = 0U;
    std::uint64_t detail = 0U;
    mmltk::frameworks::gpu::ImageCopyPath copy_path = mmltk::frameworks::gpu::ImageCopyPath::SameDevice;
    contracts::DiagnosticContext context{};
    std::string_view failure_detail{};
};
[[nodiscard]] inline auto visual_diagnostic_boundary(VisualDiagnosticFact begin, const VisualDiagnosticOperation end) noexcept {
    auto completed = begin;
    completed.operation = end;
    return std::pair{begin, completed};
}
MMLTK_REFLECT_ENUM(VisualDiagnosticOperation)
struct VisualDiagnosticSink final {
    void* context = nullptr;
    void (*write)(void*, VisualDiagnosticFact) noexcept = nullptr;
    void (*write_batch)(void*, std::span<const VisualDiagnosticFact>) noexcept = nullptr;
    bool (*enabled)(void*) noexcept = nullptr;
    bool pixel_probes = false;
    [[nodiscard]] bool valid() const noexcept { return context != nullptr && write != nullptr && (!enabled || enabled(context)); }
    [[nodiscard]] bool pixel_probes_enabled() const noexcept { return pixel_probes && valid(); }
    void operator()(const VisualDiagnosticFact fact) const noexcept {
        if (valid()) write(context, fact);
    }
    void WriteBatch(const std::span<const VisualDiagnosticFact> facts) const noexcept {
        if (!valid()) return;
        if (write_batch != nullptr) {
            write_batch(context, facts);
            return;
        }
        for (const auto& fact : facts) write(context, fact);
    }
    template <class Factory>
    void Emit(Factory&& factory) const noexcept {
        if (!valid()) return;
        try {
            write(context, std::forward<Factory>(factory)());
        } catch (...) {}
    }
};
[[nodiscard]] contracts::DiagnosticSource visual_diagnostic_source(const VisualSourceObservation&) noexcept;
void observe_workspace_storage(contracts::DiagnosticContext&, const mmltk::frameworks::gpu::ImageWorkspace&) noexcept;
[[nodiscard]] services::RuntimeDiagnosticFact visual_runtime_diagnostic(VisualDiagnosticFact) noexcept;
struct VisualWorkspaceDiagnostics final {
    VisualDiagnosticSink sink{};
    contracts::DiagnosticContext context{};
};
// The shell owns the target until after every visual worker is joined. Test
// sinks retain their direct typed injection, including dynamic disablement.
[[nodiscard]] inline VisualDiagnosticSink visual_diagnostic_sink(services::RuntimeDiagnosticTarget& target) noexcept {
    if (!target.valid()) return {};
    return {
        .context = &target,
        .write = [](void* context,
                    VisualDiagnosticFact fact) noexcept { static_cast<services::RuntimeDiagnosticTarget*>(context)->write(visual_runtime_diagnostic(fact)); },
        .write_batch =
            [](void* context, const std::span<const VisualDiagnosticFact> facts) noexcept {
                constexpr std::size_t capacity = 25U;
                auto& runtime_target = *static_cast<services::RuntimeDiagnosticTarget*>(context);
                if (facts.size() > capacity) {
                    for (const auto& fact : facts) runtime_target.write(visual_runtime_diagnostic(fact));
                    return;
                }
                std::array<services::RuntimeDiagnosticFact, capacity> runtime_facts;
                std::ranges::transform(facts, runtime_facts.begin(), visual_runtime_diagnostic);
                runtime_target.write_batch({runtime_facts.data(), facts.size()});
            },
        .enabled = [](void* context) noexcept { return static_cast<services::RuntimeDiagnosticTarget*>(context)->valid(); },
        .pixel_probes = target.pixel_probes_enabled(),
    };
}
void report_visual_worker_failure(VisualDiagnosticSink, contracts::DiagnosticOwner, int, std::string_view, std::uint64_t generation = 0U) noexcept;
inline constexpr std::size_t kVisualFailureByteCapacity = 1024U;
[[nodiscard]] std::string visual_failure_detail(std::exception_ptr, std::string_view fallback);
template <class Sink, class Event>
void publish_visual_event_noexcept(Sink& sink, Event event) noexcept {
    if (!sink) return;
    try {
        sink(std::move(event));
    } catch (...) { sink = {}; }
}
}  // namespace mmltk::controller
