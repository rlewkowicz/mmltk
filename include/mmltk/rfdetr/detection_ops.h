#pragma once

#include "mmltk/rfdetr/detection_types.h"
#include "worker_pool.h"

#include <functional>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

using AllReduceTensorFn = std::function<void(torch::Tensor&)>;

std::vector<std::pair<torch::Tensor, torch::Tensor>> matcher_indices(const ModelOutputs& outputs,
                                                                     const PreparedTargets& targets,
                                                                     const DetectionConfig& config, bool training_mode);
TensorMap detection_loss_dict(const ModelOutputs& outputs, const PreparedTargets& targets,
                              const DetectionConfig& config, bool training_mode, bool distributed_enabled,
                              const AllReduceTensorFn& distributed_all_reduce = {});
TensorMap detection_loss_dict(const ModelOutputs& outputs, const PreparedTargets& targets,
                              const DetectionConfig& config, bool training_mode, double num_boxes_value);
torch::Tensor weighted_detection_loss(const TensorMap& loss_dict, const DetectionConfig& config,
                                      const torch::Device& device);
std::vector<TensorMap> postprocess_outputs(const ModelOutputs& outputs, const torch::Tensor& target_sizes,
                                           int64_t num_select);

class ScopedWorkerPool {
   public:
    explicit ScopedWorkerPool(mmltk::WorkerPool* pool);
    ~ScopedWorkerPool();

    ScopedWorkerPool(const ScopedWorkerPool&) = delete;
    ScopedWorkerPool& operator=(const ScopedWorkerPool&) = delete;

   private:
    mmltk::WorkerPool* previous_ = nullptr;
};

}  
