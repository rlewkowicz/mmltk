#include "src/backend/models/rfdetr/inference/prediction_count.h"
#include <stdexcept>
#include "src/backend/ml/cuda/numa_host_tensor.h"

namespace mmltk::backend::models::rfdetr {
PredictionCountStorage::PredictionCountStorage(int device)
    : host_(mmltk::backend::ml::cuda::numa_empty({1}, at::kLong, device)), device_index_(device) {}
void publish_prediction_count(std::shared_ptr<PredictionCountStorage>& storage, const torch::Tensor& count,
                              mmltk::backend::ml::runtime::AnalysisAnnotationStorage& output, int device) {
    if (!count.defined() || !count.is_cuda() || count.get_device() != device || count.scalar_type() != at::kLong ||
        count.dim() != 1 || count.numel() != 1 || !count.is_contiguous() || output.value_capacity == 0U)
        throw std::invalid_argument("RF-DETR count requires one int64 device value and output capacity");
    output.count.Reset();
    if (!storage || storage.use_count() != 1 || storage->device_index_ != device)
        storage = std::make_shared<PredictionCountStorage>(device);
    storage->device_ = count;
    // Publish custody before enqueue: even a throwing copy leaves the actual
    // allocation held by the execution owner through its stream retirement.
    output.count.PublishPending(storage->device_.data_ptr<std::int64_t>(), storage->host_.data_ptr<std::int64_t>(), storage,
                                output.value_capacity);
    storage->host_.copy_(storage->device_, true);
}
}  // namespace mmltk::backend::models::rfdetr
