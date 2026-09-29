#pragma once
#include <cuda_runtime_api.h>
#include <cstdint>
namespace mmltk::testsupport {
// Holds one compute stream until the CPU releases it. Independent DMA remains
// runnable, and failure cleanup never needs another GPU submission.
class CudaTestGate final {
public:
 CudaTestGate();
 ~CudaTestGate();
 CudaTestGate(const CudaTestGate&) = delete;
 CudaTestGate& operator=(const CudaTestGate&) = delete;
 void hold(cudaStream_t);
 void release() noexcept;

private:
 std::uint32_t* word_ = nullptr;
 cudaStream_t stream_ = nullptr;
};
}  // namespace mmltk::testsupport
