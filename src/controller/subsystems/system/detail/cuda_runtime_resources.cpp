#include "cuda_runtime_resources.h"
#include <cstdint>
#include <stdexcept>
#include <utility>
#include "src/common/system/execution_policy.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
namespace mmltk::controller::detail {
CudaRuntimeResources::CudaRuntimeResources(const DirectComputeConfiguration config, Close close)
    : configuration_(config), device_(config.execution ? config.execution->device : -1), close_(std::move(close)) {
    if (!config.valid()) throw contracts::UnavailableError("compute CUDA device is unavailable");
    WithExecution([this] {
        const auto status = cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking);
        if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
    });
}
CudaRuntimeResources::~CudaRuntimeResources() noexcept {
    try {
        WithDevice([this] {
            if (close_) {
                try {
                    close_();
                } catch (...) {}
            }
            if (stream_ != nullptr) {
                static_cast<void>(cudaStreamSynchronize(stream_));
                static_cast<void>(cudaStreamDestroy(stream_));
                stream_ = nullptr;
            }
        });
    } catch (...) {}
}
contracts::ComputeTerminal CudaRuntimeResources::Run(Work work, const std::stop_token stop, bool settle) {
    if (stop.stop_requested()) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
    contracts::ComputeTerminal result;
    WithExecution([&] {
        result = work({.native_handle = reinterpret_cast<std::uintptr_t>(stream_), .valid = true});
        if (settle) {
            const auto status = cudaStreamSynchronize(stream_);
            if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
        }
    });
    return stop.stop_requested() ? contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled, 0U, result.completed) : result;
}
void CudaRuntimeResources::CloseSession() {
    WithDevice([this] {
        if (close_) close_();
    });
}
void CudaRuntimeResources::WithExecution(Command work) {
    mmltk::common::system::ScopedExecutionPolicy policy(*configuration_.worker_policy());
    WithDevice(std::move(work));
}
void CudaRuntimeResources::WithDevice(Command work) {
    frameworks::gpu::CudaDeviceScope scope{device_};
    if (!scope) throw std::runtime_error(cudaGetErrorString(scope.status()));
    try {
        work();
    } catch (...) {
        static_cast<void>(scope.Finalize());
        throw;
    }
    const auto status = scope.Finalize();
    if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}
}  // namespace mmltk::controller::detail
