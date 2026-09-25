#include "detail/training_gradient_reducer.h"
#include "detail/training_ops_private.h"
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
  std::vector<torch::Tensor> collective;
  std::vector<std::size_t> parameters;
  std::size_t closed = 0;
  at::cuda::CUDAEvent ready;
#if defined(USE_C10D_NCCL)
  c10::intrusive_ptr<c10d::Work> work;
#endif
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
  // Reverse registration order approximates backward order, and is identical
  // on every rank. Each bucket contains only one dtype.
  std::int64_t offset = 0;
  for (std::size_t reverse = master.size(); reverse > 0; --reverse) {
   const auto i = reverse - 1;
   const auto& tensor = master[i];
   if (!tensor.requires_grad() || !tensor.is_leaf() || !tensor.is_cuda() || tensor.device().index() != device || !tensor.is_floating_point() || !tensor.is_non_overlapping_and_dense())
    throw std::invalid_argument("gradient reducer requires active dense CUDA leaves");
   if (buckets.empty() || buckets.back().values.scalar_type() != tensor.scalar_type() ||
       (offset && static_cast<std::size_t>(offset + tensor.numel()) * tensor.element_size() > bucket_bytes)) {
    if (!buckets.empty()) buckets.back().values = torch::zeros({offset}, buckets.back().values.options());
    buckets.emplace_back(); buckets.back().values = torch::empty({0}, tensor.options().requires_grad(false)); offset = 0;
   }
   parameters[i] = {names[i], tensor, {}, buckets.size() - 1};
   buckets.back().parameters.push_back(i);
   offset += tensor.numel();
  }
  buckets.back().values = torch::zeros({offset}, buckets.back().values.options());
  for (auto& bucket : buckets) {
   bucket.collective = {bucket.values};
   offset = 0;
   for (auto i : bucket.parameters) {
    auto& p = parameters[i];
    p.view = bucket.values.as_strided(p.master.sizes(), p.master.strides(), offset);
    offset += p.master.numel();
   }
  }
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
#if defined(USE_C10D_NCCL)
   if (distributed.enabled) {
    // A throwing submission may have enqueued work before returning a handle.
    collectives_pending = true;
    bucket.work = distributed.process_group->allreduce(bucket.collective);
   }
#else
   if (distributed.enabled) throw std::runtime_error("gradient reduction requires NCCL");
#endif
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
 std::size_t expected = 0, admitted = 0, finished = 0, next_bucket = 0;
 bool active = false, counts_submitted = false, collectives_pending = false;
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
#if defined(USE_C10D_NCCL)
 // Aborting a communicator is not a CUDA completion event. An aborted Work
 // cannot supply a trustworthy stream join (and waiting on it may itself
 // fail). Keep the complete owner when any such collective was launched.
 if (impl_->collectives_pending) {
  std::move(terminal_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(impl_)), cudaErrorUnknown);
  return;
 }
#endif
 // The session drains worker futures before retirement. Retain all hook,
 // Work, tensor, event and stream custody on a terminal CUDA failure.
 cudaError_t status = cudaSuccess;
 try {
  tc::TorchCudaDeviceGuard device(tc::checked_device_index(impl_->device));
  // finish_attempt cleared collectives_pending only after every Work joined
  // this stream and the stream physically synchronized. Do not wait again on
  // already-settled Work after the session has shut down its process group.
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
 for (auto& bucket : p.buckets) { bucket.values.zero_(); bucket.closed = 0;
#if defined(USE_C10D_NCCL)
  bucket.work.reset();
#endif
 }
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
#if defined(USE_C10D_NCCL)
   if (bucket.work && !bucket.work->wait()) throw std::runtime_error("gradient collective completion failed");
#endif
  }
  p.usage.copy_(p.host_usage, true);
  distributed_all_reduce_tensor(p.distributed, p.usage);
  p.host_usage.copy_(p.usage, true);
  p.launch.synchronize();
  p.collectives_pending = false;
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
