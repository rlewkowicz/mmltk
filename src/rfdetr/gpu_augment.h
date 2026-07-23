#pragma once

#include "dataset_loader.h"
#include "mmltk/rfdetr/augmentation_plan.h"
#include "mmltk/rfdetr/detection_types.h"
#include "mmltk/rfdetr/target_builder.h"
#include "mmltk/rfdetr/train_types.h"

#include <cuda_runtime.h>
#include <torch/torch.h>

#include <cstdint>
#include <array>
#include <vector>

namespace mmltk::rfdetr {

class GpuBatchPreprocessor {
   public:
    GpuBatchPreprocessor(std::int64_t batch_capacity, int height, int width, int device_id, at::ScalarType output_type);
    ~GpuBatchPreprocessor();

    GpuBatchPreprocessor(const GpuBatchPreprocessor&) = delete;
    GpuBatchPreprocessor& operator=(const GpuBatchPreprocessor&) = delete;

    [[nodiscard]] torch::Tensor run(const mmltk::Batch& batch, std::int64_t output_batch_size = 0);
    void record_consumer(cudaStream_t stream);

    [[nodiscard]] std::int64_t batch_capacity() const noexcept {
        return batch_capacity_;
    }
    [[nodiscard]] at::ScalarType output_type() const noexcept {
        return output_type_;
    }

   private:
    torch::Tensor output_;
    std::int64_t batch_capacity_ = 0;
    int height_ = 0;
    int width_ = 0;
    int device_id_ = -1;
    at::ScalarType output_type_ = at::kFloat;
    cudaEvent_t consumer_complete_ = nullptr;
    bool consumer_pending_ = false;
    bool has_run_ = false;
};

class GpuBatchAugmenter {
   public:
    GpuBatchAugmenter(const GpuAugmentationConfig& config, std::int64_t batch_capacity, int height, int width,
                      int device_id, bool include_masks);
    ~GpuBatchAugmenter();

    GpuBatchAugmenter(const GpuBatchAugmenter&) = delete;
    GpuBatchAugmenter& operator=(const GpuBatchAugmenter&) = delete;

    [[nodiscard]] torch::Tensor run(const mmltk::Batch& batch, std::uint64_t seed, int epoch, int rank,
                                    std::uint64_t sequence);
    [[nodiscard]] AugmentationBatchPlan& batch_plan() noexcept {
        return batch_plan_;
    }
    [[nodiscard]] cudaStream_t finish_batch(const mmltk::Batch& batch, PreparedTargets& targets,
                                            TargetScratch& scratch);

    [[nodiscard]] bool enabled() const noexcept {
        return config_.enabled;
    }
    [[nodiscard]] bool transforms_geometry() const noexcept {
        return transforms_geometry_;
    }

   private:
    struct DonorSlotMetadata {
        bool valid = false;
        std::int64_t label = -1;
        std::uint32_t dataset_index = 0;
        float area = 0.0F;
        std::array<float, 4> box{};
    };

    void prepare_batch_plan(const mmltk::Batch& batch, std::uint64_t seed, int epoch, int rank, std::uint64_t sequence);
    void write_trace(int epoch, int rank, std::uint64_t sequence, std::int64_t batch_size) const;

    GpuAugmentationConfig config_;
    torch::Tensor output_;
    torch::Tensor parameters_;
    torch::Tensor copy_paste_parameters_cpu_;
    torch::Tensor copy_paste_parameters_gpu_;
    torch::Tensor donor_images_;
    torch::Tensor donor_masks_;
    torch::Tensor donor_boxes_cpu_;
    torch::Tensor donor_boxes_gpu_;
    torch::Tensor replacement_indices_cpu_;
    torch::Tensor replacement_indices_gpu_;
    torch::Tensor paste_donor_slots_cpu_;
    torch::Tensor paste_donor_slots_gpu_;
    torch::Tensor paste_target_indices_cpu_;
    torch::Tensor paste_target_indices_gpu_;
    AugmentationBatchPlan batch_plan_;
    std::vector<DonorSlotMetadata> donor_metadata_;
    cudaStream_t cache_stream_ = nullptr;
    cudaEvent_t image_read_complete_ = nullptr;
    cudaEvent_t cache_ready_ = nullptr;
    std::int64_t batch_capacity_ = 0;
    std::int64_t current_batch_size_ = 0;
    std::uint64_t current_sequence_ = 0;
    int current_epoch_ = 0;
    int current_rank_ = 0;
    int height_ = 0;
    int width_ = 0;
    int device_id_ = -1;
    int trace_fd_ = -1;
    std::int64_t mask_words_ = 0;
    bool remap_ = false;
    bool transforms_geometry_ = false;
    bool include_masks_ = false;
    bool copy_paste_enabled_ = false;
    bool cache_ready_pending_ = false;
};

}  
