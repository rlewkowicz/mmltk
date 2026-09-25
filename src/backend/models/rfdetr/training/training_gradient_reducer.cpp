#include "detail/training_gradient_reducer.h"
#include "detail/training_distributed.h"
#include <torch/csrc/distributed/c10d/reducer.hpp>
#include <ATen/cuda/CUDAEvent.h>
#include <ATen/ops/_foreach_add.h>
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
namespace mmltk::backend::models::rfdetr {
namespace tc = mmltk::backend::ml::cuda;
struct TrainingGradientReducer::Impl final {
 struct Parameter {
  std::string name;
  torch::Tensor master;
  torch::Tensor view;
  std::size_t bucket = 0;
 };
 struct Bucket {
  torch::Tensor values;
  std::size_t work_slot = 0;
  std::vector<std::size_t> parameters;
  std::size_t closed = 0;
  at::cuda::CUDAEvent ready;
 };
 struct Slot {
  std::vector<torch::Tensor> leaves;
  std::vector<unsigned> hooks;
  std::vector<torch::Tensor> gradients;
  std::vector<at::cuda::CUDAEvent> produced;
  std::vector<std::size_t> remaining;
  std::vector<bool> closed;
  bool armed = false;
 };
 Impl(const DistributedContext& group, int device_id, tc::TorchCudaStream stream, std::size_t bytes)
  : distributed(group), device(device_id), launch(stream), accumulation(tc::getStreamFromPool(false, tc::checked_device_index(device_id))), bucket_bytes(bytes) {
  if (!bytes) throw std::invalid_argument("gradient bucket capacity must be positive");
 }
 ~Impl() { remove_hooks(); }
 void remove_hooks() noexcept {
  for (auto& slot : slots)
   for (std::size_t i = 0; i < slot.hooks.size(); ++i) slot.leaves[i].remove_hook(slot.hooks[i]);
 }
 void check() const { if (failure) std::rethrow_exception(failure); }
 void rebuild(const std::vector<std::string>& names, const std::vector<torch::Tensor>& master, const std::vector<std::vector<torch::Tensor>>& leaves) {
  std::lock_guard lock(mutex);
  check();
  if (active) throw std::logic_error("gradient membership can only change at a drained boundary");
  if (names.size() != master.size() || master.empty() || leaves.empty()) throw std::invalid_argument("invalid gradient binding inventory");
  launch.synchronize(); accumulation.synchronize();
  for (auto& parameter : parameters) parameter.master.mutable_grad() = {};
  remove_hooks();
  slots.clear(); buckets.clear(); parameters.clear();
  parameters.resize(master.size());
  tc::TorchCudaStreamGuard guard(accumulation);
  for (const auto& tensor : master)
   if (!tensor.requires_grad() || !tensor.is_leaf() || !tensor.is_cuda() || tensor.device().index() != device || !tensor.is_floating_point() || !tensor.is_non_overlapping_and_dense())
    throw std::invalid_argument("gradient reducer requires active dense CUDA leaves");
  // c10d groups across intervening dtypes and checks limits after insertion.
  // Bound its input to contiguous backward-order regions so ready early
  // tensors cannot wait for a later region, nor overflow a multi-tensor bucket.
  std::vector<torch::Tensor> region;
  for (std::size_t end = master.size(); end != 0;) {
   const auto region_end = end;
   std::size_t bytes = 0;
   region.clear();
   do {
    const auto& tensor = master[end - 1];
    const auto tensor_bytes = tensor.nbytes();
    if (!region.empty() && (tensor.scalar_type() != region.front().scalar_type() ||
        tensor.device() != region.front().device() || bytes > bucket_bytes || tensor_bytes > bucket_bytes - bytes)) break;
    region.push_back(tensor);
    bytes += tensor_bytes;
    --end;
   } while (end != 0);
   const auto [assignment, limits] = c10d::compute_bucket_assignment_by_size(region, {bucket_bytes});
   for (const auto& indices : assignment) {
    buckets.emplace_back();
    auto& bucket = buckets.back();
    std::int64_t elements = 0;
    for (auto reverse : indices) {
     const auto i = region_end - 1 - reverse;
     parameters[i] = {names[i], master[i], {}, buckets.size() - 1};
     bucket.parameters.push_back(i); elements += master[i].numel();
    }
    bucket.values = torch::zeros({elements}, master[bucket.parameters.front()].options().requires_grad(false));
    std::int64_t offset = 0;
    for (auto i : bucket.parameters) {
     auto& p = parameters[i];
     p.view = bucket.values.as_strided(p.master.sizes(), p.master.strides(), offset);
     offset += p.master.numel();
    }
   }
  }
  work = std::make_unique<TrainingCollectiveWork>(device, buckets.size() + 1);
  usage = torch::zeros({static_cast<std::int64_t>(parameters.size())}, torch::TensorOptions().dtype(torch::kInt32).device(tc::cuda_device(device)));
  host_usage = tc::numa_empty({static_cast<std::int64_t>(parameters.size())}, torch::kInt32, device);
  sources.reserve(parameters.size()); destinations.reserve(parameters.size());
  slots.resize(leaves.size());
  for (std::size_t s = 0; s < slots.size(); ++s) {
   auto& slot = slots[s]; slot.leaves = leaves[s];
   if (slot.leaves.size() != parameters.size()) throw std::invalid_argument("lane gradient inventory differs");
   slot.gradients.resize(parameters.size()); slot.produced.resize(parameters.size());
   slot.remaining.resize(buckets.size()); slot.closed.resize(buckets.size()); slot.hooks.reserve(parameters.size());
   for (std::size_t i = 0; i < parameters.size(); ++i) {
    const auto& tensor = slot.leaves[i]; const auto& master_tensor = parameters[i].master;
    if (!tensor.requires_grad() || !tensor.is_leaf() || tensor.sizes() != master_tensor.sizes() || tensor.strides() != master_tensor.strides() || tensor.options().dtype() != master_tensor.options().dtype() || tensor.device() != master_tensor.device())
     throw std::invalid_argument("lane leaf does not match canonical gradient binding");
    slot.hooks.push_back(tensor.register_hook([this, s, i](const torch::Tensor& gradient) { hook(s, i, gradient); return gradient; }));
   }
  }
  accumulation.synchronize();
 }
 void launch_ready() {
  if (!counts_submitted) return;
  tc::TorchCudaStreamGuard guard(launch);
  while (next_bucket < buckets.size() && buckets[next_bucket].closed == expected) {
   auto& bucket = buckets[next_bucket];
   bucket.ready.block(launch);
   bucket.values.record_stream(launch);
   bucket.work_slot = work->all_reduce(distributed, bucket.values);
   ++next_bucket;
  }
 }
 void close(Slot& slot, std::size_t ordinal) {
  if (slot.closed[ordinal]) return;
  torch::NoGradGuard no_grad;
  tc::TorchCudaStreamGuard guard(accumulation);
  auto& bucket = buckets[ordinal]; sources.clear(); destinations.clear();
  for (auto i : bucket.parameters) {
   auto& value = slot.gradients[i];
   if (!value.defined()) continue;
   slot.produced[i].block(accumulation);
   value.record_stream(accumulation);
   sources.push_back(value); destinations.push_back(parameters[i].view); host_usage.data_ptr<std::int32_t>()[i] = 1;
  }
  if (!sources.empty()) at::_foreach_add_(destinations, sources);
  for (auto i : bucket.parameters) slot.gradients[i] = {};
  sources.clear(); destinations.clear();
  slot.closed[ordinal] = true;
  ++bucket.closed;
  if (bucket.closed == expected) bucket.ready.record(accumulation);
  launch_ready();
 }
 void hook(std::size_t s, std::size_t i, const torch::Tensor& gradient) {
  std::lock_guard lock(mutex); check();
  auto& slot = slots.at(s);
  if (!active || !slot.armed || slot.gradients[i].defined() || slot.closed[parameters[i].bucket]) throw std::logic_error("gradient hook outside its contribution");
  slot.gradients[i] = gradient;
  slot.produced[i].record(tc::getCurrentCUDAStream(tc::checked_device_index(device)));
  auto& remaining = slot.remaining[parameters[i].bucket];
  if (--remaining == 0) close(slot, parameters[i].bucket);
 }
 // Terminal custody must also retain the transport referenced by Work.
 DistributedContext distributed;
 int device;
 tc::TorchCudaStream launch;
 tc::TorchCudaStream accumulation;
 std::size_t bucket_bytes;
 mutable std::mutex mutex;
 std::exception_ptr failure;
 std::vector<Parameter> parameters;
 std::vector<Bucket> buckets;
 std::vector<Slot> slots;
 at::cuda::CUDAEvent optimizer_done, completed;
 std::vector<torch::Tensor> sources, destinations;
 torch::Tensor usage, host_usage;
 std::unique_ptr<TrainingCollectiveWork> work;
 std::size_t expected = 0, admitted = 0, finished = 0, next_bucket = 0;
 bool active = false, counts_submitted = false;
};
TrainingGradientReducer::TrainingGradientReducer(const DistributedContext& group, int device, tc::TorchCudaStream stream,
 const std::vector<std::string>& names, const std::vector<torch::Tensor>& master, const std::vector<std::vector<torch::Tensor>>& leaves, std::size_t bytes)
 : impl_(std::make_shared<Impl>(group, device, stream, bytes)) {
 try { impl_->rebuild(names, master, leaves); } catch (...) { retire(); throw; }
}
TrainingGradientReducer::~TrainingGradientReducer() { retire(); }
void TrainingGradientReducer::retire() noexcept {
 if (!impl_) return;
 if (impl_->active) abort(std::make_exception_ptr(std::runtime_error("training gradient attempt abandoned")));
 // A failed or absent stream join cannot be repaired by synchronizing only
 // application streams. Retain the complete trajectory with its collective slots.
 const bool uncertain = impl_->work && impl_->work->uncertain();
 if (uncertain) {
  std::move(terminal_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(impl_)), cudaErrorUnknown);
  return;
 }
 // The session drains worker futures before retirement. Retain all hook,
 // Work, tensor, event and stream custody on a terminal CUDA failure.
 cudaError_t status = cudaSuccess;
 try {
  tc::TorchCudaDeviceGuard device(tc::checked_device_index(impl_->device));
  // Settled slots never call Work::wait again after transport shutdown.
  const auto accumulation_status = cudaStreamSynchronize(impl_->accumulation.stream());
  if (status == cudaSuccess) status = accumulation_status;
  const auto launch_status = cudaStreamSynchronize(impl_->launch.stream());
  if (status == cudaSuccess) status = launch_status;
 } catch (...) { status = cudaErrorUnknown; }
 if (status != cudaSuccess) std::move(terminal_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(impl_)), status);
 else impl_.reset();
}
void TrainingGradientReducer::rebuild(const std::vector<std::string>& names, const std::vector<torch::Tensor>& master, const std::vector<std::vector<torch::Tensor>>& leaves) { impl_->rebuild(names, master, leaves); }
void TrainingGradientReducer::begin_attempt(std::size_t count) {
 auto& p = *impl_; std::lock_guard lock(p.mutex); p.check();
 if (p.active || !count) throw std::logic_error("invalid gradient attempt admission");
 torch::NoGradGuard no_grad;
 // The optimizer's preceding reads/writes must finish before bucket reuse.
 p.optimizer_done.record(tc::getCurrentCUDAStream(tc::checked_device_index(p.device))); p.optimizer_done.block(p.accumulation);
 tc::TorchCudaStreamGuard guard(p.accumulation);
 for (auto& parameter : p.parameters) parameter.master.mutable_grad() = {};
 for (auto& bucket : p.buckets) { bucket.values.zero_(); bucket.closed = 0; }
 p.host_usage.zero_(); p.usage.zero_();
 p.expected = count; p.admitted = p.finished = p.next_bucket = 0; p.counts_submitted = false; p.active = true;
}
void TrainingGradientReducer::arm(std::size_t index) {
 auto& p = *impl_; std::lock_guard lock(p.mutex); p.check(); auto& slot = p.slots.at(index);
 if (!p.active || slot.armed || p.admitted == p.expected) throw std::logic_error("invalid gradient contribution admission");
 slot.armed = true; ++p.admitted;
 std::fill(slot.closed.begin(), slot.closed.end(), false);
 for (std::size_t b = 0; b < p.buckets.size(); ++b) slot.remaining[b] = p.buckets[b].parameters.size();
}
void TrainingGradientReducer::collect(std::size_t index) {
 auto& p = *impl_; std::lock_guard lock(p.mutex); p.check(); auto& slot = p.slots.at(index);
 if (!slot.armed) throw std::logic_error("gradient graph returned without an armed contribution");
 for (std::size_t b = 0; b < p.buckets.size(); ++b) p.close(slot, b);
 slot.armed = false; ++p.finished;
}
void TrainingGradientReducer::contribute_empty() {
 auto& p = *impl_; std::lock_guard lock(p.mutex); p.check();
 if (!p.active || p.admitted == p.expected) throw std::logic_error("invalid empty gradient contribution");
 ++p.admitted; ++p.finished;
 tc::TorchCudaStreamGuard guard(p.accumulation);
 for (auto& bucket : p.buckets) if (++bucket.closed == p.expected) bucket.ready.record(p.accumulation);
 p.launch_ready();
}
void TrainingGradientReducer::enable_gradient_launch_after_counts() {
 auto& p = *impl_; std::lock_guard lock(p.mutex); p.check();
 if (!p.active) throw std::logic_error("gradient launch outside attempt");
 p.counts_submitted = true; p.launch_ready();
}
void TrainingGradientReducer::finish_attempt() {
 auto& p = *impl_; std::lock_guard lock(p.mutex); p.check();
 if (!p.active || p.finished != p.expected || p.next_bucket != p.buckets.size()) throw std::logic_error("incomplete gradient attempt");
 const auto optimizer_stream = tc::getCurrentCUDAStream(tc::checked_device_index(p.device));
 {
  tc::TorchCudaStreamGuard guard(p.launch);
  for (auto& bucket : p.buckets) {
   bucket.values.record_stream(optimizer_stream);
   p.work->join(bucket.work_slot);
  }
  p.usage.copy_(p.host_usage, true);
  p.work->join(p.work->all_reduce(p.distributed, p.usage));
  p.host_usage.copy_(p.usage, true);
  // One physical boundary settles every gradient bucket and the usage readback.
  p.work->settle();
  const auto* used = p.host_usage.const_data_ptr<std::int32_t>();
  for (std::size_t i = 0; i < p.parameters.size(); ++i) if (used[i]) p.parameters[i].master.mutable_grad() = p.parameters[i].view;
  p.completed.record(p.launch); p.completed.block(optimizer_stream);
 }
 p.active = false;
}
void TrainingGradientReducer::abort(std::exception_ptr error) noexcept {
 auto& p = *impl_;
 { std::lock_guard lock(p.mutex); if (p.failure) return; p.failure = error; }
 distributed_abort(p.distributed);
}
std::size_t TrainingGradientReducer::launched_buckets() const { std::lock_guard lock(impl_->mutex); return impl_->next_bucket; }
std::size_t TrainingGradientReducer::bucket_count() const { std::lock_guard lock(impl_->mutex); return impl_->buckets.size(); }
}
