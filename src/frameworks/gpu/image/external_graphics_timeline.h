#pragma once
#include <cuda_runtime_api.h>
#include <cstdint>
#include "src/common/io/scoped_fd.h"
namespace mmltk::frameworks::gpu {
namespace test_support {
struct ExternalGraphicsTimelineTestAccess;
}
class ExternalGraphicsTimeline final {
public:
 ExternalGraphicsTimeline() noexcept = default;
 explicit ExternalGraphicsTimeline(mmltk::common::io::ScopedFd descriptor);
 ~ExternalGraphicsTimeline() noexcept;
 ExternalGraphicsTimeline(const ExternalGraphicsTimeline&) = delete;
 ExternalGraphicsTimeline& operator=(const ExternalGraphicsTimeline&) = delete;
 ExternalGraphicsTimeline(ExternalGraphicsTimeline&&) noexcept;
 ExternalGraphicsTimeline& operator=(ExternalGraphicsTimeline&&) noexcept;
 [[nodiscard]] bool valid() const noexcept;
 [[nodiscard]] cudaError_t Release() noexcept;
 void SignalReady(cudaStream_t, std::uint64_t ready_value);
 void WaitForRelease(cudaStream_t, std::uint64_t release_value);
 void SignalReadyAndWaitForRelease(cudaStream_t, std::uint64_t ready_value);

private:
 using ImportOperation = cudaError_t (*)(cudaExternalSemaphore_t*, const cudaExternalSemaphoreHandleDesc*);
 ExternalGraphicsTimeline(mmltk::common::io::ScopedFd, ImportOperation);
 cudaExternalSemaphore_t semaphore_ = nullptr;
 friend struct test_support::ExternalGraphicsTimelineTestAccess;
};
}  // namespace mmltk::frameworks::gpu
