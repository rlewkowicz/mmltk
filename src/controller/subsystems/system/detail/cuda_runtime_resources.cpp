#include "cuda_runtime_resources.h"
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>
#include "src/common/system/execution_policy.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_context_scope.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_authority.h"
namespace mmltk::controller::detail {
void RunWithRetainedCudaContext(std::shared_ptr<void> state, frameworks::gpu::TerminalCudaRetirementAuthority& retirement, std::string_view operation_name, std::function_ref<void()> operation) {
 namespace gpu = frameworks::gpu;
 auto lease = gpu::ReserveTerminalCudaLease(retirement);
 struct Custody {
  std::shared_ptr<void> state;
  gpu::TerminalCudaRetirementLease& lease;
 } custody{std::move(state), lease};
 gpu::CudaContextScope scope({&custody, [](void* value) noexcept {
                               auto& owner = *static_cast<Custody*>(value);
                               std::move(owner.lease).Install(gpu::TerminalCudaCustody::Share(std::move(owner.state)), cudaErrorUnknown);
                              }});
 try {
  scope.Run(operation);
 } catch (...) {
  if (!retirement.admission_open() || gpu::is_image_execution_failure(std::current_exception())) {
   if (lease) std::move(lease).Install(gpu::TerminalCudaCustody::Share(std::move(custody.state)), cudaErrorUnknown);
   throw mmltk::backend::ml::runtime::CudaOperationError{cudaErrorUnknown, operation_name};
  }
  throw;
 }
}
struct CudaRuntimeResources::State final {
 DirectComputeConfiguration configuration;
 Operations operations;
 int device = -1;
 cudaStream_t stream = nullptr;
 Close close;
 bool session_closed = false;
 bool retired = false;
 cudaError_t failure = cudaSuccess;
 void RecordFailure(cudaError_t status) noexcept { if (failure == cudaSuccess) failure = status; }
};
CudaRuntimeResources::CudaRuntimeResources(const DirectComputeConfiguration config, Close close) : CudaRuntimeResources(config, std::move(close), Operations{}) {}
CudaRuntimeResources::CudaRuntimeResources(const DirectComputeConfiguration config, Close close, Operations operations)
    : state_(std::make_shared<State>(State{config, operations, config.execution ? config.execution->device : -1, nullptr, std::move(close)})),
      lease_(frameworks::gpu::ReserveTerminalCudaLease(retirement_)) {
 if (!config.valid()) throw contracts::UnavailableError("compute CUDA device is unavailable");
 if (!operations.create || !operations.synchronize || !operations.destroy) throw std::invalid_argument("compute CUDA operations are unavailable");
 try { WithExecution([this] {
  const auto status = state_->operations.create(state_->operations.context, &state_->stream, cudaStreamNonBlocking);
  if (status != cudaSuccess) {
   if (state_->stream) Retain(status);
   throw mmltk::backend::ml::runtime::CudaOperationError{status, "compute stream creation"};
  }
 }); } catch (...) {
  const auto failure = std::current_exception();
  if (state_->stream) Release();
  if (HasUnsafeCustody()) throw frameworks::gpu::ImageStreamExecutionFailure(failure);
  std::rethrow_exception(failure);
 }
}
CudaRuntimeResources::~CudaRuntimeResources() noexcept { Release(); }
void CudaRuntimeResources::Release() noexcept {
 if (HasUnsafeCustody()) return;
 try { Retire(); } catch (...) { Retain(cudaErrorUnknown); }
}
int CudaRuntimeResources::device() const noexcept { return state_->device; }
bool CudaRuntimeResources::HasUnsafeCustody() const noexcept { return !retirement_.admission_open(); }
void CudaRuntimeResources::Retain(cudaError_t failure) noexcept {
 state_->RecordFailure(failure);
 if (lease_) std::move(lease_).Install(frameworks::gpu::TerminalCudaCustody::Share(std::shared_ptr<State>{state_}), failure);
}
void CudaRuntimeResources::Settle() {
 if (!state_->stream) return;
 const auto status = state_->operations.synchronize(state_->operations.context, state_->stream);
 if (status != cudaSuccess) { Retain(status); throw mmltk::backend::ml::runtime::CudaOperationError{status, "compute stream settlement"}; }
}
contracts::ComputeTerminal CudaRuntimeResources::Run(Work work, const std::stop_token stop, bool settle) {
 if (HasUnsafeCustody()) throw contracts::UnavailableError("compute CUDA custody is unproved");
 if (state_->session_closed) throw contracts::UnavailableError("compute CUDA resources are retired");
 if (stop.stop_requested()) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
 contracts::ComputeTerminal result;
 WithExecution([&] {
  try { result = work({.native_handle = reinterpret_cast<std::uintptr_t>(state_->stream), .valid = true}); }
  catch (...) {
   const auto failure = std::current_exception();
   bool unproved = frameworks::gpu::is_image_execution_failure(failure);
   try { std::rethrow_exception(failure); }
   catch (const mmltk::backend::ml::runtime::CudaOperationError& error) {
    unproved = unproved || frameworks::gpu::cuda_custody_unproved(static_cast<cudaError_t>(error.status()));
   }
   catch (const frameworks::gpu::CudaContextFailure& error) { unproved = unproved || error.terminal(); }
   catch (...) {}
   if (unproved) { Retain(cudaErrorUnknown); std::rethrow_exception(failure); }
   // Ordinary work can throw after submission. Prove completion before reset;
   // terminal context failures above must not issue another CUDA operation.
   Settle();
   std::rethrow_exception(failure);
  }
  if (settle) Settle();
 });
 return stop.stop_requested() ? contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled, 0U, result.completed) : result;
}
void CudaRuntimeResources::Retire() {
 if (state_->retired) return;
 if (HasUnsafeCustody()) throw contracts::UnavailableError("compute CUDA custody is unproved");
 WithDevice([this] {
  Settle();
  try {
   if (!state_->session_closed && state_->close) state_->close();
   state_->session_closed = true;
  }
  catch (...) { Retain(cudaErrorUnknown); throw; }
  if (state_->stream) {
   const auto status = state_->operations.destroy(state_->operations.context, state_->stream);
   if (status != cudaSuccess) { Retain(status); throw mmltk::backend::ml::runtime::CudaOperationError{status, "compute stream destruction"}; }
   state_->stream = nullptr;
  }
 });
 // WithDevice has finalized caller restoration before retirement is observable.
 state_->retired = true;
}
void CudaRuntimeResources::WithExecution(Command work) {
 mmltk::common::system::ScopedExecutionPolicy policy(*state_->configuration.worker_policy());
 WithDevice(std::move(work));
}
void CudaRuntimeResources::WithDevice(Command work) {
 namespace gpu = frameworks::gpu;
 gpu::CudaDeviceScope scope{gpu::make_cuda_device_owner<State, &State::RecordFailure>(state_.get(), state_->device), state_->operations.device};
 std::exception_ptr failure;
 if (scope) {
  try { work(); } catch (...) { failure = std::current_exception(); }
 } else {
  state_->RecordFailure(scope.status());
 }
 const auto status = scope.Finalize();
 if (!scope.restored() || state_->failure != cudaSuccess) Retain(state_->failure != cudaSuccess ? state_->failure : status);
 if (failure) std::rethrow_exception(failure);
 if (status != cudaSuccess) throw mmltk::backend::ml::runtime::CudaOperationError{status, "compute device restoration"};
 if (HasUnsafeCustody()) throw contracts::UnavailableError("compute CUDA custody is unproved");
}
}  // namespace mmltk::controller::detail
