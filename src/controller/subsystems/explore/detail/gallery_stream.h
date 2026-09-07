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
#include "src/frameworks/gpu/system_image_runtime.h"

namespace mmltk::controller::explore_detail {

// Owns gallery execution and exact committed/candidate logical products.
// Begin/RenderDetail require PrepareOutputPublication. The dataset's shared
// owner must also retain the immutable annotated-index span's backing storage.
class GalleryStream final {
   public:
    GalleryStream(std::size_t nproc, const mmltk::frameworks::gpu::DeviceExecution&, const ExploreNativeConfiguration&);
    ~GalleryStream();
    [[nodiscard]] mmltk::common::concurrency::WorkerPool& workers() noexcept;
    void SetReadySink(ExploreAlgorithm::GalleryReadySink);
    void StopIngress() noexcept;
    [[nodiscard]] ExploreGalleryPublication Begin(const ExploreRenderPlan&, std::vector<std::uint32_t>, std::vector<std::uint32_t>,
                                                  std::shared_ptr<const mmltk::backend::data::CompiledDataset>, std::span<const std::uint32_t>,
                                                  std::span<const mmltk::backend::imaging::explore::detail::ExploreRenderClassDescriptorAbi>,
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
                      std::span<const mmltk::backend::imaging::explore::detail::ExploreRenderClassDescriptorAbi>, mmltk::frameworks::gpu::ImagePlaneView,
                      mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t);
    void Quiesce();
    [[nodiscard]] std::shared_ptr<const VisualDocument> Document() const;
    [[nodiscard]] std::vector<ExploreLabel> Labels() const;
    void ClearLogicalState();
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release ReleaseAfterRuntimeSettlement() noexcept;
    [[nodiscard]] mmltk::frameworks::gpu::SystemImageModel::Release ResetBuffersChecked() noexcept;
   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::controller::explore_detail
