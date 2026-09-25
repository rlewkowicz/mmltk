#pragma once
#include "src/backend/models/rfdetr/core/detection_statistics.h"
#include "src/common/concurrency/worker_pool.h"
#include <atomic>
#include <deque>
#include <future>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <ATen/cuda/CUDAEvent.h>
#include "src/backend/ml/cuda/shared_cuda_event.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/backend/models/rfdetr/core/runtime.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "training_ops_private.h"
#include "training_scalar_packet.h"
#include "training_gradient_reducer.h"
#include "target_builder_private.h"
#include "gpu_augment_private.h"
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
class TrainingEventOwner final {
public:
 TrainingEventOwner(const int device, const std::size_t capacity)
     : device_(device),
       retirement_owner_(1U),
       event_pool_(mmltk::frameworks::gpu::make_cuda_device_owner<TrainingEventOwner, &TrainingEventOwner::record_failure>(this, device), capacity, retirement_owner_) {}
 void record_failure(const cudaError_t failure) noexcept { mmltk::frameworks::gpu::record_first_cuda_failure(first_failure_, failure); }
 [[nodiscard]] mmltk::backend::ml::cuda::CudaEventPool& pool() noexcept { return event_pool_; }

private:
 int device_ = -1;
 std::atomic<cudaError_t> first_failure_{cudaSuccess};
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_owner_;
 mmltk::backend::ml::cuda::CudaEventPool event_pool_;
};
struct TrainLaneResult {
 torch::Tensor loss;
 torch::Tensor class_loss;
 torch::Tensor box_loss;
 TrainingScalarPacket::Tensors scalars;
 DetectionStatisticsPacket::Tensors statistics;
 // Detached scalar criterion values, bounded by the model's criterion schema.
 TensorMap loss_terms;
 // Borrowed until this wave settles; the lane retains and reuses its event.
 at::cuda::CUDAEvent* ready_event = nullptr;
};
class TrainingLossReport final {
public:
 void begin_attempt();
 void accumulate(TensorMap terms);
 [[nodiscard]] std::string details(const std::vector<torch::Tensor>& parameters, const std::vector<std::string>& names) const;
 [[nodiscard]] std::runtime_error failure(const std::vector<torch::Tensor>& parameters, const std::vector<std::string>& names) const;
private:
 std::vector<std::string> names_;
 std::vector<torch::Tensor> incoming_;
 torch::Tensor values_;
};
std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> record_current_stream_event(mmltk::backend::ml::cuda::CudaEventPool&, int, const char*);
[[nodiscard]] std::shared_ptr<NativeRfDetrModel> make_train_lane_model(NativeRfDetrModel&, int device_id);
void ensure_train_lane_model_supported(NativeRfDetrModel&, int);
class TrainingLanes final {
public:
 TrainingLanes(
  const TrainRequest&, RuntimeContext&, mmltk::backend::data::DatasetLoader&, std::shared_ptr<NativeRfDetrModel>, const std::vector<std::string>&, int lane_count, std::size_t local_batch, const mmltk::frameworks::gpu::DeviceContext&, std::function<void(std::exception_ptr)> failure = {}, std::shared_ptr<mmltk::common::concurrency::WorkerPool> workers = {});
 ~TrainingLanes();
 TrainingLanes(const TrainingLanes&) = delete;
 TrainingLanes& operator=(const TrainingLanes&) = delete;
 // CLEANUP-IGNORE: This API declaration repeats its out-of-line definition's parameter types, not implementation.
 std::future<TrainLaneResult> enqueue(RuntimeContext* runtime, mmltk::backend::data::DatasetLoader& loader, const mmltk::backend::data::Batch& batch,
  const mmltk::backend::ml::cuda::CudaEventPool::Lease* params_ready, std::size_t admitted_microbatches, double gradient_scale,
  size_t parameter_version, const DetectionConfig& detection_config, const NativeRfDetrModel& model, int device_id, int image_height, int image_width, std::uint64_t seed, int epoch, int rank,
  std::uint64_t augmentation_sequence, bool amp_enabled, at::ScalarType autocast_dtype, TrainingSupervisionRoute route, std::shared_ptr<TrainingTargetCounts> normalizer, TrainingGradientReducer& reducer, std::size_t lane_index, std::span<const TrainingDonorDescriptor> donors);
 void settle(TrainLaneResult&, int device_id);
 [[nodiscard]] std::vector<std::vector<torch::Tensor>> gradient_leaves() const;
 void reconfigure(NativeRfDetrModel&, const std::vector<std::string>& active_names, const GpuAugmentationConfig&, int batch_size, CompilationMode);
 void harvest_timing();
 void settle_targets();

private:
 void retire() noexcept;
 struct Impl;
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_{1U};
 mmltk::frameworks::gpu::TerminalCudaRetirementLease terminal_ = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement_);
 std::shared_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::models::rfdetr
