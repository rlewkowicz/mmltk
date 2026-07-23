#pragma once

#include "dataset_loader.h"
#include "mmltk/rfdetr/augmentation_plan.h"
#include "mmltk/rfdetr/detection_types.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/torch.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace mmltk::rfdetr {

c10::DeviceIndex cuda_device_index(int device_id);
torch::Device cuda_device(int device_id);

struct BatchStaticTensors {
    torch::Tensor sizes;
    torch::Tensor nested_mask;
    int device_id = -1;
    int image_height = 0;
    int image_width = 0;
    int64_t batch_capacity = 0;

    void ensure(int64_t batch_size, int height, int width, int target_device_id);
    [[nodiscard]] torch::Tensor sizes_view(int64_t batch_size) const;
    [[nodiscard]] torch::Tensor nested_mask_view(int64_t batch_size) const;
};

struct TargetScratch {
    BatchStaticTensors batch;
    torch::Tensor boxes_cpu;
    torch::Tensor labels_cpu;
    torch::Tensor area_cpu;
    torch::Tensor iscrowd_cpu;
    torch::Tensor inverse_transforms_cpu;
    torch::Tensor occluder_mask_indices_cpu;
    torch::Tensor occluder_inverse_transforms_cpu;
    torch::Tensor packed_masks_cpu;
    torch::Tensor image_ids_cpu;
    torch::Tensor offsets_cpu;
    torch::Tensor counts_cpu;
    torch::Tensor offsets_gpu;
    torch::Tensor counts_gpu;
    torch::Tensor target_indices_gpu;
    std::vector<int64_t> offsets;
    std::vector<int64_t> counts;
    int device_id = -1;
    int image_height = 0;
    int image_width = 0;
    int64_t instance_capacity = 0;
    int mask_height = 0;
    int mask_width = 0;
    int64_t mask_words_per_instance = 0;
    int copy_stream_device_id = -1;
    std::optional<c10::cuda::CUDAStream> copy_stream;
    cudaEvent_t copy_complete_event = nullptr;
    bool copy_pending = false;

    ~TargetScratch();

    void ensure_batch(size_t batch_size, int height, int width, int target_device_id);
    void ensure_instance_capacity(int64_t instances);
    void ensure_packed_mask_capacity(int64_t instances, int height, int width);
    void ensure_copy_resources(int target_device_id);
    void wait_for_pending_copy();
    void handoff_pending_copy_to_current_stream(int target_device_id) const;
};

class LoaderBatchGuard {
   public:
    LoaderBatchGuard(mmltk::DatasetLoader& loader, const mmltk::Batch& batch, int device_id);
    ~LoaderBatchGuard();

    LoaderBatchGuard(const LoaderBatchGuard&) = delete;
    LoaderBatchGuard& operator=(const LoaderBatchGuard&) = delete;

    void set_consumer_stream(cudaStream_t stream) noexcept {
        consumer_stream_ = reinterpret_cast<void*>(stream);
    }
    void release();

   private:
    mmltk::DatasetLoader* loader_ = nullptr;
    mmltk::Batch batch_{};
    int device_id_ = 0;
    void* consumer_stream_ = nullptr;
};

torch::Tensor make_size_tensor(int64_t batch_size, int64_t image_height, int64_t image_width,
                               const torch::Device& device);

torch::Tensor make_device_batch_tensor(const mmltk::Batch& batch, int device_id, int64_t image_height,
                                       int64_t image_width);

PreparedTargets build_targets(const mmltk::Batch& batch, int image_height, int image_width, bool include_masks,
                              bool require_masks, int device_id, TargetScratch& scratch,
                              AugmentationBatchPlan* augmentation_plan = nullptr);

std::string resolve_path(const std::string& path);

mmltk::DatasetLoader::Config make_loader_config(const std::string& compiled_path, size_t batch_size, bool shuffle,
                                                int prefetch_factor, int gather_workers,
                                                const std::string& cpu_affinity, int device_id, uint64_t seed,
                                                uint32_t batch_shard_rank = 0, uint32_t batch_shard_count = 1);

}  
