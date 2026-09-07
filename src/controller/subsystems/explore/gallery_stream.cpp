#include "src/controller/subsystems/explore/detail/gallery_stream.h"
#include "src/backend/data/compiled_image_stream.h"
#include "src/backend/data/compiled_dataset.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/controller/subsystems/explore/native_explore_storage.h"
#include "src/backend/imaging/explore/explore_render_storage.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <vector>

#include "src/backend/data/compiled_format.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/backend/imaging/explore/detail/explore_mask_sample.h"
#include "src/backend/models/rfdetr/augmentation/spatial_erasure.h"

import mmltk.backend.imaging.explore.compiled_explore_store;
import mmltk.backend.imaging.explore.explore_render_core;
import mmltk.backend.models.rfdetr.augmentation.augmentation_metadata;


namespace mmltk::controller::explore_detail {

namespace data = mmltk::backend::data;
namespace explore = mmltk::backend::imaging::explore;
namespace rfdetr = mmltk::backend::models::rfdetr;

namespace {
struct DescriptorBudget final {
    std::size_t annotations = 0U;
    std::size_t runs = 0U;

    [[nodiscard]] bool Admit(const std::size_t added_annotations, const std::size_t added_runs) noexcept {
        if (added_annotations > explore::kExploreRenderAnnotationCapacity - annotations ||
            added_runs > explore::kExploreRenderRleCapacity - runs)
            return false;
        annotations += added_annotations;
        runs += added_runs;
        return true;
    }

    [[nodiscard]] bool AdmitDonor(const std::size_t source_annotations, const std::size_t source_runs,
                                  const std::size_t donor_runs) noexcept {
        // A render batch has its own staging ceiling; each image additionally
        // retains the bounded editable-document contract across copy/upscale.
        if (source_annotations >= contracts::kAnnotationObjectCapacity || source_runs > contracts::kAnnotationMaskRunCapacity ||
            donor_runs > contracts::kAnnotationMaskRunCapacity - source_runs)
            return false;
        return Admit(1U, donor_runs);
    }
};

[[nodiscard]] float normalized(const data::PackedCoordinate coordinate, const std::uint32_t extent) noexcept {
    return extent == 0U ? 0.0F : std::clamp(static_cast<float>(coordinate) / static_cast<float>(extent), 0.0F, 1.0F);
}

[[nodiscard]] explore::ExploreStorageStatus allocate_device(void*, void** destination, const std::size_t bytes) noexcept {
    return static_cast<explore::ExploreStorageStatus>(cudaMalloc(destination, std::max<std::size_t>(bytes, 1U)));
}
[[nodiscard]] explore::ExploreStorageStatus release_device(void*, void* allocation) noexcept {
    return static_cast<explore::ExploreStorageStatus>(cudaFree(allocation));
}
class ExploreHostAllocations final {
   public:
    [[nodiscard]] explore::ExploreCudaAllocationApi api() noexcept {
        return {.context = this,
                .allocate_device = allocate_device,
                .release_device = release_device,
                .allocate_pinned = Allocate,
                .release_pinned = Release};
    }

   private:
    static explore::ExploreStorageStatus Allocate(void* owner, void** destination, std::size_t bytes) noexcept {
        try {
            auto storage = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
            storage->ensure_bytes(std::max<std::size_t>(bytes, 1));
            auto* data = storage->data();
            static_cast<ExploreHostAllocations*>(owner)->allocations_.emplace(data, std::move(storage));
            *destination = data;
            return explore::kExploreStorageSuccess;
        } catch (...) { return static_cast<explore::ExploreStorageStatus>(cudaErrorMemoryAllocation); }
    }
    static explore::ExploreStorageStatus Release(void* owner, void* data) noexcept {
        auto& allocations = static_cast<ExploreHostAllocations*>(owner)->allocations_;
        const auto found = allocations.find(data);
        if (found == allocations.end()) return static_cast<explore::ExploreStorageStatus>(cudaErrorInvalidValue);
        if (found->second->ReleaseSettled() != CUDA_SUCCESS) return static_cast<explore::ExploreStorageStatus>(cudaErrorUnknown);
        allocations.erase(found);
        return explore::kExploreStorageSuccess;
    }
    std::unordered_map<void*, std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer>> allocations_;
};

[[nodiscard]] constexpr std::size_t align_up(const std::size_t value, const std::size_t alignment) noexcept {
    return (value + alignment - 1U) / alignment * alignment;
}

template <class Value>
[[nodiscard]] Value load_payload(const void* const payload, const std::size_t offset) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    Value value{};
    std::memcpy(&value, static_cast<const std::byte*>(payload) + offset, sizeof(Value));
    return value;
}

template <class Value>
void store_payload(void* const payload, const std::size_t offset, const Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    std::memcpy(static_cast<std::byte*>(payload) + offset, &value, sizeof(Value));
}

template <class Value, std::size_t Extent>
void store_payload(void* const payload, const std::size_t offset, const std::span<Value, Extent> values) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    if (!values.empty()) std::memcpy(static_cast<std::byte*>(payload) + offset, values.data(), values.size_bytes());
}

void pack_rle_mask(const std::span<const data::RLEPair> runs, const std::span<std::uint64_t> words,
                   const std::size_t pixel_count) noexcept {
    std::ranges::fill(words, 0U);
    for (const auto run : runs) {
        const std::size_t begin = std::min<std::size_t>(run.start, pixel_count);
        const std::size_t end = std::min<std::size_t>(begin + run.length, pixel_count);
        for (std::size_t word = begin / 64U; word <= (end == 0U ? 0U : (end - 1U) / 64U) && begin < end; ++word) {
            const auto word_begin = word * 64U;
            const auto low = std::max(begin, word_begin) - word_begin;
            const auto high = std::min(end, word_begin + 64U) - word_begin;
            const auto below_high = high == 64U ? ~std::uint64_t{0} : (std::uint64_t{1} << high) - 1U;
            const auto below_low = low == 0U ? 0U : (std::uint64_t{1} << low) - 1U;
            words[word] |= below_high & ~below_low;
        }
    }
}

[[nodiscard]] std::uint32_t diagnostic_coordinate(const float value) noexcept {
    return static_cast<std::uint32_t>(std::clamp(value, 0.0F, 1.0F) * 65535.0F);
}

}  // namespace

class GalleryStream::Impl final {
   public:
    Impl(std::size_t nproc, const mmltk::frameworks::gpu::DeviceExecution&, const ExploreNativeConfiguration&);
    ~Impl();
    [[nodiscard]] mmltk::common::concurrency::WorkerPool& workers() noexcept;
    void SetReadySink(ExploreAlgorithm::GalleryReadySink);
    void StopIngress() noexcept;
    [[nodiscard]] ExploreGalleryPublication Begin(const ExploreRenderPlan&, std::vector<std::uint32_t>, std::vector<std::uint32_t>,
                                                  std::shared_ptr<const mmltk::backend::data::CompiledDataset>, std::span<const std::uint32_t>,
                                                  std::span<const explore::ExploreRenderClassDescriptor>,
                                                  mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView,
                                                  std::uintptr_t);
    [[nodiscard]] ExploreGalleryPublication Advance();
    [[nodiscard]] bool HasReadyTiles() const;
    void PrepareOutputPublication();
    void CommitOutputPublication() noexcept;
    [[nodiscard]] bool RollbackOutputPublication() noexcept;
    [[nodiscard]] ExploreGalleryPublication PublishTiles(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView,
                                                         std::uintptr_t);
    void RenderDetail(const ExploreRenderPlan&, std::shared_ptr<const mmltk::backend::data::CompiledDataset>, std::span<const std::uint32_t>,
                      std::span<const explore::ExploreRenderClassDescriptor>, mmltk::frameworks::gpu::ImagePlaneView,
                      mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    void Quiesce();
    [[nodiscard]] std::shared_ptr<const VisualDocument> Document() const { return State().document; }
    [[nodiscard]] std::vector<ExploreLabel> Labels() const;
    void ClearLogicalState();
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release ReleaseAfterRuntimeSettlement() noexcept;
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release ResetBuffersChecked() noexcept;

   private:
    void ClearReadinessState();

    enum class LaneState : std::uint8_t {
        Idle,
        Preparing,
        Queued,
        Reading,
        AwaitingTransfer,
        InputReady,
        StaleReady,
        GpuPending,
        GpuComplete,
        Failed,
    };
    struct StorageSpan final {
        std::size_t offset = 0U;
        std::size_t count = 0U;
    };
    struct PayloadLayout final {
        // A lane payload's card offset begins a storage layout distinct from its batch descriptor.
        std::size_t card = 0U;
        StorageSpan annotations{};
        StorageSpan rle{};
        std::size_t tile = 0U;
        std::size_t donor_pixels = 0U;
        std::size_t donor_instance = 0U;
        // Donor payload offsets are lane-local storage facts, not assembled descriptor capacities.
        std::size_t donor_box = 0U;
        StorageSpan donor_rle{};
        std::size_t donor_mask = 0U;
        std::size_t bytes = 0U;
        std::size_t pixel_bytes = 0U;
        std::size_t donor_mask_words = 0U;
    };
    struct DescriptorLayout final {
        StorageSpan cards{};
        StorageSpan annotations{};
        StorageSpan rle{};
        StorageSpan classes{};
        StorageSpan tiles{};
        std::size_t bytes = 0U;
    };
    struct TileMeaning final {
        explore::ExploreRenderCardDescriptor card{};
        std::vector<explore::ExploreRenderAnnotationDescriptor> annotations;
        std::vector<explore::detail::ExploreRenderRlePairAbi> runs;
    };
    struct GalleryProductState final {
        std::shared_ptr<const mmltk::backend::data::CompiledDataset> store;
        ExploreViewport viewport{};
        ExploreOverlay overlay{};
        ExploreRenderPlan plan{};
        std::shared_ptr<const VisualDocument> document;
        std::shared_ptr<const TileMeaning> detail_meaning;
        explore::ExploreRenderDetailView detail_view{};
        std::vector<std::uint32_t> visible_indices;
        std::vector<std::uint32_t> prefetch_indices;
        std::span<const std::uint32_t> annotated_indices;
        std::vector<explore::ExploreRenderClassDescriptor> active_classes;
        std::vector<std::uint32_t> priority_slots;
        std::vector<bool> completed_slots;
        std::size_t cumulative_tiles = 0U;
        std::size_t reused_tiles = 0U;
        std::size_t cache_active = 0U;
        ExploreRenderPlan cached_plan{};
        bool cache_valid = false;
        std::vector<std::shared_ptr<const TileMeaning>> tile_meanings;

        [[nodiscard]] std::size_t Capacity() const noexcept {
            return visible_indices.capacity() + prefetch_indices.capacity() + active_classes.capacity() +
                   priority_slots.capacity() + completed_slots.capacity() + tile_meanings.capacity() +
                   overlay.class_selection.classes.capacity() + plan.overlay.class_selection.classes.capacity() +
                   cached_plan.overlay.class_selection.classes.capacity();
        }
        [[nodiscard]] std::size_t Size() const noexcept {
            return visible_indices.size() + prefetch_indices.size() + active_classes.size() +
                   priority_slots.size() + completed_slots.size() + tile_meanings.size() +
                   overlay.class_selection.classes.size() + plan.overlay.class_selection.classes.size() +
                   cached_plan.overlay.class_selection.classes.size() + annotated_indices.size() +
                   static_cast<std::size_t>(bool(store)) + static_cast<std::size_t>(bool(document)) +
                   static_cast<std::size_t>(bool(detail_meaning)) + static_cast<std::size_t>(cache_valid);
        }
        static void ClearPlan(ExploreRenderPlan& value) noexcept {
            auto classes = std::move(value.overlay.class_selection.classes);
            value = {};
            value.overlay.class_selection.classes = std::move(classes);
            value.overlay.class_selection.classes.clear();
        }
        void Clear() noexcept {
            store.reset();
            viewport = {};
            auto overlay_classes = std::move(overlay.class_selection.classes);
            overlay = {};
            overlay.class_selection.classes = std::move(overlay_classes);
            overlay.class_selection.classes.clear();
            ClearPlan(plan);
            document.reset();
            detail_meaning.reset();
            detail_view = {};
            visible_indices.clear();
            prefetch_indices.clear();
            annotated_indices = {};
            active_classes.clear();
            priority_slots.clear();
            completed_slots.clear();
            cumulative_tiles = 0U;
            reused_tiles = 0U;
            cache_active = 0U;
            ClearPlan(cached_plan);
            cache_valid = false;
            tile_meanings.clear();
        }
    };
    struct Lane final {
        Lane(const std::size_t lane_index, const data::CompiledImageStream::Buffer& storage) : pinned(storage), index(lane_index) {}
        const data::CompiledImageStream::Buffer& pinned;
        std::shared_ptr<const data::CompiledDataset> store;
        PayloadLayout layout{};
        std::size_t index = 0U;
        LaneState state = LaneState::Idle;
        std::uint64_t generation = 0U;
        std::uint64_t tile_generation = 0U;
        std::uint32_t compiled_index = 0U;
        std::uint32_t destination_slot = 0U;
        std::uint32_t card_extent = 0U;
        std::uint32_t columns = 1U;
        std::uint32_t first_row = 0U;
        std::uint64_t preview_key = 0U;
        bool prefetch = false;
        bool transfer_ready = false;
        std::uint32_t donor_index = 0U;
        std::optional<data::PackedInstance> donor_instance;
        std::exception_ptr failure{};
        std::shared_ptr<const TileMeaning> pending_meaning;
    };

    [[nodiscard]] ExploreGalleryPublication PublicationFacts(std::size_t) const;
    [[nodiscard]] PayloadLayout LayoutFor(std::uint32_t, bool, std::size_t) const;
    void PrepareLaneStorage(Lane&, std::uint32_t, std::uint32_t, std::uint64_t);
    [[nodiscard]] std::shared_ptr<const TileMeaning> CaptureMeaning(explore::ExploreRenderCardDescriptor,
                                                                   std::size_t, std::size_t, std::size_t, std::size_t) const;
    void StartIdleLanes();
    void Prioritize(std::optional<std::uint32_t>);
    void CacheTile(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uint32_t, std::uintptr_t);
    void RenderCachedSemantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    [[nodiscard]] bool BeginReadLane(std::size_t);
    void FinishReadLane(std::size_t, std::exception_ptr, bool) noexcept;
    void FinishTransfer(std::size_t, std::exception_ptr) noexcept;
    void PublishInput(Lane&);
    void SubmitRead(Lane&, bool observed);
    void AcceptanceDiagnostic(VisualDiagnosticOperation, const Lane&, std::uint64_t = 0U) const noexcept;
    void ReadLanePayload(Lane&);
    [[nodiscard]] const rfdetr::AugmentationBatchPlan* PrepareImages(std::span<Lane* const>, DescriptorBudget, cudaStream_t);
    [[nodiscard]] explore::ExploreRenderCardDescriptor AssembleImageMeaning(const Lane&, const rfdetr::AugmentationImagePlan*, const float*,
                                                                            std::size_t, std::size_t&, std::size_t&);
    void CompleteLane(std::size_t, std::exception_ptr) noexcept;
    void CompleteTiles(bool synchronized);
    void ReleaseLane(Lane&);
    [[nodiscard]] data::CompiledImageStream::CompletionObserver LaneCompletion() noexcept;
    void SeedPlaceholders(std::span<const explore::ExploreRenderClassDescriptor>, mmltk::frameworks::gpu::ImagePlaneView,
                          mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    void RenderAtlasBatch(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, std::uint32_t,
                          std::uint64_t);
    void RenderAtlasPlane(explore::ExploreRenderAtlasView, const explore::ExploreRenderTileBatchView&, ExploreOverlay,
                          mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, bool);
    void RenderDetailPlane(explore::ExploreRenderDetailView, ExploreOverlay, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, bool);
    void DiagnoseDescriptors(std::uint64_t, std::uint64_t, std::size_t, std::size_t, std::size_t) const;
    void DiagnoseRendered(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, std::uint64_t,
                          std::uint64_t, std::uint32_t, std::optional<std::size_t> = std::nullopt, std::uint32_t = 0U, std::uint32_t = 0U,
                          std::uint32_t = 0U, std::uint32_t = 0U);
    [[nodiscard]] explore::ExploreRenderScratchView Scratch() const;
    [[nodiscard]] explore::ExploreRenderSemanticView Semantics(ExploreOverlay, bool) const;
    void PrepareDescriptors(std::size_t, std::size_t, std::size_t, std::size_t, std::size_t);
    void UploadDescriptors(cudaStream_t, bool);
    void UploadDescriptor(explore::ExploreHighWaterBuffer&, std::size_t, std::size_t, cudaStream_t);
    void SettleDescriptors();
    static void EnsureBuffer(explore::ExploreHighWaterBuffer&, std::size_t, const char*);
    static void EnsureCuda(cudaError_t, const char*);
    static void Clear(mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    [[nodiscard]] static explore::ExploreRenderTargetView Target(mmltk::frameworks::gpu::ImagePlaneView);

    std::shared_ptr<ExploreAcceptanceGate> acceptance_;
    data::CompiledImageStream image_stream_;
    mutable std::mutex lanes_mutex_;
    std::vector<std::unique_ptr<Lane>> lanes_;
    std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> ready_sink_;
    std::atomic<std::uint64_t> desired_generation_{0U};
    std::atomic<std::size_t> stale_discarded_{0U};
    std::uint64_t next_tile_generation_ = 0U;
    cudaStream_t stream_ = nullptr;
    std::vector<const float*> batch_input_slots_, batch_donor_slots_;
    std::size_t next_prefetch_ = 0U;
    // CLEANUP-IGNORE: Typed gallery scheduling and augmentation buffers are not capture-session atomic counters.
    std::vector<bool> scheduled_slots_;
    std::vector<std::uint32_t> batch_indices_;
    std::vector<std::uint64_t> batch_keys_;
    std::vector<rfdetr::GpuAugmentationDonor> batch_donors_;
    std::vector<rfdetr::AugmentationPreviewAnnotation> projected_annotations_;
    std::size_t next_priority_ = 0U;
    // Ordinary aggregate assignment reuses vector high-water capacity. The inactive
    // value releases its logical contents after settlement; execution never
    // borrows a different artifact through a restored logical state.
    GalleryProductState committed_;
    GalleryProductState candidate_;
    [[nodiscard]] GalleryProductState& State() noexcept { return publication_active_ ? candidate_ : committed_; }
    [[nodiscard]] const GalleryProductState& State() const noexcept { return publication_active_ ? candidate_ : committed_; }
    bool publication_active_ = false;
    bool rollback_failed_ = false;
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
    std::unique_ptr<rfdetr::GpuAugmentationExecutor> augmenter_;
    int device_ = 0;
    std::uint32_t augmentation_width_ = 0U;
    std::uint32_t augmentation_height_ = 0U;
    DescriptorLayout descriptor_layout_{};
    // Only the GPU owner clears this after an explicit stream boundary; an
    // older lane callback may run while newer work is already queued.
    bool descriptors_pending_ = false;
    VisualDiagnosticSink diagnostics_{};
};

GalleryStream::Impl::Impl(const std::size_t nproc, const mmltk::frameworks::gpu::DeviceExecution& execution,
                             const ExploreNativeConfiguration& configuration)
    : acceptance_(configuration.acceptance),
      image_stream_(
          {.slots = nproc, .workers = nproc, .device = execution.device, .loading = configuration.loading, .execution = execution}),
      device_(execution.device),
      diagnostics_(configuration.diagnostics) {
    if (image_stream_.workers().size() != nproc)
        throw contracts::InvalidIntentError("Explore nproc exceeds the current Linux CPU affinity");
    lanes_.reserve(nproc);
    for (std::size_t index = 0U; index != nproc; ++index) {
        lanes_.push_back(std::make_unique<Lane>(index, image_stream_.metadata_storage(index)));
    }
    storage_.Bind(host_allocations_.api());
}

GalleryStream::Impl::~Impl() {
    StopIngress();
    try {
        Quiesce();
    } catch (...) {}
    // close joins both worker groups even if GPU settlement fails. Keep lane
    // observers and their mutex alive until those joins have completed.
    try {
        image_stream_.close();
    } catch (...) {}
}

mmltk::common::concurrency::WorkerPool& GalleryStream::Impl::workers() noexcept { return image_stream_.workers(); }

void GalleryStream::Impl::SetReadySink(ExploreAlgorithm::GalleryReadySink sink) {
    auto retained = sink ? std::make_shared<const ExploreAlgorithm::GalleryReadySink>(std::move(sink)) : nullptr;
    std::scoped_lock lock(lanes_mutex_);
    ready_sink_ = std::move(retained);
}

void GalleryStream::Impl::StopIngress() noexcept {
    desired_generation_.store(0U, std::memory_order_release);
    image_stream_.cancel_reads();
    if (acceptance_) acceptance_->Stop();
    {
        std::scoped_lock lock(lanes_mutex_);
        ready_sink_ = {};
    }
}

ExploreGalleryPublication GalleryStream::Impl::Begin(const ExploreRenderPlan& plan, std::vector<std::uint32_t> visible,
                                               // CLEANUP-IGNORE: Gallery scheduling and detail rendering accept
                                               // distinct operations despite sharing immutable render inputs.
                                               std::vector<std::uint32_t> prefetch, std::shared_ptr<const mmltk::backend::data::CompiledDataset> store,
                                               const std::span<const std::uint32_t> annotated_indices,
                                               const std::span<const explore::ExploreRenderClassDescriptor> classes,
                                               const mmltk::frameworks::gpu::ImagePlaneView clean,
                                               const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) {
    if (!publication_active_) throw std::logic_error("Explore gallery output was not prepared");
    if (State().store == store && State().cache_valid && plan.generation == State().cached_plan.generation && plan.viewport == State().viewport && visible == State().visible_indices) {
        State().plan = plan;
        State().cached_plan = plan;
        Prioritize(plan.focused_image);
        return PublicationFacts(0U);
    }
    image_stream_.cancel_reads();
    const auto side = explore_atlas_card_extent(plan.viewport);
    const bool reusable =
        State().cache_valid && State().store == store && State().cached_plan.dataset_identity == plan.dataset_identity && explore_atlas_card_extent(State().viewport) == side;
    // Detail rendering has its own current plan. Only the retained atlas plan
    // identifies the pixels and semantics covered by State().completed_slots.
    const bool same_content = reusable && State().cached_plan.augmentation.enabled == plan.augmentation.enabled &&
                              State().cached_plan.augmentation.seed == plan.augmentation.seed &&
                              State().cached_plan.augmentation_config == plan.augmentation_config;
    const bool same_overlay = State().overlay == plan.overlay;
    const auto& previous_completed = committed_.completed_slots;
    const auto& previous_meanings = committed_.tile_meanings;
    State().tile_meanings.assign(visible.size(), {});
    std::unordered_map<std::uint32_t, std::uint32_t> ready_slots;
    if (reusable)
        for (std::size_t slot = 0U; slot < State().visible_indices.size(); ++slot)
            if (previous_meanings[slot]) ready_slots.emplace(State().visible_indices[slot], static_cast<std::uint32_t>(slot));
    const auto previous_cache = State().cache_active;
    State().cache_active = 1U - State().cache_active;
    const auto tile_bytes = static_cast<std::size_t>(side) * side * 4U;
    const auto cache_bytes = std::max<std::size_t>(1U, visible.size() * tile_bytes);
    EnsureBuffer(storage_.buffers_.cached_clean_[State().cache_active], cache_bytes, "Explore clean tile cache allocation failed");
    EnsureBuffer(storage_.buffers_.cached_semantic_[State().cache_active], cache_bytes, "Explore semantic tile cache allocation failed");
    State().store = std::move(store);
    State().annotated_indices = annotated_indices;
    State().active_classes.assign(classes.begin(), classes.end());
    stream_ = reinterpret_cast<cudaStream_t>(stream);
    State().visible_indices = std::move(visible);
    State().prefetch_indices = std::move(prefetch);
    next_prefetch_ = 0U;
    State().viewport = plan.viewport;
    State().overlay = plan.overlay;
    State().plan = plan;
    if (acceptance_)
        acceptance_->ObserveProduct({.artifact = State().store, .logical_size = State().Size(),
                                     .capacity_before = State().Capacity(), .capacity_after = State().Capacity()});
    desired_generation_.store(plan.generation, std::memory_order_release);
    if (acceptance_) acceptance_->AdvanceGeneration(plan.generation);
    State().cumulative_tiles = 0U;
    State().reused_tiles = 0U;
    next_priority_ = 0U;
    State().completed_slots.assign(State().visible_indices.size(), false);
    scheduled_slots_.assign(State().visible_indices.size(), false);
    Prioritize(plan.focused_image);
    {
        std::scoped_lock lock(lanes_mutex_);
        for (auto& lane : lanes_) {
            if (same_content && lane->state == LaneState::InputReady) {
                const auto found = std::ranges::find(State().visible_indices, lane->compiled_index);
                if (found != State().visible_indices.end() && !ready_slots.contains(lane->compiled_index)) {
                    const auto slot = static_cast<std::uint32_t>(found - State().visible_indices.begin());
                    lane->generation = plan.generation;
                    lane->destination_slot = slot;
                    lane->prefetch = false;
                    const explore::ExploreRenderTileDescriptor tile{
                        .card_index = slot,
                        .destination_x = slot % State().viewport.columns * side,
                        .destination_y = slot / State().viewport.columns * side,
                        .destination_width = side,
                        .destination_height = side,
                        .generation = {.viewport = plan.generation, .tile = ++next_tile_generation_}};
                    store_payload(lane->pinned.data(), lane->layout.tile, tile);
                    scheduled_slots_[slot] = true;
                    continue;
                }
            }
            if (lane->state == LaneState::InputReady || lane->state == LaneState::Failed) {
                lane->state = LaneState::StaleReady;
            } else if (lane->state == LaneState::GpuComplete) {
                lane->state = LaneState::Idle;
                stale_discarded_.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }
    SeedPlaceholders(classes, clean, semantic, stream);
    for (std::size_t slot = 0U; slot < State().visible_indices.size(); ++slot) {
        const auto found = ready_slots.find(State().visible_indices[slot]);
        if (found == ready_slots.end()) continue;
        State().tile_meanings[slot] = previous_meanings[found->second];
        const auto copy = [&](const auto plane, const auto& cache) {
            const auto x = slot % State().viewport.columns * side;
            const auto y = slot / State().viewport.columns * side;
            EnsureCuda(cudaMemcpy2DAsync(reinterpret_cast<void*>(plane.data + y * plane.descriptor.pitch_bytes + x * 4U),
                                         plane.descriptor.pitch_bytes,
                                         static_cast<const std::byte*>(cache[previous_cache].data()) + found->second * tile_bytes,
                                         side * 4U, side * 4U, side, cudaMemcpyDeviceToDevice, reinterpret_cast<cudaStream_t>(stream)),
                       "Explore reused tile copy failed");
        };
        copy(clean, storage_.buffers_.cached_clean_);
        copy(semantic, storage_.buffers_.cached_semantic_);
        CacheTile(clean, semantic, static_cast<std::uint32_t>(slot), stream);
        State().completed_slots[slot] = same_content && previous_completed[found->second];
        scheduled_slots_[slot] = State().completed_slots[slot];
        if (State().completed_slots[slot]) {
            ++State().cumulative_tiles;
            ++State().reused_tiles;
        }
    }
    State().cached_plan = plan;
    State().cache_valid = true;
    // Opt-in content diagnostics project the retained semantic descriptors too,
    // so cache hits have the same current-slot evidence as newly read tiles.
    if ((!same_overlay || diagnostics_.valid()) && !ready_slots.empty()) {
        RenderCachedSemantics(clean, semantic, stream);
        for (std::size_t slot = 0U; slot < State().tile_meanings.size(); ++slot)
            if (State().tile_meanings[slot]) CacheTile(clean, semantic, static_cast<std::uint32_t>(slot), stream);
    }
    if (acceptance_ && diagnostics_.valid()) {
        for (std::size_t slot = 0U; slot != State().visible_indices.size(); ++slot)
            diagnostics_({.system = VisualSystemKind::Explore,
                          .operation = VisualDiagnosticOperation::AcceptancePlaceholderSlot,
                          .generation = plan.generation,
                          .value = slot,
                          .detail = State().visible_indices[slot],
                          .context = {.capacity_width = static_cast<std::uint32_t>(State().tile_meanings[slot] != nullptr)}});
        diagnostics_({.system = VisualSystemKind::Explore,
                      .operation = VisualDiagnosticOperation::AcceptancePlaceholderComplete,
                      .generation = plan.generation,
                      .value = State().visible_indices.size()});
    }
    auto publication = PublicationFacts(stale_discarded_.exchange(0U, std::memory_order_acq_rel));
    publication.reused_tiles = std::exchange(State().reused_tiles, 0U);
    return publication;
}

ExploreGalleryPublication GalleryStream::Impl::Advance() {
    std::exception_ptr failure;
    const auto generation = desired_generation_.load(std::memory_order_acquire);
    const auto diagnose_stage = [this, generation](const std::uint64_t stage) {
        if (acceptance_ && diagnostics_.valid())
            diagnostics_({.system = VisualSystemKind::Explore,
                          .operation = VisualDiagnosticOperation::ExploreContinuationStarted,
                          .device = device_,
                          .generation = generation,
                          .detail = stage});
    };
    diagnose_stage(10U);
    CompleteTiles(false);
    const auto settle = [&](auto&& observe_stale) {
        std::scoped_lock lock(lanes_mutex_);
        for (auto& lane : lanes_) {
            if (lane->state == LaneState::Failed) {
                failure = lane->failure;
                lane->state = LaneState::Idle;
                break;
            }
            if (lane->state == LaneState::StaleReady) {
                observe_stale(*lane);
                lane->state = LaneState::Idle;
                stale_discarded_.fetch_add(1U, std::memory_order_relaxed);
            } else if (lane->state == LaneState::GpuComplete && (!lane->pending_meaning || lane->generation != generation)) {
                lane->pending_meaning.reset();
                if (lane->generation != generation) stale_discarded_.fetch_add(1U, std::memory_order_relaxed);
                lane->state = LaneState::Idle;
            }
        }
    };
    if (acceptance_ && diagnostics_.valid()) {
        std::array<VisualDiagnosticFact, kExploreMaximumParallelism> discarded{};
        std::size_t discarded_count = 0U;
        settle([&](const Lane& lane) {
            discarded[discarded_count++] = {
                .system = VisualSystemKind::Explore,
                .operation = VisualDiagnosticOperation::AcceptanceStaleReadDiscarded,
                .generation = lane.generation,
                .value = lane.destination_slot,
                .detail = lane.compiled_index,
                .context = {.staging_bytes = lane.pinned.capacity_bytes()},
            };
        });
        for (const auto& fact : std::span{discarded}.first(discarded_count))
            diagnostics_(fact);
    } else {
        settle([](const Lane&) {});
    }
    diagnose_stage(11U);
    if (failure) std::rethrow_exception(failure);
    diagnose_stage(12U);
    StartIdleLanes();
    diagnose_stage(13U);
    auto publication = PublicationFacts(stale_discarded_.exchange(0U, std::memory_order_acq_rel));
    publication.reused_tiles = std::exchange(State().reused_tiles, 0U);
    diagnose_stage(14U);
    return publication;
}

bool GalleryStream::Impl::HasReadyTiles() const {
    const auto generation = desired_generation_.load(std::memory_order_acquire);
    std::scoped_lock lock(lanes_mutex_);
    if (std::ranges::any_of(lanes_,
                            [](const auto& lane) { return lane->state == LaneState::GpuPending || lane->state == LaneState::GpuComplete; }))
        return false;
    return std::ranges::any_of(lanes_, [generation](const auto& lane) {
        return lane->state == LaneState::InputReady && lane->generation == generation && !lane->prefetch;
    });
}

void GalleryStream::Impl::PrepareOutputPublication() {
    if (rollback_failed_) throw std::runtime_error("Explore publication rollback previously failed");
    if (publication_active_) return;
    SettleDescriptors();
    CompleteTiles(true);
    candidate_ = committed_;
    publication_active_ = true;
}

void GalleryStream::Impl::CommitOutputPublication() noexcept {
    if (!publication_active_) return;
    std::swap(committed_, candidate_);
    publication_active_ = false;
    ClearInactiveProduct();
}

bool GalleryStream::Impl::RollbackOutputPublication() noexcept {
    if (rollback_failed_) return false;
    if (!publication_active_) return true;
    try {
        Quiesce();
        publication_active_ = false;
        next_priority_ = 0U;
        next_prefetch_ = 0U;
        scheduled_slots_ = State().completed_slots;
        desired_generation_.store(State().plan.generation, std::memory_order_release);
        std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
        {
            std::scoped_lock lock(lanes_mutex_);
            sink = ready_sink_;
        }
        if (sink) (*sink)();
        ClearInactiveProduct();
        return true;
    } catch (...) {
        // Keep both artifact owners and all lane state alive for checked runtime
        // retirement. Failed settlement is not permission to clear borrowed data.
        rollback_failed_ = true;
        return false;
    }
}

void GalleryStream::Impl::CompleteTiles(const bool synchronized) {
    const auto generation = desired_generation_.load(std::memory_order_acquire);
    std::scoped_lock lock(lanes_mutex_);
    if (!synchronized && std::ranges::any_of(lanes_, [](const auto& lane) { return lane->state == LaneState::GpuPending; })) return;
    const auto observe = [this, generation](const std::size_t slot) {
        if (acceptance_ && diagnostics_.valid())
            diagnostics_({.system = VisualSystemKind::Explore,
                          .operation = VisualDiagnosticOperation::AcceptanceSlotPatched,
                          .generation = generation,
                          .value = slot,
                          .detail = State().visible_indices[slot]});
    };
    for (auto& lane : lanes_) {
        if (!lane->pending_meaning) continue;
        if (lane->generation == generation && lane->destination_slot < State().visible_indices.size() &&
            State().visible_indices[lane->destination_slot] == lane->compiled_index &&
            (lane->state == LaneState::GpuComplete || (synchronized && lane->state == LaneState::GpuPending))) {
            State().tile_meanings[lane->destination_slot] = std::move(lane->pending_meaning);
            if (!State().completed_slots[lane->destination_slot]) {
                State().completed_slots[lane->destination_slot] = true;
                ++State().cumulative_tiles;
                observe(lane->destination_slot);
            }
        }
    }
}

GalleryStream::Impl::PayloadLayout GalleryStream::Impl::LayoutFor(const std::uint32_t compiled_index, const bool has_donor,
                                                      const std::size_t donor_rle_count) const {
    const auto& header = State().store->header();
    PayloadLayout result;
    constexpr std::size_t channels_bytes = 3U * sizeof(float);
    if (header.image_height != 0U &&
        static_cast<std::size_t>(header.image_width) > std::numeric_limits<std::size_t>::max() / header.image_height)
        throw std::overflow_error("Explore thumbnail pixel count exceeds addressable storage");
    const auto pixels = static_cast<std::size_t>(header.image_width) * header.image_height;
    if (pixels > std::numeric_limits<std::size_t>::max() / channels_bytes)
        throw std::overflow_error("Explore thumbnail payload exceeds addressable storage");
    result.pixel_bytes = pixels * channels_bytes;
    const auto labels = State().store->image_labels(compiled_index);
    result.annotations.count = labels.size();
    if (result.annotations.count > explore::kExploreRenderAnnotationCapacity)
        throw contracts::BusyError("Explore thumbnail annotations exceed renderer capacity");
    for (const auto& label : labels) {
        const auto run_count = State().store->instance_rle(label).size();
        if (run_count > explore::kExploreRenderRleCapacity - result.rle.count)
            throw contracts::BusyError("Explore thumbnail annotations exceed renderer capacity");
        result.rle.count += run_count;
    }
    result.card = 0U;
    result.annotations.offset =
        align_up(result.card + sizeof(explore::ExploreRenderCardDescriptor), alignof(explore::ExploreRenderAnnotationDescriptor));
    result.rle.offset = align_up(result.annotations.offset + result.annotations.count * sizeof(explore::ExploreRenderAnnotationDescriptor),
                                 alignof(data::RLEPair));
    result.tile = align_up(result.rle.offset + result.rle.count * sizeof(data::RLEPair), alignof(explore::ExploreRenderTileDescriptor));
    result.donor_rle.count = donor_rle_count;
    result.donor_mask_words = has_donor ? (pixels + 63U) / 64U : 0U;
    result.donor_pixels = align_up(result.tile + sizeof(explore::ExploreRenderTileDescriptor), alignof(float));
    result.donor_instance = align_up(result.donor_pixels, alignof(data::PackedInstance));
    result.donor_box = align_up(result.donor_instance + (has_donor ? sizeof(data::PackedInstance) : 0U), alignof(float));
    result.donor_rle.offset = align_up(result.donor_box + (has_donor ? 4U * sizeof(float) : 0U), alignof(data::RLEPair));
    result.donor_mask = align_up(result.donor_rle.offset + donor_rle_count * sizeof(data::RLEPair), alignof(std::uint64_t));
    result.bytes = result.donor_mask + result.donor_mask_words * sizeof(std::uint64_t);
    return result;
}

void GalleryStream::Impl::PrepareLaneStorage(Lane& lane, const std::uint32_t compiled_index, const std::uint32_t slot,
                                       const std::uint64_t generation) {
    image_stream_.bind_current_context();
    lane.store = State().store;
    lane.transfer_ready = false;
    lane.pending_meaning.reset();
    lane.preview_key = rfdetr::augmentation_preview_image_key(State().plan.dataset_identity, State().plan.augmentation.seed, compiled_index);
    lane.donor_instance.reset();
    const bool copy_paste_requested = State().plan.augmentation.enabled && State().plan.augmentation_config.enabled &&
                                      rfdetr::augmentation_paste_admitted(State().plan.augmentation_config, lane.preview_key) &&
                                      !State().annotated_indices.empty();
    if (copy_paste_requested) {
        lane.donor_index = rfdetr::select_augmentation_preview_donor_image(State().annotated_indices, compiled_index, lane.preview_key);
        const auto donor_instances = State().store->image_labels(lane.donor_index);
        if (lane.donor_index != compiled_index && !donor_instances.empty())
            lane.donor_instance =
                donor_instances[rfdetr::select_augmentation_preview_donor_instance(donor_instances.size(), lane.preview_key)];
    }
    lane.layout =
        LayoutFor(compiled_index, lane.donor_instance.has_value(), lane.donor_instance ? lane.donor_instance->mask_rle_pairs : 0U);
    image_stream_.prepare_metadata(lane.index, lane.layout.bytes);
    image_stream_.prepare_images(lane.index, 2U * lane.layout.pixel_bytes);
    lane.generation = generation;
    lane.tile_generation = ++next_tile_generation_;
    lane.compiled_index = compiled_index;
    lane.destination_slot = slot;
    lane.card_extent = explore_atlas_card_extent(State().viewport);
    lane.columns = State().viewport.columns;
    lane.first_row = State().viewport.first_row;
    lane.failure = {};
    const explore::ExploreRenderTileDescriptor tile{
        .card_index = slot,
        .destination_x = slot % lane.columns * lane.card_extent,
        .destination_y = slot / lane.columns * lane.card_extent,
        .destination_width = lane.card_extent,
        .destination_height = lane.card_extent,
        .generation = {.viewport = generation, .tile = lane.tile_generation},
    };
    store_payload(lane.pinned.data(), lane.layout.tile, tile);
}

bool GalleryStream::Impl::BeginReadLane(const std::size_t lane_index) {
    Lane& lane = *lanes_[lane_index];
    {
        std::scoped_lock lock(lanes_mutex_);
        if (lane.state != LaneState::Queued || lane.generation != desired_generation_.load(std::memory_order_acquire)) return false;
        lane.state = LaneState::Reading;
        if (diagnostics_.valid())
            diagnostics_({.system = VisualSystemKind::Explore,
                          .operation = VisualDiagnosticOperation::GalleryReadStarted,
                          .device = device_,
                          .generation = lane.generation,
                          .value = lane.destination_slot,
                          .detail = lane.compiled_index});
    }
    if (acceptance_ && !lane.prefetch) {
        AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceLaneStarted, lane, lane.compiled_index);
        if (acceptance_->AwaitInitialRelease(lane.generation) == ExploreAcceptanceGate::WaitResult::Stale) {
            if (acceptance_->ClaimTerminalReport())
                AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceGateTerminal, lane, lane.compiled_index);
            return false;
        }
    }
    AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceCompiledRead, lane, lane.compiled_index);
    return true;
}

void GalleryStream::Impl::FinishReadLane(const std::size_t lane_index, std::exception_ptr failure, const bool read) noexcept {
    Lane& lane = *lanes_[lane_index];
    std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
    if (diagnostics_.valid()) {
        std::scoped_lock lock(lanes_mutex_);
        diagnostics_({.system = VisualSystemKind::Explore,
                      .operation = VisualDiagnosticOperation::GalleryReadCompleted,
                      .device = device_,
                      .generation = lane.generation,
                      .value = lane.destination_slot,
                      .detail = lane.compiled_index,
                      .context = {.capacity_width = read ? 1U : 0U, .capacity_height = failure ? 1U : 0U}});
    }
    try {
        if (failure) std::rethrow_exception(failure);
        if (!read) {
            {
                std::scoped_lock lock(lanes_mutex_);
                lane.state = lane.transfer_ready ? LaneState::StaleReady : LaneState::AwaitingTransfer;
                sink = ready_sink_;
            }
            if (sink) (*sink)();
            return;
        }
        ReadLanePayload(lane);
        AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceCompiledReadCompleted, lane, lane.compiled_index);
        if (acceptance_ && !lane.prefetch && lane.first_row != 0U && acceptance_->ClaimHeldCompletion()) {
            AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceCompletionHeld, lane, lane.compiled_index);
            const auto released = acceptance_->AwaitHeldCompletion();
            if (released == ExploreAcceptanceGate::WaitResult::Proceed)
                AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceCompletionReleased, lane, lane.compiled_index);
            else if (acceptance_->ClaimTerminalReport())
                AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceGateTerminal, lane, lane.compiled_index);
            if (released == ExploreAcceptanceGate::WaitResult::Stale) {
                {
                    std::scoped_lock lock(lanes_mutex_);
                    lane.state = lane.transfer_ready ? LaneState::StaleReady : LaneState::AwaitingTransfer;
                    sink = ready_sink_;
                }
                if (sink) (*sink)();
                return;
            }
        }
        AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceReadyStateStarted, lane, lane.compiled_index);
        {
            std::scoped_lock lock(lanes_mutex_);
            PublishInput(lane);
            sink = ready_sink_;
        }
        AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceReadyStateCompleted, lane, lane.compiled_index);
    } catch (...) {
        {
            std::scoped_lock lock(lanes_mutex_);
            lane.failure = std::current_exception();
            lane.state = !lane.transfer_ready                                                     ? LaneState::AwaitingTransfer
                         : lane.generation == desired_generation_.load(std::memory_order_acquire) ? LaneState::Failed
                                                                                                  : LaneState::StaleReady;
            sink = ready_sink_;
        }
    }
    if (sink) {
        AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceReadySinkStarted, lane, lane.compiled_index);
        (*sink)();
        AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceReadySinkCompleted, lane, lane.compiled_index);
    }
}

void GalleryStream::Impl::PublishInput(Lane& lane) {
    if (!lane.transfer_ready)
        lane.state = LaneState::AwaitingTransfer;
    else if (lane.generation != desired_generation_.load(std::memory_order_acquire))
        lane.state = LaneState::StaleReady;
    else
        lane.state = lane.failure ? LaneState::Failed : LaneState::InputReady;
    if (lane.prefetch && lane.state == LaneState::InputReady && diagnostics_.valid())
        diagnostics_(
            {.system = VisualSystemKind::Explore,
             .operation = VisualDiagnosticOperation::ExplorePrefetchReady,
             .generation = lane.generation,
             .value = lane.compiled_index,
             .detail = lanes_.size(),
             .context = {.staging_bytes = lane.pinned.capacity_bytes() + image_stream_.host_storage(lane.index).capacity_bytes()}});
}

void GalleryStream::Impl::FinishTransfer(std::size_t index, std::exception_ptr failure) noexcept {
    std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
    {
        std::scoped_lock lock(lanes_mutex_);
        auto& lane = *lanes_[index];
        lane.transfer_ready = true;
        if (failure) lane.failure = failure;
        if (diagnostics_.valid())
            diagnostics_({.system = VisualSystemKind::Explore,
                          .operation = VisualDiagnosticOperation::GalleryTransferCompleted,
                          .device = device_,
                          .generation = lane.generation,
                          .value = lane.destination_slot,
                          .detail = lane.compiled_index,
                          .context = {.capacity_height = failure ? 1U : 0U}});
        if (lane.state == LaneState::AwaitingTransfer) {
            PublishInput(lane);
            sink = ready_sink_;
        }
    }
    if (sink) (*sink)();
}

void GalleryStream::Impl::AcceptanceDiagnostic(const VisualDiagnosticOperation operation, const Lane& lane,
                                         const std::uint64_t detail) const noexcept {
    if (!acceptance_ || !diagnostics_.valid()) return;
    std::scoped_lock lock(lanes_mutex_);
    if (lane.prefetch) return;
    diagnostics_({.system = VisualSystemKind::Explore,
                  .operation = operation,
                  .generation = lane.generation,
                  .value = lane.destination_slot,
                  .detail = detail,
                  .context = {.staging_bytes = lane.pinned.capacity_bytes()}});
}

void GalleryStream::Impl::ReadLanePayload(Lane& lane) {
    const auto labels = lane.store->image_labels(lane.compiled_index);
    std::size_t rle_cursor = 0U;
    for (std::size_t label_index = 0U; label_index != labels.size(); ++label_index) {
        const auto& label = labels[label_index];
        const auto runs = lane.store->instance_rle(label);
        store_payload(lane.pinned.data(), lane.layout.rle.offset + rle_cursor * sizeof(data::RLEPair), runs);
        const explore::ExploreRenderAnnotationDescriptor annotation{
            .rle_offset = static_cast<std::uint32_t>(rle_cursor),
            .rle_count = static_cast<std::uint32_t>(runs.size()),
            .card_index = lane.destination_slot,
            .class_id = label.class_id,
        };
        store_payload(lane.pinned.data(), lane.layout.annotations.offset + label_index * sizeof(explore::ExploreRenderAnnotationDescriptor),
                      annotation);
        rle_cursor += runs.size();
    }
    const auto image = explore::make_explore_contain_rect(lane.store->header().image_width, lane.store->header().image_height, lane.card_extent);
    const explore::ExploreRenderCardDescriptor card{
        .source_width = lane.store->header().image_width,
        .source_height = lane.store->header().image_height,
        .image_x = image.x,
        .image_y = image.y,
        .image_width = image.width,
        .image_height = image.height,
        .annotation_count = static_cast<std::uint32_t>(labels.size()),
        .compiled_index = lane.compiled_index,
    };
    store_payload(lane.pinned.data(), lane.layout.card, card);
    if (lane.donor_instance) {
        store_payload(lane.pinned.data(), lane.layout.donor_instance, *lane.donor_instance);
        const std::array donor_box{
            normalized(lane.donor_instance->bbox_x1, lane.store->header().image_width),
            normalized(lane.donor_instance->bbox_y1, lane.store->header().image_height),
            normalized(lane.donor_instance->bbox_x2, lane.store->header().image_width),
            normalized(lane.donor_instance->bbox_y2, lane.store->header().image_height),
        };
        store_payload(lane.pinned.data(), lane.layout.donor_box, std::span{donor_box});
        const auto donor_runs = lane.store->instance_rle(*lane.donor_instance);
        store_payload(lane.pinned.data(), lane.layout.donor_rle.offset, donor_runs);
        auto* const donor_mask = reinterpret_cast<std::uint64_t*>(static_cast<std::byte*>(lane.pinned.data()) + lane.layout.donor_mask);
        pack_rle_mask(donor_runs, std::span{donor_mask, lane.layout.donor_mask_words},
                      static_cast<std::size_t>(lane.store->header().image_width) * lane.store->header().image_height);
    }
}

void GalleryStream::Impl::CompleteLane(const std::size_t lane_index, std::exception_ptr failure) noexcept {
    std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
    VisualDiagnosticFact fact;
    const bool observed = diagnostics_.valid();
    {
        std::scoped_lock lock(lanes_mutex_);
        Lane& lane = *lanes_[lane_index];
        if (failure) {
            lane.failure = failure;
            lane.state = LaneState::Failed;
        } else if (lane.state == LaneState::GpuPending) {
            lane.state = LaneState::GpuComplete;
        }
        if (observed)
            fact = {.system = VisualSystemKind::Explore,
                    .operation = failure ? VisualDiagnosticOperation::GalleryGpuFailed : VisualDiagnosticOperation::GalleryGpuCompleted,
                    .device = device_,
                    .generation = lane.generation,
                    .value = lane.destination_slot,
                    .detail = lane.compiled_index};
        sink = ready_sink_;
    }
    if (observed) diagnostics_(fact);
    if (sink) (*sink)();
}

data::CompiledImageStream::CompletionObserver GalleryStream::Impl::LaneCompletion() noexcept {
    return {.context = this, .complete = [](void* context, std::size_t index, std::exception_ptr failure) noexcept {
                static_cast<Impl*>(context)->CompleteLane(index, failure);
            }};
}

void GalleryStream::Impl::ReleaseLane(Lane& lane) { image_stream_.release(lane.index, stream_, LaneCompletion()); }

void GalleryStream::Impl::SubmitRead(Lane& lane, const bool observed) {
    const std::array reads{data::CompiledImageRead{lane.compiled_index, 0U},
                           data::CompiledImageRead{lane.donor_index, lane.layout.pixel_bytes}};
    data::CompiledImageStream::ReadObserver observer;
    if (observed)
        observer = {.context = this,
                    .before = [](void* context, std::size_t index) { return static_cast<Impl*>(context)->BeginReadLane(index); },
                    .complete = [](void* context, std::size_t index, std::exception_ptr failure,
                                   bool read) noexcept { static_cast<Impl*>(context)->FinishReadLane(index, failure, read); }};
    image_stream_.submit(lane.index, *lane.store, std::span{reads}.first(lane.donor_instance ? 2U : 1U), observer,
                         {.context = this, .complete = [](void* context, std::size_t index, std::exception_ptr error) noexcept {
                              static_cast<Impl*>(context)->FinishTransfer(index, error);
                          }});
}

ExploreGalleryPublication GalleryStream::Impl::PublicationFacts(const std::size_t stale) const {
    std::size_t pinned = 0U;
    {
        std::scoped_lock lock(lanes_mutex_);
        for (const auto& lane : lanes_)
            if (lane->state != LaneState::Idle)
                pinned += lane->pinned.capacity_bytes() + image_stream_.host_storage(lane->index).capacity_bytes();
    }
    return {
        .generation = desired_generation_.load(std::memory_order_acquire),
        .ready_slots = State().completed_slots,
        .cumulative_tiles = State().cumulative_tiles,
        .remaining_tiles = State().visible_indices.size() - std::min(State().visible_indices.size(), State().cumulative_tiles),
        .active_pinned_bytes = pinned,
        .stale_discarded = stale,
    };
}

void GalleryStream::Impl::Prioritize(const std::optional<std::uint32_t> focused_image) {
    next_priority_ = 0U;
    State().priority_slots.clear();
    State().priority_slots.reserve(State().visible_indices.size());
    if (focused_image) {
        const auto focused = std::ranges::find(State().visible_indices, *focused_image);
        if (focused != State().visible_indices.end()) State().priority_slots.push_back(static_cast<std::uint32_t>(focused - State().visible_indices.begin()));
    }
    for (std::size_t slot = 0U; slot != State().visible_indices.size(); ++slot)
        if (State().priority_slots.empty() || slot != State().priority_slots.front()) State().priority_slots.push_back(static_cast<std::uint32_t>(slot));
}

void GalleryStream::Impl::StartIdleLanes() {
    const auto generation = desired_generation_.load(std::memory_order_acquire);
    if (generation == 0U) return;
    // Rollback quiesces candidate ingress. Rebind the acceptance gate to the
    // restored stream before resuming unfinished reads.
    if (acceptance_) acceptance_->AdvanceGeneration(generation);
    for (std::size_t lane_index = 0U; lane_index != lanes_.size(); ++lane_index) {
        std::uint32_t slot = 0U;
        std::uint32_t compiled_index = 0U;
        bool prefetch = false;
        {
            std::scoped_lock lock(lanes_mutex_);
            while (next_priority_ < State().priority_slots.size() &&
                   (State().completed_slots[State().priority_slots[next_priority_]] || scheduled_slots_[State().priority_slots[next_priority_]]))
                ++next_priority_;
            if (next_priority_ == State().priority_slots.size() && next_prefetch_ == State().prefetch_indices.size()) return;
            Lane& lane = *lanes_[lane_index];
            if (lane.state != LaneState::Idle) continue;
            prefetch = next_priority_ == State().priority_slots.size();
            if (prefetch)
                compiled_index = State().prefetch_indices[next_prefetch_++];
            else {
                slot = State().priority_slots[next_priority_++];
                compiled_index = State().visible_indices[slot];
                scheduled_slots_[slot] = true;
            }
            lane.state = LaneState::Preparing;
        }
        Lane& lane = *lanes_[lane_index];
        try {
            PrepareLaneStorage(lane, compiled_index, slot, generation);
            lane.prefetch = prefetch;
            {
                std::scoped_lock lock(lanes_mutex_);
                lane.state = LaneState::Queued;
            }
            SubmitRead(lane, true);
        } catch (...) {
            std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
            {
                std::scoped_lock lock(lanes_mutex_);
                lane.failure = std::current_exception();
                lane.state = LaneState::Failed;
                sink = ready_sink_;
            }
            if (sink) (*sink)();
            throw;
        }
    }
}

void GalleryStream::Impl::CacheTile(const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                              const std::uint32_t slot, const std::uintptr_t stream) {
    const auto side = explore_atlas_card_extent(State().viewport);
    const auto tile_bytes = static_cast<std::size_t>(side) * side * 4U;
    if (publication_active_ && State().cache_active == committed_.cache_active) {
        const auto source = committed_.cache_active;
        const auto destination = 1U - source;
        const auto bytes = State().visible_indices.size() * tile_bytes;
        // A progressive patch must not overwrite the incumbent cache. Reuse
        // the other high-water allocation and retain exact untouched tiles.
        for (auto* cache : {&storage_.buffers_.cached_clean_, &storage_.buffers_.cached_semantic_}) {
            EnsureBuffer((*cache)[destination], std::max<std::size_t>(bytes, 1U), "Explore candidate tile cache allocation failed");
            if (bytes != 0U)
                EnsureCuda(cudaMemcpyAsync((*cache)[destination].data(), (*cache)[source].data(), bytes, cudaMemcpyDeviceToDevice,
                                           reinterpret_cast<cudaStream_t>(stream)),
                           "Explore candidate tile cache copy failed");
        }
        State().cache_active = destination;
    }
    const auto x = slot % State().viewport.columns * side;
    const auto y = slot / State().viewport.columns * side;
    const auto copy = [&](const auto plane, auto& buffer) {
        EnsureCuda(cudaMemcpy2DAsync(static_cast<std::byte*>(buffer.data()) + slot * tile_bytes, side * 4U,
                                     reinterpret_cast<const void*>(plane.data + y * plane.descriptor.pitch_bytes + x * 4U),
                                     plane.descriptor.pitch_bytes, side * 4U, side, cudaMemcpyDeviceToDevice,
                                     reinterpret_cast<cudaStream_t>(stream)),
                   "Explore tile cache copy failed");
    };
    copy(clean, storage_.buffers_.cached_clean_[State().cache_active]);
    copy(semantic, storage_.buffers_.cached_semantic_[State().cache_active]);
}

void GalleryStream::Impl::RenderCachedSemantics(const mmltk::frameworks::gpu::ImagePlaneView clean,
                                          const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t stream) {
    std::size_t cursor = 0U;
    const auto side = explore_atlas_card_extent(State().viewport);
    while (cursor < State().tile_meanings.size()) {
        std::vector<std::uint32_t> slots;
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
        PrepareDescriptors(slots.size(), annotation_count, run_count, State().active_classes.size(), slots.size());
        std::size_t annotation_offset = 0U;
        std::size_t run_offset = 0U;
        for (std::size_t index = 0U; index < slots.size(); ++index) {
            const auto slot = slots[index];
            const auto& meaning = *State().tile_meanings[slot];
            auto card = meaning.card;
            card.annotation_offset = static_cast<std::uint32_t>(annotation_offset);
            store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.cards.offset + index * sizeof(card), card);
            for (auto annotation : meaning.annotations) {
                annotation.rle_offset += static_cast<std::uint32_t>(run_offset);
                annotation.card_index = static_cast<std::uint32_t>(index);
                if (annotation.occluder_index >= 0) annotation.occluder_index += static_cast<std::int32_t>(card.annotation_offset);
                store_payload(storage_.buffers_.descriptors_.data(),
                              descriptor_layout_.annotations.offset + annotation_offset++ * sizeof(annotation), annotation);
            }
            store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.rle.offset + run_offset * sizeof(data::RLEPair),
                          std::span{meaning.runs});
            run_offset += meaning.runs.size();
            const explore::ExploreRenderTileDescriptor tile{.card_index = static_cast<std::uint32_t>(index),
                                                            .destination_x = slot % State().viewport.columns * side,
                                                            .destination_y = slot / State().viewport.columns * side,
                                                            .destination_width = side,
                                                            .destination_height = side,
                                                            .generation = {.viewport = State().plan.generation, .tile = ++next_tile_generation_}};
            store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.tiles.offset + index * sizeof(tile), tile);
        }
        store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.classes.offset, std::span{State().active_classes});
        UploadDescriptors(reinterpret_cast<cudaStream_t>(stream), true);
        RenderAtlasPlane({.card_extent = side,
                          .card_count = static_cast<std::uint32_t>(slots.size()),
                          .source_width = State().store->header().image_width,
                          .source_height = State().store->header().image_height},
                         {.tile_count = static_cast<std::uint32_t>(slots.size()),
                          .tile_capacity = static_cast<std::uint32_t>(slots.size()),
                          .max_tile_width = side,
                          .max_tile_height = side,
                          .viewport_generation = State().plan.generation},
                         State().overlay, target, stream, true);
        descriptors_pending_ = true;
        if (diagnostics_.valid())
            for (std::size_t index = 0U; index < slots.size(); ++index) {
                const auto slot = slots[index];
                const auto& meaning = *State().tile_meanings[slot];
                DiagnoseDescriptors(State().plan.generation, State().visible_indices[slot], slot, meaning.annotations.size(), meaning.runs.size());
                DiagnoseRendered(clean, target, stream, State().plan.generation, slot, State().visible_indices[slot], index,
                                 slot % State().viewport.columns * side, slot / State().viewport.columns * side, side, side);
            }
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

const rfdetr::AugmentationBatchPlan* GalleryStream::Impl::PrepareImages(const std::span<Lane* const> ready_lanes,
                                                                  const DescriptorBudget source_budget, const cudaStream_t cuda_stream) {
    if (ready_lanes.empty() || ready_lanes.size() > lanes_.size()) throw std::logic_error("Explore prepared image batch is invalid");
    const auto pixel_bytes = ready_lanes.front()->layout.pixel_bytes;
    batch_input_slots_.clear();
    batch_donor_slots_.clear();
    batch_input_slots_.reserve(lanes_.size());
    batch_donor_slots_.reserve(lanes_.size());
    const bool preview_active = State().plan.augmentation.enabled && State().plan.augmentation_config.enabled;
    if (preview_active)
        EnsureBuffer(storage_.buffers_.augmented_batch_, lanes_.size() * pixel_bytes,
                     "Explore augmented batch high-water allocation failed");
    batch_indices_.clear();
    batch_keys_.clear();
    if (preview_active) {
        batch_indices_.reserve(lanes_.size());
        batch_keys_.reserve(lanes_.size());
    }
    std::size_t projection_capacity = 0U;
    for (std::size_t slot = 0U; slot != ready_lanes.size(); ++slot) {
        const Lane& lane = *ready_lanes[slot];
        projection_capacity = std::max(projection_capacity, lane.layout.annotations.count + 1U);
        image_stream_.handoff(lane.index, cuda_stream);
        const auto* pixels = static_cast<const float*>(image_stream_.device_storage(lane.index).data());
        batch_input_slots_.push_back(pixels);
        batch_donor_slots_.push_back(pixels + pixel_bytes / sizeof(float));
        if (preview_active) {
            batch_indices_.push_back(lane.compiled_index);
            batch_keys_.push_back(lane.preview_key);
        }
    }
    projected_annotations_.reserve(projection_capacity);
    auto augmentation_config = State().plan.augmentation_config;
    augmentation_config.enabled = preview_active;
    if (State().annotated_indices.empty()) augmentation_config.copy_paste_probability = 0.0F;
    if (preview_active &&
        (!augmenter_ || augmentation_width_ != State().store->header().image_width || augmentation_height_ != State().store->header().image_height)) {
        augmenter_ = std::make_unique<rfdetr::GpuAugmentationExecutor>(augmentation_config, lanes_.size(),
                                                                       static_cast<int>(State().store->header().image_height),
                                                                       static_cast<int>(State().store->header().image_width), device_);
        augmentation_width_ = State().store->header().image_width;
        augmentation_height_ = State().store->header().image_height;
    } else if (preview_active) {
        augmenter_->Reconfigure(augmentation_config);
    }
    batch_donors_.clear();
    auto donor_budget = source_budget;
    rfdetr::GpuAugmentationDonorBatchView donor_view;
    if (preview_active && augmenter_->copy_paste_enabled()) {
        batch_donors_.reserve(lanes_.size());
        batch_donors_.resize(ready_lanes.size());
        const auto donor_box_bytes = ready_lanes.size() * 4U * sizeof(float);
        const auto pixels_per_image = static_cast<std::size_t>(State().store->header().image_width) * State().store->header().image_height;
        const auto mask_words = (pixels_per_image + 63U) / 64U;
        const auto donor_mask_bytes = ready_lanes.size() * mask_words * sizeof(std::uint64_t);
        bool donor_storage_ready = false;
        for (std::size_t slot = 0U; slot != ready_lanes.size(); ++slot) {
            const Lane& lane = *ready_lanes[slot];
            // Aligned donors are optional. Reserve their complete semantic
            // footprint before either image paste or mask transformation.
            if (!lane.donor_instance ||
                !donor_budget.AdmitDonor(lane.layout.annotations.count, lane.layout.rle.count, lane.layout.donor_rle.count))
                continue;
            if (lane.layout.donor_mask_words != mask_words) throw std::logic_error("Explore prepared donor payload is unavailable");
            if (!donor_storage_ready) {
                EnsureBuffer(storage_.buffers_.donor_boxes_device_, donor_box_bytes, "Explore donor box high-water allocation failed");
                EnsureBuffer(storage_.buffers_.donor_masks_device_, donor_mask_bytes, "Explore donor mask high-water allocation failed");
                donor_storage_ready = true;
            }
            const auto label = load_payload<data::PackedInstance>(lane.pinned.data(), lane.layout.donor_instance);
            auto& donor = batch_donors_[slot];
            donor.label = label.class_id;
            donor.dataset_index = lane.donor_index;
            donor.area = static_cast<float>((label.bbox_x2 - label.bbox_x1) * (label.bbox_y2 - label.bbox_y1));
            std::memcpy(donor.box.data(), static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_box, 4U * sizeof(float));
            donor.has_mask = lane.layout.donor_rle.count != 0U;

            EnsureCuda(cudaMemcpyAsync(static_cast<float*>(storage_.buffers_.donor_boxes_device_.data()) + slot * 4U,
                                       static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_box, 4U * sizeof(float),
                                       cudaMemcpyHostToDevice, cuda_stream),
                       "Explore prepared donor box upload failed");
            EnsureCuda(cudaMemcpyAsync(static_cast<std::uint64_t*>(storage_.buffers_.donor_masks_device_.data()) + slot * mask_words,
                                       static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_mask,
                                       mask_words * sizeof(std::uint64_t), cudaMemcpyHostToDevice, cuda_stream),
                       "Explore prepared donor mask upload failed");
        }
        donor_view = {.images = nullptr,
                      .masks = static_cast<const std::int64_t*>(storage_.buffers_.donor_masks_device_.data()),
                      .boxes = static_cast<const float*>(storage_.buffers_.donor_boxes_device_.data()),
                      .mask_words = static_cast<std::int64_t>(mask_words),
                      .image_slots = batch_donor_slots_};
    }
    const rfdetr::AugmentationBatchPlan* augmentation_plan = nullptr;
    if (preview_active)
        augmentation_plan = &augmenter_->Run({.input = nullptr,
                                              .output = static_cast<float*>(storage_.buffers_.augmented_batch_.data()),
                                              .image_indices = batch_indices_,
                                              .height = static_cast<int>(State().store->header().image_height),
                                              .width = static_cast<int>(State().store->header().image_width),
                                              .output_domain = rfdetr::GpuAugmentationOutputDomain::UnitRgb,
                                              .input_slots = batch_input_slots_},
                                             batch_keys_, batch_donors_, donor_view, cuda_stream, State().plan.augmentation.seed % 2U);
    return augmentation_plan;
}

explore::ExploreRenderCardDescriptor GalleryStream::Impl::AssembleImageMeaning(const Lane& lane, const rfdetr::AugmentationImagePlan* image_plan,
                                                                         const float* pixels, const std::size_t card_index,
                                                                         std::size_t& annotation_count, std::size_t& rle_count) {
    const auto source_instances = State().store->image_labels(lane.compiled_index);
    const bool has_paste = image_plan != nullptr && image_plan->paste_donor_slot >= 0;
    const auto donor =
        has_paste ? std::optional{load_payload<data::PackedInstance>(lane.pinned.data(), lane.layout.donor_instance)} : std::nullopt;
    auto support_plan = image_plan != nullptr ? *image_plan : rfdetr::AugmentationImagePlan{};
    if (has_paste) {
        support_plan.paste_support =
            reinterpret_cast<const data::RLEPair*>(static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_rle.offset);
        support_plan.paste_support_count = lane.layout.donor_rle.count;
    }
    rfdetr::build_augmentation_preview_annotations(
        source_instances, donor ? &*donor : nullptr, image_plan != nullptr ? &support_plan : nullptr,
        static_cast<int>(State().store->header().image_width), static_cast<int>(State().store->header().image_height), projected_annotations_,
        State().store->rle_pairs());
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
                         lane.pinned.data(),
                         lane.layout.annotations.offset + projected.source_ordinal * sizeof(explore::ExploreRenderAnnotationDescriptor))
                   : explore::ExploreRenderAnnotationDescriptor{.rle_offset = static_cast<std::uint32_t>(lane.layout.rle.count),
                                                                .rle_count = static_cast<std::uint32_t>(lane.layout.donor_rle.count)};
        std::copy(projected.box_xyxy.begin(), projected.box_xyxy.end(), std::begin(annotation.box_xyxy));
        std::copy(projected.inverse.begin(), projected.inverse.end(), std::begin(annotation.inverse));
        annotation.class_id = projected.class_id;
        annotation.card_index = static_cast<std::uint32_t>(card_index);
        annotation.rle_offset += static_cast<std::uint32_t>(rle_count);
        annotation.occluder_index =
            projected.occluder_index < 0 ? -1 : projected.occluder_index + static_cast<std::int32_t>(card.annotation_offset);
        store_payload(storage_.buffers_.descriptors_.data(),
                      descriptor_layout_.annotations.offset + annotation_count++ * sizeof(annotation), annotation);
    }
    const auto append_runs = [&](const std::size_t offset, const std::size_t count) {
        if (count != 0U)
            std::memcpy(static_cast<std::byte*>(storage_.buffers_.descriptors_.data()) + descriptor_layout_.rle.offset +
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
    const auto generation = desired_generation_.load(std::memory_order_acquire);
    std::array<Lane*, kExploreMaximumParallelism> ready{};
    std::size_t ready_count = 0U;
    DescriptorBudget source_budget;
    {
        std::scoped_lock lock(lanes_mutex_);
        for (auto& lane : lanes_)
            if (lane->state == LaneState::InputReady && lane->generation == generation && !lane->prefetch &&
                source_budget.Admit(lane->layout.annotations.count, lane->layout.rle.count))
                ready[ready_count++] = lane.get();
    }
    const std::span ready_lanes{ready.data(), ready_count};
    if (ready_lanes.empty()) return PublicationFacts(0U);
    auto* const cuda_stream = reinterpret_cast<cudaStream_t>(stream);
    const auto pixel_bytes = ready_lanes.front()->layout.pixel_bytes;
    const auto* augmentation_plan = PrepareImages(ready_lanes, source_budget, cuda_stream);
    const bool preview_active = augmentation_plan != nullptr;
    if (preview_active && diagnostics_.valid()) {
        const auto valid_donors = static_cast<std::uint32_t>(
            std::ranges::count_if(std::views::iota(std::size_t{0U}, batch_donors_.size()), [&](const std::size_t slot) {
                return batch_donors_[slot].label >= 0 && batch_donors_[slot].dataset_index != batch_indices_[slot] &&
                       batch_donors_[slot].box[0] < batch_donors_[slot].box[2] && batch_donors_[slot].box[1] < batch_donors_[slot].box[3];
            }));
        std::size_t image_capacity = 0U;
        for (const auto& lane : lanes_)
            image_capacity += image_stream_.device_storage(lane->index).capacity_bytes();
        const auto planned_pastes =
            static_cast<std::uint32_t>(std::ranges::count_if(augmentation_plan->images | std::views::take(augmentation_plan->active_size),
                                                             [](const auto& image) { return image.paste_donor_slot >= 0; }));
        diagnostics_({.system = VisualSystemKind::Explore,
                      .operation = VisualDiagnosticOperation::ExploreAugmentationBatchPrepared,
                      .device = device_,
                      .generation = generation,
                      .value = ready_lanes.size(),
                      .detail = State().plan.augmentation.seed,
                      .context = {.capacity_width = valid_donors, .capacity_height = planned_pastes, .staging_bytes = image_capacity}});
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
    PrepareDescriptors(ready_lanes.size(), ready_annotation_count, ready_rle_count, State().active_classes.size(), ready_lanes.size());
    std::size_t annotation_count = 0U;
    std::size_t rle_count = 0U;
    std::size_t tile_count = 0U;
    std::size_t ready_slot = 0U;
    for (Lane* const lane : ready_lanes) {
        const auto* image_plan = augmentation_plan != nullptr ? &augmentation_plan->images[ready_slot] : nullptr;
        const auto card_annotation_offset = annotation_count;
        const auto card_rle_offset = rle_count;
        const auto* image_batch = static_cast<const float*>(preview_active ? storage_.buffers_.augmented_batch_.data()
                                                                           : image_stream_.device_storage(lane->index).data());
        const auto card =
            AssembleImageMeaning(*lane, image_plan, image_batch + (preview_active ? ready_slot * pixel_bytes / sizeof(float) : 0U),
                                 tile_count, annotation_count, rle_count);
        store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.cards.offset + tile_count * sizeof(card), card);
        auto tile = load_payload<explore::ExploreRenderTileDescriptor>(lane->pinned.data(), lane->layout.tile);
        lane->pending_meaning = CaptureMeaning(card, card_annotation_offset, annotation_count, card_rle_offset, rle_count);
        tile.card_index = static_cast<std::uint32_t>(tile_count);
        store_payload(storage_.buffers_.descriptors_.data(),
                      descriptor_layout_.tiles.offset + tile_count++ * sizeof(explore::ExploreRenderTileDescriptor), tile);
        DiagnoseDescriptors(generation, lane->destination_slot, card_annotation_offset, annotation_count - card_annotation_offset,
                            rle_count - card_rle_offset);
        ++ready_slot;
    }
    descriptor_layout_.annotations.count = annotation_count;
    descriptor_layout_.rle.count = rle_count;
    descriptor_layout_.tiles.count = tile_count;
    if (diagnostics_enabled && donor_annotation_count != 0U)
        diagnostics_({.system = VisualSystemKind::Explore,
                      .operation = VisualDiagnosticOperation::ExploreDonorDescriptorsPrepared,
                      .device = device_,
                      .generation = generation,
                      .value = donor_annotation_count,
                      .detail = donor_rle_count,
                      .context = {.capacity_width = static_cast<std::uint32_t>(ready_annotation_count),
                                  .capacity_height = static_cast<std::uint32_t>(ready_rle_count)}});
    // Descriptor storage is execution scratch. A failed candidate may have
    // replaced its classes, so derive the batch from this product's exact facts.
    store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.classes.offset, std::span{State().active_classes});
    UploadDescriptors(cuda_stream, true);
    RenderAtlasBatch(clean, semantic, stream, static_cast<std::uint32_t>(tile_count), generation);
    if (diagnostics_enabled) {
        const auto card_extent = explore_atlas_card_extent(State().viewport);
        for (std::size_t card_index = 0U; card_index != ready_lanes.size(); ++card_index) {
            const Lane* const lane = ready_lanes[card_index];
            DiagnoseRendered(clean, semantic, stream, generation, lane->destination_slot, lane->compiled_index, card_index,
                             lane->destination_slot % State().viewport.columns * card_extent,
                             lane->destination_slot / State().viewport.columns * card_extent, card_extent, card_extent);
        }
    }
    for (const Lane* const lane : ready_lanes)
        CacheTile(clean, semantic, lane->destination_slot, stream);
    descriptors_pending_ = true;
    {
        std::scoped_lock lock(lanes_mutex_);
        for (Lane* const lane : ready_lanes) {
            lane->state = LaneState::GpuPending;
        }
    }
    for (Lane* const lane : ready_lanes) {
        ReleaseLane(*lane);
    }
    // The output candidate owns overlay completion. Source lanes may remain
    // unavailable until their independent release callbacks arrive.
    CompleteTiles(true);
    return PublicationFacts(0U);
}

std::shared_ptr<const GalleryStream::Impl::TileMeaning> GalleryStream::Impl::CaptureMeaning(
    explore::ExploreRenderCardDescriptor card, const std::size_t annotation_begin, const std::size_t annotation_end,
    const std::size_t run_begin, const std::size_t run_end) const {
    auto meaning = std::make_shared<TileMeaning>();
    card.pixels = nullptr;
    card.annotation_offset = 0U;
    meaning->card = card;
    meaning->annotations.reserve(annotation_end - annotation_begin);
    for (auto index = annotation_begin; index != annotation_end; ++index) {
        auto annotation = load_payload<explore::ExploreRenderAnnotationDescriptor>(
            storage_.buffers_.descriptors_.data(),
            descriptor_layout_.annotations.offset + index * sizeof(explore::ExploreRenderAnnotationDescriptor));
        annotation.rle_offset -= static_cast<std::uint32_t>(run_begin);
        if (annotation.occluder_index >= 0) annotation.occluder_index -= static_cast<std::int32_t>(annotation_begin);
        meaning->annotations.push_back(annotation);
    }
    meaning->runs.resize(run_end - run_begin);
    if (!meaning->runs.empty())
        std::memcpy(meaning->runs.data(),
                    static_cast<const std::byte*>(storage_.buffers_.descriptors_.data()) + descriptor_layout_.rle.offset +
                        run_begin * sizeof(data::RLEPair),
                    meaning->runs.size() * sizeof(data::RLEPair));
    return meaning;
}

void GalleryStream::Impl::RenderDetail(const ExploreRenderPlan& plan, std::shared_ptr<const mmltk::backend::data::CompiledDataset> store,
                                 const std::span<const std::uint32_t> annotated_indices,
                                 const std::span<const explore::ExploreRenderClassDescriptor> classes,
                                 const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                 const std::uintptr_t stream) {
    if (!publication_active_) throw std::logic_error("Explore detail output was not prepared");
    if (State().store == store && State().document && State().detail_meaning && State().plan.mode == ExploreMode::Detail && State().plan.dataset_identity == plan.dataset_identity &&
        State().plan.selected_image == plan.selected_image && State().plan.augmentation.enabled == plan.augmentation.enabled &&
        State().plan.augmentation.seed == plan.augmentation.seed && State().plan.augmentation_config == plan.augmentation_config) {
        const auto& meaning = *State().detail_meaning;
        PrepareDescriptors(1U, meaning.annotations.size(), meaning.runs.size(), classes.size(), 0U);
        store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.cards.offset, meaning.card);
        store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.annotations.offset, std::span{meaning.annotations});
        store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.rle.offset, std::span{meaning.runs});
        store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.classes.offset, classes);
        UploadDescriptors(reinterpret_cast<cudaStream_t>(stream), true);
        RenderDetailPlane(State().detail_view, plan.overlay, semantic, stream, true);
        EnsureCuda(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream)), "Explore semantic update failed");
        descriptors_pending_ = false;
        State().plan = plan;
        return;
    }
    Lane* reusable_lane = nullptr;
    if (State().store == store && State().plan.dataset_identity == plan.dataset_identity && State().plan.augmentation.enabled == plan.augmentation.enabled &&
        State().plan.augmentation.seed == plan.augmentation.seed && State().plan.augmentation_config == plan.augmentation_config) {
        std::scoped_lock lock(lanes_mutex_);
        for (auto& candidate : lanes_)
            if (candidate->compiled_index == *plan.selected_image && candidate->state == LaneState::InputReady)
                reusable_lane = candidate.get();
    }
    desired_generation_.store(0U, std::memory_order_release);
    Quiesce();
    State().store = std::move(store);
    State().annotated_indices = annotated_indices;
    State().plan = plan;
    stream_ = reinterpret_cast<cudaStream_t>(stream);
    Lane& lane = reusable_lane ? *reusable_lane : *lanes_.front();
    if (!reusable_lane) {
        PrepareLaneStorage(lane, *plan.selected_image, 0U, plan.generation);
        lane.prefetch = false;
        lane.state = LaneState::Reading;
        SubmitRead(lane, false);
        if (!image_stream_.wait_read(lane.index)) throw std::runtime_error("Explore detail read cancelled");
        ReadLanePayload(lane);
    }
    auto* const cuda_stream = reinterpret_cast<cudaStream_t>(stream);
    const std::array ready_lanes{&lane};
    const auto* augmentation_plan = PrepareImages(ready_lanes, {lane.layout.annotations.count, lane.layout.rle.count}, cuda_stream);
    const bool preview_active = augmentation_plan != nullptr;
    const bool has_paste = augmentation_plan != nullptr && augmentation_plan->images.front().paste_donor_slot >= 0;
    const auto* const donor_rle =
        reinterpret_cast<const data::RLEPair*>(static_cast<const std::byte*>(lane.pinned.data()) + lane.layout.donor_rle.offset);
    const auto donor_runs = has_paste ? std::span{donor_rle, lane.layout.donor_rle.count} : std::span<const data::RLEPair>{};
    PrepareDescriptors(1U, lane.layout.annotations.count + (has_paste ? 1U : 0U), lane.layout.rle.count + donor_runs.size(), classes.size(),
                       0U);
    std::size_t annotation_count = 0U;
    std::size_t rle_count = 0U;
    const auto card = AssembleImageMeaning(lane, augmentation_plan != nullptr ? &augmentation_plan->images.front() : nullptr,
                                           static_cast<const float*>(preview_active ? storage_.buffers_.augmented_batch_.data()
                                                                                    : image_stream_.device_storage(lane.index).data()),
                                           0U, annotation_count, rle_count);
    store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.cards.offset, card);
    descriptor_layout_.annotations.count = annotation_count;
    descriptor_layout_.rle.count = rle_count;
    DiagnoseDescriptors(plan.generation, *plan.selected_image, 0U, descriptor_layout_.annotations.count,
                        lane.layout.rle.count + donor_runs.size());
    store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.classes.offset, classes);
    UploadDescriptors(cuda_stream, true);
    explore::ExploreRenderDetailView detail{
        .erasure = augmentation_plan != nullptr ? augmentation_plan->images.front().erasure : rfdetr::AugmentationSpatialErasure{},
        .pixels = static_cast<const float*>(preview_active ? storage_.buffers_.augmented_batch_.data()
                                                           : image_stream_.device_storage(lane.index).data()),
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
    for (const auto& name : State().store->class_names())
        document->scene.categories.push_back({.value = name});
    document->scene.palette = contracts::annotation_class_palette(document->scene.categories.size());
    const float document_width = static_cast<float>(detail.source_width);
    const float document_height = static_cast<float>(detail.source_height);
    for (std::size_t index = 0U; index < descriptor_layout_.annotations.count; ++index) {
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
    document->mask_contains = [meaning = State().detail_meaning, width = detail.source_width,
                               height = detail.source_height, erasure = detail.erasure](std::size_t index, float x, float y) {
        const auto& annotations = meaning->annotations;
        const auto& runs = meaning->runs;
        if (index >= annotations.size() || rfdetr::augment_math::erases_sample(erasure, x, y, width, height)) return false;
        const auto sample = [&](std::size_t item) {
            return explore::detail::sample_annotation_mask(runs.data(), annotations[item], static_cast<std::uint32_t>(runs.size()), width,
                                                           height, x, y);
        };
        const auto occluder = annotations[index].occluder_index;
        return sample(index) && !(occluder >= 0 && static_cast<std::size_t>(occluder) < annotations.size() &&
                                  explore::detail::sample_annotation_support(runs.data(), annotations[occluder],
                                                                             static_cast<std::uint32_t>(runs.size()), width, height, x, y));
    };
    State().document = std::move(document);
    DiagnoseRendered(clean, semantic, stream, plan.generation, *plan.selected_image, *plan.selected_image);
    EnsureCuda(cudaStreamSynchronize(cuda_stream), "Explore detail completion failed");
    image_stream_.synchronize();
    descriptors_pending_ = false;
    lane.state = LaneState::Idle;
}

void GalleryStream::Impl::SeedPlaceholders(const std::span<const explore::ExploreRenderClassDescriptor> classes,
                                     const mmltk::frameworks::gpu::ImagePlaneView clean,
                                     const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) {
    Clear(clean, stream);
    Clear(semantic, stream);
    if (State().visible_indices.empty()) return;
    PrepareDescriptors(State().visible_indices.size(), 0U, 0U, classes.size(), State().visible_indices.size());
    for (std::size_t card = 0U; card != descriptor_layout_.cards.count; ++card)
        store_payload(storage_.buffers_.descriptors_.data(),
                      descriptor_layout_.cards.offset + card * sizeof(explore::ExploreRenderCardDescriptor),
                      explore::ExploreRenderCardDescriptor{.placeholder = 1U});
    store_payload(storage_.buffers_.descriptors_.data(), descriptor_layout_.classes.offset, classes);
    const auto card_extent = explore_atlas_card_extent(State().viewport);
    for (std::size_t slot = 0U; slot != descriptor_layout_.tiles.count; ++slot) {
        const auto destination = static_cast<std::uint32_t>(slot);
        const explore::ExploreRenderTileDescriptor tile{
            .card_index = destination,
            .destination_x = destination % State().viewport.columns * card_extent,
            .destination_y = destination / State().viewport.columns * card_extent,
            .destination_width = card_extent,
            .destination_height = card_extent,
            .generation = {.viewport = desired_generation_.load(std::memory_order_acquire), .tile = ++next_tile_generation_},
            .placeholder = 1U,
        };
        store_payload(storage_.buffers_.descriptors_.data(),
                      descriptor_layout_.tiles.offset + slot * sizeof(explore::ExploreRenderTileDescriptor), tile);
    }
    auto* const cuda_stream = reinterpret_cast<cudaStream_t>(stream);
    UploadDescriptors(cuda_stream, true);
    RenderAtlasBatch(clean, semantic, stream, static_cast<std::uint32_t>(descriptor_layout_.tiles.count),
                     desired_generation_.load(std::memory_order_acquire));
    descriptors_pending_ = true;
}

void GalleryStream::Impl::RenderAtlasBatch(const mmltk::frameworks::gpu::ImagePlaneView clean,
                                     const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream,
                                     const std::uint32_t tile_count, const std::uint64_t generation) {
    const auto card_extent = explore_atlas_card_extent(State().viewport);
    const explore::ExploreRenderAtlasView atlas{
        .card_extent = card_extent,
        .card_count = static_cast<std::uint32_t>(descriptor_layout_.cards.count),
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
    RenderAtlasPlane(atlas, batch, State().overlay, clean, stream, false);
    RenderAtlasPlane(atlas, batch, State().overlay, semantic, stream, true);
}

void GalleryStream::Impl::RenderAtlasPlane(explore::ExploreRenderAtlasView atlas, const explore::ExploreRenderTileBatchView& batch,
                                     const ExploreOverlay overlay, const mmltk::frameworks::gpu::ImagePlaneView target,
                                     const std::uintptr_t stream, const bool semantic) {
    atlas.draw_base = semantic ? 0U : 1U;
    if (explore::render_explore_atlas_tiles(atlas, batch, Semantics(overlay, semantic), Scratch(), Target(target), stream) !=
        explore::kExploreStorageSuccess)
        throw std::runtime_error("Explore retained atlas renderer failed");
}

void GalleryStream::Impl::RenderDetailPlane(explore::ExploreRenderDetailView detail, const ExploreOverlay overlay,
                                      const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t stream,
                                      const bool semantic) {
    detail.draw_base = semantic ? 0U : 1U;
    // The ABI validates a source address even for a semantic-only launch.
    // That launch never samples source pixels; bind live receiver storage,
    // rather than retaining the raw read lane's expired float allocation.
    if (semantic) detail.pixels = reinterpret_cast<const float*>(target.data);
    if (explore::render_explore_detail(detail, Semantics(overlay, semantic), Scratch(), Target(target), stream) !=
        explore::kExploreStorageSuccess)
        throw std::runtime_error("Explore retained detail renderer failed");
}

void GalleryStream::Impl::DiagnoseDescriptors(const std::uint64_t generation, const std::uint64_t slot, const std::size_t first,
                                        const std::size_t count, const std::size_t rle_count) const {
    if (!diagnostics_.valid()) return;
    float minimum_x = 1.0F;
    float minimum_y = 1.0F;
    float maximum_x = 0.0F;
    float maximum_y = 0.0F;
    for (std::size_t local = 0U; local != count; ++local) {
        const auto annotation = load_payload<explore::ExploreRenderAnnotationDescriptor>(
            storage_.buffers_.descriptors_.data(),
            descriptor_layout_.annotations.offset + (first + local) * sizeof(explore::ExploreRenderAnnotationDescriptor));
        minimum_x = std::min(minimum_x, annotation.box_xyxy[0]);
        minimum_y = std::min(minimum_y, annotation.box_xyxy[1]);
        maximum_x = std::max(maximum_x, annotation.box_xyxy[2]);
        maximum_y = std::max(maximum_y, annotation.box_xyxy[3]);
    }
    diagnostics_({.system = VisualSystemKind::Explore,
                  .operation = VisualDiagnosticOperation::ExploreTransformedBounds,
                  .device = device_,
                  .generation = generation,
                  .value = slot,
                  .detail = (static_cast<std::uint64_t>(count) << 32U) | rle_count,
                  .context = {.capacity_width = count == 0U ? 0U : diagnostic_coordinate(minimum_x),
                              .capacity_height = count == 0U ? 0U : diagnostic_coordinate(minimum_y),
                              .staging_bytes = count == 0U ? 0U
                                                           : (static_cast<std::size_t>(diagnostic_coordinate(maximum_x)) << 32U) |
                                                                 diagnostic_coordinate(maximum_y)}});
}

void GalleryStream::Impl::DiagnoseRendered(const mmltk::frameworks::gpu::ImagePlaneView clean,
                                     const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream,
                                     const std::uint64_t generation, const std::uint64_t slot, const std::uint32_t compiled_index,
                                     const std::optional<std::size_t> card_index, const std::uint32_t x, const std::uint32_t y,
                                     const std::uint32_t width, const std::uint32_t height) {
    if (!diagnostics_.valid()) return;
    constexpr std::size_t fact_count = 7U;
    EnsureBuffer(storage_.buffers_.semantic_count_device_, fact_count * sizeof(std::uint64_t),
                 "Explore rendered diagnostic device allocation failed");
    EnsureBuffer(storage_.buffers_.semantic_count_pinned_, fact_count * sizeof(std::uint64_t),
                 "Explore rendered diagnostic staging allocation failed");
    auto* const cuda_stream = reinterpret_cast<cudaStream_t>(stream);
    auto count_target = Target(semantic);
    auto checksum_target = Target(clean);
    if (width != 0U || height != 0U) {
        if (width == 0U || height == 0U || x >= count_target.width || y >= count_target.height || width > count_target.width - x ||
            height > count_target.height - y)
            throw std::logic_error("Explore rendered diagnostic region is invalid");
        count_target.data += static_cast<std::size_t>(y) * count_target.pitch_bytes + static_cast<std::size_t>(x) * 4U;
        checksum_target.data += static_cast<std::size_t>(y) * checksum_target.pitch_bytes + static_cast<std::size_t>(x) * 4U;
        count_target.width = width;
        count_target.height = height;
        checksum_target.width = width;
        checksum_target.height = height;
    }
    EnsureCuda(cudaMemsetAsync(storage_.buffers_.semantic_count_device_.data(), 0, fact_count * sizeof(std::uint64_t), cuda_stream),
               "Explore rendered diagnostic clear failed");
    if (explore::count_explore_nonzero_alpha(count_target, static_cast<std::uint64_t*>(storage_.buffers_.semantic_count_device_.data()),
                                             stream) != explore::kExploreStorageSuccess)
        throw std::runtime_error("Explore semantic diagnostic count failed");
    auto* const device_facts = static_cast<std::uint64_t*>(storage_.buffers_.semantic_count_device_.data());
    if (explore::checksum_explore_pixels(checksum_target, device_facts + 1U, stream) != explore::kExploreStorageSuccess)
        throw std::runtime_error("Explore image diagnostic checksum failed");
    std::optional<explore::ExploreRenderCardDescriptor> card;
    if (card_index)
        card = load_payload<explore::ExploreRenderCardDescriptor>(
            storage_.buffers_.descriptors_.data(),
            descriptor_layout_.cards.offset + *card_index * sizeof(explore::ExploreRenderCardDescriptor));
    std::size_t selected_annotations = 0U;
    std::size_t hidden_annotations = 0U;
    std::size_t selected_rle = 0U;
    std::uint16_t selected_class_identity = 0U;
    std::uint16_t hidden_class_identity = 0U;
    std::optional<explore::ExploreRenderAnnotationDescriptor> probe_annotation;
    for (std::size_t local = 0U; card && local != card->annotation_count; ++local) {
        const auto annotation = load_payload<explore::ExploreRenderAnnotationDescriptor>(
            storage_.buffers_.descriptors_.data(),
            descriptor_layout_.annotations.offset + (card->annotation_offset + local) * sizeof(explore::ExploreRenderAnnotationDescriptor));
        const bool selected = annotation.class_id < State().active_classes.size() && State().active_classes[annotation.class_id].visible != 0U;
        if (selected) {
            ++selected_annotations;
            selected_rle += annotation.rle_count;
            if (selected_class_identity == 0U) selected_class_identity = static_cast<std::uint16_t>(annotation.class_id + 1U);
            if (!probe_annotation && annotation.rle_count != 0U) probe_annotation = annotation;
        } else {
            ++hidden_annotations;
            if (hidden_class_identity == 0U) hidden_class_identity = static_cast<std::uint16_t>(annotation.class_id + 1U);
        }
    }
    const auto letterbox = State().store->letterbox(compiled_index);
    const auto scale_coordinate = [](const std::uint32_t coordinate, const std::uint32_t destination, const std::uint32_t source) {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(coordinate) * destination) / source);
    };
    const auto scale_interval = [&scale_coordinate](const std::uint32_t offset, const std::uint32_t extent, const std::uint32_t destination,
                                                    const std::uint32_t source) {
        const auto begin = scale_coordinate(offset, destination, source);
        const auto end = scale_coordinate(offset + extent, destination, source);
        return std::pair{begin, end - begin};
    };
    const auto [content_x, content_width] =
        card ? scale_interval(letterbox.offset_x, letterbox.resized_width, card->image_width, card->source_width) : std::pair{0U, 0U};
    const auto [content_y, content_height] =
        card ? scale_interval(letterbox.offset_y, letterbox.resized_height, card->image_height, card->source_height) : std::pair{0U, 0U};
    const explore::ExploreRenderedCardProbe probe{
        .source_pixels = card ? card->pixels : nullptr,
        .source_width = card ? card->source_width : 0U,
        .source_height = card ? card->source_height : 0U,
        .content_x = card ? card->image_x + content_x : 0U,
        .content_y = card ? card->image_y + content_y : 0U,
        .content_width = content_width,
        .content_height = content_height,
    };
    auto rendered_probe = probe;
    if (card && probe_annotation) {
        rendered_probe.box_x =
            card->image_x + static_cast<std::uint32_t>(probe_annotation->box_xyxy[0] * static_cast<float>(card->image_width));
        rendered_probe.box_y =
            card->image_y + static_cast<std::uint32_t>(probe_annotation->box_xyxy[1] * static_cast<float>(card->image_height));
        const auto box_right =
            card->image_x + static_cast<std::uint32_t>(probe_annotation->box_xyxy[2] * static_cast<float>(card->image_width));
        const auto box_bottom =
            card->image_y + static_cast<std::uint32_t>(probe_annotation->box_xyxy[3] * static_cast<float>(card->image_height));
        rendered_probe.box_width = box_right > rendered_probe.box_x ? box_right - rendered_probe.box_x : 0U;
        rendered_probe.box_height = box_bottom > rendered_probe.box_y ? box_bottom - rendered_probe.box_y : 0U;
        const bool padded = static_cast<std::uint64_t>(rendered_probe.content_width) * rendered_probe.content_height <
                            static_cast<std::uint64_t>(checksum_target.width) * checksum_target.height;
        const bool full_card = card->image_x == 0U && card->image_y == 0U && card->image_width == checksum_target.width &&
                               card->image_height == checksum_target.height;
        const bool identity_preview = !State().plan.augmentation.enabled || !State().plan.augmentation_config.enabled;
        // Retained semantic descriptors deliberately outlive their borrowed source pixels.
        const bool probe_valid = rendered_probe.source_pixels != nullptr && identity_preview && padded && full_card &&
                                 rendered_probe.box_width > 2U && rendered_probe.box_height > 2U &&
                                 rendered_probe.box_x < checksum_target.width && rendered_probe.box_y < checksum_target.height &&
                                 rendered_probe.box_width <= checksum_target.width - rendered_probe.box_x &&
                                 rendered_probe.box_height <= checksum_target.height - rendered_probe.box_y;
        if (probe_valid) {
            if (explore::probe_explore_rendered_card(checksum_target, count_target, rendered_probe, device_facts + 2U, stream) !=
                explore::kExploreStorageSuccess)
                throw std::runtime_error("Explore rendered card diagnostic probe failed");
        } else {
            probe_annotation.reset();
        }
    }
    EnsureCuda(cudaMemcpyAsync(storage_.buffers_.semantic_count_pinned_.data(), storage_.buffers_.semantic_count_device_.data(),
                               fact_count * sizeof(std::uint64_t), cudaMemcpyDeviceToHost, cuda_stream),
               "Explore rendered diagnostic transfer failed");
    EnsureCuda(cudaStreamSynchronize(cuda_stream), "Explore rendered diagnostic completion failed");
    const auto* const facts = static_cast<const std::uint64_t*>(storage_.buffers_.semantic_count_pinned_.data());
    diagnostics_({.system = VisualSystemKind::Explore,
                  .operation = VisualDiagnosticOperation::ExploreSemanticPixels,
                  .device = device_,
                  .generation = generation,
                  .value = slot,
                  .detail = facts[0],
                  .context = {.capacity_width = count_target.width, .capacity_height = count_target.height}});
    diagnostics_({.system = VisualSystemKind::Explore,
                  .operation = VisualDiagnosticOperation::ExploreImagePixels,
                  .device = device_,
                  .generation = generation,
                  .value = compiled_index,
                  .detail = facts[1],
                  .context = {.capacity_width = static_cast<std::uint32_t>(State().plan.augmentation.seed),
                              .capacity_height = static_cast<std::uint32_t>(slot),
                              .staging_bytes = (checksum_target.width == checksum_target.height ? 1U : 0U) |
                                               (State().plan.augmentation.enabled ? 2U : 0U) | (card ? 4U : 0U)}});
    if (!card) return;
    const auto packed_content = (static_cast<std::uint64_t>(probe.content_x & 0xffffU) << 48U) |
                                (static_cast<std::uint64_t>(probe.content_y & 0xffffU) << 32U) |
                                (static_cast<std::uint64_t>(probe.content_width & 0xffffU) << 16U) | (probe.content_height & 0xffffU);
    diagnostics_(
        {.system = VisualSystemKind::Explore,
         .operation = VisualDiagnosticOperation::ExploreCardGeometryProbe,
         .device = device_,
         .generation = generation,
         .value = slot,
         .detail = compiled_index,
         .context = {.capacity_width = checksum_target.width, .capacity_height = checksum_target.height, .staging_bytes = packed_content}});
    diagnostics_({.system = VisualSystemKind::Explore,
                  .operation = VisualDiagnosticOperation::ExploreOverlaySelectionProbe,
                  .device = device_,
                  .generation = generation,
                  .value = selected_annotations,
                  .detail = compiled_index,
                  .context = {.capacity_width = static_cast<std::uint32_t>(hidden_annotations),
                              .capacity_height = static_cast<std::uint32_t>(selected_rle),
                              .staging_bytes =
                                  (slot << 32U) | (static_cast<std::uint64_t>(selected_class_identity) << 16U) | hidden_class_identity}});
    if (probe_annotation) {
        const auto flags = (facts[2] != 0U ? 1U : 0U) | (facts[3] != 0U ? 2U : 0U) | (facts[4] != 0U ? 4U : 0U) |
                           (facts[5] != 0U ? 8U : 0U) | (facts[6] != 0U ? 16U : 0U);
        diagnostics_({.system = VisualSystemKind::Explore,
                      .operation = VisualDiagnosticOperation::ExploreRenderedCardProbe,
                      .device = device_,
                      .generation = generation,
                      .value = slot,
                      .detail = compiled_index,
                      .context = {.capacity_width = static_cast<std::uint32_t>(flags),
                                  .capacity_height = static_cast<std::uint32_t>(
                                      std::min<std::uint64_t>(facts[2], std::numeric_limits<std::uint32_t>::max())),
                                  .staging_bytes = (std::min<std::uint64_t>(facts[3], std::numeric_limits<std::uint32_t>::max()) << 32U) |
                                                   (std::min<std::uint64_t>(facts[4], 0xffffU) << 16U) |
                                                   std::min<std::uint64_t>(facts[5], 0xffffU)}});
        const auto transition_count = (probe.content_y != 0U ? probe.content_width : 0U) +
                                      (probe.content_y + probe.content_height < checksum_target.height ? probe.content_width : 0U) +
                                      (probe.content_x != 0U ? probe.content_height : 0U) +
                                      (probe.content_x + probe.content_width < checksum_target.width ? probe.content_height : 0U);
        diagnostics_({.system = VisualSystemKind::Explore,
                      .operation = VisualDiagnosticOperation::ExploreRenderedTransitionProbe,
                      .device = device_,
                      .generation = generation,
                      .value = slot,
                      .detail = compiled_index,
                      .context = {.capacity_width = static_cast<std::uint32_t>(facts[3]),
                                  .capacity_height = static_cast<std::uint32_t>(facts[6]),
                                  .staging_bytes = transition_count}});
    }
}

explore::ExploreRenderScratchView GalleryStream::Impl::Scratch() const {
    return {
        // CLEANUP-IGNORE: This is the single explicit CUDA scratch ABI projection of distinct buffer types and
        // capacities.
        .cards = static_cast<const explore::ExploreRenderCardDescriptor*>(storage_.buffers_.cards_device_.data()),
        .card_capacity = static_cast<std::uint32_t>(descriptor_layout_.cards.count),
        .annotations = static_cast<const explore::ExploreRenderAnnotationDescriptor*>(storage_.buffers_.annotations_device_.data()),
        .annotation_capacity = static_cast<std::uint32_t>(descriptor_layout_.annotations.count),
        .rle_pairs = static_cast<const data::RLEPair*>(storage_.buffers_.rle_device_.data()),
        .rle_capacity = static_cast<std::uint32_t>(descriptor_layout_.rle.count),
        .classes = static_cast<const explore::ExploreRenderClassDescriptor*>(storage_.buffers_.classes_device_.data()),
        .class_capacity = static_cast<std::uint32_t>(descriptor_layout_.classes.count),
        .tiles = static_cast<const explore::ExploreRenderTileDescriptor*>(storage_.buffers_.tiles_device_.data()),
        .tile_capacity = static_cast<std::uint32_t>(descriptor_layout_.tiles.count),
    };
}

explore::ExploreRenderSemanticView GalleryStream::Impl::Semantics(const ExploreOverlay overlay, const bool semantic) const {
    return {
        .annotation_count = static_cast<std::uint32_t>(descriptor_layout_.annotations.count),
        .rle_count = static_cast<std::uint32_t>(descriptor_layout_.rle.count),
        .class_count = State().store->header().num_classes,
        .show_boxes = static_cast<std::uint8_t>(semantic && overlay.show_boxes),
        .show_masks = static_cast<std::uint8_t>(semantic && overlay.show_masks),
    };
}

void GalleryStream::Impl::PrepareDescriptors(const std::size_t cards, const std::size_t annotations, const std::size_t rle,
                                       const std::size_t classes, const std::size_t tiles) {
    if (descriptors_pending_) throw std::logic_error("Explore descriptor staging was not settled before output publication");
    DescriptorLayout layout;
    layout.cards.count = cards;
    layout.annotations.count = annotations;
    layout.rle.count = rle;
    layout.classes.count = classes;
    layout.tiles.count = tiles;
    layout.annotations.offset =
        align_up(cards * sizeof(explore::ExploreRenderCardDescriptor), alignof(explore::ExploreRenderAnnotationDescriptor));
    layout.rle.offset =
        align_up(layout.annotations.offset + annotations * sizeof(explore::ExploreRenderAnnotationDescriptor), alignof(data::RLEPair));
    layout.classes.offset = align_up(layout.rle.offset + rle * sizeof(data::RLEPair), alignof(explore::ExploreRenderClassDescriptor));
    layout.tiles.offset = align_up(layout.classes.offset + classes * sizeof(explore::ExploreRenderClassDescriptor),
                                   alignof(explore::ExploreRenderTileDescriptor));
    layout.bytes = layout.tiles.offset + tiles * sizeof(explore::ExploreRenderTileDescriptor);
    EnsureBuffer(storage_.buffers_.descriptors_, std::max<std::size_t>(layout.bytes, 1U),
                 "Explore pinned descriptor staging allocation failed");
    EnsureBuffer(storage_.buffers_.cards_device_, std::max<std::size_t>(cards * sizeof(explore::ExploreRenderCardDescriptor), 1U),
                 "Explore card descriptor high-water allocation failed");
    EnsureBuffer(storage_.buffers_.annotations_device_,
                 std::max<std::size_t>(annotations * sizeof(explore::ExploreRenderAnnotationDescriptor), 1U),
                 "Explore annotation descriptor high-water allocation failed");
    EnsureBuffer(storage_.buffers_.rle_device_, std::max<std::size_t>(rle * sizeof(data::RLEPair), 1U),
                 "Explore RLE descriptor high-water allocation failed");
    EnsureBuffer(storage_.buffers_.classes_device_, std::max<std::size_t>(classes * sizeof(explore::ExploreRenderClassDescriptor), 1U),
                 "Explore class descriptor high-water allocation failed");
    EnsureBuffer(storage_.buffers_.tiles_device_, std::max<std::size_t>(tiles * sizeof(explore::ExploreRenderTileDescriptor), 1U),
                 "Explore tile descriptor high-water allocation failed");
    descriptor_layout_ = layout;
}

void GalleryStream::Impl::UploadDescriptors(const cudaStream_t stream, const bool upload_classes) {
    UploadDescriptor(storage_.buffers_.cards_device_, descriptor_layout_.cards.offset,
                     descriptor_layout_.cards.count * sizeof(explore::ExploreRenderCardDescriptor), stream);
    UploadDescriptor(storage_.buffers_.annotations_device_, descriptor_layout_.annotations.offset,
                     descriptor_layout_.annotations.count * sizeof(explore::ExploreRenderAnnotationDescriptor), stream);
    UploadDescriptor(storage_.buffers_.rle_device_, descriptor_layout_.rle.offset, descriptor_layout_.rle.count * sizeof(data::RLEPair),
                     stream);
    if (upload_classes)
        UploadDescriptor(storage_.buffers_.classes_device_, descriptor_layout_.classes.offset,
                         descriptor_layout_.classes.count * sizeof(explore::ExploreRenderClassDescriptor), stream);
    UploadDescriptor(storage_.buffers_.tiles_device_, descriptor_layout_.tiles.offset,
                     descriptor_layout_.tiles.count * sizeof(explore::ExploreRenderTileDescriptor), stream);
    if (acceptance_) acceptance_->CheckPublication(ExploreAcceptanceGate::PublicationStage::DescriptorsPrepared);
    if (diagnostics_.valid())
        diagnostics_({.system = VisualSystemKind::Explore,
                      .operation = VisualDiagnosticOperation::ExploreOverlayDescriptorsPrepared,
                      .device = device_,
                      .generation = State().plan.generation,
                      .value = descriptor_layout_.annotations.count,
                      .detail = descriptor_layout_.rle.count,
                      .context = {.capacity_width = State().store->header().image_width, .capacity_height = State().store->header().image_height}});
}

void GalleryStream::Impl::UploadDescriptor(explore::ExploreHighWaterBuffer& destination, const std::size_t offset, const std::size_t bytes,
                                     const cudaStream_t stream) {
    if (bytes != 0U)
        EnsureCuda(cudaMemcpyAsync(destination.data(), static_cast<std::byte*>(storage_.buffers_.descriptors_.data()) + offset, bytes,
                                   cudaMemcpyHostToDevice, stream),
                   "Explore pinned descriptor upload failed");
}

void GalleryStream::Impl::SettleDescriptors() {
    if (!descriptors_pending_) return;
    EnsureCuda(cudaStreamSynchronize(stream_), "Explore descriptor staging settlement failed");
    descriptors_pending_ = false;
}

void GalleryStream::Impl::EnsureBuffer(explore::ExploreHighWaterBuffer& buffer, const std::size_t bytes, const char* const detail) {
    if (!buffer.ensure_bytes(bytes)) throw std::runtime_error(detail);
}

void GalleryStream::Impl::EnsureCuda(const cudaError_t status, const char* const detail) {
    if (status != cudaSuccess) throw std::runtime_error(detail);
}

void GalleryStream::Impl::Clear(const mmltk::frameworks::gpu::ImagePlaneView target, const std::uintptr_t stream) {
    EnsureCuda(cudaMemset2DAsync(reinterpret_cast<void*>(target.data), target.descriptor.pitch_bytes, 0, target.descriptor.row_bytes(),
                                 target.descriptor.height, reinterpret_cast<cudaStream_t>(stream)),
               "Explore target clear failed");
}

explore::ExploreRenderTargetView GalleryStream::Impl::Target(const mmltk::frameworks::gpu::ImagePlaneView target) {
    return {.data = reinterpret_cast<std::uint8_t*>(target.data),
            .pitch_bytes = target.descriptor.pitch_bytes,
            .width = target.descriptor.width,
            .height = target.descriptor.height};
}

void GalleryStream::Impl::Quiesce() {
    desired_generation_.store(0U, std::memory_order_release);
    image_stream_.cancel_reads();
    if (acceptance_) acceptance_->AdvanceGeneration(0U);
    image_stream_.wait_reads();
    image_stream_.synchronize();
    if (stream_ != nullptr && cudaStreamSynchronize(stream_) != cudaSuccess) throw std::runtime_error("Explore gallery quiescence failed");
    descriptors_pending_ = false;
    std::scoped_lock lock(lanes_mutex_);
    for (auto& lane : lanes_) {
        lane->state = LaneState::Idle;
        lane->store.reset();
        lane->pending_meaning.reset();
        lane->failure = {};
    }
    next_priority_ = State().priority_slots.size();
}

void GalleryStream::Impl::ClearLogicalState() {
    candidate_.Clear();
    committed_.Clear();
    publication_active_ = false;
    desired_generation_.store(0U, std::memory_order_release);
    stale_discarded_.store(0U, std::memory_order_release);
    stream_ = nullptr;
    scheduled_slots_.clear();
    next_prefetch_ = 0U;
    batch_indices_.clear();
    batch_keys_.clear();
    batch_donors_.clear();
    projected_annotations_.clear();
    ClearReadinessState();
    next_tile_generation_ = 0U;
    descriptor_layout_ = {};
    descriptors_pending_ = false;
    std::scoped_lock lock(lanes_mutex_);
    for (auto& lane : lanes_) {
        lane->layout = {};
        lane->state = LaneState::Idle;
        lane->generation = 0U;
        lane->tile_generation = 0U;
        lane->compiled_index = 0U;
        lane->destination_slot = 0U;
        lane->card_extent = 0U;
        lane->columns = 1U;
        lane->first_row = 0U;
        lane->preview_key = 0U;
        lane->donor_index = 0U;
        lane->donor_instance.reset();
        lane->pending_meaning.reset();
        lane->failure = {};
    }
}

void GalleryStream::Impl::ClearReadinessState() {
    State().priority_slots.clear();
    State().completed_slots.clear();
    next_priority_ = 0U;
    State().cumulative_tiles = 0U;
    State().reused_tiles = 0U;
}

auto GalleryStream::Impl::ReleaseAfterRuntimeSettlement() noexcept -> mmltk::frameworks::gpu::SystemImageModel::Release {
    try {
        Quiesce();
        ClearLogicalState();
    } catch (...) {
        const auto failure = std::current_exception();
        // Failed completion still stops and joins the physical workers. The
        // runtime retains CUDA custody when settlement cannot be proved.
        try {
            image_stream_.close();
        } catch (...) {}
        return {.all_released = false, .failure = failure};
    }
    return ResetBuffersChecked();
}

auto GalleryStream::Impl::ResetBuffersChecked() noexcept -> mmltk::frameworks::gpu::SystemImageModel::Release {
    std::exception_ptr failure;
    try {
        image_stream_.close();
    } catch (...) { failure = std::current_exception(); }
    // Runtime and lane settlement precede this call. The augmenter must
    // relinquish its borrowed staging before the fixed family is released.
    augmenter_.reset();
    const auto shared = storage_.ResetChecked();
    if (shared.failure != explore::kExploreStorageSuccess && !failure) {
        try {
            throw std::runtime_error("Explore high-water release failed");
        } catch (...) { failure = std::current_exception(); }
    }
    const bool lane_owned = image_stream_.owns_resources();
    const bool all_released = !lane_owned && shared.all_released;
    if (all_released) {
        stream_ = nullptr;
        State().store = nullptr;
        State().visible_indices.clear();
        ClearReadinessState();
        descriptor_layout_ = {};
    }
    return {.all_released = all_released, .failure = failure};
}


GalleryStream::GalleryStream(const std::size_t nproc, const mmltk::frameworks::gpu::DeviceExecution& execution,
                             const ExploreNativeConfiguration& configuration)
    : impl_(std::make_unique<Impl>(nproc, execution, configuration)) {}
GalleryStream::~GalleryStream() = default;
mmltk::common::concurrency::WorkerPool& GalleryStream::workers() noexcept { return impl_->workers(); }
void GalleryStream::SetReadySink(ExploreAlgorithm::GalleryReadySink sink) { impl_->SetReadySink(std::move(sink)); }
void GalleryStream::StopIngress() noexcept { impl_->StopIngress(); }
ExploreGalleryPublication GalleryStream::Begin(
    const ExploreRenderPlan& plan, std::vector<std::uint32_t> visible, std::vector<std::uint32_t> prefetch,
    std::shared_ptr<const data::CompiledDataset> store, const std::span<const std::uint32_t> annotated,
    const std::span<const explore::detail::ExploreRenderClassDescriptorAbi> classes,
    const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
    const std::uintptr_t stream) {
    return impl_->Begin(plan, std::move(visible), std::move(prefetch), std::move(store), annotated, classes, clean, semantic, stream);
}
ExploreGalleryPublication GalleryStream::Advance() { return impl_->Advance(); }
bool GalleryStream::HasReadyTiles() const { return impl_->HasReadyTiles(); }
void GalleryStream::PrepareOutputPublication() { impl_->PrepareOutputPublication(); }
void GalleryStream::CommitOutputPublication() noexcept { impl_->CommitOutputPublication(); }
bool GalleryStream::RollbackOutputPublication() noexcept { return impl_->RollbackOutputPublication(); }
ExploreGalleryPublication GalleryStream::PublishTiles(const mmltk::frameworks::gpu::ImagePlaneView clean,
                                                      const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                                      const std::uintptr_t stream) {
    return impl_->PublishTiles(clean, semantic, stream);
}
void GalleryStream::RenderDetail(const ExploreRenderPlan& plan, std::shared_ptr<const data::CompiledDataset> store,
                                 const std::span<const std::uint32_t> annotated,
                                 const std::span<const explore::detail::ExploreRenderClassDescriptorAbi> classes,
                                 const mmltk::frameworks::gpu::ImagePlaneView clean,
                                 const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) {
    impl_->RenderDetail(plan, std::move(store), annotated, classes, clean, semantic, stream);
}
void GalleryStream::Quiesce() { impl_->Quiesce(); }
std::shared_ptr<const VisualDocument> GalleryStream::Document() const { return impl_->Document(); }
std::vector<ExploreLabel> GalleryStream::Labels() const { return impl_->Labels(); }
void GalleryStream::ClearLogicalState() { impl_->ClearLogicalState(); }
auto GalleryStream::ReleaseAfterRuntimeSettlement() noexcept -> mmltk::frameworks::gpu::SystemImageModel::Release {
    return impl_->ReleaseAfterRuntimeSettlement();
}
auto GalleryStream::ResetBuffersChecked() noexcept -> mmltk::frameworks::gpu::SystemImageModel::Release {
    return impl_->ResetBuffersChecked();
}

}  // namespace mmltk::controller::explore_detail
