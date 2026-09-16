#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <cuda_runtime_api.h>

#include "src/backend/data/dataset_loader.h"
#include "src/backend/ml/torch/detail/torch_api.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"

namespace mmltk::backend::models::rfdetr {

namespace torch_types = mmltk::backend::ml::torch_api;

class GpuBatchPreprocessor {
   public:
    GpuBatchPreprocessor(std::int64_t batch_capacity, int height, int width, int device_id, torch_types::ScalarType output_type);
    ~GpuBatchPreprocessor();

    GpuBatchPreprocessor(const GpuBatchPreprocessor&) = delete;
    GpuBatchPreprocessor& operator=(const GpuBatchPreprocessor&) = delete;

    [[nodiscard]] torch_types::Tensor run(const mmltk::backend::data::Batch& batch, std::int64_t output_batch_size = 0);
    void record_consumer(cudaStream_t stream);

    [[nodiscard]] inline std::int64_t batch_capacity() const noexcept { return batch_capacity_; }
    [[nodiscard]] inline torch_types::ScalarType output_type() const noexcept { return output_type_; }

   private:
    torch_types::Tensor output_;
    std::int64_t batch_capacity_ = 0;
    int height_ = 0;
    int width_ = 0;
    int device_id_ = -1;
    torch_types::ScalarType output_type_ = torch_types::kFloat;
    cudaEvent_t consumer_complete_ = nullptr;
    bool consumer_pending_ = false;
    bool has_run_ = false;
};

class GpuBatchAugmenter {
   public:
    GpuBatchAugmenter(const GpuAugmentationConfig& config, std::int64_t batch_capacity, int height, int width, int device_id);
    ~GpuBatchAugmenter();

    GpuBatchAugmenter(const GpuBatchAugmenter&) = delete;
    GpuBatchAugmenter& operator=(const GpuBatchAugmenter&) = delete;

    void reconfigure(const GpuAugmentationConfig& config);
    [[nodiscard]] torch_types::Tensor run(const mmltk::backend::data::Batch& batch, std::uint64_t seed, int epoch, int rank,
                                          std::uint64_t sequence);
    [[nodiscard]] inline AugmentationBatchPlan& batch_plan() noexcept { return batch_plan_; }
    [[nodiscard]] cudaStream_t prepare_batch_consumer();
    [[nodiscard]] cudaStream_t finish_batch(const mmltk::backend::data::Batch& batch);

    [[nodiscard]] inline bool enabled() const noexcept { return executor_->enabled(); }
    [[nodiscard]] inline bool transforms_geometry() const noexcept { return executor_->transforms_geometry(); }

   private:
    void ensure_copy_paste_resources();
    void release_copy_paste_resources() noexcept;

    GpuAugmentationConfig config_;
    torch_types::Tensor output_;
    torch_types::Tensor donor_images_;
    torch_types::Tensor donor_masks_;
    torch_types::Tensor donor_masks_cpu_;
    torch_types::Tensor donor_boxes_cpu_;
    torch_types::Tensor donor_boxes_gpu_;
    torch_types::Tensor replacement_indices_cpu_;
    torch_types::Tensor replacement_indices_gpu_;
    AugmentationBatchPlan batch_plan_;
    std::vector<GpuAugmentationDonor> donor_metadata_;
    std::vector<std::vector<mmltk::backend::data::RLEPair>> donor_support_;
    std::unique_ptr<GpuAugmentationExecutor> executor_;
    cudaStream_t cache_stream_ = nullptr;
    cudaEvent_t image_read_complete_ = nullptr;
    cudaEvent_t cache_ready_ = nullptr;
    cudaEvent_t cache_upload_complete_ = nullptr;
    bool cache_upload_pending_ = false;
    std::int64_t batch_capacity_ = 0;
    std::int64_t current_batch_size_ = 0;
    std::uint64_t current_sequence_ = 0;
    int current_epoch_ = 0;
    int current_rank_ = 0;
    int height_ = 0;
    int width_ = 0;
    int device_id_ = -1;
    std::int64_t mask_words_ = 0;
    bool cache_ready_pending_ = false;
    bool batch_run_pending_ = false;
    bool cache_consumer_prepared_ = false;
};

}  // namespace mmltk::backend::models::rfdetr
