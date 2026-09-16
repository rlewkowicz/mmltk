#pragma once

#include <cuda_runtime.h>
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_authority.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "src/backend/models/rfdetr/augmentation/augmentation_plan.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment_cuda.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment_types.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"

namespace mmltk::backend::models::rfdetr {
namespace test_support { struct GpuAugmentationTestAccess; }

enum class GpuAugmentationInputFormat : std::uint8_t {
    PlanarFloat32,
    Rgba8,
};

struct GpuAugmentationBatchView {
    const void* input = nullptr;
    float* output = nullptr;
    std::span<const std::uint32_t> image_indices;
    int height = 0;
    int width = 0;
    GpuAugmentationInputFormat input_format = GpuAugmentationInputFormat::PlanarFloat32;
    GpuAugmentationOutputDomain output_domain = GpuAugmentationOutputDomain::ModelNormalized;
    // Optional CPU list of stable device image pointers, in logical batch order.
    // Empty uses contiguous identity addressing. The executor stages only this list.
    std::span<const float* const> input_slots{};
    // Exact producer aggregate; logical bytes per slot, or whole contiguous batch.
    std::shared_ptr<const void> input_custody;
    std::shared_ptr<const void> output_custody;
    std::size_t input_capacity_bytes = 0;
    std::size_t output_capacity_bytes = 0;
};

struct GpuAugmentationDonor {
    std::int64_t label = -1;
    std::uint32_t dataset_index = 0U;
    float area = 0.0F;
    std::array<float, 4> box{};
    bool has_mask = false;
};

[[nodiscard]] bool augmentation_paste_admitted(const GpuAugmentationConfig& config, std::uint64_t key) noexcept;

enum class GpuAugmentationDonorSelection : std::uint8_t {
    Aligned,
    Cached,
};

struct GpuAugmentationDonorBatchView {
    // Donor images and boxes are device-resident planar float32 data; masks are
    // device-resident packed 64-bit words.
    const float* images = nullptr;
    const std::int64_t* masks = nullptr;
    const float* boxes = nullptr;
    std::int64_t mask_words = 0;
    GpuAugmentationDonorSelection selection = GpuAugmentationDonorSelection::Aligned;
    // Optional device image pointers corresponding to donor metadata slots.
    std::span<const float* const> image_slots{};
    std::shared_ptr<const void> image_custody;
    std::size_t image_capacity_bytes = 0;
};

[[nodiscard]] inline std::uint64_t augmentation_mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] inline std::uint64_t augmentation_preview_image_key(const std::uint64_t dataset_identity, const std::uint64_t preview_seed,
                                                                  const std::uint32_t compiled_index) noexcept {
    return augmentation_mix64(dataset_identity ^ augmentation_mix64(preview_seed) ^
                              (static_cast<std::uint64_t>(compiled_index) * 0xd2b74407b1ce6e93ULL));
}

[[nodiscard]] inline std::uint32_t select_augmentation_preview_donor_image(const std::span<const std::uint32_t> annotated_indices,
                                                                           const std::uint32_t source_index,
                                                                           const std::uint64_t image_key) noexcept {
    if (annotated_indices.empty()) { return source_index; }
    std::size_t position = static_cast<std::size_t>(augmentation_mix64(image_key ^ 0x51ed2705ULL) % annotated_indices.size());
    if (annotated_indices[position] == source_index && annotated_indices.size() > 1U) {
        position = (position + 1U) % annotated_indices.size();
    }
    return annotated_indices[position];
}

[[nodiscard]] inline std::size_t select_augmentation_preview_donor_instance(const std::size_t instance_count,
                                                                            const std::uint64_t image_key) noexcept {
    return instance_count == 0U ? 0U : static_cast<std::size_t>(augmentation_mix64(image_key ^ 0xa0761d6478bd642fULL) % instance_count);
}

// Owns augmentation planning and the reusable raw-CUDA launch workspace. The
// caller owns the CUDA context, stream, input, donor, and output storage.
class GpuAugmentationExecutor final {
   public:
    GpuAugmentationExecutor(const GpuAugmentationConfig& config, std::size_t batch_capacity, int height, int width, mmltk::frameworks::gpu::DeviceContext context,
                            mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement);
    ~GpuAugmentationExecutor();

    GpuAugmentationExecutor(const GpuAugmentationExecutor&) = delete;
    GpuAugmentationExecutor& operator=(const GpuAugmentationExecutor&) = delete;
    GpuAugmentationExecutor(GpuAugmentationExecutor&&) = delete;
    GpuAugmentationExecutor& operator=(GpuAugmentationExecutor&&) = delete;

    void Finish();
    void Reconfigure(const GpuAugmentationConfig& config);

    [[nodiscard]] const AugmentationBatchPlan& Run(const GpuAugmentationBatchView& batch, std::span<const std::uint64_t> image_keys,
                                                   std::span<const GpuAugmentationDonor> donors,
                                                   const GpuAugmentationDonorBatchView& donor_batch, cudaStream_t stream,
                                                   std::size_t staging_slot = 0U);
    [[nodiscard]] const AugmentationBatchPlan& RunTraining(const GpuAugmentationBatchView& batch, std::uint64_t seed, int epoch, int rank,
                                                           std::uint64_t sequence, std::span<const GpuAugmentationDonor> donors,
                                                           const GpuAugmentationDonorBatchView& donor_batch, cudaStream_t stream,
                                                           std::size_t staging_slot = 0U);

    // Image parameters and returned semantic geometry/erasure are planned together.
    // The returned plan remains owned by this executor and is replaced by the next run.
    [[nodiscard]] const AugmentationBatchPlan& plan() const;
    // Opt-in diagnostic JSON from the actual host plan and staged launch parameters.
    // Call after Run/RunTraining returns, with that run's staging slot, before
    // reconfiguration or another run. Reads no device storage and retains nothing.
    [[nodiscard]] std::string prepared_image_diagnostic(std::size_t image, std::size_t staging_slot) const;
    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool transforms_geometry() const;
    [[nodiscard]] bool copy_paste_enabled() const;
    [[nodiscard]] bool remaps_pixels() const;
    [[nodiscard]] std::size_t batch_capacity() const;
    [[nodiscard]] std::size_t workspace_capacity_bytes() const;
    [[nodiscard]] std::size_t device_capacity_bytes() const;
    [[nodiscard]] std::size_t pinned_capacity_bytes() const;

   private:
    [[nodiscard]] const AugmentationBatchPlan& RunImpl(const GpuAugmentationBatchView& batch, std::span<const std::uint64_t> image_keys,
                                                       std::span<const GpuAugmentationDonor> donors,
                                                       const GpuAugmentationDonorBatchView& donor_batch, cudaStream_t stream,
                                                       std::size_t staging_slot, bool explicit_keys, std::uint64_t seed, int epoch,
                                                       int rank, std::uint64_t sequence);

    void RequireActive() const;
    void Retire(cudaError_t) noexcept;
    void CheckSettlement(cudaError_t, const char*);
    decltype(&cudaEventSynchronize) event_wait_ = &cudaEventSynchronize;
    decltype(&cudaStreamSynchronize) stream_wait_ = &cudaStreamSynchronize;
    friend struct test_support::GpuAugmentationTestAccess;
    struct Impl;
    std::shared_ptr<Impl> impl_;
    mmltk::frameworks::gpu::TerminalCudaRetirementLease retirement_;
};

[[nodiscard]] std::uint64_t training_augmentation_image_key(std::uint64_t seed, int epoch, int rank, std::uint64_t sequence,
                                                            std::size_t image) noexcept;

}  // namespace mmltk::backend::models::rfdetr
