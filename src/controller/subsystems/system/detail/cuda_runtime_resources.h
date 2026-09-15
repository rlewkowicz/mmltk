#pragma once

#include <cuda_runtime_api.h>

#include <functional>
#include <optional>
#include <stop_token>

#include "src/backend/ml/runtime/backend_factory.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/frameworks/gpu/device_execution.h"

namespace mmltk::controller::detail {

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

    std::optional<mmltk::frameworks::gpu::DeviceExecution> execution_;
    int device_ = -1;
    cudaStream_t stream_ = nullptr;
    Close close_;
};

// Declaration order keeps the session alive while resources close it and settle
// the stream. The shared aggregate owns the physical lifecycle, not product policy.
template <class Session>
class CudaSessionRuntimeState {
   public:
    explicit CudaSessionRuntimeState(const DirectComputeConfiguration config)
        : resources(config, [this] { static_cast<void>(session.Close()); }) {}

    Session session;
    CudaRuntimeResources resources;
};

}  // namespace mmltk::controller::detail
