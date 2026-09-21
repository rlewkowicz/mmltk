#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <cuda_runtime_api.h>
#include "src/backend/imaging/explore/detail/explore_render_cuda_abi.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/explore/detail/gallery_buffer_family.h"
#include "src/controller/subsystems/explore/detail/gallery_read_scheduler.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include "src/controller/subsystems/explore/detail/gallery_host_allocations.h"
#include "src/frameworks/gpu/system_image_runtime.h"
namespace mmltk::controller::explore_detail {
struct GalleryProductState;
class GalleryDescriptorStorage;
class GalleryStream;
class GalleryThumbnailCache;
// Constructed only for opted-in pixel probes; callbacks retain this owner's
// address through the containing transaction's terminal custody.
class GalleryStreamProbe final {
 friend class GalleryStream;
 friend struct NativeExploreStorageTestAccess;

public:
 GalleryStreamProbe(int, VisualDiagnosticSink, std::shared_ptr<ExploreAcceptanceGate>);
 GalleryStreamProbe(const GalleryStreamProbe&) = delete;
 GalleryStreamProbe& operator=(const GalleryStreamProbe&) = delete;

private:
 using Buffer = mmltk::backend::imaging::explore::ExploreHighWaterBuffer;
 using Memory = mmltk::backend::imaging::explore::ExploreBufferMemory;
 struct Family final {
  Buffer semantic_count_device_;
  Buffer semantic_count_pinned_{Memory::PinnedHost};
 };
 ExploreHostAllocations host_allocations_;
 GalleryBufferFamily<Family> storage_;
 int device_;
 VisualDiagnosticSink diagnostics_;
 std::shared_ptr<ExploreAcceptanceGate> acceptance_;
 struct RenderedProbe final {
  std::uint64_t generation = 0U;
  std::uint64_t slot = 0U;
  std::uint32_t compiled_index = 0U;
  mmltk::backend::imaging::explore::detail::ExploreRenderTargetViewAbi count_target{};
  mmltk::backend::imaging::explore::detail::ExploreRenderTargetViewAbi checksum_target{};
  mmltk::backend::imaging::explore::detail::ExploreRenderedCardProbeAbi probe{};
  std::uint64_t seed = 0U;
  bool augmented = false;
  bool card = false;
  bool probe_annotation = false;
  std::uint64_t dataset_identity = 0U;
  std::uint64_t image_key = 0U;
  std::array<std::uint32_t, 2U> source_extent{};
  bool sampled_card = false;
  bool augmentation_config_enabled = false;
 };
 static constexpr std::size_t kCardSamplesOffset = 7U;
 static constexpr std::size_t kProbeFacts = kCardSamplesOffset + mmltk::backend::imaging::explore::detail::ExploreRenderedCardSampleGridAbi::kSampleCount *
                                                                  mmltk::backend::imaging::explore::detail::ExploreRenderedCardSampleGridAbi::kWordsPerSample;
 std::array<RenderedProbe, kExploreVisibleItemCapacity> probes_{};
 std::size_t probe_count_ = 0U;
 std::size_t submitted_probes_ = 0U;
 std::atomic_bool probes_pending_{false};
 cudaStream_t diagnostic_stream_ = nullptr;
 cudaEvent_t probes_ready_ = nullptr;
 bool probes_disabled_ = false;
 void DiagnoseRendered(const GalleryProductState&, const GalleryDescriptorStorage&, mmltk::frameworks::gpu::ImagePlaneView, const GalleryThumbnailCache*,
                       mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, std::uint64_t, std::uint64_t,
                       std::uint32_t, std::optional<std::size_t>, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
 static void DiagnoseDescriptors(const GalleryDescriptorStorage&, VisualDiagnosticSink, int, std::uint64_t, std::uint64_t, std::size_t, std::size_t,
                                 std::size_t);
 static void DiagnosePreparedImage(const ExploreRenderPlan&, const GalleryReadScheduler&, const mmltk::backend::models::rfdetr::GpuAugmentationExecutor&,
                                   VisualDiagnosticSink, const GalleryReadScheduler::Lane&, std::size_t, std::size_t) noexcept;
 void FlushProbes(std::uintptr_t);
 bool InitializeProbes() noexcept;
 void CollectProbes() noexcept;
 void EmitProbe(const RenderedProbe&, const std::uint64_t*) const noexcept;
 void EmitCardSamples(const RenderedProbe&, const std::uint64_t*) const noexcept;
 mmltk::frameworks::gpu::SystemImageModel::Release ReleaseProbesChecked(decltype(&cudaStreamSynchronize), cudaError_t&) noexcept;
 [[nodiscard]] VisualDiagnosticFact Diagnostic(VisualDiagnosticOperation operation, std::uint64_t generation) const noexcept {
  return {.system = contracts::DiagnosticOwner::Explore, .operation = operation, .device = device_, .generation = generation};
 }
};
}  // namespace mmltk::controller::explore_detail
