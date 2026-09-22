#include "tensor_readback.h"
#include "numa_host_tensor.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include "src/common/system/numa_memory.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
namespace mmltk::backend::ml::cuda {
namespace gpu = mmltk::frameworks::gpu;
namespace sys = mmltk::common::system;
namespace {
std::size_t add_bytes(std::size_t left, std::size_t right) {
 if (right > std::numeric_limits<std::size_t>::max() - left) throw std::overflow_error("tensor readback aggregate overflows");
 return left + right;
}
std::size_t tensor_bytes(const at::Tensor& tensor) {
 if (!tensor.defined() || tensor.layout() != at::kStrided || tensor.is_quantized() || (!tensor.is_cpu() && !tensor.is_cuda())) throw std::invalid_argument("unsupported tensor readback storage");
 std::size_t elements = 1;
 for (const auto extent : tensor.sizes()) {
  if (extent < 0 || (extent && elements > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(extent))) throw std::overflow_error("tensor readback shape overflows");
  elements *= static_cast<std::size_t>(extent);
 }
 const auto item = tensor.element_size();
 if (item && elements > std::numeric_limits<std::size_t>::max() / item) throw std::overflow_error("tensor readback byte extent overflows");
 return elements * item;
}
void check(cudaError_t status, const char* operation) {
 if (status != cudaSuccess) throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
}
class LocalMemory final {
public:
 explicit LocalMemory(int node) : previous_(sys::capture_memory_policy()) { sys::bind_memory_node(node); }
 ~LocalMemory() noexcept {
  try {
   sys::restore_memory_policy(previous_);
  } catch (...) { std::terminate(); }
 }

private:
 sys::MemoryPolicy previous_;
};
struct SourceDevice final {
 explicit SourceDevice(c10::DeviceIndex index) : device(index), stream(c10::cuda::getCurrentCUDAStream(index)), producer(stream) {
  c10::cuda::CUDAGuard guard(index);
  CUcontext current{};
  if (cuCtxGetCurrent(&current) != CUDA_SUCCESS || cuDevicePrimaryCtxRetain(&context, index) != CUDA_SUCCESS) throw std::runtime_error("retain tensor readback context");
  if (current != context) {
   (void)cuDevicePrimaryCtxRelease(index);
   context = nullptr;
   throw std::invalid_argument("Torch tensor readback requires its owning primary context");
  }
  try {
   numa_node = gpu::resolve_device_execution(index, sys::NumaTopology::Capture(), sys::bound_memory_node(sys::capture_memory_policy())).placement.numa_node;
   check(cudaStreamCreateWithFlags(&owned_stream, cudaStreamNonBlocking), "create tensor readback stream");
   check(cudaEventCreateWithFlags(&producer_ready, cudaEventDisableTiming), "create tensor readback producer event");
   stream = c10::cuda::getStreamFromExternal(owned_stream, index);
  } catch (...) {
   if (producer_ready) (void)cudaEventDestroy(producer_ready);
   if (owned_stream) (void)cudaStreamDestroy(owned_stream);
   (void)cuDevicePrimaryCtxRelease(index);
   context = nullptr;
   throw;
  }
 }
 ~SourceDevice() noexcept {
  if (!context) return;
  // Every ordinary destruction follows proven completion. Bind the exact
  // retained context, including when the last completed CPU reader lives
  // on a different thread/device. Terminal aggregates never reach here.
  if (cuCtxPushCurrent(context) != CUDA_SUCCESS) return;
  scratch = at::Tensor{};
  (void)cuEventDestroy(producer_ready);
  (void)cuStreamDestroy(owned_stream);
  CUcontext popped{};
  (void)cuCtxPopCurrent(&popped);
  (void)cuDevicePrimaryCtxRelease(device);
 }
 c10::DeviceIndex device;
 c10::cuda::CUDAStream stream;
 c10::cuda::CUDAStream producer;
 cudaStream_t owned_stream = nullptr;
 cudaEvent_t producer_ready = nullptr;
 CUcontext context{};
 at::Tensor scratch;
 std::size_t scratch_bytes = 0;
 bool pending = false;
 bool producer_dependency = false;
 int numa_node = -1;
 std::vector<std::size_t> requested_slots;
 std::size_t requested_scratch = 0;
};
struct Slot final {
 at::Tensor source;
 at::Tensor view;
 std::unique_ptr<NumaHostTensor> host;
 std::size_t bytes = 0;
 int device = -1;
 bool staged = false;
};
struct SnapshotStorage final {
 // Device custody outlives pinned slots and scratch tensor destruction.
 std::vector<std::shared_ptr<SourceDevice>> devices;
 std::vector<Slot> slots;
};
}  // namespace
struct TensorReadbackBuffers::Impl final {
 gpu::TerminalCudaRetirementOwner terminal{1};
 gpu::TerminalCudaRetirementLease lease = gpu::ReserveTerminalCudaLease(terminal);
 std::shared_ptr<SnapshotStorage> storage = std::make_shared<SnapshotStorage>();
 bool active = false;
 void require_open() const {
  if (!storage || !terminal.admission_open()) throw std::runtime_error("tensor readback admission closed after failed CUDA settlement");
 }
 SourceDevice& device(c10::DeviceIndex index) {
  for (auto& value : storage->devices)
   if (value->device == index) return *value;
  storage->devices.push_back(std::make_shared<SourceDevice>(index));
  return *storage->devices.back();
 }
 bool pending() const {
  return std::ranges::any_of(storage->devices, [](const auto& device) { return device->pending; });
 }
 void complete() {
  require_open();
  cudaError_t failure = cudaSuccess;
  for (auto& device : storage->devices) {
   if (!device->pending) continue;
   try {
    c10::cuda::CUDAGuard guard(device->device);
    auto status = cudaSuccess;
    if (!device->producer_dependency) status = cudaStreamSynchronize(device->producer.stream());
    const auto copied = cudaStreamSynchronize(device->stream.stream());
    if (status == cudaSuccess) status = copied;
    if (status == cudaSuccess) {
     device->pending = false;
     device->producer_dependency = false;
    } else if (failure == cudaSuccess)
     failure = status;
   } catch (...) {
    if (failure == cudaSuccess) failure = cudaErrorUnknown;
   }
  }
  if (failure != cudaSuccess) {
   std::move(lease).Install(gpu::TerminalCudaCustody::Share(std::move(storage)), failure);
   check(failure, "settle tensor readback");
  }
 }
 void release() {
  complete();
  // Torch archives keep TensorImpl handles until their actual destruction.
  for (const auto& slot : storage->slots)
   if (slot.host && slot.source.defined() && slot.source.is_cuda() && slot.bytes && slot.view.defined() && (slot.view.use_count() != 1 || slot.view.storage().use_count() != 1))
    throw std::logic_error("tensor readback still has serializer readers");
  for (auto& slot : storage->slots) {
   slot.source = at::Tensor{};
   slot.view = at::Tensor{};
   slot.staged = false;
  }
  active = false;
 }
 void release_capacity() {
  release();
  for (auto& slot : storage->slots) {
   if (slot.host && slot.host->ReleaseSettled() != CUDA_SUCCESS) throw std::runtime_error("release settled tensor readback pages");
   slot.host.reset();
  }
  storage->slots.clear();
  storage->devices.clear();
 }
};
TensorReadbackBuffers::TensorReadbackBuffers() : impl_(std::make_unique<Impl>()) {}
TensorReadbackBuffers::~TensorReadbackBuffers() noexcept {
 if (!impl_->storage) return;
 try {
  impl_->release_capacity();
 } catch (...) {
  // A serializer may still own a completed view. Its storage deleter retains
  // registered pages; only unproved DMA installs the complete aggregate.
 }
}
void TensorReadbackBuffers::Begin() {
 impl_->require_open();
 if (impl_->active) throw std::logic_error("tensor readback snapshot already active");
 impl_->active = true;
}
void TensorReadbackBuffers::Reserve(std::span<const at::Tensor> sources, std::size_t first_slot) {
 impl_->require_open();
 if (!impl_->active || impl_->pending()) throw std::logic_error("reserve tensor readback before submitting the next boundary");
 const auto end = add_bytes(first_slot, sources.size());
 auto& slots = impl_->storage->slots;
 // Inventory and aggregate page bounds are checked before any registration.
 std::size_t total = capacity_bytes();
 for (std::size_t index = 0; index < sources.size(); ++index) {
  const auto bytes = tensor_bytes(sources[index]);
  if (sources[index].is_cuda() && bytes) {
   const auto capacity = first_slot + index < slots.size() && slots[first_slot + index].host ? slots[first_slot + index].host->capacity_bytes() : 0;
   const auto rounded = sys::page_rounded_bytes(bytes);
   if (rounded > capacity) total = add_bytes(total, rounded - capacity);
  }
 }
 slots.resize(std::max(slots.size(), end));
 for (auto& device : impl_->storage->devices) {
  device->requested_slots.clear();
  device->requested_scratch = 0;
 }
 for (std::size_t index = 0; index < sources.size(); ++index) {
  auto& slot = slots[first_slot + index];
  if (slot.staged) throw std::logic_error("cannot replace staged tensor readback slot");
  slot.source = sources[index].detach();
  slot.bytes = tensor_bytes(slot.source);
  if (!slot.source.is_cuda() || !slot.bytes) continue;
  auto& device = impl_->device(slot.source.get_device());
  device.requested_slots.push_back(first_slot + index);
  if (!slot.source.is_contiguous() || slot.source.is_conj() || slot.source.is_neg()) device.requested_scratch = std::max(device.requested_scratch, slot.bytes);
 }
 for (const auto& device : impl_->storage->devices) {
  total = add_bytes(total, std::max(device->scratch_bytes, device->requested_scratch));
  if (device->requested_scratch > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) throw std::overflow_error("tensor packing scratch exceeds tensor extent");
 }
 // One placement/context scope and at most one scratch growth per device.
 for (auto& retained : impl_->storage->devices) {
  auto& device = *retained;
  if (device.requested_slots.empty()) continue;
  device.producer = c10::cuda::getCurrentCUDAStream(device.device);
  c10::cuda::CUDAStreamGuard guard(device.stream);
  LocalMemory placement(device.numa_node);
  for (const auto index : device.requested_slots) {
   auto& slot = slots[index];
   if (slot.host && (slot.device != device.device || slot.bytes > slot.host->capacity_bytes())) {
    slot.view = at::Tensor{};
    if (slot.host->ReleaseSettled() != CUDA_SUCCESS) throw std::logic_error("tensor readback capacity still borrowed");
    slot.host.reset();
   }
   if (!slot.host) slot.host = std::make_unique<NumaHostTensor>(device.device, retained);
   slot.device = device.device;
   slot.view = slot.host->view(slot.source.sizes(), slot.source.scalar_type());
  }
  if (device.requested_scratch > device.scratch_bytes) {
   device.scratch = at::Tensor{};
   device.scratch_bytes = 0;
   device.scratch = at::empty({static_cast<std::int64_t>(device.requested_scratch)}, at::TensorOptions().device(at::Device(at::kCUDA, device.device)).dtype(at::kByte));
   device.scratch_bytes = device.requested_scratch;
  }
 }
}
at::Tensor TensorReadbackBuffers::Stage(std::size_t index) {
 impl_->require_open();
 if (!impl_->active || index >= impl_->storage->slots.size()) throw std::out_of_range("tensor readback slot");
 auto& slot = impl_->storage->slots[index];
 if (slot.staged) return slot.view;
 if (!slot.source.defined()) throw std::logic_error("tensor readback slot not reserved");
 if (!slot.source.is_cuda()) {
  slot.view = slot.source.resolve_conj().resolve_neg().contiguous();
 } else if (!slot.bytes) {
  slot.view = at::empty(slot.source.sizes(), slot.source.options().device(at::kCPU));
 } else {
  auto& device = impl_->device(slot.source.get_device());
  c10::cuda::CUDAStreamGuard guard(device.stream);
  // Mark before the first operation, including a packing launch that may fail.
  const bool first = !device.pending;
  device.pending = true;
  try {
   if (first) {
    check(cudaEventRecord(device.producer_ready, device.producer.stream()), "record tensor readback producer");
    check(cudaStreamWaitEvent(device.stream.stream(), device.producer_ready, 0), "join tensor readback producer");
    device.producer_dependency = true;
   }
   at::Tensor packed;
   const void* source = slot.source.const_data_ptr();
   if (!slot.source.is_contiguous() || slot.source.is_conj() || slot.source.is_neg()) {
    packed = at::from_blob(device.scratch.mutable_data_ptr(), slot.source.sizes(), slot.source.options());
    packed.copy_(slot.source);
    source = packed.const_data_ptr();
   }
   check(cudaMemcpyAsync(slot.view.mutable_data_ptr(), source, slot.bytes, cudaMemcpyDeviceToHost, device.stream.stream()), "submit tensor readback");
  } catch (...) {
   const auto failure = std::current_exception();
   try {
    impl_->complete();
   } catch (...) {}
   std::rethrow_exception(failure);
  }
 }
 slot.staged = true;
 return slot.view;
}
void TensorReadbackBuffers::Complete() { impl_->complete(); }
void TensorReadbackBuffers::Release() { impl_->release(); }
void TensorReadbackBuffers::ReleaseSettled() { impl_->release_capacity(); }
bool TensorReadbackBuffers::admission_open() const noexcept { return impl_->terminal.admission_open(); }
std::size_t TensorReadbackBuffers::capacity_bytes() const noexcept {
 if (!impl_->storage) return 0;
 std::size_t total = 0;
 for (const auto& slot : impl_->storage->slots)
  if (slot.host) total += slot.host->capacity_bytes();
 return total;
}
}  // namespace mmltk::backend::ml::cuda
