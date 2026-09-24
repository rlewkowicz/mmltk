#include "prediction_count.h"
#include <stdexcept>
#include <utility>
namespace mmltk::backend::models::rfdetr {
PredictionCountStorage::PredictionCountStorage(int device, std::size_t capacity,
 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement, mmltk::frameworks::gpu::PinnedHostBuffer::Operations operations)
 : storage_(device, {}, std::move(retirement), operations), host_(storage_.view({static_cast<std::int64_t>(capacity)}, at::kLong)), devices_(capacity), device_index_(device) {}
void PredictionCountStorage::Prepare(std::shared_ptr<PredictionCountStorage>& storage, std::size_t count, int device,
 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement, mmltk::frameworks::gpu::PinnedHostBuffer::Operations operations) {
 if (!storage || storage.use_count() != 1 || storage->device_index_ != device || storage->devices_.size() < count)
  storage = std::make_shared<PredictionCountStorage>(device, count, std::move(retirement), operations);
}
void PredictionCountStorage::Publish(std::size_t index, const torch::Tensor& count, mmltk::backend::ml::runtime::AnalysisAnnotationStorage& output) {
 if (!count.defined() || !count.is_cuda() || count.get_device() != device_index_ || count.scalar_type() != at::kLong || count.dim() != 1 || count.numel() != 1 || !count.is_contiguous() ||
     output.value_capacity == 0U || index >= devices_.size())
  throw std::invalid_argument("RF-DETR count requires one int64 device value and output capacity");
 output.count.Reset();
 devices_[index] = count;
 auto host = host_.narrow(0, static_cast<std::int64_t>(index), 1);
 output.count.PublishPending(count.data_ptr<std::int64_t>(), host.data_ptr<std::int64_t>(), shared_from_this(), output.value_capacity);
 host.copy_(count, true);
}
CUresult PredictionCountStorage::ReleaseSettled() noexcept {
 if (weak_from_this().use_count() != 1) return CUDA_ERROR_NOT_READY;
 host_ = {};
 const auto status = storage_.ReleaseSettled();
 if (status != CUDA_SUCCESS) return status;
 devices_.clear();
 return CUDA_SUCCESS;
}
}  // namespace mmltk::backend::models::rfdetr
