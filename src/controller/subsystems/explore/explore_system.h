#pragma once

#include "src/controller/presentation/visual_source_projection.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/artifact_catalog.h"
#include "src/controller/contracts/explore_filter.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/frameworks/gpu/image_types.h"
#include "src/frameworks/gpu/system_image_model.h"
#include "src/common/system/execution_policy.h"
#include "src/controller/presentation/visual_document.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"

namespace mmltk::controller {

class SettingsSystem;

inline constexpr std::size_t kExploreVisibleItemCapacity = 256U;
inline constexpr std::size_t kExploreLabelCapacity = kExploreVisibleItemCapacity * contracts::kAnnotationObjectCapacity;
inline constexpr std::size_t kExploreMaximumParallelism = 64U;
[[nodiscard]] std::size_t normalize_explore_parallelism(std::size_t requested);
[[nodiscard]] std::size_t normalize_explore_parallelism(std::size_t requested, const mmltk::common::system::ExecutionPlacement&);

enum class ExploreFailureKind : std::uint8_t { None, Operation, RuntimeInitialization, SelectedTransportUnavailable };
enum class ExploreViewportOutcome : std::uint8_t { Ready, VisibleCapacityExceeded, AtlasExtentExceeded };
enum class ExploreMode : std::uint8_t { Gallery, Detail };
enum class ExploreNavigation : std::uint8_t { Previous, Next };
struct ExploreViewport final {
    bool operator==(const ExploreViewport&) const = default;
    VisualExtent extent{};
    std::uint32_t first_row = 0U;
    std::uint32_t row_count = 1U;
    std::uint32_t columns = 1U;
    [[nodiscard]] bool square_geometry() const noexcept {
        return extent.valid() && row_count != 0U && columns != 0U && extent.width % columns == 0U && extent.height % row_count == 0U &&
               extent.width / columns == extent.height / row_count;
    }
    [[nodiscard]] bool valid() const noexcept {
        return square_geometry() && static_cast<std::uint64_t>(row_count) * columns <= kExploreVisibleItemCapacity;
    }
};
[[nodiscard]] constexpr std::uint32_t explore_atlas_card_extent(const ExploreViewport& viewport) noexcept {
    return std::max(
        1U, std::min(viewport.extent.width / std::max(1U, viewport.columns), viewport.extent.height / std::max(1U, viewport.row_count)));
}
struct ExploreOpen final {
    ExploreViewport viewport{};
    [[= mmltk::frameworks::reflection::MaxBytes{4096U}]] std::string compiled_source;
};
struct ExploreViewportUpdate final {
    ExploreViewport viewport{};
    std::optional<std::uint32_t> focused_compiled_index{};
    [[nodiscard]] bool valid() const noexcept { return viewport.square_geometry(); }
};
struct ExploreViewportResult final {
    ExploreViewportUpdate request{};
    ExploreViewportOutcome outcome = ExploreViewportOutcome::Ready;
};
struct ExploreSelect final {
    std::uint32_t compiled_index = 0U;
};
struct ExploreNavigate final {
    ExploreNavigation direction = ExploreNavigation::Next;
};
struct ExploreDatasetFacts final {
    std::uint64_t identity = 0U;
    std::uint32_t image_count = 0U;
    std::uint32_t image_width = 0U;
    std::uint32_t image_height = 0U;
    ExploreClassCatalogIdentity class_catalog_identity = 0U;
    [[= mmltk::frameworks::reflection::MaxItems{kExploreClassCapacity}]] std::vector<contracts::ArtifactClassName> class_names{};
    [[= mmltk::frameworks::reflection::MaxItems{kExploreClassCapacity}]] std::vector<contracts::AnnotationColor> palette{};
};
struct ExploreLabel final {
    contracts::AnnotationBox box{};
    std::uint16_t category = 0U;
    std::uint32_t compiled_index = 0U;
};
struct ExploreOrderFacts final {
    std::uint32_t matching_count = 0U;
    std::uint64_t shuffle_seed = 0U;
    [[= mmltk::frameworks::reflection::MaxItems{kExploreVisibleItemCapacity}]] std::vector<std::uint32_t> visible_indices{};
};
[[nodiscard]] constexpr std::uint64_t explore_visible_indices_digest(const std::span<const std::uint32_t> visible_indices) noexcept {
    constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    std::uint64_t digest = offset;
    const auto append = [&](const std::uint32_t value) {
        for (unsigned shift = 0U; shift != 32U; shift += 8U) {
            digest ^= (value >> shift) & 0xffU;
            digest *= prime;
        }
    };
    for (const auto compiled_index : visible_indices)
        append(compiled_index);
    append(static_cast<std::uint32_t>(visible_indices.size()));
    return digest;
}
struct ExploreAugmentationPreview final {
    bool operator==(const ExploreAugmentationPreview&) const = default;
    bool enabled = false;
    std::uint64_t seed = 0U;
};
struct ExploreDetailView final {
    bool show_original_dimensions = false;
};
struct ExploreAugmentationUpdate final {
    bool enabled = false;
};
struct ExploreDetailUpdate final {
    bool show_original_dimensions = false;
};
struct ExploreGalleryReadiness final {
    std::uint64_t generation = 0U;
    [[= mmltk::frameworks::reflection::MaxItems{kExploreVisibleItemCapacity}]] std::vector<bool> slots{};
};
struct ExploreSnapshot final {
    std::uint64_t revision = 0U;
    bool busy = false;
    bool cancellation_requested = false;
    bool ready = false;
    [[= mmltk::frameworks::reflection::MaxBytes{kVisualFailureByteCapacity}]] std::string failure{};
    ExploreFailureKind failure_kind = ExploreFailureKind::None;
    std::size_t nproc = 1U;
    VisualExtent maximum_atlas_extent{};
    ExploreDatasetFacts dataset{};
    ExploreOrderFacts order{};
    ExploreGalleryReadiness gallery{};
    ExploreViewport viewport{};
    std::optional<ExploreViewportResult> viewport_result{};
    ExploreFilter filter{};
    ExploreOverlay overlay{};
    ExploreAugmentationPreview augmentation{};
    ExploreDetailView detail{};
    ExploreMode mode = ExploreMode::Gallery;
    std::optional<std::uint32_t> selected_image{};
    std::optional<std::uint32_t> focused_image{};
    VisualFrame frame{};
    VisualDocumentFacts document{};
    contracts::AnnotationSceneContent scene{};
    [[= mmltk::frameworks::reflection::MaxItems{kExploreLabelCapacity}]] std::vector<ExploreLabel> labels{};
};

struct ExploreRenderPlan final {
    ExploreViewport viewport{};
    ExploreOverlay overlay{};
    ExploreMode mode = ExploreMode::Gallery;
    std::optional<std::uint32_t> selected_image{};
    std::optional<std::uint32_t> focused_image{};
    mmltk::backend::models::rfdetr::GpuAugmentationConfig augmentation_config{};
    ExploreAugmentationPreview augmentation{};
    ExploreDetailView detail{};
    std::uint64_t dataset_identity = 0U;
    std::uint64_t generation = 0U;
    // Issued by ExploreSystem for native class/box/mask pixels, independently
    // of demand generations and presentation-only overlay fields.
    std::uint64_t semantic_identity = 0U;
};
struct ExploreOpened final {
    ExploreDatasetFacts dataset{};
    ExploreOrderFacts order{};
    std::uint64_t dataset_identity = 0U;
};
struct ExploreOrderCandidate final {
    ExploreFilter filter{};
    ExploreOrderFacts order{};
    std::uint64_t generation = 0U;
};
struct ExploreGalleryPublication final {
    std::uint64_t generation = 0U;
    std::vector<bool> ready_slots{};
    std::size_t cumulative_tiles = 0U;
    std::size_t reused_tiles = 0U;
    std::size_t remaining_tiles = 0U;
    std::size_t active_pinned_bytes = 0U;
    std::size_t stale_discarded = 0U;
};
class ExploreDemandCheck final {
   public:
    ExploreDemandCheck() noexcept = default;
    explicit ExploreDemandCheck(std::shared_ptr<const std::atomic<std::uint64_t>> generation) noexcept
        : generation_(std::move(generation)) {}
    [[nodiscard]] bool operator()(std::uint64_t generation) const noexcept {
        return !generation_ || generation_->load(std::memory_order_acquire) == generation;
    }
    [[nodiscard]] const std::atomic<std::uint64_t>* generation() const noexcept { return generation_.get(); }

   private:
    // Retain only the issuer's authoritative scalar, never the system owner.
    std::shared_ptr<const std::atomic<std::uint64_t>> generation_;
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
enum class ExploreOutputChange : std::uint8_t { Initialize, Semantic, Unchanged };
// Renderer-owned physical high-water capacities. Host bytes cover retained
// cache meaning and render/scheduler metadata, not the compiled dataset, order,
// presentation documents, worker infrastructure, or allocator bookkeeping.
// Cache and descriptor bytes are subsets, not additions to device/pinned bytes.
struct ExploreStorageFootprint final {
    std::size_t host_bytes = 0U;
    std::size_t device_bytes = 0U;
    std::size_t pinned_bytes = 0U;
    std::size_t cache_device_bytes = 0U;
    std::size_t descriptor_bytes = 0U;
    std::size_t augmentation_device_bytes = 0U;
    std::size_t augmentation_pinned_bytes = 0U;
    std::size_t cache_cards = 0U;
};
class ExploreAlgorithm : public mmltk::frameworks::gpu::SystemImageModel {
   public:
    class GalleryReadySink final {
       public:
        GalleryReadySink() noexcept = default;
        explicit GalleryReadySink(std::function<void()> callback)
            : callback_(std::make_shared<const std::function<void()>>(std::move(callback))) {}

        [[nodiscard]] explicit operator bool() const noexcept { return callback_ != nullptr; }
        void operator()() const noexcept {
            if (!callback_) return;
            try {
                (*callback_)();
            } catch (...) {}
        }

       private:
        // CUDA completion-path copies retain only this shared immutable target
        // and therefore cannot allocate.
        std::shared_ptr<const std::function<void()>> callback_;
    };
    ~ExploreAlgorithm() override = default;
    [[nodiscard]] virtual bool UsesLoadingOptions(const mmltk::backend::data::DataLoadingOptions&) const { return true; }
    [[nodiscard]] virtual ExploreOpened Open(std::string_view, std::stop_token) = 0;
    [[nodiscard]] virtual ExploreOrderCandidate PrepareFilter(const ExploreFilter&, std::uint64_t, std::size_t, std::stop_token) = 0;
    virtual void Commit(ExploreOrderCandidate) noexcept = 0;
    virtual void AbortRenderGeneration() = 0;
    virtual void DiscardCandidate() = 0;
    virtual void Reset() = 0;
    [[nodiscard]] virtual ExploreOrderFacts Visible(ExploreViewport, const ExploreOrderCandidate* = nullptr) const = 0;
    [[nodiscard]] virtual bool Contains(std::uint32_t) const = 0;
    [[nodiscard]] virtual VisualExtent DetailExtent(const ExploreRenderPlan& plan) const { return plan.viewport.extent; }
    [[nodiscard]] virtual VisualRegion DetailContent(const ExploreRenderPlan&) const { return {}; }
    [[nodiscard]] virtual std::shared_ptr<const VisualDocument> Document() const { return {}; }
    [[nodiscard]] virtual std::vector<ExploreLabel> Labels() const { return {}; }
    [[nodiscard]] virtual std::optional<std::uint32_t> Adjacent(std::uint32_t, std::int64_t offset) const = 0;
    virtual void SetGalleryReadySink(GalleryReadySink) = 0;
    // Bind once during runtime construction, before any image ingress.
    virtual void SetCurrentDemand(ExploreDemandCheck) = 0;
    [[nodiscard]] virtual ExploreOutputChange OutputChange(const ExploreRenderPlan&, const ExploreOrderCandidate*) const = 0;
    [[nodiscard]] virtual ExploreStorageFootprint StorageFootprint() const { return {}; }
    // Logical candidate meaning follows the prepared GPU output. Commit this
    // publication before committing its order; discard an order only after
    // checked rollback. False means physical completion requires retirement.
    virtual void PrepareOutputPublication(ExploreOutputChange) = 0;
    virtual void CommitOutputPublication() noexcept = 0;
    [[nodiscard]] virtual bool RollbackOutputPublication() noexcept = 0;
    [[nodiscard]] virtual ExploreGalleryPublication BeginGallery(const ExploreRenderPlan&, const ExploreOrderCandidate*, std::size_t,
                                                                 mmltk::frameworks::gpu::ImagePlaneView,
                                                                 mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) = 0;
    [[nodiscard]] virtual ExploreGalleryPublication AdvanceGallery() = 0;
    [[nodiscard]] virtual bool HasGalleryTiles() const = 0;
    [[nodiscard]] virtual ExploreGalleryPublication PublishGalleryTiles(mmltk::frameworks::gpu::ImagePlaneView,
                                                                        mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) = 0;
    virtual void RenderDetail(const ExploreRenderPlan&, std::size_t, mmltk::frameworks::gpu::ImagePlaneView,
                              // CLEANUP-IGNORE: Explore rendering and its typed events are distinct from Upscale's snapshot and events.
                              mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) = 0;
    // CLEANUP-IGNORE: ExploreAlgorithm is a sealed domain interface despite matching another class terminator.
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::LatestState}]] ExploreChanged final {
    ExploreSnapshot snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] ExploreFailed final {
    ExploreSnapshot snapshot{};
    [[= mmltk::frameworks::reflection::MaxBytes{kVisualFailureByteCapacity}]] std::string detail;
};

class ExploreSystem final {
   public:
    using visual_source = VisualSourceProjection<ExploreSnapshot, PresentationSourceKind::Explore,
                                                 mmltk::frameworks::reflection::member_path<&ExploreSnapshot::frame>,
                                                 mmltk::frameworks::reflection::member_path<&ExploreSnapshot::revision>>;
    using event_type = std::variant<ExploreChanged, ExploreFailed>;
    // CLEANUP-IGNORE: Explore construction retains its own generated system identity and runtime dependencies.
    ExploreSystem(SettingsSystem&, VisualDeviceSettings, std::size_t nproc, VisualRuntimeFactory, SystemEventSink<event_type> = {},
                  VisualDiagnosticSink = {});
    ~ExploreSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot Open(ExploreOpen);
    [[= contracts::reflection::direct::InteractionEndpoint{true}]] void UpdateViewport(ExploreViewportUpdate);
    // CLEANUP-IGNORE: Interaction-generation observation is not part of the later reflected intent sequence.
    [[nodiscard]] std::uint64_t LastInteractionGeneration() const;
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot UpdateFilter(ExploreFilterUpdate);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot UpdateOverlay(ExploreOverlay);
    // CLEANUP-IGNORE: Reroll is an independently identified reflected domain endpoint.
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot Reroll();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot UpdateAugmentation(ExploreAugmentationUpdate);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot RerollAugmentation();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot UpdateDetail(ExploreDetailUpdate);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot Select(ExploreSelect);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot Navigate(ExploreNavigate);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot CloseDetail();
    // CLEANUP-IGNORE: Stop is an independently identified Explore endpoint on the sealed facade.
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ExploreSnapshot Stop() noexcept;
    void ExecutionSettingsChanged() noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[= contracts::reflection::Snapshot{contracts::kAnnotationUiStateByteBudget}]] [[nodiscard]] ExploreSnapshot snapshot() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
    [[nodiscard]] VisualDocumentRead BorrowDocument(const VisualFrame&) const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class ExploreAcceptanceGate final {
   public:
    enum class PublicationStage : std::uint8_t { None, DescriptorsPrepared, ProductPrepared };
    struct ProductObservation final {
        bool released = false;
        std::weak_ptr<const void> artifact;
        std::size_t logical_size = 0U;
        std::size_t capacity_before = 0U;
        std::size_t capacity_after = 0U;
        std::size_t copied_entries = 0U;
    };
    // Test controls are installed before constructing the native runtime.
    void SetProductObserver(void*, void (*)(void*, ProductObservation) noexcept) noexcept;
    void ObserveProduct(ProductObservation) const noexcept;
    enum class SubmissionStage : std::uint8_t { Foreground, Background, Probe };
    void SetSubmissionObserver(void*, void (*)(void*, std::uintptr_t, SubmissionStage)) noexcept;
    void ObserveSubmission(std::uintptr_t, SubmissionStage) const;
    // Install before admitting reads; invoked outside the lane control mutex.
    void SetReadObserver(void*, void (*)(void*, std::uint64_t, std::uint32_t)) noexcept;
    void ObserveRead(std::uint64_t, std::uint32_t) const;
    void FailNextPublicationAt(PublicationStage) noexcept;
    void CheckPublication(PublicationStage) const;
    void FailNextProbe() noexcept;
    void CheckProbe() const;
    enum class WaitResult : std::uint8_t {
        Proceed,
        Stale,
    };
    enum class ControlEvent : std::uint8_t {
        InitialWait = 0x80U,
        HeldWait = 0x81U,
        HeldProceed = 0x82U,
        HeldStale = 0x83U,
    };
    struct ControlObservation final {
        ControlEvent event = ControlEvent::InitialWait;
        std::uint64_t generation = 0U;
        std::uint64_t slot = 0U;
        std::uint64_t compiled_index = 0U;
        std::uint64_t staging_bytes = 0U;
    };

    explicit ExploreAcceptanceGate(int command_descriptor);
    ~ExploreAcceptanceGate();
    ExploreAcceptanceGate(const ExploreAcceptanceGate&) = delete;
    ExploreAcceptanceGate& operator=(const ExploreAcceptanceGate&) = delete;
    void AdvanceGeneration(std::uint64_t generation) noexcept;
    [[nodiscard]] WaitResult AwaitInitialRelease(std::uint64_t generation);
    [[nodiscard]] WaitResult AwaitHeldCompletion(std::uint64_t generation, std::uint64_t slot, std::uint64_t compiled_index,
                                                 std::uint64_t staging_bytes);
    [[nodiscard]] bool ClaimHeldCompletion();
    [[nodiscard]] bool ClaimTerminalReport();
    void Stop() noexcept;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct ExploreNativeConfiguration final {
    std::size_t image_limit = 1'000'000U;
    mmltk::backend::data::DataLoadingOptions loading{};
    std::shared_ptr<ExploreAcceptanceGate> acceptance{};
    VisualDiagnosticSink diagnostics{};
};
[[nodiscard]] VisualRuntimeFactory make_native_explore_runtime_factory(
    VisualDeviceSettings, std::size_t,
    // CLEANUP-IGNORE: This native factory and canonical Explore registrations do not duplicate provider schemas.
    ExploreNativeConfiguration, std::optional<mmltk::frameworks::gpu::DeviceExecution> execution = {});

// CLEANUP-IGNORE: Explore's separately identified canonical types are not Annotation's reflection declarations.
MMLTK_REFLECT_ENUM(ExploreFailureKind)
MMLTK_REFLECT_ENUM(ExploreViewportOutcome)
MMLTK_REFLECT_ENUM(ExploreMode)
MMLTK_REFLECT_ENUM(ExploreNavigation)
MMLTK_REFLECT_FIELDS(ExploreViewport)
MMLTK_REFLECT_FIELDS(ExploreOpen)
MMLTK_REFLECT_FIELDS(ExploreViewportUpdate)
MMLTK_REFLECT_FIELDS(ExploreViewportResult)
MMLTK_REFLECT_FIELDS(ExploreSelect)
MMLTK_REFLECT_FIELDS(ExploreNavigate)
MMLTK_REFLECT_FIELDS(ExploreDatasetFacts)
MMLTK_REFLECT_FIELDS(ExploreLabel)
MMLTK_REFLECT_FIELDS(ExploreOrderFacts)
MMLTK_REFLECT_FIELDS(ExploreAugmentationPreview)
MMLTK_REFLECT_FIELDS(ExploreDetailView)
MMLTK_REFLECT_FIELDS(ExploreAugmentationUpdate)
MMLTK_REFLECT_FIELDS(ExploreDetailUpdate)
MMLTK_REFLECT_FIELDS(ExploreGalleryReadiness)
MMLTK_REFLECT_FIELDS(ExploreSnapshot)
MMLTK_REFLECT_FIELDS(ExploreChanged)
MMLTK_REFLECT_FIELDS(ExploreFailed)

}  // namespace mmltk::controller
