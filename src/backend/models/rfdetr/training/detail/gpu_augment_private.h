#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include "training_data_state.h"
#include <cuda_runtime_api.h>
#include "src/backend/data/loading/dataset_loader.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
#include <torch/types.h>
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
namespace mmltk::backend::models::rfdetr {
namespace test_support {
struct GpuBatchAugmenterTestAccess;
}
class GpuBatchAugmenter {
public:
 GpuBatchAugmenter(const GpuAugmentationConfig& config, std::int64_t batch_capacity, int height, int width, mmltk::frameworks::gpu::DeviceContext context);
 ~GpuBatchAugmenter();
 GpuBatchAugmenter(const GpuBatchAugmenter&) = delete;
 GpuBatchAugmenter& operator=(const GpuBatchAugmenter&) = delete;
 void reconfigure(const GpuAugmentationConfig& config);
 [[nodiscard]] torch::Tensor run(const mmltk::backend::data::Batch& batch, std::uint64_t seed, int epoch, int rank, std::uint64_t sequence, const mmltk::backend::data::DatasetLoader* source = nullptr,
  std::span<const TrainingDonorDescriptor> donors = {});
 // Host planning accepts annotation-only Batch views and never touches pixels.
 void prepare(const mmltk::backend::data::Batch&, std::uint64_t seed, int epoch, int rank, std::uint64_t sequence, const mmltk::backend::data::DatasetLoader* source = nullptr,
  std::span<const TrainingDonorDescriptor> donors = {});
 [[nodiscard]] torch::Tensor run_prepared(const mmltk::backend::data::Batch&, const mmltk::backend::data::DatasetLoader* source = nullptr, std::span<const TrainingDonorDescriptor> donors = {});
 [[nodiscard]] inline AugmentationBatchPlan& batch_plan() {
  RequireActive();
  return batch_plan_;
 }
 [[nodiscard]] cudaStream_t prepare_batch_consumer();
 [[nodiscard]] cudaStream_t finish_batch(const mmltk::backend::data::Batch& batch);
 [[nodiscard]] inline bool enabled() const {
  RequireActive();
  return executor_->enabled();
 }
 [[nodiscard]] inline bool transforms_geometry() const {
  RequireActive();
  return executor_->transforms_geometry();
 }

private:
 void RequireActive() const;
 void Retire(cudaError_t) noexcept;
 void CheckSettlement(cudaError_t, const char*);
 cudaError_t (*copy_)(void*, const void*, std::size_t, cudaMemcpyKind, cudaStream_t) = &cudaMemcpyAsync;
 decltype(&cudaEventSynchronize) event_wait_ = &cudaEventSynchronize;
 decltype(&cudaStreamSynchronize) stream_wait_ = &cudaStreamSynchronize;
 friend struct test_support::GpuBatchAugmenterTestAccess;
 void materialize_donors(const mmltk::backend::data::DatasetLoader&, std::span<const TrainingDonorDescriptor>);
 void ensure_copy_paste_resources();
 [[nodiscard]] GpuAugmentationBatchView batch_view(const mmltk::backend::data::Batch&) const;
 [[nodiscard]] cudaError_t release_copy_paste_resources() noexcept;
 GpuAugmentationConfig config_;
 struct Resources final {
  explicit Resources(mmltk::frameworks::gpu::DeviceContext value) : context(std::move(value)) {}
  mmltk::frameworks::gpu::DeviceContext context;
  torch::Tensor output_;
  torch::Tensor donor_images_;
  torch::Tensor donor_images_cpu_;
  torch::Tensor donor_masks_;
  torch::Tensor donor_masks_cpu_;
  torch::Tensor donor_boxes_cpu_;
  torch::Tensor donor_boxes_gpu_;
  torch::Tensor replacement_indices_cpu_;
  torch::Tensor replacement_indices_gpu_;
  cudaStream_t cache_stream_ = nullptr;
  cudaEvent_t image_read_complete_ = nullptr;
  cudaEvent_t cache_ready_ = nullptr;
  cudaEvent_t cache_upload_complete_ = nullptr;
 };
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_{3U};
 mmltk::frameworks::gpu::TerminalCudaRetirementLease resources_retirement_{mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement_)};
 std::shared_ptr<Resources> resources_;
 AugmentationBatchPlan batch_plan_;
 std::vector<GpuAugmentationDonor> donor_metadata_;
 std::vector<GpuAugmentationDonor> planned_donor_metadata_;
 std::vector<TrainingDonorDescriptor> materialized_donors_;
 std::vector<std::span<const mmltk::backend::data::RLEPair>> planned_support_;
 std::vector<std::uint64_t> planned_keys_;
 std::vector<TrainingDonorDescriptor> planned_donors_;
 const mmltk::backend::data::DatasetLoader* planned_source_ = nullptr;
 std::uint64_t planned_microbatch_key_ = 0;
 bool plan_prepared_ = false;
 std::size_t next_staging_slot_ = 0;
 bool logical_donors_ = false;
 std::vector<std::vector<mmltk::backend::data::RLEPair>> donor_support_;
 std::unique_ptr<GpuAugmentationExecutor> executor_;
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
