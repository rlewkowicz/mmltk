#include "src/controller/presentation/annotation_palette.h"
#include "src/backend/models/rfdetr/augmentation/sampling.h"
#include "src/controller/subsystems/explore/detail/gallery_stream.h"
#include "src/controller/subsystems/explore/detail/gallery_read_scheduler.h"
#include "src/controller/subsystems/explore/detail/gallery_payload.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/controller/subsystems/explore/detail/gallery_atlas.h"
#include "src/backend/data/compiled_image_stream.h"
#include "src/backend/data/compiled_dataset.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/controller/subsystems/explore/native_explore_storage.h"
#include "src/controller/subsystems/explore/detail/gallery_descriptor_storage.h"
#include "src/controller/subsystems/explore/detail/gallery_stream_probe.h"
#include "src/controller/subsystems/explore/detail/gallery_host_allocations.h"
#include "src/backend/imaging/explore/explore_render_storage.h"
#include <cuda_runtime_api.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <vector>
#include "src/backend/data/compiled_format.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/backend/imaging/explore/mask_sample.h"
#include "src/backend/models/rfdetr/augmentation/spatial_erasure.h"
import mmltk.backend.imaging.explore.compiled_explore_store;
import mmltk.backend.imaging.explore.explore_render_core;
import mmltk.backend.models.rfdetr.augmentation.augmentation_metadata;
namespace mmltk::controller::explore_detail {
namespace data = mmltk::backend::data;
namespace explore = mmltk::backend::imaging::explore;
namespace rfdetr = mmltk::backend::models::rfdetr;
std::size_t GalleryProductState::Capacity() const noexcept {
    return visible_indices.capacity() + window_indices.capacity() + cache.capacity() + active_classes.capacity() + priority_slots.capacity() +
           completed_slots.capacity() + tile_meanings.capacity() + plan.overlay.class_selection.classes.capacity();
}
std::size_t GalleryProductState::MetadataBytes() const noexcept {
    return (visible_indices.capacity() + window_indices.capacity() + priority_slots.capacity() + plan.overlay.class_selection.classes.capacity()) *
               sizeof(std::uint32_t) +
           active_classes.capacity() * sizeof(decltype(active_classes)::value_type) + (completed_slots.capacity() + 7U) / 8U +
           tile_meanings.capacity() * sizeof(decltype(tile_meanings)::value_type);
}
std::size_t GalleryProductState::Size() const noexcept {
    return visible_indices.size() + window_indices.size() + cache.size() + active_classes.size() + priority_slots.size() + completed_slots.size() +
           tile_meanings.size() + plan.overlay.class_selection.classes.size() + annotated_indices.size() + static_cast<std::size_t>(bool(store)) +
           static_cast<std::size_t>(bool(document)) + static_cast<std::size_t>(bool(detail_meaning));
}
void GalleryProductState::ReserveFor(const GalleryProductState& source) {
    visible_indices.reserve(source.visible_indices.size());
    window_indices.reserve(source.window_indices.size());
    cache.Reserve(source.cache.size());
    active_classes.reserve(source.active_classes.size());
    priority_slots.reserve(source.window_indices.size());
    completed_slots.reserve(source.visible_indices.size());
    tile_meanings.reserve(source.visible_indices.size());
    plan.overlay.class_selection.classes.reserve(source.plan.overlay.class_selection.classes.size());
}
void GalleryProductState::Clear() noexcept {
    store.reset();
    viewport = {};
    atlas = {};
    const auto clear_overlay = [](ExploreOverlay& value) {
        auto classes = std::move(value.class_selection.classes);
        value = {};
        value.class_selection.classes = std::move(classes);
        value.class_selection.classes.clear();
    };
    auto retained_overlay = std::move(plan.overlay);
    plan = {};
    plan.overlay = std::move(retained_overlay);
    clear_overlay(plan.overlay);
    document.reset();
    detail_meaning.reset();
    detail_view = {};
    visible_indices.clear();
    window_indices.clear();
    window_first = 0U;
    source_window = nullptr;
    cache.Clear();
    annotated_indices = {};
    active_classes.clear();
    priority_slots.clear();
    completed_slots.clear();
    cumulative_tiles = 0U;
    reused_tiles = 0U;
    cache_active = 0U;
    tile_meanings.clear();
}
namespace {
struct DescriptorBudget final {
    std::size_t annotations = 0U;
    std::size_t runs = 0U;
    [[nodiscard]] bool Admit(const std::size_t added_annotations, const std::size_t added_runs) noexcept {
        if (added_annotations > explore::kExploreRenderAnnotationCapacity - annotations || added_runs > explore::kExploreRenderRleCapacity - runs) return false;
        annotations += added_annotations;
        runs += added_runs;
        return true;
    }
    [[nodiscard]] bool AdmitDonor(const std::size_t source_annotations, const std::size_t source_runs, const std::size_t donor_runs) noexcept {
        // A render batch has its own staging ceiling; each image additionally
        // retains the bounded editable-document contract across copy/upscale.
        if (source_annotations >= contracts::kAnnotationObjectCapacity || source_runs > contracts::kAnnotationMaskRunCapacity ||
            donor_runs > contracts::kAnnotationMaskRunCapacity - source_runs)
            return false;
        return Admit(1U, donor_runs);
    }
};
}  // namespace
class GalleryStream::Impl final : public std::enable_shared_from_this<GalleryStream::Impl> {
   public:
    Impl(std::size_t nproc, const mmltk::frameworks::gpu::DeviceExecution&, const ExploreNativeConfiguration&, std::uint32_t maximum_height,
         mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement, mmltk::frameworks::gpu::TerminalCudaRetirementLease lease);
    ~Impl();
    void Retire(cudaError_t) noexcept;
    void CheckSettlement(cudaError_t, const char*);
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release TerminalRelease() const noexcept {
        return {.all_released = false, .failure = terminal_failure_};
    }
    void SetStreamSettlement(decltype(&cudaStreamSynchronize) operation) { stream_wait_ = operation; }
    void BindExecutionContext(const mmltk::frameworks::gpu::DeviceContext& context, std::shared_ptr<mmltk::frameworks::gpu::ImageStream> stream) {
        retained_context_ = context;
        retained_stream_ = std::move(stream);
        stream_ = reinterpret_cast<cudaStream_t>(retained_stream_->native_handle());
    }
    [[nodiscard]] mmltk::common::concurrency::WorkerPool& workers() noexcept;
    void SetCurrentDemand(ExploreDemandCheck check) { scheduler_.SetCurrentDemand(std::move(check)); }
    void SetReadySink(ExploreAlgorithm::GalleryReadySink);
    void StopIngress() noexcept;
    [[nodiscard]] bool HasReadyTiles() const;
    [[nodiscard]] ExploreStorageFootprint StorageFootprint() const;
    [[nodiscard]] ExploreOutputChange OutputChange(const ExploreRenderPlan& plan, std::span<const std::uint32_t> visible, const data::CompiledDataset* store,
                                                   std::span<const std::uint32_t> window) const {
        const auto& prior = plan.mode == selected_mode_ ? State() : retained_;
        if (plan.mode == ExploreMode::Detail && prior.plan.mode == ExploreMode::Detail && prior.store.get() == store && prior.document &&
            prior.detail_meaning && plan.selected_image == prior.plan.selected_image && plan.dataset_identity == prior.plan.dataset_identity &&
            plan.augmentation == prior.plan.augmentation && plan.augmentation_config == prior.plan.augmentation_config)
            return plan.semantic_identity == prior.plan.semantic_identity ? ExploreOutputChange::Unchanged : ExploreOutputChange::Semantic;
        if (plan.mode != ExploreMode::Gallery || prior.plan.mode != ExploreMode::Gallery || prior.store.get() != store || plan.viewport != prior.viewport ||
            visible.size() != prior.visible_indices.size() || window.data() != prior.source_window || window.size() != prior.window_indices.size() ||
            plan.dataset_identity != prior.plan.dataset_identity || plan.augmentation != prior.plan.augmentation ||
            plan.augmentation_config != prior.plan.augmentation_config)
            return ExploreOutputChange::Initialize;
        return plan.semantic_identity == prior.plan.semantic_identity ? ExploreOutputChange::Unchanged : ExploreOutputChange::Semantic;
    }
    // CLEANUP-IGNORE: The private implementation mirrors this method at the single sealed GalleryStream pimpl boundary.
    [[nodiscard]] ExploreGalleryPublication Begin(const ExploreRenderPlan&, std::span<const std::uint32_t>, std::span<const std::uint32_t>, std::size_t,
                                                  std::shared_ptr<const mmltk::backend::data::CompiledDataset>,
                                                  // CLEANUP-IGNORE: The private Begin signature mirrors the public facade without leaking Impl.
                                                  std::span<const std::uint32_t>, std::span<const explore::ExploreRenderClassDescriptor>,
                                                  mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    // CLEANUP-IGNORE: The private execution surface mirrors the public facade at its one pimpl boundary.
    [[nodiscard]] ExploreGalleryPublication Advance();
    void PrepareDetailOutput(mmltk::frameworks::gpu::ImageAllocation allocation) noexcept { atlas_.Invalidate(allocation); }
    void PrepareOutputPublication(ExploreOutputChange, ExploreMode = ExploreMode::Gallery);
    void CommitOutputPublication() noexcept;
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceCoverage WorkspaceCoverage(const mmltk::frameworks::gpu::ImageWorkspaceObservation& output) {
        return atlas_.WorkspaceCoverage(output);
    }
    [[nodiscard]] bool RollbackOutputPublication() noexcept;
    [[nodiscard]] ExploreGalleryPublication PublishTiles(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    void RenderDetail(const ExploreRenderPlan&, std::shared_ptr<const mmltk::backend::data::CompiledDataset>, std::span<const std::uint32_t>,
                      std::span<const explore::ExploreRenderClassDescriptor>, mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView,
                      std::uintptr_t);
    void Quiesce();
    void Suspend();
    [[nodiscard]] std::shared_ptr<const VisualDocument> Document() const { return State().document; }
    [[nodiscard]] std::vector<ExploreLabel> Labels() const;
    void ClearLogicalState();
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release ReleaseAfterRuntimeSettlement() noexcept;
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release ResetBuffersChecked() noexcept;

   private:
    using Lane = GalleryReadScheduler::Lane;
    using LaneState = GalleryReadScheduler::LaneState;
    using PayloadLayout = GalleryReadScheduler::PayloadLayout;
    using TileMeaning = GalleryTileMeaning;
    [[nodiscard]] const GalleryThumbnailCache* ProtectedCache() const noexcept {
        return publication_active_ && State().cache_active == committed_.cache_active ? &committed_.cache : nullptr;
    }
    void ClearReadinessState();
    [[nodiscard]] ExploreGalleryPublication PublicationFacts(std::size_t) const;
    [[nodiscard]] std::shared_ptr<const TileMeaning> CaptureMeaning(explore::ExploreRenderCardDescriptor, std::size_t, std::size_t, std::size_t,
                                                                    std::size_t) const;
    [[nodiscard]] ExploreGalleryPublication RenderReadyTiles(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t,
                                                             bool);
    [[nodiscard]] mmltk::frameworks::gpu::ImagePlaneView CachePlane(bool) const;
    void PrepareCacheWrite(std::uintptr_t);
    [[nodiscard]] std::uint8_t WritableCacheBank(std::size_t, bool semantic = false) const;
    void PlaceTile(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uint32_t, std::uintptr_t, bool semantic_only = false);
    [[nodiscard]] std::uint32_t AtlasY(const std::size_t logical) const noexcept {
        return static_cast<std::uint32_t>((State().atlas.row_origin + logical / State().atlas.columns) % State().atlas.row_capacity) *
               State().atlas.card_extent;
    }
    void RenderCachedSemantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    [[nodiscard]] VisualDiagnosticFact Diagnostic(const VisualDiagnosticOperation operation, const std::uint64_t generation) const noexcept {
        return {.system = contracts::DiagnosticOwner::Explore, .operation = operation, .device = device_, .generation = generation};
    }
    [[nodiscard]] const rfdetr::AugmentationBatchPlan* PrepareImages(std::span<Lane* const>, DescriptorBudget, cudaStream_t);
    [[nodiscard]] explore::ExploreRenderCardDescriptor AssembleImageMeaning(const Lane&, const rfdetr::AugmentationImagePlan*, const float*, std::size_t,
                                                                            std::size_t&, std::size_t&);
    void CompleteTiles(bool synchronized);
    void SeedPlaceholders(std::span<const explore::ExploreRenderClassDescriptor>, mmltk::frameworks::gpu::ImagePlaneView,
                          mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    void RestoreVisibleTiles(std::span<const explore::ExploreRenderClassDescriptor>, mmltk::frameworks::gpu::ImagePlaneView,
                             mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    enum class AtlasBatch : std::uint8_t { Placeholders, Cache };
    enum class BatchSubmission : std::uint8_t { Skipped, Complete };
    [[nodiscard]] BatchSubmission RenderAtlasBatch(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t,
                                                   std::uint32_t, std::uint64_t, AtlasBatch);
    void RenderAtlasPlane(explore::ExploreRenderAtlasView, const explore::ExploreRenderTileBatchView&, const ExploreOverlay&,
                          mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, bool, explore::detail::ExploreRenderDemand);
    void RenderDetailPlane(explore::ExploreRenderDetailView, const ExploreOverlay&, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, bool);
    void DiagnoseRendered(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, std::uint64_t, std::uint64_t,
                          std::uint32_t, std::optional<std::size_t> = std::nullopt, std::uint32_t = 0U, std::uint32_t = 0U, std::uint32_t = 0U,
                          std::uint32_t = 0U);
    [[nodiscard]] explore::ExploreRenderScratchView Scratch() const;
    [[nodiscard]] explore::ExploreRenderSemanticView Semantics(const ExploreOverlay&, bool) const;
    void UploadDescriptors(cudaStream_t, bool);
    void SettleDescriptors();
    static void Clear(mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    std::optional<mmltk::frameworks::gpu::DeviceContext> retained_context_;
    std::shared_ptr<mmltk::frameworks::gpu::ImageStream> retained_stream_;
    mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement_;
    mmltk::frameworks::gpu::TerminalCudaRetirementLease terminal_lease_;
    std::exception_ptr terminal_failure_;
    decltype(&cudaStreamSynchronize) stream_wait_ = &cudaStreamSynchronize;
    std::shared_ptr<ExploreAcceptanceGate> acceptance_;
    GalleryReadScheduler scheduler_;
    [[nodiscard]] explore::detail::ExploreRenderDemand Demand() const noexcept {
        return {.latest_generation = scheduler_.current_demand_.generation(), .generation = State().plan.generation};
    }
    cudaStream_t stream_ = nullptr;
    // CLEANUP-IGNORE: Typed gallery scheduling and augmentation buffers are not capture-session atomic counters.
    // Complete inactive construction is reserved for initialization. A patch
    // journals only changed slots and bounded overlay facts; exact reuse stores
    // only its pending non-pixel plan facts.
    GalleryProductState committed_;
    GalleryProductState candidate_;
    GalleryProductState retained_;
    ExploreMode selected_mode_ = ExploreMode::Gallery;
    bool mode_switched_ = false;
    GalleryAtlas atlas_;
    ExploreOutputChange publication_change_ = ExploreOutputChange::Initialize;
    [[nodiscard]] bool CompleteCandidate() const noexcept { return publication_active_ && publication_change_ == ExploreOutputChange::Initialize; }
    [[nodiscard]] GalleryProductState& State() noexcept { return CompleteCandidate() ? candidate_ : committed_; }
    [[nodiscard]] const GalleryProductState& State() const noexcept { return CompleteCandidate() ? candidate_ : committed_; }
    bool publication_active_ = false;
    bool rollback_failed_ = false;
    struct SlotUndo final {
        std::size_t slot = 0U;
        GalleryThumbnailCache::Entry entry;
        std::size_t visible = kExploreVisibleItemCapacity;
        std::shared_ptr<const TileMeaning> meaning;
        bool completed = false;
    };
    std::array<SlotUndo, kExploreVisibleItemCapacity> slot_undo_{};
    std::size_t undo_count_ = 0U;
    std::vector<std::uint64_t> undo_marks_;
    std::uint64_t undo_sequence_ = 0U;
    struct ReusedPlan final {
        ExploreViewport viewport{};
        ExploreScrollDirection scroll_direction = ExploreScrollDirection::Forward;
        ExploreDetailView detail{};
        std::optional<std::uint32_t> selected_image;
        std::uint64_t generation = 0U;
        bool show_labels = false;
    } pending_plan_;
    void PrepareReuse(const ExploreRenderPlan& plan) noexcept {
        pending_plan_ = {plan.viewport, plan.scroll_direction, plan.detail, plan.selected_image, plan.generation, plan.overlay.show_labels};
    }
    std::size_t prior_cumulative_ = 0U;
    std::size_t prior_reused_ = 0U;
    ExploreAtlasLayout prior_atlas_{};
    bool overlay_undo_ = false;
    bool borrowed_cache_ = false;
    cudaStream_t incumbent_stream_ = nullptr;
    void SaveOverlay();
    void SaveSlot(std::size_t);
    void ClearUndo() noexcept;
    void ClearInactiveProduct() noexcept {
        ExploreAcceptanceGate::ProductObservation observation;
        if (acceptance_) {
            observation.released = true;
            observation.artifact = candidate_.store;
            observation.capacity_before = candidate_.Capacity();
        }
        candidate_.Clear();
        if (acceptance_) {
            observation.logical_size = candidate_.Size();
            observation.capacity_after = candidate_.Capacity();
            acceptance_->ObserveProduct(std::move(observation));
        }
    }
    ExploreHostAllocations host_allocations_;
    NativeExploreStorage storage_;
    GalleryDescriptorStorage descriptors_;
    std::vector<rfdetr::AugmentationPreviewAnnotation> projected_annotations_;
    std::uint32_t maximum_height_ = 0U;
    int device_ = 0;
    // Only the GPU owner clears this after an explicit stream boundary; an
    // older lane callback may run while newer work is already queued.
    VisualDiagnosticSink diagnostics_{};
    std::unique_ptr<GalleryStreamProbe> probe_;
    void FlushProbes(std::uintptr_t stream) {
        if (probe_) probe_->FlushProbes(stream);
    }
    bool resources_released_ = false;
};
GalleryStream::Impl::Impl(const std::size_t nproc, const mmltk::frameworks::gpu::DeviceExecution& execution, const ExploreNativeConfiguration& configuration,
                          const std::uint32_t maximum_height, mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement,
                          mmltk::frameworks::gpu::TerminalCudaRetirementLease lease)
    : retirement_(retirement),
      terminal_lease_(std::move(lease)),
      acceptance_(configuration.acceptance),
      scheduler_(nproc, execution, configuration),
      maximum_height_(maximum_height),
      device_(execution.device),
      diagnostics_(configuration.diagnostics) {
    if (diagnostics_.pixel_probes_enabled()) probe_ = std::make_unique<GalleryStreamProbe>(device_, diagnostics_, acceptance_);
    storage_.Bind(host_allocations_.api());
}
void GalleryStream::Impl::Retire(const cudaError_t status) noexcept {
    if (!terminal_lease_) return;
    scheduler_.StopIngress();
    try {
        mmltk::frameworks::gpu::ensure_cuda_ok(status, "Explore gallery settlement failed");
    } catch (...) { terminal_failure_ = std::current_exception(); }
    std::move(terminal_lease_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(shared_from_this()), status);
    // Join CPU ingress/completion workers without attempting GPU storage release.
    try {
        scheduler_.image_stream_.stop_workers();
    } catch (...) {}
}
void GalleryStream::Impl::CheckSettlement(const cudaError_t status, const char* detail) {
    if (status == cudaSuccess) return;
    Retire(status);
    mmltk::frameworks::gpu::ensure_cuda_ok(status, detail);
}
GalleryStream::Impl::~Impl() = default;
void GalleryStream::Impl::SetReadySink(ExploreAlgorithm::GalleryReadySink sink) { scheduler_.SetReadySink(std::move(sink)); }
void GalleryStream::Impl::StopIngress() noexcept { scheduler_.StopIngress(); }
bool GalleryStream::Impl::HasReadyTiles() const { return scheduler_.HasReadyTiles(); }
mmltk::common::concurrency::WorkerPool& GalleryStream::Impl::workers() noexcept { return scheduler_.image_stream_.workers(); }
ExploreGalleryPublication GalleryStream::Impl::Begin(const ExploreRenderPlan& plan, std::span<const std::uint32_t> visible,
                                                     // CLEANUP-IGNORE: Gallery scheduling and detail rendering accept
                                                     // distinct operations despite sharing immutable render inputs.
                                                     std::span<const std::uint32_t> window, const std::size_t window_first,
                                                     std::shared_ptr<const mmltk::backend::data::CompiledDataset> store,
                                                     const std::span<const std::uint32_t> annotated_indices,
                                                     const std::span<const explore::ExploreRenderClassDescriptor> classes,
                                                     const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                                     const std::uintptr_t stream) {
    if (!publication_active_) throw std::logic_error("Explore gallery output was not prepared");
    const auto output_change = OutputChange(plan, visible, store.get(), window);
    if (publication_change_ == ExploreOutputChange::Unchanged) {
        if (output_change != ExploreOutputChange::Unchanged) throw std::logic_error("Explore exact reuse changed during preparation");
        PrepareReuse(plan);
        return {.generation = plan.generation,
                .layout = State().atlas,
                .cumulative_tiles = State().cumulative_tiles,
                .remaining_tiles = State().visible_indices.size() - State().cumulative_tiles};
    }
    if (output_change != ExploreOutputChange::Initialize) {
        const bool direction_changed = State().plan.scroll_direction != plan.scroll_direction;
        SaveOverlay();
        const auto previous = State().plan.generation;
        State().plan = plan;
        State().active_classes.assign(classes.begin(), classes.end());
        scheduler_.desired_generation_.store(plan.generation, std::memory_order_release);
        {
            std::scoped_lock lock(scheduler_.lanes_mutex_);
            for (auto& lane : scheduler_.lanes_)
                if (lane->generation == previous && lane->state == LaneState::InputReady) {
                    lane->generation = plan.generation;
                    lane->reserved_generation = plan.generation;
                    scheduler_.scheduled_slots_[State().cache.Slot(lane->position)] = plan.generation;
                }
        }
        if (output_change == ExploreOutputChange::Semantic) {
            stream_ = reinterpret_cast<cudaStream_t>(stream);
            RestoreVisibleTiles(classes, clean, semantic, stream);
            RenderCachedSemantics(clean, semantic, stream);
        }
        if (direction_changed)
            scheduler_.Prioritize(State());
        else
            scheduler_.next_priority_ = 0U;
        return PublicationFacts(0U);
    }
    if (!CompleteCandidate()) throw std::logic_error("Explore initialization requires complete inactive construction");
    const auto side = explore_atlas_card_extent(plan.viewport);
    const GalleryThumbnailCache::Identity identity{.incarnation = store.get(),
                                                   .dataset = plan.dataset_identity,
                                                   .seed = plan.augmentation.seed,
                                                   .augmentation = plan.augmentation_config,
                                                   .extent = side,
                                                   .augmented = plan.augmentation.enabled};
    const auto maximum_rows = std::min<std::size_t>(kExploreVisibleItemCapacity / plan.viewport.columns, maximum_height_ / side);
    const auto retained_capacity =
        std::min<std::size_t>(store->header().num_images, (maximum_rows + 2U * GalleryThumbnailCache::kNeighborRows) * plan.viewport.columns);
    if (identity == committed_.cache.identity() && retained_capacity <= committed_.cache.size()) {
        std::swap(State().cache, committed_.cache);
        borrowed_cache_ = true;
        State().cache.BeginUpdate();
    } else {
        State().cache = committed_.cache;
        State().cache.Configure(retained_capacity, identity);
    }
    State().cache.Admit(window, window_first);
    State().tile_meanings.assign(visible.size(), {});
    State().store = std::move(store);
    State().annotated_indices = annotated_indices;
    State().active_classes.assign(classes.begin(), classes.end());
    stream_ = reinterpret_cast<cudaStream_t>(stream);
    State().visible_indices.assign(visible.begin(), visible.end());
    State().window_indices.assign(window.begin(), window.end());
    State().source_window = window.data();
    State().window_first = window_first;
    State().viewport = plan.viewport;
    State().plan = plan;
    committed_.ReserveFor(State());
    PrepareCacheWrite(stream);
    if (acceptance_)
        acceptance_->ObserveProduct(
            {.artifact = State().store, .logical_size = State().Size(), .capacity_before = State().Capacity(), .capacity_after = State().Capacity()});
    scheduler_.desired_generation_.store(plan.generation, std::memory_order_release);
    if (acceptance_) acceptance_->AdvanceGeneration(plan.generation);
    State().cumulative_tiles = 0U;
    State().reused_tiles = 0U;
    scheduler_.next_priority_ = 0U;
    State().completed_slots.assign(State().visible_indices.size(), false);
    scheduler_.scheduled_slots_.resize(State().cache.size());
    undo_marks_.resize(State().cache.size());
    scheduler_.Prioritize(State());
    // Incumbent physical lanes remain untouched until this logical candidate
    // commits. Restore every valid cached visible tile before continuation
    // admits misses; this applies equally to either side of a scroll reversal.
    // The ordinary continuation scan rebinds settled useful inputs.
    SeedPlaceholders(classes, clean, semantic, stream);
    bool refresh_semantics = false;
    for (std::size_t slot = 0U; slot < State().visible_indices.size(); ++slot) {
        const auto position = static_cast<std::size_t>(plan.viewport.first_row) * plan.viewport.columns + slot;
        const auto* entry = State().cache.Retained(State().visible_indices[slot]);
        if (!entry) continue;
        State().tile_meanings[slot] = entry->meaning;
        refresh_semantics |= entry->semantic_identity != plan.semantic_identity;
        PlaceTile(clean, semantic, static_cast<std::uint32_t>(slot), stream);
        if (!entry->refresh_pending) {
            scheduler_.scheduled_slots_[State().cache.Slot(position)] = plan.generation;
            ++State().reused_tiles;
        }
    }
    if (refresh_semantics) {
        RenderCachedSemantics(clean, semantic, stream);
    } else if (diagnostics_.pixel_probes_enabled()) {
        for (std::size_t slot = 0U; slot < State().tile_meanings.size(); ++slot) {
            if (!State().tile_meanings[slot]) continue;
            const auto x = static_cast<std::uint32_t>(slot % State().viewport.columns) * side;
            const auto y = AtlasY(slot);
            DiagnoseRendered(clean, semantic, stream, plan.generation, slot, State().visible_indices[slot], std::nullopt, x, y, side, side);
        }
        FlushProbes(stream);
    }
    if (acceptance_ && diagnostics_.valid()) {
        std::uint32_t restored_tiles = 0U;
        for (std::size_t slot = 0U; slot != State().visible_indices.size(); ++slot) {
            const auto restored = static_cast<std::uint32_t>(State().tile_meanings[slot] != nullptr);
            restored_tiles += restored;
            diagnostics_.Emit([&] {
                return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                            .operation = VisualDiagnosticOperation::AcceptancePlaceholderSlot,
                                            .generation = plan.generation,
                                            .value = slot,
                                            .detail = State().visible_indices[slot],
                                            .context = {.capacity_width = restored}};
            });
        }
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::AcceptancePlaceholderComplete,
                                        .generation = plan.generation,
                                        .value = State().visible_indices.size(),
                                        .detail = explore_visible_indices_digest(State().visible_indices),
                                        .context = {.capacity_width = restored_tiles,
                                                    .admission = {.admission_first_row = plan.viewport.first_row,
                                                                  .admission_row_count = plan.viewport.row_count,
                                                                  .admission_columns = plan.viewport.columns,
                                                                  .admission_forward = plan.scroll_direction == ExploreScrollDirection::Forward}}};
        });
    }
    auto publication = PublicationFacts(scheduler_.stale_discarded_.exchange(0U, std::memory_order_acq_rel));
    publication.reused_tiles = std::exchange(State().reused_tiles, 0U);
    return publication;
}
ExploreGalleryPublication GalleryStream::Impl::Advance() {
    std::exception_ptr failure;
    std::uint64_t failure_generation = 0U;
    const auto generation = scheduler_.desired_generation_.load(std::memory_order_acquire);
    const auto diagnose_stage = [this, generation, &failure](const std::uint64_t stage) {
        if (acceptance_ && diagnostics_.valid())
            diagnostics_.Emit([&] {
                return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                            .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                                            .device = device_,
                                            .generation = generation,
                                            .value = failure ? 1U : 0U,
                                            .detail = stage};
            });
    };
    diagnose_stage(10U);
    CompleteTiles(false);
    const auto settle = [&](auto&& observe_stale) {
        std::scoped_lock lock(scheduler_.lanes_mutex_);
        for (auto& lane : scheduler_.lanes_) {
            if (lane->state == LaneState::Queued || lane->state == LaneState::Reading || lane->state == LaneState::AwaitingTransfer ||
                lane->state == LaneState::GpuPending)
                static_cast<void>(scheduler_.ReserveInput(State(), *lane));
            const bool was_stale = lane->state == LaneState::StaleReady;
            if (lane->state == LaneState::InputReady || lane->state == LaneState::StaleReady ||
                (lane->state == LaneState::GpuComplete && lane->generation != generation))
                scheduler_.RebindInput(State(), ProtectedCache(), *lane);
            if (was_stale && lane->state == LaneState::Idle) {
                observe_stale(*lane);
                scheduler_.stale_discarded_.fetch_add(1U, std::memory_order_relaxed);
            }
            if (lane->state == LaneState::Failed && !scheduler_.Current(lane->DemandGeneration())) scheduler_.DiscardSettledInput(State(), *lane);
            if (lane->state == LaneState::Failed) {
                failure = lane->failure;
                failure_generation = lane->DemandGeneration();
                scheduler_.DiscardSettledInput(State(), *lane);
                break;
            }
            if (lane->state == LaneState::StaleReady) {
                observe_stale(*lane);
                lane->state = LaneState::Idle;
                scheduler_.stale_discarded_.fetch_add(1U, std::memory_order_relaxed);
            } else if (lane->state == LaneState::GpuComplete && (!lane->pending_meaning || lane->generation != generation || !scheduler_.Current(generation))) {
                if (lane->generation != generation) scheduler_.stale_discarded_.fetch_add(1U, std::memory_order_relaxed);
                scheduler_.DiscardSettledInput(State(), *lane);
            }
        }
    };
    if (acceptance_ && diagnostics_.valid()) {
        std::array<VisualDiagnosticFact, kExploreMaximumParallelism> discarded{};
        std::size_t discarded_count = 0U;
        settle([&](const Lane& lane) {
            discarded[discarded_count++] = {
                .system = contracts::DiagnosticOwner::Explore,
                .operation = VisualDiagnosticOperation::AcceptanceStaleReadDiscarded,
                .generation = lane.generation,
                .value = lane.destination_slot,
                .detail = lane.compiled_index,
                .context = {.staging_bytes = lane.pinned.capacity_bytes()},
            };
        });
        for (const auto& fact : std::span{discarded}.first(discarded_count)) diagnostics_(fact);
    } else {
        settle([](const Lane&) {});
    }
    diagnose_stage(11U);
    if (failure && scheduler_.Current(failure_generation)) std::rethrow_exception(failure);
    diagnose_stage(12U);
    scheduler_.StartIdleLanes(State(), ProtectedCache());
    bool background_ready = false;
    bool foreground_ready = false;
    {
        std::scoped_lock lock(scheduler_.lanes_mutex_);
        for (const auto& lane : scheduler_.lanes_) {
            if (lane->state != LaneState::InputReady || lane->generation != generation) continue;
            (lane->prefetch ? background_ready : foreground_ready) = true;
        }
    }
    if (scheduler_.Current(generation) && background_ready && !foreground_ready && State().cumulative_tiles == State().visible_indices.size()) {
        SettleDescriptors();
        static_cast<void>(RenderReadyTiles(CachePlane(false), CachePlane(true), reinterpret_cast<std::uintptr_t>(stream_), true));
    }
    diagnose_stage(13U);
    auto publication = PublicationFacts(scheduler_.stale_discarded_.exchange(0U, std::memory_order_acq_rel));
    publication.reused_tiles = std::exchange(State().reused_tiles, 0U);
    diagnose_stage(14U);
    return publication;
}
void GalleryStream::Impl::PrepareOutputPublication(const ExploreOutputChange change, const ExploreMode mode) {
    if (rollback_failed_) throw std::runtime_error("Explore publication rollback previously failed");
    if (publication_active_) return;
    try {
        if (mode != selected_mode_) {
            SettleDescriptors();
            CompleteTiles(true);
            Suspend();
            std::swap(committed_, retained_);
            selected_mode_ = mode;
            mode_switched_ = true;
            scheduler_.scheduled_slots_.resize(committed_.cache.size());
            undo_marks_.resize(committed_.cache.size());
            std::ranges::fill(scheduler_.scheduled_slots_, 0U);
        }
        publication_change_ = change;
        if (change != ExploreOutputChange::Unchanged) {
            SettleDescriptors();
            CompleteTiles(true);
        }
        if (change == ExploreOutputChange::Initialize) {
            // Geometry changes need inactive logical metadata, but the cache itself
            // remains one retained owner with a changed-entry rollback journal.
            auto cache = std::move(committed_.cache);
            try {
                candidate_ = committed_;
            } catch (...) {
                committed_.cache = std::move(cache);
                throw;
            }
            committed_.cache = std::move(cache);
            incumbent_stream_ = stream_;
        }
        prior_cumulative_ = committed_.cumulative_tiles;
        prior_reused_ = committed_.reused_tiles;
        prior_atlas_ = committed_.atlas;
        if (change == ExploreOutputChange::Semantic && ++undo_sequence_ == 0U) {
            std::ranges::fill(undo_marks_, 0U);
            ++undo_sequence_;
        }
        publication_active_ = true;
    } catch (...) {
        if (!retirement_.admission_open()) throw;
        if (mode_switched_) {
            std::swap(committed_, retained_);
            selected_mode_ = committed_.plan.mode;
            mode_switched_ = false;
            scheduler_.scheduled_slots_.resize(committed_.cache.size());
            undo_marks_.resize(committed_.cache.size());
            scheduler_.desired_generation_.store(committed_.plan.generation, std::memory_order_release);
            if (committed_.plan.mode == ExploreMode::Gallery) scheduler_.Prioritize(State());
        }
        throw;
    }
}
void GalleryStream::Impl::CommitOutputPublication() noexcept {
    if (!publication_active_) return;
    atlas_.Commit();
    if (publication_change_ == ExploreOutputChange::Initialize) {
        State().cache.CommitUpdate();
        borrowed_cache_ = false;
        std::swap(committed_, candidate_);
        publication_active_ = false;
        // Only successful publication can retire or retarget settled incumbent
        // inputs. In-flight reads retain their slots until their callback; the
        // continuation scan applies the same rebind after completion.
        std::scoped_lock lock(scheduler_.lanes_mutex_);
        std::ranges::fill(scheduler_.scheduled_slots_, 0U);
        for (auto& lane : scheduler_.lanes_) lane->reserved_generation = 0U;
        if (committed_.plan.mode == ExploreMode::Gallery)
            for (auto& lane : scheduler_.lanes_) scheduler_.ReconcileInitializationLane(State(), ProtectedCache(), *lane);
    } else if (publication_change_ == ExploreOutputChange::Unchanged) {
        const bool direction_changed = committed_.plan.scroll_direction != pending_plan_.scroll_direction;
        committed_.plan.generation = pending_plan_.generation;
        committed_.plan.viewport = pending_plan_.viewport;
        committed_.plan.scroll_direction = pending_plan_.scroll_direction;
        committed_.plan.detail = pending_plan_.detail;
        committed_.plan.selected_image = pending_plan_.selected_image;
        committed_.plan.overlay.show_labels = pending_plan_.show_labels;
        scheduler_.desired_generation_.store(pending_plan_.generation, std::memory_order_release);
        {
            std::scoped_lock lock(scheduler_.lanes_mutex_);
            if (committed_.plan.mode == ExploreMode::Gallery)
                for (auto& lane : scheduler_.lanes_) {
                    if (lane->state == LaneState::InputReady)
                        scheduler_.RebindInput(State(), ProtectedCache(), *lane);
                    else if (lane->state == LaneState::Queued || lane->state == LaneState::Reading || lane->state == LaneState::AwaitingTransfer ||
                             lane->state == LaneState::GpuPending)
                        static_cast<void>(scheduler_.ReserveInput(State(), *lane));
                }
        }
        if ((direction_changed || mode_switched_) && committed_.plan.mode == ExploreMode::Gallery)
            scheduler_.Prioritize(State());
        else
            scheduler_.next_priority_ = 0U;
    }
    publication_active_ = false;
    mode_switched_ = false;
    ClearUndo();
    ClearInactiveProduct();
}
bool GalleryStream::Impl::RollbackOutputPublication() noexcept {
    if (!retirement_.admission_open()) return false;
    if (rollback_failed_) return false;
    if (!publication_active_) return true;
    atlas_.Rollback();
    try {
        if (publication_change_ != ExploreOutputChange::Unchanged) Suspend();
        if (publication_change_ == ExploreOutputChange::Semantic) {
            for (auto& undo : std::span{slot_undo_}.first(undo_count_)) {
                committed_.cache.Restore(undo.slot, std::move(undo.entry));
                if (undo.visible < committed_.tile_meanings.size()) {
                    committed_.tile_meanings[undo.visible] = std::move(undo.meaning);
                    committed_.completed_slots[undo.visible] = undo.completed;
                }
            }
            committed_.cumulative_tiles = prior_cumulative_;
            committed_.reused_tiles = prior_reused_;
            committed_.atlas = prior_atlas_;
            if (overlay_undo_) {
                std::swap(committed_.plan, candidate_.plan);
                std::swap(committed_.active_classes, candidate_.active_classes);
            }
        }
        if (borrowed_cache_) {
            candidate_.cache.RollbackUpdate();
            std::swap(candidate_.cache, committed_.cache);
            borrowed_cache_ = false;
        }
        publication_active_ = false;
        const bool restored_mode = mode_switched_;
        if (restored_mode) {
            std::swap(committed_, retained_);
            selected_mode_ = committed_.plan.mode;
            mode_switched_ = false;
        }
        // Initialization may have shrunk these logical dimensions. Their
        // high-water capacity already includes the incumbent ring; restore
        // them before any callback can request automatic continuation.
        if (publication_change_ == ExploreOutputChange::Initialize || restored_mode) {
            scheduler_.scheduled_slots_.resize(committed_.cache.size());
            undo_marks_.resize(committed_.cache.size());
            stream_ = incumbent_stream_;
        }
        scheduler_.next_priority_ = 0U;
        if (publication_change_ != ExploreOutputChange::Unchanged) std::ranges::fill(scheduler_.scheduled_slots_, 0U);
        scheduler_.desired_generation_.store(State().plan.generation, std::memory_order_release);
        if ((publication_change_ != ExploreOutputChange::Unchanged || restored_mode) && State().plan.mode == ExploreMode::Gallery) {
            std::scoped_lock lock(scheduler_.lanes_mutex_);
            for (auto& lane : scheduler_.lanes_) {
                lane->reserved_generation = 0U;
                scheduler_.ReconcileInitializationLane(State(), ProtectedCache(), *lane);
            }
        }
        if ((publication_change_ == ExploreOutputChange::Initialize || overlay_undo_ || restored_mode) && State().plan.mode == ExploreMode::Gallery)
            scheduler_.Prioritize(State());
        std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
        {
            std::scoped_lock lock(scheduler_.lanes_mutex_);
            sink = scheduler_.ready_sink_;
        }
        if (sink) (*sink)();
        ClearUndo();
        ClearInactiveProduct();
        return true;
    } catch (...) {
        // Keep both artifact owners and all lane state alive for checked runtime
        // retirement. Failed settlement is not permission to clear borrowed data.
        rollback_failed_ = true;
        return false;
    }
}
void GalleryStream::Impl::SaveOverlay() {
    if (publication_change_ != ExploreOutputChange::Semantic || overlay_undo_) return;
    candidate_.plan = committed_.plan;
    candidate_.active_classes = committed_.active_classes;
    candidate_.store = committed_.store;
    overlay_undo_ = true;
}
void GalleryStream::Impl::SaveSlot(const std::size_t position) {
    if (!publication_active_ || publication_change_ != ExploreOutputChange::Semantic) return;
    const auto slot = committed_.cache.Slot(position);
    if (undo_marks_[slot] == undo_sequence_) return;
    if (undo_count_ == slot_undo_.size()) throw std::logic_error("Explore publication exceeds its visible slot journal");
    auto& undo = slot_undo_[undo_count_++];
    undo.slot = slot;
    undo.entry = committed_.cache.Physical(slot);
    if (acceptance_) acceptance_->ObserveProduct({.artifact = {}, .copied_entries = 1U});
    const auto first = static_cast<std::size_t>(committed_.viewport.first_row) * committed_.viewport.columns;
    undo.visible = position >= first && position - first < committed_.tile_meanings.size() ? position - first : kExploreVisibleItemCapacity;
    if (undo.visible < committed_.tile_meanings.size()) {
        undo.meaning = committed_.tile_meanings[undo.visible];
        undo.completed = committed_.completed_slots[undo.visible];
    }
    undo_marks_[slot] = undo_sequence_;
}
void GalleryStream::Impl::ClearUndo() noexcept {
    for (auto& undo : std::span{slot_undo_}.first(undo_count_)) undo = {};
    undo_count_ = 0U;
    overlay_undo_ = false;
    pending_plan_ = {};
}
void GalleryStream::Impl::CompleteTiles(const bool synchronized) {
    std::scoped_lock lock(scheduler_.lanes_mutex_);
    for (auto& lane : scheduler_.lanes_) {
        if (!lane->pending_meaning || lane->identity != State().cache.identity()) continue;
        if (lane->state != LaneState::GpuComplete && !(synchronized && lane->state == LaneState::GpuPending)) continue;
        const auto position = State().cache.Position(lane->compiled_index);
        if (position == std::numeric_limits<std::size_t>::max() || State().cache.Slot(position) != lane->cache_slot) continue;
        // Only a completely submitted batch enters GpuPending. Its successful
        // settlement is independent of whether the old demand can still
        // publish an atlas; output readiness is installed only by PlaceTile.
        SaveSlot(position);
        State().cache.Complete(position, lane->compiled_index, std::move(lane->pending_meaning), lane->semantic_identity, lane->cache_bank,
                               lane->semantic_bank);
    }
}
ExploreGalleryPublication GalleryStream::Impl::PublicationFacts(const std::size_t stale) const {
    std::size_t pinned = 0U;
    {
        std::scoped_lock lock(scheduler_.lanes_mutex_);
        for (const auto& lane : scheduler_.lanes_)
            if (lane->state != LaneState::Idle) pinned += lane->pinned.capacity_bytes() + scheduler_.image_stream_.host_storage(lane->index).capacity_bytes();
    }
    return {
        .generation = scheduler_.desired_generation_.load(std::memory_order_acquire),
        .layout = State().atlas,
        .ready_slots = State().completed_slots,
        .cumulative_tiles = State().cumulative_tiles,
        .remaining_tiles = State().visible_indices.size() - std::min(State().visible_indices.size(), State().cumulative_tiles),
        .active_pinned_bytes = pinned,
        .stale_discarded = stale,
    };
}
ExploreStorageFootprint GalleryStream::Impl::StorageFootprint() const {
    std::size_t device_bytes = 0U;
    std::size_t staging_bytes =
        descriptors_.storage_.buffers_.descriptors_.capacity_bytes() + (probe_ ? probe_->storage_.buffers_.semantic_count_pinned_.capacity_bytes() : 0U);
    const auto account = [&](const auto& buffer) { device_bytes += buffer.capacity_bytes(); };
    storage_.storage_.Visit(account);
    descriptors_.storage_.Visit(account);
    if (probe_) probe_->storage_.Visit(account);
    device_bytes -= staging_bytes;
    for (const auto& lane : scheduler_.lanes_) {
        device_bytes += scheduler_.image_stream_.device_storage(lane->index).capacity_bytes();
        staging_bytes += lane->pinned.capacity_bytes() + scheduler_.image_stream_.host_storage(lane->index).capacity_bytes();
    }
    device_bytes += scheduler_.image_stream_.device_storage(scheduler_.detail_lane_->index).capacity_bytes();
    staging_bytes += scheduler_.detail_lane_->pinned.capacity_bytes() + scheduler_.image_stream_.host_storage(scheduler_.detail_lane_->index).capacity_bytes();
    // Both logical products and the active journal retain meaning. Count their
    // shared pointees once, including protected incumbent versions.
    std::array<std::shared_ptr<const GalleryTileMeaning>, 10U * kExploreVisibleItemCapacity + kExploreMaximumParallelism + 4U> meanings;
    std::size_t meaning_count = 0U;
    for (const auto* product : {&committed_, &candidate_, &retained_}) {
        meanings[meaning_count++] = product->detail_meaning;
        for (const auto& meaning : product->tile_meanings) meanings[meaning_count++] = meaning;
    }
    for (const auto& lane : scheduler_.lanes_) meanings[meaning_count++] = lane->pending_meaning;
    meanings[meaning_count++] = scheduler_.detail_lane_->pending_meaning;
    for (std::size_t index = 0U; index < undo_count_; ++index) {
        meanings[meaning_count++] = slot_undo_[index].entry.meaning;
        meanings[meaning_count++] = slot_undo_[index].meaning;
    }
    meaning_count += atlas_.AppendMeanings(std::span{meanings}.subspan(meaning_count));
    const auto& gallery_cache = selected_mode_ == ExploreMode::Gallery ? committed_.cache : retained_.cache;
    auto host_bytes = gallery_cache.MeaningBytes(&candidate_.cache, std::span{meanings}.first(meaning_count));
    const auto vector_bytes = [](const auto& values) { return values.capacity() * sizeof(typename std::remove_cvref_t<decltype(values)>::value_type); };
    host_bytes += sizeof(slot_undo_) + (probe_ ? sizeof(probe_->probes_) : 0U) + vector_bytes(undo_marks_) + vector_bytes(descriptors_.batch_input_slots_) +
                  vector_bytes(descriptors_.batch_donor_slots_) + vector_bytes(scheduler_.scheduled_slots_) + vector_bytes(descriptors_.batch_indices_) +
                  vector_bytes(descriptors_.semantic_slots_) + vector_bytes(descriptors_.batch_keys_) + vector_bytes(descriptors_.batch_donors_) +
                  vector_bytes(projected_annotations_) + vector_bytes(scheduler_.priority_rank_) + vector_bytes(scheduler_.lanes_) +
                  (scheduler_.lanes_.size() + 1U) * sizeof(Lane);
    host_bytes += committed_.MetadataBytes() + candidate_.MetadataBytes() + retained_.MetadataBytes() + atlas_.MetadataBytes();
    const auto augmentation_device = descriptors_.augmenter_ ? descriptors_.augmenter_->device_capacity_bytes() : 0U;
    const auto augmentation_pinned = descriptors_.augmenter_ ? descriptors_.augmenter_->pinned_capacity_bytes() : 0U;
    device_bytes += augmentation_device;
    staging_bytes += augmentation_pinned;
    std::size_t cache_bytes = 0U;
    for (const auto* family : {&storage_.storage_.buffers_.cached_clean_, &storage_.storage_.buffers_.cached_semantic_})
        for (const auto& allocation : *family) cache_bytes += allocation.capacity_bytes();
    const auto descriptor_bytes =
        descriptors_.storage_.buffers_.descriptors_.capacity_bytes() + descriptors_.storage_.buffers_.cards_device_.capacity_bytes() +
        descriptors_.storage_.buffers_.annotations_device_.capacity_bytes() + descriptors_.storage_.buffers_.rle_device_.capacity_bytes() +
        descriptors_.storage_.buffers_.classes_device_.capacity_bytes() + descriptors_.storage_.buffers_.tiles_device_.capacity_bytes();
    return {.host_bytes = host_bytes,
            .device_bytes = device_bytes,
            .pinned_bytes = staging_bytes,
            .cache_device_bytes = cache_bytes,
            .descriptor_bytes = descriptor_bytes,
            .augmentation_device_bytes = augmentation_device,
            .augmentation_pinned_bytes = augmentation_pinned,
            .cache_cards = std::max(State().cache.size(), retained_.cache.size())};
}
void GalleryStream::Impl::PrepareCacheWrite(const std::uintptr_t stream) {
    const auto side = State().cache.identity().extent;
    const auto height = 2U * State().cache.size() * static_cast<std::size_t>(side);
    const auto pitch = static_cast<std::size_t>(side) * 4U;
    if (height > std::numeric_limits<std::uint32_t>::max() || (pitch != 0U && height > std::numeric_limits<std::size_t>::max() / pitch))
        throw std::overflow_error("Explore retained cache planes exceed addressable raster storage");
    const auto bytes = height * pitch;
    const bool replacement = publication_active_ && !borrowed_cache_ &&
                             (State().cache.size() != committed_.cache.size() || !State().cache.identity().SameSource(committed_.cache.identity()));
    if (replacement) State().cache_active = 1U - committed_.cache_active;
    for (auto* family : {&storage_.storage_.buffers_.cached_clean_, &storage_.storage_.buffers_.cached_semantic_})
        ensure_gallery_buffer((*family)[State().cache_active], std::max<std::size_t>(bytes, 1U), "Explore candidate tile cache allocation failed", diagnostics_,
                              device_, State().plan.generation);
    if (!replacement || !State().cache.identity().SameSource(committed_.cache.identity())) return;
    // Capacity growth relocates retained individual tiles into an unpublished
    // allocation. Bank strides use physical capacity, never logical demand.
    for (std::size_t slot = 0U; slot < committed_.cache.size(); ++slot) {
        const auto& entry = State().cache.Physical(slot);
        const auto& old = committed_.cache.Physical(slot);
        if (!entry.meaning || entry.compiled_index != old.compiled_index) continue;
        for (const bool semantic : {false, true}) {
            const auto bank = semantic ? entry.semantic_bank : entry.bank;
            auto& family = semantic ? storage_.storage_.buffers_.cached_semantic_ : storage_.storage_.buffers_.cached_clean_;
            const auto source = committed_.cache.PhysicalRow(slot, bank) * pitch;
            const auto destination = State().cache.PhysicalRow(slot, bank) * pitch;
            ensure_gallery_cuda(cudaMemcpyAsync(static_cast<std::byte*>(family[State().cache_active].data()) + destination,
                                                static_cast<const std::byte*>(family[committed_.cache_active].data()) + source,
                                                static_cast<std::size_t>(side) * side * 4U, cudaMemcpyDeviceToDevice, reinterpret_cast<cudaStream_t>(stream)),
                                "Explore retained cache growth copy failed");
        }
    }
}
std::uint8_t GalleryStream::Impl::WritableCacheBank(std::size_t position, bool semantic) const {
    return State().cache.WritableBank(position, ProtectedCache(), semantic);
}
mmltk::frameworks::gpu::ImagePlaneView GalleryStream::Impl::CachePlane(const bool semantic) const {
    const auto side = State().cache.identity().extent;
    const auto& buffer =
        semantic ? storage_.storage_.buffers_.cached_semantic_[State().cache_active] : storage_.storage_.buffers_.cached_clean_[State().cache_active];
    return {.data = reinterpret_cast<CUdeviceptr>(buffer.data()),
            .descriptor = {.kind = semantic ? mmltk::frameworks::gpu::ImagePlaneKind::Semantic : mmltk::frameworks::gpu::ImagePlaneKind::Clean,
                           .width = side,
                           .height = static_cast<std::uint32_t>(2U * State().cache.size() * side),
                           .pitch_bytes = static_cast<std::size_t>(side) * 4U}};
}
void GalleryStream::Impl::PlaceTile(const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                    const std::uint32_t slot, const std::uintptr_t stream, const bool semantic_only) {
    const auto side = State().cache.identity().extent;
    const auto position = static_cast<std::size_t>(State().viewport.first_row) * State().viewport.columns + slot;
    const auto* entry = State().cache.Retained(State().visible_indices[slot]);
    if (!entry) throw std::logic_error("Explore atlas placement requires a completed cache tile");
    const auto x = slot % State().viewport.columns * side;
    const auto physical = atlas_.Physical(slot);
    const auto y = static_cast<std::uint32_t>(physical / State().viewport.columns) * side;
    const bool unchanged = atlas_.Contains(physical, entry->meaning, entry->semantic_identity);
    const bool clean_unchanged = semantic_only || atlas_.ContainsClean(physical, entry->meaning);
    const auto copy = [&](const auto plane, const bool semantic_plane) {
        const auto cache = CachePlane(semantic_plane);
        const auto bank = semantic_plane ? entry->semantic_bank : entry->bank;
        ensure_gallery_cuda(cudaMemcpy2DAsync(reinterpret_cast<void*>(plane.data + y * plane.descriptor.pitch_bytes + x * 4U), plane.descriptor.pitch_bytes,
                                              reinterpret_cast<const void*>(cache.data + State().cache.PhysicalRow(State().cache.Slot(position), bank) *
                                                                                             cache.descriptor.pitch_bytes),
                                              cache.descriptor.pitch_bytes, side * 4U, side, cudaMemcpyDeviceToDevice, reinterpret_cast<cudaStream_t>(stream)),
                            "Explore cache tile placement failed");
    };
    if (!unchanged) {
        atlas_.Touch(physical);
        if (!clean_unchanged) copy(clean, false);
        copy(semantic, true);
        atlas_.Stage(physical, entry->meaning, entry->semantic_identity);
    }
    const bool completed = !entry->refresh_pending && !State().completed_slots[slot];
    if (State().tile_meanings[slot] != entry->meaning || completed) SaveSlot(position);
    State().tile_meanings[slot] = entry->meaning;
    if (completed) {
        State().completed_slots[slot] = true;
        ++State().cumulative_tiles;
    }
    if (!unchanged && acceptance_ && diagnostics_.valid() && publication_change_ != ExploreOutputChange::Initialize)
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::AcceptanceSlotPatched,
                                        .generation = State().plan.generation,
                                        .value = slot,
                                        .detail = State().visible_indices[slot]};
        });
    if (!unchanged)
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::ExploreCacheTransfer,
                                        .device = device_,
                                        .generation = State().plan.generation,
                                        .value = (clean_unchanged ? 1U : 2U) * static_cast<std::size_t>(side) * side * 4U,
                                        .detail = slot,
                                        .context = {.condition = clean_unchanged}};
        });
}
void GalleryStream::Impl::RenderCachedSemantics(const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView target,
                                                const std::uintptr_t stream) {
    std::size_t cursor = 0U;
    const auto side = explore_atlas_card_extent(State().viewport);
    while (cursor < State().tile_meanings.size()) {
        if (!scheduler_.Current(State().plan.generation)) return;
        auto& slots = descriptors_.semantic_slots_;
        slots.clear();
        std::size_t annotation_count = 0U;
        std::size_t run_count = 0U;
        while (cursor < State().tile_meanings.size()) {
            const auto& meaning = State().tile_meanings[cursor];
            if (!meaning) {
                ++cursor;
                continue;
            }
            if (meaning->annotations.size() > explore::kExploreRenderAnnotationCapacity - annotation_count ||
                meaning->runs.size() > explore::kExploreRenderRleCapacity - run_count)
                break;
            annotation_count += meaning->annotations.size();
            run_count += meaning->runs.size();
            slots.push_back(static_cast<std::uint32_t>(cursor++));
        }
        if (slots.empty()) {
            if (cursor == State().tile_meanings.size()) break;
            throw contracts::InvalidIntentError("Explore cached semantic batch exceeds capacity");
        }
        SettleDescriptors();
        descriptors_.PrepareDescriptors(slots.size(), annotation_count, run_count, State().active_classes.size(), slots.size(), diagnostics_, device_,
                                        State().plan.generation);
        std::size_t annotation_offset = 0U;
        std::size_t run_offset = 0U;
        for (std::size_t index = 0U; index < slots.size(); ++index) {
            const auto slot = slots[index];
            const auto& meaning = *State().tile_meanings[slot];
            auto card = meaning.card;
            card.annotation_offset = static_cast<std::uint32_t>(annotation_offset);
            store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.cards.offset + index * sizeof(card), card);
            for (auto annotation : meaning.annotations) {
                annotation.rle_offset += static_cast<std::uint32_t>(run_offset);
                annotation.card_index = static_cast<std::uint32_t>(index);
                if (annotation.occluder_index >= 0) annotation.occluder_index += static_cast<std::int32_t>(card.annotation_offset);
                store_payload(descriptors_.storage_.buffers_.descriptors_.data(),
                              descriptors_.descriptor_layout_.annotations.offset + annotation_offset++ * sizeof(annotation), annotation);
            }
            store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.rle.offset + run_offset * sizeof(data::RLEPair),
                          std::span{meaning.runs});
            run_offset += meaning.runs.size();
            const auto position = static_cast<std::size_t>(State().viewport.first_row) * State().viewport.columns + slot;
            const explore::ExploreRenderTileDescriptor tile{
                .card_index = static_cast<std::uint32_t>(index),
                .destination_x = 0U,
                .destination_y = static_cast<std::uint32_t>(State().cache.PhysicalRow(State().cache.Slot(position), WritableCacheBank(position, true))),
                .destination_width = side,
                .destination_height = side,
                .generation = {.viewport = State().plan.generation, .tile = ++scheduler_.next_tile_generation_}};
            store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.tiles.offset + index * sizeof(tile), tile);
        }
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.classes.offset, std::span{State().active_classes});
        if (!scheduler_.Current(State().plan.generation)) return;
        UploadDescriptors(reinterpret_cast<cudaStream_t>(stream), true);
        if (!scheduler_.Current(State().plan.generation)) {
            descriptors_.descriptors_pending_ = true;
            return;
        }
        RenderAtlasPlane({.card_extent = side,
                          .card_count = static_cast<std::uint32_t>(slots.size()),
                          .source_width = State().store->header().image_width,
                          .source_height = State().store->header().image_height},
                         {.tile_count = static_cast<std::uint32_t>(slots.size()),
                          .tile_capacity = static_cast<std::uint32_t>(slots.size()),
                          .max_tile_width = side,
                          .max_tile_height = side,
                          .viewport_generation = State().plan.generation},
                         State().plan.overlay, CachePlane(true), stream, true, Demand());
        descriptors_.descriptors_pending_ = true;
        SettleDescriptors();
        for (const auto slot : slots) {
            if (!scheduler_.Current(State().plan.generation)) return;
            const auto position = static_cast<std::size_t>(State().viewport.first_row) * State().viewport.columns + slot;
            const auto* incumbent = State().cache.Retained(State().visible_indices[slot]);
            if (!incumbent) throw std::logic_error("Explore semantic refresh lost its clean tile");
            const auto semantic_bank = WritableCacheBank(position, true);
            SaveSlot(position);
            State().cache.UpdateSemantics(position, State().plan.semantic_identity, semantic_bank);
            PlaceTile(clean, target, slot, stream, true);
        }
        if (diagnostics_.valid())
            for (std::size_t index = 0U; index < slots.size(); ++index) {
                const auto slot = slots[index];
                const auto& meaning = *State().tile_meanings[slot];
                const auto descriptor = load_payload<explore::ExploreRenderCardDescriptor>(
                    descriptors_.storage_.buffers_.descriptors_.data(),
                    descriptors_.descriptor_layout_.cards.offset + index * sizeof(explore::ExploreRenderCardDescriptor));
                GalleryStreamProbe::DiagnoseDescriptors(descriptors_, diagnostics_, device_, State().plan.generation, slot, descriptor.annotation_offset,
                                                        meaning.annotations.size(), meaning.runs.size());
                DiagnoseRendered(clean, target, stream, State().plan.generation, slot, State().visible_indices[slot], index,
                                 slot % State().viewport.columns * side, AtlasY(slot), side, side);
            }
        FlushProbes(stream);
    }
}
std::vector<ExploreLabel> GalleryStream::Impl::Labels() const {
    if (acceptance_) acceptance_->CheckPublication(ExploreAcceptanceGate::PublicationStage::ProductPrepared);
    std::vector<ExploreLabel> labels;
    if (State().plan.mode != ExploreMode::Gallery) return labels;
    // Tile semantics survive render batches. Publication covers the entire
    // atlas, with one editable image's object budget for each visible slot.
    std::size_t annotation_count = 0U;
    for (const auto& tile : State().tile_meanings) {
        if (!tile) continue;
        if (tile->annotations.size() > kExploreLabelCapacity - annotation_count)
            throw contracts::InvalidIntentError("Explore label publication exceeds capacity");
        annotation_count += tile->annotations.size();
    }
    labels.reserve(annotation_count);
    const auto side = explore_atlas_card_extent(State().viewport);
    for (std::size_t slot = 0U; slot < State().tile_meanings.size(); ++slot) {
        if (!State().tile_meanings[slot]) continue;
        const auto& tile = *State().tile_meanings[slot];
        const float x = static_cast<float>(slot % State().viewport.columns * side + tile.card.image_x);
        const float y = static_cast<float>(slot / State().viewport.columns * side + tile.card.image_y);
        const float width = static_cast<float>(tile.card.image_width);
        const float height = static_cast<float>(tile.card.image_height);
        for (const auto& annotation : tile.annotations) {
            if (annotation.class_id >= State().active_classes.size() || !State().active_classes[annotation.class_id].visible) continue;
            labels.push_back({.box = {{x + annotation.box_xyxy[0] * width, y + annotation.box_xyxy[1] * height},
                                      {x + annotation.box_xyxy[2] * width, y + annotation.box_xyxy[3] * height}},
                              .category = annotation.class_id,
                              .compiled_index = State().visible_indices[slot]});
        }
    }
    return labels;
}
const rfdetr::AugmentationBatchPlan* GalleryStream::Impl::PrepareImages(const std::span<Lane* const> ready_lanes, const DescriptorBudget source_budget,
                                                                        const cudaStream_t cuda_stream) {
    if (ready_lanes.empty() || ready_lanes.size() > scheduler_.lanes_.size()) throw std::logic_error("Explore prepared image batch is invalid");
    const auto pixel_bytes = ready_lanes.front()->layout.pixel_bytes;
    descriptors_.batch_input_slots_.clear();
    descriptors_.batch_donor_slots_.clear();
    descriptors_.batch_input_slots_.reserve(scheduler_.lanes_.size());
    descriptors_.batch_donor_slots_.reserve(scheduler_.lanes_.size());
    const bool preview_active = State().plan.augmentation.enabled && State().plan.augmentation_config.enabled;
    if (preview_active)
        ensure_gallery_buffer(descriptors_.storage_.buffers_.augmented_batch_, scheduler_.lanes_.size() * pixel_bytes,
                              "Explore augmented batch high-water allocation failed", diagnostics_, device_, State().plan.generation);
    descriptors_.batch_indices_.clear();
    descriptors_.semantic_slots_.clear();
    descriptors_.batch_keys_.clear();
    if (preview_active) {
        descriptors_.batch_indices_.reserve(scheduler_.lanes_.size());
        descriptors_.batch_keys_.reserve(scheduler_.lanes_.size());
    }
    std::size_t projection_capacity = 0U;
    for (std::size_t slot = 0U; slot != ready_lanes.size(); ++slot) {
        if (!scheduler_.current_demand_(State().plan.generation)) return nullptr;
        const Lane& lane = *ready_lanes[slot];
        projection_capacity = std::max(projection_capacity, lane.layout.annotations.count + 1U);
        scheduler_.image_stream_.handoff(lane.index, cuda_stream);
        const auto* pixels = static_cast<const float*>(scheduler_.image_stream_.device_storage(lane.index).data());
        descriptors_.batch_input_slots_.push_back(pixels);
        descriptors_.batch_donor_slots_.push_back(pixels + pixel_bytes / sizeof(float));
        if (preview_active) {
            descriptors_.batch_indices_.push_back(lane.compiled_index);
            descriptors_.batch_keys_.push_back(lane.preview_key);
        }
    }
    projected_annotations_.reserve(projection_capacity);
    auto augmentation_config = State().plan.augmentation_config;
    augmentation_config.enabled = preview_active;
    if (State().annotated_indices.empty()) augmentation_config.copy_paste_probability = 0.0F;
    if (preview_active && (!descriptors_.augmenter_ || descriptors_.augmentation_width_ != State().store->header().image_width ||
                           descriptors_.augmentation_height_ != State().store->header().image_height)) {
        descriptors_.augmenter_ = std::make_unique<rfdetr::GpuAugmentationExecutor>(
            augmentation_config, scheduler_.lanes_.size(), static_cast<int>(State().store->header().image_height),
            static_cast<int>(State().store->header().image_width), retained_context_.value(), retirement_);
        descriptors_.augmentation_width_ = State().store->header().image_width;
        descriptors_.augmentation_height_ = State().store->header().image_height;
    } else if (preview_active) {
        descriptors_.augmenter_->Reconfigure(augmentation_config);
    }
    descriptors_.batch_donors_.clear();
    auto donor_budget = source_budget;
    rfdetr::GpuAugmentationDonorBatchView donor_view;
    if (preview_active && descriptors_.augmenter_->copy_paste_enabled()) {
        descriptors_.batch_donors_.reserve(scheduler_.lanes_.size());
        descriptors_.batch_donors_.resize(ready_lanes.size());
        const auto donor_box_bytes = ready_lanes.size() * 4U * sizeof(float);
        const auto pixels_per_image = static_cast<std::size_t>(State().store->header().image_width) * State().store->header().image_height;
        const auto mask_words = (pixels_per_image + 63U) / 64U;
        const auto donor_mask_bytes = ready_lanes.size() * mask_words * sizeof(std::uint64_t);
        bool donor_storage_ready = false;
        for (std::size_t slot = 0U; slot != ready_lanes.size(); ++slot) {
            if (!scheduler_.current_demand_(State().plan.generation)) return nullptr;
            const Lane& lane = *ready_lanes[slot];
            // Aligned donors are optional. Reserve their complete semantic
            // footprint before either image paste or mask transformation.
            if (!lane.donor_instance || !donor_budget.AdmitDonor(lane.layout.annotations.count, lane.layout.rle.count, lane.layout.donor_rle.count)) continue;
            if (lane.layout.donor_mask_words != mask_words) throw std::logic_error("Explore prepared donor payload is unavailable");
            if (!donor_storage_ready) {
                ensure_gallery_buffer(descriptors_.storage_.buffers_.donor_boxes_device_, donor_box_bytes, "Explore donor box high-water allocation failed",
                                      diagnostics_, device_, State().plan.generation);
                ensure_gallery_buffer(descriptors_.storage_.buffers_.donor_masks_device_, donor_mask_bytes, "Explore donor mask high-water allocation failed",
                                      diagnostics_, device_, State().plan.generation);
                donor_storage_ready = true;
            }
            const auto label = load_payload<data::PackedInstance>(lane.pinned.data(), lane.layout.donor_instance);
            auto& donor = descriptors_.batch_donors_[slot];
            donor.label = label.class_id;
            donor.dataset_index = lane.donor_index;
            donor.area = static_cast<float>((label.bbox_x2 - label.bbox_x1) * (label.bbox_y2 - label.bbox_y1));
            std::memcpy(donor.box.data(), static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_box, 4U * sizeof(float));
            donor.has_mask = lane.layout.donor_rle.count != 0U;
            if (!scheduler_.current_demand_(State().plan.generation)) return nullptr;
            ensure_gallery_cuda(cudaMemcpyAsync(static_cast<float*>(descriptors_.storage_.buffers_.donor_boxes_device_.data()) + slot * 4U,
                                                static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_box, 4U * sizeof(float),
                                                cudaMemcpyHostToDevice, cuda_stream),
                                "Explore prepared donor box upload failed");
            if (!scheduler_.current_demand_(State().plan.generation)) return nullptr;
            ensure_gallery_cuda(cudaMemcpyAsync(static_cast<std::uint64_t*>(descriptors_.storage_.buffers_.donor_masks_device_.data()) + slot * mask_words,
                                                static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_mask, mask_words * sizeof(std::uint64_t),
                                                cudaMemcpyHostToDevice, cuda_stream),
                                "Explore prepared donor mask upload failed");
        }
        donor_view = {.images = nullptr,
                      .masks = static_cast<const std::int64_t*>(descriptors_.storage_.buffers_.donor_masks_device_.data()),
                      .boxes = static_cast<const float*>(descriptors_.storage_.buffers_.donor_boxes_device_.data()),
                      .mask_words = static_cast<std::int64_t>(mask_words),
                      .image_slots = descriptors_.batch_donor_slots_,
                      .image_custody = scheduler_.image_stream_.storage_custody().lock(),
                      .image_capacity_bytes = pixel_bytes};
    }
    const rfdetr::AugmentationBatchPlan* augmentation_plan = nullptr;
    if (!scheduler_.current_demand_(State().plan.generation)) return nullptr;
    if (preview_active)
        augmentation_plan =
            &descriptors_.augmenter_->Run({.input = nullptr,
                                           .output = static_cast<float*>(descriptors_.storage_.buffers_.augmented_batch_.data()),
                                           .image_indices = descriptors_.batch_indices_,
                                           .height = static_cast<int>(State().store->header().image_height),
                                           .width = static_cast<int>(State().store->header().image_width),
                                           .output_domain = rfdetr::GpuAugmentationOutputDomain::UnitRgb,
                                           .input_slots = descriptors_.batch_input_slots_,
                                           .input_custody = scheduler_.image_stream_.storage_custody().lock(),
                                           .output_custody = shared_from_this(),
                                           .input_capacity_bytes = pixel_bytes,
                                           .output_capacity_bytes = ready_lanes.size() * pixel_bytes},
                                          descriptors_.batch_keys_, descriptors_.batch_donors_, donor_view, cuda_stream, State().plan.augmentation.seed % 2U);
    if (augmentation_plan != nullptr && diagnostics_.valid())
        for (std::size_t slot = 0U; slot != ready_lanes.size(); ++slot)
            GalleryStreamProbe::DiagnosePreparedImage(State().plan, scheduler_, *descriptors_.augmenter_, diagnostics_, *ready_lanes[slot], slot,
                                                      State().plan.augmentation.seed % 2U);
    return augmentation_plan;
}
explore::ExploreRenderCardDescriptor GalleryStream::Impl::AssembleImageMeaning(const Lane& lane, const rfdetr::AugmentationImagePlan* image_plan,
                                                                               const float* pixels, const std::size_t card_index, std::size_t& annotation_count,
                                                                               std::size_t& rle_count) {
    const auto source_instances = State().store->image_labels(lane.compiled_index);
    const bool has_paste = image_plan != nullptr && image_plan->paste_donor_slot >= 0;
    const auto donor = has_paste ? std::optional{load_payload<data::PackedInstance>(lane.pinned.data(), lane.layout.donor_instance)} : std::nullopt;
    auto support_plan = image_plan != nullptr ? *image_plan : rfdetr::AugmentationImagePlan{};
    if (has_paste) {
        support_plan.paste_support = reinterpret_cast<const data::RLEPair*>(static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_rle.offset);
        support_plan.paste_support_count = lane.layout.donor_rle.count;
    }
    rfdetr::build_augmentation_preview_annotations(source_instances, donor ? &*donor : nullptr, image_plan != nullptr ? &support_plan : nullptr,
                                                   static_cast<int>(State().store->header().image_width),
                                                   static_cast<int>(State().store->header().image_height), projected_annotations_, State().store->rle_pairs());
    auto card = load_payload<explore::ExploreRenderCardDescriptor>(lane.pinned.data(), lane.layout.card);
    card.pixels = pixels;
    card.erasure = image_plan != nullptr ? image_plan->erasure : rfdetr::AugmentationSpatialErasure{};
    card.annotation_offset = static_cast<std::uint32_t>(annotation_count);
    card.annotation_count = static_cast<std::uint32_t>(projected_annotations_.size());
    for (const auto& projected : projected_annotations_) {
        // Compiled offsets address the dataset. Source ordinals locate the
        // compact lane records even when earlier sources were culled.
        const bool source = projected.source_ordinal < source_instances.size();
        auto annotation =
            source ? load_payload<explore::ExploreRenderAnnotationDescriptor>(
                         lane.pinned.data(), lane.layout.annotations.offset + projected.source_ordinal * sizeof(explore::ExploreRenderAnnotationDescriptor))
                   : explore::ExploreRenderAnnotationDescriptor{.rle_offset = static_cast<std::uint32_t>(lane.layout.rle.count),
                                                                .rle_count = static_cast<std::uint32_t>(lane.layout.donor_rle.count)};
        std::copy(projected.box_xyxy.begin(), projected.box_xyxy.end(), std::begin(annotation.box_xyxy));
        std::copy(projected.inverse.begin(), projected.inverse.end(), std::begin(annotation.inverse));
        annotation.class_id = projected.class_id;
        annotation.card_index = static_cast<std::uint32_t>(card_index);
        annotation.rle_offset += static_cast<std::uint32_t>(rle_count);
        annotation.occluder_index = projected.occluder_index < 0 ? -1 : projected.occluder_index + static_cast<std::int32_t>(card.annotation_offset);
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(),
                      descriptors_.descriptor_layout_.annotations.offset + annotation_count++ * sizeof(annotation), annotation);
    }
    const auto append_runs = [&](const std::size_t offset, const std::size_t count) {
        if (count != 0U)
            std::memcpy(static_cast<std::byte*>(descriptors_.storage_.buffers_.descriptors_.data()) + descriptors_.descriptor_layout_.rle.offset +
                            rle_count * sizeof(data::RLEPair),
                        static_cast<const std::byte*>(lane.pinned.data()) + offset, count * sizeof(data::RLEPair));
        rle_count += count;
    };
    append_runs(lane.layout.rle.offset, lane.layout.rle.count);
    if (has_paste) append_runs(lane.layout.donor_rle.offset, lane.layout.donor_rle.count);
    return card;
}
ExploreGalleryPublication GalleryStream::Impl::PublishTiles(const mmltk::frameworks::gpu::ImagePlaneView clean,
                                                            const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) {
    if (!publication_active_) throw std::logic_error("Explore tile output was not prepared");
    RestoreVisibleTiles(State().active_classes, clean, semantic, stream);
    return RenderReadyTiles(clean, semantic, stream, false);
}
void GalleryStream::Impl::RestoreVisibleTiles(const std::span<const explore::ExploreRenderClassDescriptor> classes,
                                              const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                              const std::uintptr_t stream) {
    SeedPlaceholders(classes, clean, semantic, stream);
    for (std::size_t slot = 0U; slot < State().visible_indices.size(); ++slot)
        if (State().cache.Retained(State().visible_indices[slot])) PlaceTile(clean, semantic, static_cast<std::uint32_t>(slot), stream);
}
ExploreGalleryPublication GalleryStream::Impl::RenderReadyTiles(const mmltk::frameworks::gpu::ImagePlaneView clean,
                                                                const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream,
                                                                const bool background) {
    const auto generation = scheduler_.desired_generation_.load(std::memory_order_acquire);
    std::array<Lane*, kExploreMaximumParallelism> ready{};
    std::size_t ready_count = 0U;
    DescriptorBudget source_budget;
    {
        std::scoped_lock lock(scheduler_.lanes_mutex_);
        for (auto& lane : scheduler_.lanes_)
            if (lane->state == LaneState::InputReady && lane->generation == generation && lane->prefetch == background) ready[ready_count++] = lane.get();
        std::ranges::sort(std::span{ready}.first(ready_count), [&](const Lane* left, const Lane* right) {
            return scheduler_.priority_rank_[State().cache.Slot(left->position)] < scheduler_.priority_rank_[State().cache.Slot(right->position)];
        });
        std::size_t admitted = 0U;
        for (auto* lane : std::span{ready}.first(ready_count)) {
            if (!source_budget.Admit(lane->layout.annotations.count, lane->layout.rle.count)) break;
            ready[admitted++] = lane;
        }
        ready_count = admitted;
    }
    const std::span ready_lanes{ready.data(), ready_count};
    if (ready_lanes.empty()) return PublicationFacts(0U);
    if (!scheduler_.Current(generation)) return PublicationFacts(0U);
    // Allocation-local placeholders may have used the shared descriptor/scratch
    // storage earlier in this publication.
    SettleDescriptors();
    auto* const cuda_stream = reinterpret_cast<cudaStream_t>(stream);
    const auto pixel_bytes = ready_lanes.front()->layout.pixel_bytes;
    const auto* augmentation_plan = PrepareImages(ready_lanes, source_budget, cuda_stream);
    if (!scheduler_.Current(generation)) {
        descriptors_.descriptors_pending_ = true;
        return PublicationFacts(0U);
    }
    const bool preview_active = augmentation_plan != nullptr;
    if (preview_active && diagnostics_.valid()) {
        const auto valid_donors =
            static_cast<std::uint32_t>(std::ranges::count_if(std::views::iota(std::size_t{0U}, descriptors_.batch_donors_.size()), [&](const std::size_t slot) {
                return descriptors_.batch_donors_[slot].label >= 0 && descriptors_.batch_donors_[slot].dataset_index != descriptors_.batch_indices_[slot] &&
                       descriptors_.batch_donors_[slot].box[0] < descriptors_.batch_donors_[slot].box[2] &&
                       descriptors_.batch_donors_[slot].box[1] < descriptors_.batch_donors_[slot].box[3];
            }));
        std::size_t image_capacity = 0U;
        for (const auto& lane : scheduler_.lanes_) image_capacity += scheduler_.image_stream_.device_storage(lane->index).capacity_bytes();
        const auto planned_pastes = static_cast<std::uint32_t>(std::ranges::count_if(
            augmentation_plan->images | std::views::take(augmentation_plan->active_size), [](const auto& image) { return image.paste_donor_slot >= 0; }));
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::ExploreAugmentationBatchPrepared,
                                        .device = device_,
                                        .generation = generation,
                                        .value = ready_lanes.size(),
                                        .detail = State().plan.augmentation.seed,
                                        .context = {.capacity_width = valid_donors, .capacity_height = planned_pastes, .staging_bytes = image_capacity}};
        });
    }
    std::size_t ready_annotation_count = source_budget.annotations;
    std::size_t ready_rle_count = source_budget.runs;
    std::size_t donor_annotation_count = 0U;
    std::size_t donor_rle_count = 0U;
    const bool diagnostics_enabled = diagnostics_.valid();
    if (augmentation_plan != nullptr) {
        for (std::size_t slot = 0U; slot != ready_lanes.size(); ++slot) {
            if (augmentation_plan->images[slot].paste_donor_slot < 0) continue;
            ++ready_annotation_count;
            const auto card_donor_rle_count = ready_lanes[slot]->layout.donor_rle.count;
            ready_rle_count += card_donor_rle_count;
            if (diagnostics_enabled) {
                ++donor_annotation_count;
                donor_rle_count += card_donor_rle_count;
            }
        }
    }
    descriptors_.PrepareDescriptors(ready_lanes.size(), ready_annotation_count, ready_rle_count, State().active_classes.size(), ready_lanes.size(),
                                    diagnostics_, device_, State().plan.generation);
    std::size_t annotation_count = 0U;
    std::size_t rle_count = 0U;
    std::size_t tile_count = 0U;
    std::size_t ready_slot = 0U;
    for (Lane* const lane : ready_lanes) {
        if (!scheduler_.Current(generation)) {
            descriptors_.descriptors_pending_ = true;
            return PublicationFacts(0U);
        }
        lane->cache_slot = State().cache.Slot(lane->position);
        lane->semantic_identity = State().plan.semantic_identity;
        lane->cache_bank = WritableCacheBank(lane->position);
        lane->semantic_bank = WritableCacheBank(lane->position, true);
        const auto* image_plan = augmentation_plan != nullptr ? &augmentation_plan->images[ready_slot] : nullptr;
        const auto card_annotation_offset = annotation_count;
        const auto card_rle_offset = rle_count;
        const auto* image_batch = static_cast<const float*>(preview_active ? descriptors_.storage_.buffers_.augmented_batch_.data()
                                                                           : scheduler_.image_stream_.device_storage(lane->index).data());
        const auto card = AssembleImageMeaning(*lane, image_plan, image_batch + (preview_active ? ready_slot * pixel_bytes / sizeof(float) : 0U), tile_count,
                                               annotation_count, rle_count);
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.cards.offset + tile_count * sizeof(card), card);
        auto tile = load_payload<explore::ExploreRenderTileDescriptor>(lane->pinned.data(), lane->layout.tile);
        tile.generation.viewport = generation;
        {
            tile.destination_x = 0U;
            tile.destination_y = static_cast<std::uint32_t>(State().cache.PhysicalRow(lane->cache_slot, lane->cache_bank));
            tile.semantic_y_offset =
                static_cast<std::int32_t>(State().cache.PhysicalRow(lane->cache_slot, lane->semantic_bank)) - static_cast<std::int32_t>(tile.destination_y);
        }
        lane->pending_meaning = CaptureMeaning(card, card_annotation_offset, annotation_count, card_rle_offset, rle_count);
        tile.card_index = static_cast<std::uint32_t>(tile_count);
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(),
                      descriptors_.descriptor_layout_.tiles.offset + tile_count++ * sizeof(explore::ExploreRenderTileDescriptor), tile);
        GalleryStreamProbe::DiagnoseDescriptors(descriptors_, diagnostics_, device_, generation, lane->destination_slot, card_annotation_offset,
                                                annotation_count - card_annotation_offset, rle_count - card_rle_offset);
        ++ready_slot;
    }
    descriptors_.descriptor_layout_.annotations.count = annotation_count;
    descriptors_.descriptor_layout_.rle.count = rle_count;
    descriptors_.descriptor_layout_.tiles.count = tile_count;
    if (diagnostics_enabled && donor_annotation_count != 0U)
        diagnostics_.Emit([&] {
            auto fact = Diagnostic(VisualDiagnosticOperation::ExploreDonorDescriptorsPrepared, generation);
            fact.value = donor_annotation_count;
            fact.detail = donor_rle_count;
            fact.context.capacity_width = static_cast<std::uint32_t>(ready_annotation_count);
            fact.context.capacity_height = static_cast<std::uint32_t>(ready_rle_count);
            return fact;
        });
    // Descriptor storage is execution scratch. A failed candidate may have
    // replaced its classes, so derive the batch from this product's exact facts.
    store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.classes.offset, std::span{State().active_classes});
    if (!scheduler_.Current(generation)) {
        // Preparation may have submitted augmentation. It retains its source
        // lanes until the ordinary owner settlement; no obsolete atlas launch.
        descriptors_.descriptors_pending_ = true;
        return PublicationFacts(0U);
    }
    // From descriptor upload through every raster pass, exceptions and skipped
    // admission retain scratch and source custody for ordinary settlement.
    descriptors_.descriptors_pending_ = true;
    UploadDescriptors(cuda_stream, true);
    const auto submission =
        RenderAtlasBatch(CachePlane(false), CachePlane(true), stream, static_cast<std::uint32_t>(tile_count), generation, AtlasBatch::Cache);
    if (submission == BatchSubmission::Skipped) return PublicationFacts(0U);
    diagnostics_.Emit([&] {
        return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                    .operation = VisualDiagnosticOperation::ExploreRenderSubmitted,
                                    .device = device_,
                                    .generation = generation,
                                    .value = tile_count,
                                    .detail = State().cumulative_tiles,
                                    .context = {.condition = background}};
    });
    if (acceptance_)
        acceptance_->ObserveSubmission(stream,
                                       background ? ExploreAcceptanceGate::SubmissionStage::Background : ExploreAcceptanceGate::SubmissionStage::Foreground);
    {
        std::scoped_lock lock(scheduler_.lanes_mutex_);
        for (Lane* const lane : ready_lanes) {
            // Staged meaning alone is insufficient: only the return from all
            // required cache passes allows a source release to complete a tile.
            lane->state = LaneState::GpuPending;
        }
    }
    for (Lane* const lane : ready_lanes) { scheduler_.ReleaseLane(*lane, stream_); }
    if (!background) {
        SettleDescriptors();
        CompleteTiles(true);
        if (!scheduler_.Current(generation)) return PublicationFacts(0U);
        for (const Lane* const lane : ready_lanes) PlaceTile(clean, semantic, lane->destination_slot, stream);
    }
    if (diagnostics_enabled && !background) {
        const auto card_extent = explore_atlas_card_extent(State().viewport);
        for (std::size_t card_index = 0U; card_index != ready_lanes.size(); ++card_index) {
            const Lane* const lane = ready_lanes[card_index];
            DiagnoseRendered(clean, semantic, stream, generation, lane->destination_slot, lane->compiled_index, card_index,
                             lane->destination_slot % State().viewport.columns * card_extent, AtlasY(lane->destination_slot), card_extent, card_extent);
        }
    }
    FlushProbes(stream);
    return PublicationFacts(0U);
}
std::shared_ptr<const GalleryStream::Impl::TileMeaning> GalleryStream::Impl::CaptureMeaning(explore::ExploreRenderCardDescriptor card,
                                                                                            const std::size_t annotation_begin,
                                                                                            const std::size_t annotation_end, const std::size_t run_begin,
                                                                                            const std::size_t run_end) const {
    auto meaning = MakeGalleryShared<TileMeaning>();
    card.pixels = nullptr;
    card.annotation_offset = 0U;
    meaning->card = card;
    meaning->annotations.reserve(annotation_end - annotation_begin);
    for (auto index = annotation_begin; index != annotation_end; ++index) {
        auto annotation = load_payload<explore::ExploreRenderAnnotationDescriptor>(
            descriptors_.storage_.buffers_.descriptors_.data(),
            descriptors_.descriptor_layout_.annotations.offset + index * sizeof(explore::ExploreRenderAnnotationDescriptor));
        annotation.rle_offset -= static_cast<std::uint32_t>(run_begin);
        if (annotation.occluder_index >= 0) annotation.occluder_index -= static_cast<std::int32_t>(annotation_begin);
        meaning->annotations.push_back(annotation);
    }
    meaning->runs.resize(run_end - run_begin);
    if (!meaning->runs.empty())
        std::memcpy(meaning->runs.data(),
                    static_cast<const std::byte*>(descriptors_.storage_.buffers_.descriptors_.data()) + descriptors_.descriptor_layout_.rle.offset +
                        run_begin * sizeof(data::RLEPair),
                    meaning->runs.size() * sizeof(data::RLEPair));
    return meaning;
}
void GalleryStream::Impl::RenderDetail(const ExploreRenderPlan& plan, std::shared_ptr<const mmltk::backend::data::CompiledDataset> store,
                                       const std::span<const std::uint32_t> annotated_indices,
                                       const std::span<const explore::ExploreRenderClassDescriptor> classes, const mmltk::frameworks::gpu::ImagePlaneView clean,
                                       const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) {
    if (!publication_active_) throw std::logic_error("Explore detail output was not prepared");
    if (CompleteCandidate() && !borrowed_cache_) {
        std::swap(State().cache, committed_.cache);
        borrowed_cache_ = true;
        State().cache.BeginUpdate();
    }
    if (publication_change_ == ExploreOutputChange::Unchanged) {
        PrepareReuse(plan);
        return;
    }
    if (State().store == store && State().document && State().detail_meaning && State().plan.mode == ExploreMode::Detail &&
        State().plan.dataset_identity == plan.dataset_identity && State().plan.selected_image == plan.selected_image &&
        State().plan.augmentation.enabled == plan.augmentation.enabled && State().plan.augmentation.seed == plan.augmentation.seed &&
        State().plan.augmentation_config == plan.augmentation_config) {
        SaveOverlay();
        const bool unchanged = State().plan.semantic_identity == plan.semantic_identity;
        State().plan = plan;
        State().active_classes.assign(classes.begin(), classes.end());
        if (unchanged) return;
        stream_ = reinterpret_cast<cudaStream_t>(stream);
        const auto& meaning = *State().detail_meaning;
        descriptors_.PrepareDescriptors(1U, meaning.annotations.size(), meaning.runs.size(), classes.size(), 0U, diagnostics_, device_,
                                        State().plan.generation);
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.cards.offset, meaning.card);
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.annotations.offset, std::span{meaning.annotations});
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.rle.offset, std::span{meaning.runs});
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.classes.offset, classes);
        UploadDescriptors(reinterpret_cast<cudaStream_t>(stream), true);
        descriptors_.descriptors_pending_ = true;
        if (!scheduler_.current_demand_(plan.generation)) return;
        RenderDetailPlane(State().detail_view, plan.overlay, semantic, stream, true);
        CheckSettlement(stream_wait_(reinterpret_cast<cudaStream_t>(stream)), "Explore semantic update failed");
        descriptors_.descriptors_pending_ = false;
        return;
    }
    Suspend();
    Lane* reusable_lane = nullptr;
    {
        std::scoped_lock lock(scheduler_.lanes_mutex_);
        for (auto& candidate : scheduler_.lanes_)
            if (candidate->store == store && candidate->identity.dataset == plan.dataset_identity &&
                candidate->identity.augmented == plan.augmentation.enabled && candidate->identity.seed == plan.augmentation.seed &&
                candidate->identity.augmentation == plan.augmentation_config && candidate->compiled_index == *plan.selected_image &&
                candidate->state == LaneState::InputReady)
                reusable_lane = candidate.get();
    }
    State().store = std::move(store);
    State().annotated_indices = annotated_indices;
    State().plan = plan;
    stream_ = reinterpret_cast<cudaStream_t>(stream);
    Lane& lane = reusable_lane ? *reusable_lane : *scheduler_.detail_lane_;
    if (!reusable_lane) {
        if (!scheduler_.current_demand_(plan.generation)) return;
        scheduler_.PrepareLaneStorage(State(), lane, *plan.selected_image, 0U, plan.generation);
        lane.prefetch = false;
        lane.state = LaneState::Reading;
        scheduler_.SubmitRead(lane, false);
        if (!scheduler_.image_stream_.wait_read(lane.index)) throw std::runtime_error("Explore detail read cancelled");
        scheduler_.ReadLanePayload(lane);
    }
    auto* const cuda_stream = reinterpret_cast<cudaStream_t>(stream);
    const std::array ready_lanes{&lane};
    const auto* augmentation_plan = PrepareImages(ready_lanes, {lane.layout.annotations.count, lane.layout.rle.count}, cuda_stream);
    if (!scheduler_.current_demand_(plan.generation)) {
        descriptors_.descriptors_pending_ = true;
        return;
    }
    const bool preview_active = augmentation_plan != nullptr;
    const bool has_paste = augmentation_plan != nullptr && augmentation_plan->images.front().paste_donor_slot >= 0;
    const auto* const donor_rle = reinterpret_cast<const data::RLEPair*>(static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_rle.offset);
    const auto donor_runs = has_paste ? std::span{donor_rle, lane.layout.donor_rle.count} : std::span<const data::RLEPair>{};
    descriptors_.PrepareDescriptors(1U, lane.layout.annotations.count + (has_paste ? 1U : 0U), lane.layout.rle.count + donor_runs.size(), classes.size(), 0U,
                                    diagnostics_, device_, State().plan.generation);
    std::size_t annotation_count = 0U;
    std::size_t rle_count = 0U;
    const auto card = AssembleImageMeaning(lane, augmentation_plan != nullptr ? &augmentation_plan->images.front() : nullptr,
                                           static_cast<const float*>(preview_active ? descriptors_.storage_.buffers_.augmented_batch_.data()
                                                                                    : scheduler_.image_stream_.device_storage(lane.index).data()),
                                           0U, annotation_count, rle_count);
    store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.cards.offset, card);
    descriptors_.descriptor_layout_.annotations.count = annotation_count;
    descriptors_.descriptor_layout_.rle.count = rle_count;
    GalleryStreamProbe::DiagnoseDescriptors(descriptors_, diagnostics_, device_, plan.generation, *plan.selected_image, 0U,
                                            descriptors_.descriptor_layout_.annotations.count, lane.layout.rle.count + donor_runs.size());
    store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.classes.offset, classes);
    UploadDescriptors(cuda_stream, true);
    explore::ExploreRenderDetailView detail{
        .erasure = augmentation_plan != nullptr ? augmentation_plan->images.front().erasure : rfdetr::AugmentationSpatialErasure{},
        .pixels = static_cast<const float*>(preview_active ? descriptors_.storage_.buffers_.augmented_batch_.data()
                                                           : scheduler_.image_stream_.device_storage(lane.index).data()),
        .source_width = State().store->header().image_width,
        .source_height = State().store->header().image_height,
        .crop_width = State().store->header().image_width,
        .crop_height = State().store->header().image_height,
    };
    RenderDetailPlane(detail, plan.overlay, clean, stream, false);
    State().detail_view = detail;
    State().detail_view.pixels = nullptr;
    State().detail_meaning = CaptureMeaning(card, 0U, annotation_count, 0U, rle_count);
    RenderDetailPlane(detail, plan.overlay, semantic, stream, true);
    auto document = std::make_shared<VisualDocument>();
    document->scene.document = contracts::WorkspaceResource::From("explore://image", plan.generation);
    document->scene.frame_index = *plan.selected_image;
    for (const auto& name : State().store->class_names()) document->scene.categories.push_back({.value = name});
    document->scene.palette = mmltk::controller::annotation_class_palette(document->scene.categories.size());
    const float document_width = static_cast<float>(detail.source_width);
    const float document_height = static_cast<float>(detail.source_height);
    for (std::size_t index = 0U; index < descriptors_.descriptor_layout_.annotations.count; ++index) {
        const auto& annotation = State().detail_meaning->annotations[index];
        contracts::AnnotationObject object;
        object.name = contracts::AnnotationText::From("object " + std::to_string(index + 1U));
        object.category = annotation.class_id;
        object.box = {{annotation.box_xyxy[0] * document_width, annotation.box_xyxy[1] * document_height},
                      {annotation.box_xyxy[2] * document_width, annotation.box_xyxy[3] * document_height}};
        object.mask.present = annotation.rle_count != 0U;
        object.shape = object.mask.present ? contracts::AnnotationShape::Mask : contracts::AnnotationShape::Box;
        document->scene.objects.push_back(std::move(object));
    }
    document->mask_contains = [meaning = State().detail_meaning, width = detail.source_width, height = detail.source_height, erasure = detail.erasure](
                                  std::size_t index, float x, float y) {
        const auto& annotations = meaning->annotations;
        const auto& runs = meaning->runs;
        if (index >= annotations.size() || rfdetr::augment_math::erases_sample(erasure, x, y, width, height)) return false;
        const auto sample = [&](std::size_t item) {
            return explore::detail::sample_annotation_mask(runs.data(), annotations[item], static_cast<std::uint32_t>(runs.size()), width, height, x, y);
        };
        const auto occluder = annotations[index].occluder_index;
        return sample(index) &&
               !(occluder >= 0 && static_cast<std::size_t>(occluder) < annotations.size() &&
                 explore::detail::sample_annotation_support(runs.data(), annotations[occluder], static_cast<std::uint32_t>(runs.size()), width, height, x, y));
    };
    State().document = std::move(document);
    DiagnoseRendered(clean, semantic, stream, plan.generation, *plan.selected_image, *plan.selected_image);
    FlushProbes(stream);
    CheckSettlement(stream_wait_(cuda_stream), "Explore detail completion failed");
    scheduler_.image_stream_.synchronize(lane.index);
    descriptors_.descriptors_pending_ = false;
    lane.state = reusable_lane ? LaneState::InputReady : LaneState::Idle;
}
void GalleryStream::Impl::SeedPlaceholders(const std::span<const explore::ExploreRenderClassDescriptor> classes,
                                           const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                           const std::uintptr_t stream) {
    State().atlas = atlas_.Begin(clean, semantic, State().viewport, State().cache.identity());
    const auto side = State().atlas.card_extent;
    const auto columns = State().atlas.columns;
    const auto cells = static_cast<std::size_t>(State().atlas.row_capacity) * columns;
    auto& placeholders = descriptors_.semantic_slots_;
    placeholders.clear();
    for (std::size_t physical = 0U; physical < cells; ++physical) {
        const auto row = (physical / columns + State().atlas.row_capacity - State().atlas.row_origin) % State().atlas.row_capacity;
        const auto logical = row * columns + physical % columns;
        const bool visible = row < State().atlas.row_count && logical < State().visible_indices.size();
        if (visible && State().cache.Retained(State().visible_indices[logical])) continue;
        if (atlas_.Empty(physical, visible)) continue;
        atlas_.Touch(physical);
        if (visible) {
            placeholders.push_back(static_cast<std::uint32_t>(physical));
        } else {
            const auto clear_cell = [&](auto plane) {
                plane.data += physical / columns * side * plane.descriptor.pitch_bytes + physical % columns * side * 4U;
                plane.descriptor.width = side;
                plane.descriptor.height = side;
                Clear(plane, stream);
            };
            clear_cell(clean);
            clear_cell(semantic);
            atlas_.Stage(physical);
        }
    }
    if (placeholders.empty() || !scheduler_.Current(State().plan.generation)) return;
    descriptors_.PrepareDescriptors(placeholders.size(), 0U, 0U, classes.size(), placeholders.size(), diagnostics_, device_, State().plan.generation);
    for (std::size_t index = 0U; index < placeholders.size(); ++index) {
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(),
                      descriptors_.descriptor_layout_.cards.offset + index * sizeof(explore::ExploreRenderCardDescriptor),
                      explore::ExploreRenderCardDescriptor{.placeholder = 1U});
        const auto physical = placeholders[index];
        const explore::ExploreRenderTileDescriptor tile{
            .card_index = static_cast<std::uint32_t>(index),
            .destination_x = physical % columns * side,
            .destination_y = physical / columns * side,
            .destination_width = side,
            .destination_height = side,
            .generation = {.viewport = State().plan.generation, .tile = ++scheduler_.next_tile_generation_},
            .placeholder = 1U,
        };
        store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.tiles.offset + index * sizeof(tile), tile);
    }
    store_payload(descriptors_.storage_.buffers_.descriptors_.data(), descriptors_.descriptor_layout_.classes.offset, classes);
    UploadDescriptors(reinterpret_cast<cudaStream_t>(stream), true);
    descriptors_.descriptors_pending_ = true;
    if (RenderAtlasBatch(clean, semantic, stream, static_cast<std::uint32_t>(placeholders.size()), State().plan.generation, AtlasBatch::Placeholders) ==
        BatchSubmission::Complete)
        for (const auto physical : placeholders) atlas_.Stage(physical, {}, 0U, true);
}
auto GalleryStream::Impl::RenderAtlasBatch(const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                           const std::uintptr_t stream, const std::uint32_t tile_count, const std::uint64_t generation,
                                           const AtlasBatch purpose) -> BatchSubmission {
    const bool cache = purpose == AtlasBatch::Cache;
    if (cache && acceptance_) acceptance_->ObserveSubmission(stream, ExploreAcceptanceGate::SubmissionStage::BeforeCacheAdmission);
    if (!scheduler_.Current(generation)) return BatchSubmission::Skipped;
    // Admission commits this bounded cache batch, including backend base and
    // box passes. Later viewport demand controls atlas authorization, never
    // whether retained pixels were completely produced.
    const auto demand = cache ? explore::detail::ExploreRenderDemand{} : Demand();
    const auto card_extent = explore_atlas_card_extent(State().viewport);
    const explore::ExploreRenderAtlasView atlas{
        .card_extent = card_extent,
        .card_count = static_cast<std::uint32_t>(descriptors_.descriptor_layout_.cards.count),
        .source_width = State().store->header().image_width,
        .source_height = State().store->header().image_height,
    };
    const explore::ExploreRenderTileBatchView batch{
        .tile_count = tile_count,
        .tile_capacity = tile_count,
        .max_tile_width = card_extent,
        .max_tile_height = card_extent,
        .viewport_generation = generation,
    };
    RenderAtlasPlane(atlas, batch, State().plan.overlay, clean, stream, false, demand);
    if (cache && acceptance_) acceptance_->ObserveSubmission(stream, ExploreAcceptanceGate::SubmissionStage::CacheCleanSubmitted);
    if (!cache && !scheduler_.Current(generation)) return BatchSubmission::Skipped;
    RenderAtlasPlane(atlas, batch, State().plan.overlay, semantic, stream, true, demand);
    return BatchSubmission::Complete;
}
void GalleryStream::Impl::RenderAtlasPlane(explore::ExploreRenderAtlasView atlas, const explore::ExploreRenderTileBatchView& batch,
                                           const ExploreOverlay& overlay, const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t stream,
                                           const bool semantic, const explore::detail::ExploreRenderDemand demand) {
    atlas.draw_base = semantic ? 0U : 1U;
    if (explore::render_explore_atlas_tiles(atlas, batch, Semantics(overlay, semantic), Scratch(), gallery_target(target), stream, demand) !=
        explore::kExploreStorageSuccess)
        throw std::runtime_error("Explore retained atlas renderer failed");
}
void GalleryStream::Impl::RenderDetailPlane(explore::ExploreRenderDetailView detail, const ExploreOverlay& overlay,
                                            const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t stream, const bool semantic) {
    detail.draw_base = semantic ? 0U : 1U;
    // The ABI validates a source address even for a semantic-only launch.
    // That launch never samples source pixels; bind live receiver storage,
    // rather than retaining the raw read lane's expired float allocation.
    if (semantic) detail.pixels = reinterpret_cast<const float*>(target.data);
    if (explore::render_explore_detail(detail, Semantics(overlay, semantic), Scratch(), gallery_target(target), stream, Demand()) !=
        explore::kExploreStorageSuccess)
        throw std::runtime_error("Explore retained detail renderer failed");
}
void GalleryStream::Impl::DiagnoseRendered(mmltk::frameworks::gpu::ImagePlaneView clean, mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t stream,
                                           std::uint64_t generation, std::uint64_t slot, std::uint32_t compiled_index, std::optional<std::size_t> card_index,
                                           std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) try {
    if (!diagnostics_.pixel_probes_enabled() || (probe_ && probe_->probes_disabled_)) return;
    if (!probe_) probe_ = std::make_unique<GalleryStreamProbe>(device_, diagnostics_, acceptance_);
    probe_->DiagnoseRendered(State(), descriptors_, State().cache.size() != 0U ? CachePlane(false) : mmltk::frameworks::gpu::ImagePlaneView{}, ProtectedCache(),
                             clean, semantic, stream, generation, slot, compiled_index, card_index, x, y, width, height);
} catch (...) {
    diagnostics_.Emit([&] { return Diagnostic(VisualDiagnosticOperation::ExploreProbeFailed, generation); });
}
explore::ExploreRenderScratchView GalleryStream::Impl::Scratch() const {
    return {
        // CLEANUP-IGNORE: This is the single explicit CUDA scratch ABI projection of distinct buffer types and
        // capacities.
        .cards = static_cast<const explore::ExploreRenderCardDescriptor*>(descriptors_.storage_.buffers_.cards_device_.data()),
        .card_capacity = static_cast<std::uint32_t>(descriptors_.descriptor_layout_.cards.count),
        .annotations = static_cast<const explore::ExploreRenderAnnotationDescriptor*>(descriptors_.storage_.buffers_.annotations_device_.data()),
        .annotation_capacity = static_cast<std::uint32_t>(descriptors_.descriptor_layout_.annotations.count),
        .rle_pairs = static_cast<const data::RLEPair*>(descriptors_.storage_.buffers_.rle_device_.data()),
        .rle_capacity = static_cast<std::uint32_t>(descriptors_.descriptor_layout_.rle.count),
        .classes = static_cast<const explore::ExploreRenderClassDescriptor*>(descriptors_.storage_.buffers_.classes_device_.data()),
        .class_capacity = static_cast<std::uint32_t>(descriptors_.descriptor_layout_.classes.count),
        .tiles = static_cast<const explore::ExploreRenderTileDescriptor*>(descriptors_.storage_.buffers_.tiles_device_.data()),
        .tile_capacity = static_cast<std::uint32_t>(descriptors_.descriptor_layout_.tiles.count),
    };
}
explore::ExploreRenderSemanticView GalleryStream::Impl::Semantics(const ExploreOverlay& overlay, const bool semantic) const {
    return {
        .annotation_count = static_cast<std::uint32_t>(descriptors_.descriptor_layout_.annotations.count),
        .rle_count = static_cast<std::uint32_t>(descriptors_.descriptor_layout_.rle.count),
        .class_count = State().store->header().num_classes,
        .show_boxes = static_cast<std::uint8_t>(semantic && overlay.show_boxes),
        .show_masks = static_cast<std::uint8_t>(semantic && overlay.show_masks),
    };
}
void GalleryStream::Impl::UploadDescriptors(cudaStream_t stream, bool upload_classes) {
    descriptors_.UploadDescriptors(stream, upload_classes, scheduler_.current_demand_, State().plan.generation);
    if (acceptance_) acceptance_->CheckPublication(ExploreAcceptanceGate::PublicationStage::DescriptorsPrepared);
    if (diagnostics_.valid())
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{
                .system = contracts::DiagnosticOwner::Explore,
                .operation = VisualDiagnosticOperation::ExploreOverlayDescriptorsPrepared,
                .device = device_,
                .generation = State().plan.generation,
                .value = descriptors_.descriptor_layout_.annotations.count,
                .detail = descriptors_.descriptor_layout_.rle.count,
                .context = {.capacity_width = State().store->header().image_width, .capacity_height = State().store->header().image_height}};
        });
}
void GalleryStream::Impl::SettleDescriptors() {
    if (!descriptors_.descriptors_pending_) return;
    CheckSettlement(stream_wait_(stream_), "Explore descriptor staging settlement failed");
    descriptors_.descriptors_pending_ = false;
}
void GalleryStream::Impl::Clear(const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t stream) {
    ensure_gallery_cuda(cudaMemset2DAsync(reinterpret_cast<void*>(target.data), target.descriptor.pitch_bytes, 0, target.descriptor.row_bytes(),
                                          target.descriptor.height, reinterpret_cast<cudaStream_t>(stream)),
                        "Explore target clear failed");
}
void GalleryStream::Impl::Suspend() try {
    scheduler_.desired_generation_.store(0U, std::memory_order_release);
    if (acceptance_) acceptance_->AdvanceGeneration(0U);
    if (!retirement_.admission_open()) throw std::runtime_error("Explore gallery has terminal custody");
    if (stream_ != nullptr) CheckSettlement(stream_wait_(stream_), "Explore gallery quiescence failed");
    try {
        if (descriptors_.augmenter_) descriptors_.augmenter_->Finish();
    } catch (...) {
        Retire(retirement_.fact().first_failure != cudaSuccess ? retirement_.fact().first_failure : cudaErrorUnknown);
        throw;
    }
    scheduler_.SettleConsumers();
    if (probe_ && probe_->diagnostic_stream_ != nullptr) CheckSettlement(stream_wait_(probe_->diagnostic_stream_), "Explore diagnostic quiescence failed");
    descriptors_.descriptors_pending_ = false;
    if (probe_) {
        probe_->probe_count_ = 0U;
        probe_->probes_pending_.store(false, std::memory_order_release);
    }
    descriptors_.descriptor_layout_ = {};
    scheduler_.RestoreSettledLanes();
} catch (...) {
    Retire(retirement_.fact().first_failure != cudaSuccess ? retirement_.fact().first_failure : cudaErrorUnknown);
    throw;
}
void GalleryStream::Impl::Quiesce() try {
    Suspend();
    scheduler_.QuiesceReads(State().priority_slots.size());
} catch (...) {
    Retire(retirement_.fact().first_failure != cudaSuccess ? retirement_.fact().first_failure : cudaErrorUnknown);
    throw;
}
void GalleryStream::Impl::ClearLogicalState() {
    ClearUndo();
    candidate_.Clear();
    committed_.Clear();
    retained_.Clear();
    atlas_.Clear();
    selected_mode_ = ExploreMode::Gallery;
    mode_switched_ = false;
    publication_active_ = false;
    stream_ = nullptr;
    descriptors_.batch_indices_.clear();
    descriptors_.batch_keys_.clear();
    descriptors_.batch_donors_.clear();
    projected_annotations_.clear();
    ClearReadinessState();
    descriptors_.descriptor_layout_ = {};
    descriptors_.descriptors_pending_ = false;
    scheduler_.ResetLogical();
}
void GalleryStream::Impl::ClearReadinessState() {
    State().priority_slots.clear();
    State().completed_slots.clear();
    scheduler_.next_priority_ = 0U;
    State().cumulative_tiles = 0U;
    State().reused_tiles = 0U;
}
auto GalleryStream::Impl::ReleaseAfterRuntimeSettlement() noexcept -> mmltk::frameworks::gpu::SystemImageModel::Release {
    if (!retirement_.admission_open()) {
        Retire(retirement_.fact().first_failure);
        return TerminalRelease();
    }
    if (resources_released_) return {};
    try {
        Quiesce();
        ClearLogicalState();
    } catch (...) {
        const auto failure = std::current_exception();
        // Failed completion retains the complete physical owner. Do not retry
        // resource release after its settlement authority has become terminal.
        if (!retirement_.admission_open()) {
            Retire(retirement_.fact().first_failure);
            return TerminalRelease();
        }
        try {
            scheduler_.image_stream_.close();
        } catch (...) {}
        return {.all_released = false, .failure = failure};
    }
    return ResetBuffersChecked();
}
auto GalleryStream::Impl::ResetBuffersChecked() noexcept -> mmltk::frameworks::gpu::SystemImageModel::Release {
    if (!retirement_.admission_open()) {
        Retire(retirement_.fact().first_failure);
        return TerminalRelease();
    }
    if (resources_released_) return {};
    cudaError_t unsettled = cudaSuccess;
    const auto probes = probe_ ? probe_->ReleaseProbesChecked(stream_wait_, unsettled) : mmltk::frameworks::gpu::SystemImageModel::Release{};
    if (unsettled != cudaSuccess) Retire(unsettled);
    if (!probes.all_released) return probes;
    std::exception_ptr failure = probes.failure;
    try {
        scheduler_.image_stream_.close();
    } catch (...) { failure = mmltk::frameworks::gpu::combine_image_failures(failure, std::current_exception()); }
    // Runtime and lane settlement precede this call. The augmenter must
    // relinquish its borrowed staging before the fixed family is released.
    descriptors_.augmenter_.reset();
    auto shared = storage_.ResetChecked();
    const auto merge_storage = [&](const auto result) {
        shared.all_released &= result.all_released;
        if (shared.failure == explore::kExploreStorageSuccess) shared.failure = result.failure;
    };
    merge_storage(descriptors_.storage_.ResetChecked());
    if (probe_) merge_storage(probe_->storage_.ResetChecked());
    if (shared.failure != explore::kExploreStorageSuccess) {
        try {
            throw std::runtime_error("Explore high-water release failed");
        } catch (...) { failure = mmltk::frameworks::gpu::combine_image_failures(failure, std::current_exception()); }
    }
    const bool lane_owned = scheduler_.image_stream_.owns_resources();
    const bool all_released = !lane_owned && shared.all_released && (!probe_ || (!probe_->diagnostic_stream_ && !probe_->probes_ready_));
    if (all_released) {
        stream_ = nullptr;
        State().store = nullptr;
        State().visible_indices.clear();
        ClearReadinessState();
        descriptors_.descriptor_layout_ = {};
        resources_released_ = true;
    }
    return {.all_released = all_released, .failure = failure};
}
GalleryStream::GalleryStream(const std::size_t nproc, const mmltk::frameworks::gpu::DeviceExecution& execution, const ExploreNativeConfiguration& configuration,
                             const std::uint32_t maximum_height)
    : impl_(std::make_shared<Impl>(nproc, execution, configuration, maximum_height, terminal_, std::move(lease_))) {}
auto GalleryStream::Active() const -> Impl& {
    if (!impl_) throw std::runtime_error("Explore gallery has no physical owner");
    if (!terminal_.admission_open()) {
        impl_->Retire(terminal_.fact().first_failure);
        throw std::runtime_error("Explore gallery has terminal custody");
    }
    return *impl_;
}
void GalleryStream::SetStreamSettlement(decltype(&cudaStreamSynchronize) operation) { Active().SetStreamSettlement(operation); }
void GalleryStream::BindExecutionContext(const mmltk::frameworks::gpu::DeviceContext& context, std::shared_ptr<mmltk::frameworks::gpu::ImageStream> stream) {
    Active().BindExecutionContext(context, std::move(stream));
}
GalleryStream::~GalleryStream() {
    if (!impl_) return;
    if (!terminal_.admission_open()) {
        impl_->Retire(terminal_.fact().first_failure);
        return;
    }
    impl_->StopIngress();
    const auto released = impl_->ReleaseAfterRuntimeSettlement();
    if (!released.all_released) impl_->Retire(terminal_.fact().first_failure != cudaSuccess ? terminal_.fact().first_failure : cudaErrorUnknown);
}
mmltk::common::concurrency::WorkerPool& GalleryStream::workers() { return Active().workers(); }
void GalleryStream::SetReadySink(ExploreAlgorithm::GalleryReadySink sink) { Active().SetReadySink(std::move(sink)); }
void GalleryStream::SetCurrentDemand(ExploreDemandCheck check) { Active().SetCurrentDemand(std::move(check)); }
ExploreOutputChange GalleryStream::OutputChange(const ExploreRenderPlan& plan, std::span<const std::uint32_t> visible, const data::CompiledDataset* store,
                                                std::span<const std::uint32_t> window) const {
    return Active().OutputChange(plan, visible, store, window);
}
void GalleryStream::StopIngress() noexcept {
    if (impl_) impl_->StopIngress();
}
ExploreGalleryPublication GalleryStream::Begin(const ExploreRenderPlan& plan, std::span<const std::uint32_t> visible, std::span<const std::uint32_t> window,
                                               const std::size_t window_first, std::shared_ptr<const data::CompiledDataset> store,
                                               const std::span<const std::uint32_t> annotated,
                                               const std::span<const explore::detail::ExploreRenderClassDescriptorAbi> classes,
                                               const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                               const std::uintptr_t stream) {
    return Active().Begin(plan, visible, window, window_first, std::move(store), annotated, classes, clean, semantic, stream);
}
ExploreGalleryPublication GalleryStream::Advance() { return Active().Advance(); }
bool GalleryStream::HasReadyTiles() const { return Active().HasReadyTiles(); }
void GalleryStream::PrepareDetailOutput(mmltk::frameworks::gpu::ImageAllocation allocation) noexcept {
    if (terminal_.admission_open()) Active().PrepareDetailOutput(allocation);
}
void GalleryStream::PrepareOutputPublication(ExploreOutputChange change, ExploreMode mode) { Active().PrepareOutputPublication(change, mode); }
void GalleryStream::CommitOutputPublication() noexcept {
    if (terminal_.admission_open()) Active().CommitOutputPublication();
}
mmltk::frameworks::gpu::ImageWorkspaceCoverage GalleryStream::WorkspaceCoverage(const mmltk::frameworks::gpu::ImageWorkspaceObservation& output) {
    return Active().WorkspaceCoverage(output);
}
bool GalleryStream::RollbackOutputPublication() noexcept { return terminal_.admission_open() && Active().RollbackOutputPublication(); }
ExploreGalleryPublication GalleryStream::PublishTiles(const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                                      const std::uintptr_t stream) {
    return Active().PublishTiles(clean, semantic, stream);
}
void GalleryStream::RenderDetail(const ExploreRenderPlan& plan, std::shared_ptr<const data::CompiledDataset> store,
                                 const std::span<const std::uint32_t> annotated,
                                 const std::span<const explore::detail::ExploreRenderClassDescriptorAbi> classes,
                                 const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                 const std::uintptr_t stream) {
    Active().RenderDetail(plan, std::move(store), annotated, classes, clean, semantic, stream);
}
void GalleryStream::Quiesce() { Active().Quiesce(); }
void GalleryStream::Suspend() { Active().Suspend(); }
std::shared_ptr<const VisualDocument> GalleryStream::Document() const { return Active().Document(); }
std::vector<ExploreLabel> GalleryStream::Labels() const { return Active().Labels(); }
ExploreStorageFootprint GalleryStream::StorageFootprint() const { return Active().StorageFootprint(); }
void GalleryStream::ClearLogicalState() { Active().ClearLogicalState(); }
auto GalleryStream::ReleaseAfterRuntimeSettlement() noexcept -> mmltk::frameworks::gpu::SystemImageModel::Release {
    if (!terminal_.admission_open()) {
        impl_->Retire(terminal_.fact().first_failure);
        return impl_->TerminalRelease();
    }
    return Active().ReleaseAfterRuntimeSettlement();
}
auto GalleryStream::ResetBuffersChecked() noexcept -> mmltk::frameworks::gpu::SystemImageModel::Release {
    if (!terminal_.admission_open()) {
        impl_->Retire(terminal_.fact().first_failure);
        return impl_->TerminalRelease();
    }
    return Active().ResetBuffersChecked();
}
}  // namespace mmltk::controller::explore_detail
