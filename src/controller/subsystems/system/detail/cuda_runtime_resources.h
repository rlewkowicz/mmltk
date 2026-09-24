#pragma once
#include <cuda_runtime_api.h>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <stdexcept>
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
namespace mmltk::frameworks::gpu {
class TerminalCudaRetirementAuthority;
}
namespace mmltk::controller::detail {
// Reserve terminal custody before entering CUDA; retain the complete output
// owner if execution or caller-context restoration cannot be settled.
void RunWithRetainedCudaContext(std::shared_ptr<void>, mmltk::frameworks::gpu::TerminalCudaRetirementAuthority&, std::string_view, std::function_ref<void()>);
class CudaRuntimeResources final {
public:
 using Close = std::move_only_function<void()>;
 using Work = std::move_only_function<contracts::ComputeTerminal(mmltk::backend::ml::runtime::BorrowedCommandStream)>;
 struct Operations final {
  frameworks::gpu::CudaDeviceApi device = frameworks::gpu::cuda_runtime_device_api();
  void* context = nullptr;
  cudaError_t (*create)(void*, cudaStream_t*, unsigned) = [](void*, cudaStream_t* stream, unsigned flags) { return cudaStreamCreateWithFlags(stream, flags); };
  cudaError_t (*synchronize)(void*, cudaStream_t) = [](void*, cudaStream_t stream) { return cudaStreamSynchronize(stream); };
  cudaError_t (*destroy)(void*, cudaStream_t) = [](void*, cudaStream_t stream) { return cudaStreamDestroy(stream); };
 };
 CudaRuntimeResources(DirectComputeConfiguration, Close);
 CudaRuntimeResources(DirectComputeConfiguration, Close, Operations);
 ~CudaRuntimeResources() noexcept;
 CudaRuntimeResources(const CudaRuntimeResources&) = delete;
 CudaRuntimeResources& operator=(const CudaRuntimeResources&) = delete;
 [[nodiscard]] contracts::ComputeTerminal Run(Work, std::stop_token, bool settle = true);
 void Retire();
 [[nodiscard]] int device() const noexcept;
 [[nodiscard]] bool HasUnsafeCustody() const noexcept;

private:
 struct State;
 using Command = std::move_only_function<void()>;
 void WithExecution(Command);
 void WithDevice(Command);
 void Settle();
 void Release() noexcept;
 void Retain(cudaError_t) noexcept;
 std::shared_ptr<State> state_;
 frameworks::gpu::TerminalCudaRetirementOwner retirement_{1U};
 frameworks::gpu::TerminalCudaRetirementLease lease_;
};
// The close closure and resource state share the session itself. An unsafe
// aggregate therefore survives wrapper destruction and constructor unwinding.
template <class Session>
class CudaSessionRuntimeState {
 std::shared_ptr<Session> session_owner_ = std::make_shared<Session>();
public:
 explicit CudaSessionRuntimeState(const DirectComputeConfiguration config)
     : session(*session_owner_), resources(config, [owner = session_owner_] {
        if (static_cast<int>(owner->Close()) != 0) throw std::runtime_error("compute session CUDA retirement failed");
       }) {}
 Session& session;
 CudaRuntimeResources resources;
};
}  // namespace mmltk::controller::detail
