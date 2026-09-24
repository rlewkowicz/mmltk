#pragma once
#include <cuda_runtime_api.h>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/frameworks/gpu/device_execution.h"
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
 CudaRuntimeResources(DirectComputeConfiguration, Close);
 ~CudaRuntimeResources() noexcept;
 [[nodiscard]] contracts::ComputeTerminal Run(Work, std::stop_token, bool settle = true);
 void CloseSession();
 [[nodiscard]] int device() const noexcept { return device_; }

private:
 using Command = std::move_only_function<void()>;
 void WithExecution(Command);
 void WithDevice(Command);
 DirectComputeConfiguration configuration_;
 int device_ = -1;
 cudaStream_t stream_ = nullptr;
 Close close_;
};
// Declaration order keeps the session alive while resources close it and settle
// the stream. The shared aggregate owns the physical lifecycle, not product policy.
template <class Session>
class CudaSessionRuntimeState {
public:
 explicit CudaSessionRuntimeState(const DirectComputeConfiguration config) : resources(config, [this] { static_cast<void>(session.Close()); }) {}
 Session session;
 CudaRuntimeResources resources;
};
}  // namespace mmltk::controller::detail
