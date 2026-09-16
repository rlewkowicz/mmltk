#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "src/backend/data/compiled_dataset.h"
#include "src/backend/imaging/explore/detail/explore_render_cuda_abi.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"

namespace mmltk::controller::explore_detail {
struct GalleryStreamTestAccess;

struct GalleryProductState final {
    std::shared_ptr<const mmltk::backend::data::CompiledDataset> store;
    ExploreViewport viewport{};
    ExploreAtlasLayout atlas{};
    ExploreRenderPlan plan{};
    std::shared_ptr<const VisualDocument> document;
    std::shared_ptr<const GalleryTileMeaning> detail_meaning;
    mmltk::backend::imaging::explore::detail::ExploreRenderDetailViewAbi detail_view{};
    std::vector<std::uint32_t> visible_indices;
    std::vector<std::uint32_t> window_indices;
    std::size_t window_first = 0U;
    const std::uint32_t* source_window = nullptr;
    GalleryThumbnailCache cache;
    std::span<const std::uint32_t> annotated_indices;
    std::vector<mmltk::backend::imaging::explore::detail::ExploreRenderClassDescriptorAbi> active_classes;
    std::vector<std::uint32_t> priority_slots;
    std::vector<bool> completed_slots;
    std::size_t cumulative_tiles = 0U;
    std::size_t reused_tiles = 0U;
    std::size_t cache_active = 0U;
    std::vector<std::shared_ptr<const GalleryTileMeaning>> tile_meanings;

    [[nodiscard]] std::size_t Capacity() const noexcept;
    [[nodiscard]] std::size_t MetadataBytes() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;
    void Clear() noexcept;
    void ReserveFor(const GalleryProductState&);
};

// Owns gallery execution and exact committed/candidate logical products.
// Begin/RenderDetail require PrepareOutputPublication. The dataset's shared
// owner must also retain the immutable annotated-index span's backing storage.
class GalleryStream final {
   public:
    GalleryStream(std::size_t nproc, const mmltk::frameworks::gpu::DeviceExecution&, const ExploreNativeConfiguration&,
                  std::uint32_t maximum_height);
    ~GalleryStream();
    void BindExecutionContext(const mmltk::frameworks::gpu::DeviceContext&, std::shared_ptr<mmltk::frameworks::gpu::ImageStream>);
    [[nodiscard]] mmltk::common::concurrency::WorkerPool& workers();
    void SetReadySink(ExploreAlgorithm::GalleryReadySink);
    // Construction-only binding; retained unchanged through native retirement.
    void SetCurrentDemand(ExploreDemandCheck);
    [[nodiscard]] ExploreStorageFootprint StorageFootprint() const;
    [[nodiscard]] ExploreOutputChange OutputChange(const ExploreRenderPlan&, std::span<const std::uint32_t>,
                                                   const mmltk::backend::data::CompiledDataset*, std::span<const std::uint32_t>) const;
    void StopIngress() noexcept;
    [[nodiscard]] ExploreGalleryPublication Begin(
        const ExploreRenderPlan&, std::span<const std::uint32_t>, std::span<const std::uint32_t>, std::size_t,
        std::shared_ptr<const mmltk::backend::data::CompiledDataset>, std::span<const std::uint32_t>,
        std::span<const mmltk::backend::imaging::explore::detail::ExploreRenderClassDescriptorAbi>, mmltk::frameworks::gpu::ImagePlaneView,
        // CLEANUP-IGNORE: The public facade mirrors this declaration tail at its one private pimpl boundary.
        mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    [[nodiscard]] ExploreGalleryPublication Advance();
    [[nodiscard]] bool HasReadyTiles() const;
    void PrepareDetailOutput(mmltk::frameworks::gpu::ImageAllocation) noexcept;
    void PrepareOutputPublication(ExploreOutputChange, ExploreMode = ExploreMode::Gallery);
    void CommitOutputPublication() noexcept;
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceCoverage WorkspaceCoverage(
        const mmltk::frameworks::gpu::ImageWorkspaceObservation&);
    // CLEANUP-IGNORE: The public gallery API and its private implementation declare one boundary, not duplicated execution.
    [[nodiscard]] bool RollbackOutputPublication() noexcept;
    [[nodiscard]] ExploreGalleryPublication PublishTiles(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView,
                                                         std::uintptr_t);
    void RenderDetail(const ExploreRenderPlan&, std::shared_ptr<const mmltk::backend::data::CompiledDataset>,
                      std::span<const std::uint32_t>,
                      std::span<const mmltk::backend::imaging::explore::detail::ExploreRenderClassDescriptorAbi>,
                      mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    void Quiesce();
    // Stop/settle ingress without discarding reusable physical input custody.
    void Suspend();
    [[nodiscard]] std::shared_ptr<const VisualDocument> Document() const;
    [[nodiscard]] std::vector<ExploreLabel> Labels() const;
    void ClearLogicalState();
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release ReleaseAfterRuntimeSettlement() noexcept;
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release ResetBuffersChecked() noexcept;

   private:
    class Impl;
    [[nodiscard]] Impl& Active() const;
    void SetStreamSettlement(decltype(&cudaStreamSynchronize));
    friend struct GalleryStreamTestAccess;
    mmltk::frameworks::gpu::TerminalCudaRetirementOwner terminal_{3U};
    mmltk::frameworks::gpu::TerminalCudaRetirementLease lease_{mmltk::frameworks::gpu::ReserveTerminalCudaLease(terminal_)};
    std::shared_ptr<Impl> impl_;
};

}  // namespace mmltk::controller::explore_detail
