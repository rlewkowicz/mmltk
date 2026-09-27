#include "src/frameworks/gpu/memory/pinned_host_buffer.h"
#include <stdexcept>
#include <utility>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include "src/common/io/scoped_fd.h"
#include "src/common/system/numa_memory.h"
#include "src/frameworks/gpu/cuda/device_execution.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
namespace mmltk::frameworks::gpu {
namespace {
void check(CUresult error, const char* operation) {
 if (error != CUDA_SUCCESS) throw std::runtime_error(std::string(operation) + ": CUDA status " + std::to_string(error));
}
}  // namespace
struct PinnedHostBuffer::State {
 State(CUcontext owner, int node, bool shared) : context(owner), memory(node), portable(shared) {}
 CUcontext context;
 mmltk::common::system::NumaMemory memory;
 bool portable;
 bool registered = false;
 bool unsafe = false;
};
struct PinnedHostBuffer::Retention {
 std::shared_ptr<TerminalCudaRetirementOwner> terminal;
 TerminalCudaRetirementLease lease;
 mmltk::common::io::ScopedFd trace;
 Retention(std::shared_ptr<TerminalCudaRetirementOwner> owner) : terminal(owner ? std::move(owner) : std::make_shared<TerminalCudaRetirementOwner>(2U)), lease(ReserveTerminalCudaLease(*terminal)) {
  if (const auto* path = std::getenv("MMLTK_NUMA_TRANSFER_TRACE_FILE"); path && *path) trace.reset(::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
 }
};
PinnedHostBuffer::PinnedHostBuffer(
 CUcontext context, const mmltk::common::system::ExecutionPlacement& placement, bool portable, Register registration, std::shared_ptr<TerminalCudaRetirementOwner> retirement, Operations operations)
    : operations_(operations),
      registration_(registration),
      placement_(placement),
      retention_(std::make_unique<Retention>(std::move(retirement))),
      state_(std::make_shared<State>(context, placement.numa_node, portable)) {
 if (!context || !registration || !operations.synchronize || !operations.unregister || placement.cpus.empty())
  throw std::invalid_argument("pinned host buffer requires an owning context and placement");
}
PinnedHostBuffer::~PinnedHostBuffer() noexcept {
 if (!state_->registered || state_->unsafe) return;
 static_cast<void>(Release(true));
}
void PinnedHostBuffer::Retain() noexcept {
 state_->unsafe = true;
 if (retention_->lease) std::move(retention_->lease).Install(TerminalCudaCustody::Share(std::shared_ptr<State>{state_}), cudaErrorUnknown);
}
std::unique_ptr<PinnedHostBuffer> PinnedHostBuffer::ForCurrentDevice(bool portable, std::shared_ptr<TerminalCudaRetirementOwner> retirement, Operations operations) {
 CUcontext context{};
 CUdevice device{};
 check(cuCtxGetCurrent(&context), "resolve local host allocation context");
 check(cuCtxGetDevice(&device), "resolve local host allocation device");
 const int node = mmltk::common::system::bound_memory_node(mmltk::common::system::capture_memory_policy());
 const auto execution = resolve_device_execution(device, mmltk::common::system::NumaTopology::Capture(), node);
 return std::make_unique<PinnedHostBuffer>(context, execution.placement, portable, &cuMemHostRegister, std::move(retirement), operations);
}
void PinnedHostBuffer::ensure_bytes(std::size_t bytes) {
 if (state_->unsafe || !retention_->terminal->admission_open()) throw std::runtime_error("pinned host custody is unproved");
 if (bytes <= capacity_bytes()) return;
 // The candidate owns its own physical retirement lease before registration.
 // Failure at any subsequent boundary leaves both allocations under RAII custody.
 PinnedHostBuffer replacement(state_->context, placement_, state_->portable, registration_, retention_->terminal, operations_);
 replacement.state_->memory.ensure_bytes(bytes);
 CudaContextScope scope({&replacement, [](void* owner) noexcept { static_cast<PinnedHostBuffer*>(owner)->Retain(); }}, operations_.context);
 scope.Run([&] {
  scope.Select(state_->context);
  check(registration_(replacement.data(), replacement.capacity_bytes(), state_->portable ? CU_MEMHOSTREGISTER_PORTABLE : 0), "register strictly local host pages");
  replacement.state_->registered = true;
  replacement.log("registered", bytes);
 });
 check(Release(true), "release pinned host growth");
 state_.swap(replacement.state_);
}
void PinnedHostBuffer::log(const char* event, std::size_t active_bytes) const noexcept {
 if (retention_->trace.get() < 0) return;
 char record[384];
 const int size = std::snprintf(record, sizeof(record),
  "{\"event\":\"%s\",\"owner_context\":%llu,\"node\":%d,\"allocation\":%llu,\"capacity_bytes\":%zu,"
  "\"active_bytes\":%zu,\"portable\":%s,\"pages\":\"owned-local-verified\"}\n",
  event, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(state_->context)), node(), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(data())), capacity_bytes(),
  active_bytes, state_->portable ? "true" : "false");
 if (size > 0 && static_cast<std::size_t>(size) < sizeof(record)) {
  const auto written = ::write(retention_->trace.get(), record, static_cast<std::size_t>(size));
  (void)written;
 }
}
CUresult PinnedHostBuffer::ReleaseSettled() noexcept { return Release(false); }
CUresult PinnedHostBuffer::Release(bool synchronize) noexcept {
 if (state_->unsafe) return CUDA_ERROR_UNKNOWN;
 if (!state_->registered) {
  state_->memory.reset();
  return CUDA_SUCCESS;
 }
 if (!retention_->terminal->admission_open()) {
  Retain();
  return CUDA_ERROR_UNKNOWN;
 }
 CUresult result = CUDA_SUCCESS;
 try {
  CudaContextScope scope({this, [](void* owner) noexcept { static_cast<PinnedHostBuffer*>(owner)->Retain(); }}, operations_.context);
  scope.Run([&] {
   scope.Select(state_->context);
   if (synchronize) result = operations_.synchronize(operations_.context.context);
   if (result != CUDA_SUCCESS) return;
   result = operations_.unregister(operations_.context.context, state_->memory.data());
   if (result != CUDA_SUCCESS) return;
   state_->registered = false;
   log("unregistered");
  });
 } catch (...) { result = CUDA_ERROR_UNKNOWN; }
 if (result != CUDA_SUCCESS) {
  Retain();
  return result;
 }
 state_->memory.reset();
 return CUDA_SUCCESS;
}
void* PinnedHostBuffer::data() const noexcept { return state_->memory.data(); }
std::size_t PinnedHostBuffer::capacity_bytes() const noexcept { return state_->memory.capacity_bytes(); }
int PinnedHostBuffer::node() const noexcept { return state_->memory.node(); }
}  // namespace mmltk::frameworks::gpu
