#pragma once
#include <atomic>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include "src/backend/ml/cuda/shared_cuda_event.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/backend/models/rfdetr/core/runtime.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "training_ops_private.h"
#include "target_builder_private.h"
#include "gpu_augment_private.h"
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
class TrainingEventOwner final {
   public:
    TrainingEventOwner(const int device, const std::size_t capacity)
        : device_(device),
          retirement_owner_(1U),
          event_pool_(mmltk::frameworks::gpu::make_cuda_device_owner<TrainingEventOwner, &TrainingEventOwner::record_failure>(this, device), capacity,
                      retirement_owner_) {}
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
    scalar_packet::Tensors scalars;
    std::vector<torch::Tensor> gradients;
    std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> ready_event;
};
std::optional<mmltk::backend::ml::cuda::CudaEventPool::Lease> record_current_stream_event(mmltk::backend::ml::cuda::CudaEventPool&, int, const char*);
void ensure_train_lane_model_supported(NativeRfDetrModel&, int);
class TrainingLanes final {
 public:
    TrainingLanes(const TrainRequest&, RuntimeContext&, mmltk::backend::data::DatasetLoader&, NativeRfDetrModel&, const std::vector<std::string>&, int lane_count, const mmltk::frameworks::gpu::DeviceContext&);
    ~TrainingLanes();
    TrainingLanes(const TrainingLanes&) = delete;
    TrainingLanes& operator=(const TrainingLanes&) = delete;
    std::future<TrainLaneResult> enqueue(RuntimeContext* runtime,
                                                mmltk::backend::data::DatasetLoader& loader, const mmltk::backend::data::Batch& batch,
                                                const mmltk::backend::ml::cuda::CudaEventPool::Lease* params_ready,
                                                mmltk::backend::ml::cuda::CudaEventPool& event_pool, double scaled_loss_factor, size_t parameter_version,
                                                const DetectionConfig& detection_config, const NativeRfDetrModel& model, int device_id, int image_height,
                                                int image_width, std::uint64_t seed, int epoch, int rank, std::uint64_t augmentation_sequence, bool amp_enabled,
                                                at::ScalarType autocast_dtype, TrainingSupervisionRoute route,
                                                std::shared_ptr<WaveTargetNormalizer> wave_normalizer, std::size_t lane_index);
    void merge(TrainLaneResult&, std::vector<torch::Tensor>&, int device_id);
    void harvest_timing();
    void settle_targets();
 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
