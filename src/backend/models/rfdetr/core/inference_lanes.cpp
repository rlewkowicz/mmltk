#include "src/backend/models/rfdetr/core/inference_lanes.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
#include "src/frameworks/gpu/cuda/cuda_priority.h"
#include "src/frameworks/gpu/cuda/cuda_context_scope.h"
#include <cuda_runtime_api.h>
#include "src/pch_std.h"
namespace mmltk::backend::models::rfdetr {
namespace runtime = mmltk::backend::ml::runtime;
namespace {
void check(cudaError_t status, const char* operation) {
 if (status != cudaSuccess) throw runtime::CudaOperationError(status, operation);
}
}  // namespace
struct InferenceLanes::State final {
 struct Lane final {
  cudaStream_t stream = nullptr;
  cudaEvent_t source_read = nullptr, completed = nullptr;
  bool submitted = false;
 };
 explicit State(int value) : device(value) {}
 int device;
 std::vector<Lane> lanes;
 std::size_t head = 0, count = 0;
 bool closed = false, unproved_context = false;
 [[nodiscard]] mmltk::frameworks::gpu::CudaContextOwner context_owner() noexcept {
  return {this, [](void* state) noexcept { static_cast<State*>(state)->unproved_context = true; }};
 }
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement{1};
 mmltk::frameworks::gpu::TerminalCudaRetirementLease lease = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement);
};
InferenceLanes::InferenceLanes(int device, std::size_t capacity) : state_(std::make_shared<State>(device)) {
 if (device < 0 || !capacity) throw std::invalid_argument("inference lanes require a device and positive capacity");
 try {
  mmltk::frameworks::gpu::CudaContextScope context(state_->context_owner());
  context.Run([&] {
   check(cudaSetDevice(device), "inference lane device");
   state_->lanes.resize(capacity);
   for (auto& lane : state_->lanes) {
    check(cudaStreamCreateWithPriority(&lane.stream, cudaStreamNonBlocking, mmltk::frameworks::gpu::current_cuda_highest_stream_priority()), "create inference lane stream");
    check(cudaEventCreateWithFlags(&lane.source_read, cudaEventDisableTiming), "create inference source event");
    check(cudaEventCreateWithFlags(&lane.completed, cudaEventDisableTiming), "create inference completion event");
   }
  });
 } catch (...) {
  retire();
  throw;
 }
}
InferenceLanes::~InferenceLanes() { retire(); }
void InferenceLanes::retire() noexcept {
 if (Close() != runtime::kRuntimeSuccess) {
  // CLEANUP-IGNORE: Install already owns shared CUDA custody; this lease belongs to lane streams, not validation sessions.
  auto lease = std::move(state_->lease);
  std::move(lease).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(state_)), cudaErrorUnknown);
 }
}
std::size_t InferenceLanes::capacity() const noexcept { return state_->lanes.size(); }
std::size_t InferenceLanes::pending() const noexcept { return state_->count; }
void InferenceLanes::RestrictCapacity(std::size_t capacity) {
 if (!capacity || capacity > state_->lanes.size() || state_->count || state_->closed) throw std::logic_error("invalid inference source capacity");
 while (state_->lanes.size() > capacity) {
  auto& lane = state_->lanes.back();
  check(cudaEventDestroy(lane.source_read), "retire unused inference source event");
  lane.source_read = nullptr;
  check(cudaEventDestroy(lane.completed), "retire unused inference completion event");
  lane.completed = nullptr;
  check(cudaStreamDestroy(lane.stream), "retire unused inference lane");
  lane.stream = nullptr;
  state_->lanes.pop_back();
 }
}
runtime::BorrowedCommandStream InferenceLanes::stream(std::size_t lane) const {
 if (state_->closed) throw std::logic_error("inference lane pool is closed");
 return {reinterpret_cast<std::uintptr_t>(state_->lanes.at(lane).stream), true};
}
std::size_t InferenceLanes::Admit() {
 if (state_->closed || state_->count == capacity()) throw std::logic_error("inference lane capacity exhausted");
 const auto index = (state_->head + state_->count) % capacity();
 ++state_->count;
 state_->lanes[index].submitted = false;
 return index;
}
void InferenceLanes::WaitSource(std::size_t index, runtime::BorrowedCommandStream producer) {
 auto& lane = state_->lanes.at(index);
 check(cudaEventRecord(lane.source_read, reinterpret_cast<cudaStream_t>(producer.native_handle)), "record source readiness");
 check(cudaStreamWaitEvent(lane.stream, lane.source_read, 0), "wait for inference source readiness");
}
void InferenceLanes::ReleaseSource(std::size_t index, runtime::BorrowedCommandStream producer) {
 auto& lane = state_->lanes.at(index);
 check(cudaEventRecord(lane.source_read, lane.stream), "record inference source read");
 if (producer) check(cudaStreamWaitEvent(reinterpret_cast<cudaStream_t>(producer.native_handle), lane.source_read, 0), "order decoder reuse after inference source copy");
 check(cudaEventSynchronize(lane.source_read), "complete inference source read");
}
void InferenceLanes::Submitted(std::size_t index) {
 auto& lane = state_->lanes.at(index);
 check(cudaEventRecord(lane.completed, lane.stream), "record inference completion");
 lane.submitted = true;
}
std::size_t InferenceLanes::WaitOldest() {
 if (!state_->count) throw std::logic_error("no inference batch to deliver");
 auto& lane = state_->lanes[state_->head];
 if (!lane.submitted) throw std::logic_error("inference batch was not submitted completely");
 check(cudaEventSynchronize(lane.completed), "complete inference batch");
 return state_->head;
}
void InferenceLanes::ReleaseOldest() {
 if (!state_->count) throw std::logic_error("no inference batch to release");
 auto& lane = state_->lanes[state_->head];
 // Receiver copies and deferred mask work may follow the forward event.
 check(cudaStreamSynchronize(lane.stream), "complete inference delivery readers");
 lane.submitted = false;
 state_->head = (state_->head + 1) % capacity();
 --state_->count;
}
runtime::RuntimeStatus InferenceLanes::Drain() noexcept {
 if (!state_) return runtime::kRuntimeSuccess;
 if (state_->unproved_context) return static_cast<runtime::RuntimeStatus>(cudaErrorUnknown);
 if (state_->closed) return runtime::kRuntimeSuccess;
 try {
  mmltk::frameworks::gpu::CudaContextScope context(state_->context_owner());
  return context.Run([&]() -> runtime::RuntimeStatus {
   auto status = cudaSetDevice(state_->device);
   if (status != cudaSuccess) return static_cast<runtime::RuntimeStatus>(status);
   for (auto& lane : state_->lanes) {
    if (lane.stream) status = cudaStreamSynchronize(lane.stream);
    if (status != cudaSuccess) return static_cast<runtime::RuntimeStatus>(status);
   }
   state_->count = 0;
   state_->head = 0;
   return runtime::kRuntimeSuccess;
  });
 } catch (...) { return static_cast<runtime::RuntimeStatus>(cudaErrorUnknown); }
}
runtime::RuntimeStatus InferenceLanes::Close() noexcept {
 const auto drained = Drain();
 if (drained != runtime::kRuntimeSuccess || !state_ || state_->closed) return drained;
 try {
  mmltk::frameworks::gpu::CudaContextScope context(state_->context_owner());
  return context.Run([&]() -> runtime::RuntimeStatus {
   const auto selected = cudaSetDevice(state_->device);
   if (selected != cudaSuccess) return static_cast<runtime::RuntimeStatus>(selected);
   for (auto& lane : state_->lanes) {
    for (auto* event : {&lane.source_read, &lane.completed})
     if (*event) {
      const auto status = cudaEventDestroy(*event);
      if (status != cudaSuccess) return static_cast<runtime::RuntimeStatus>(status);
      *event = nullptr;
     }
    if (lane.stream) {
     const auto status = cudaStreamDestroy(lane.stream);
     if (status != cudaSuccess) return static_cast<runtime::RuntimeStatus>(status);
     lane.stream = nullptr;
    }
   }
   state_->closed = true;
   return runtime::kRuntimeSuccess;
  });
 } catch (...) { return static_cast<runtime::RuntimeStatus>(cudaErrorUnknown); }
}
}  // namespace mmltk::backend::models::rfdetr
