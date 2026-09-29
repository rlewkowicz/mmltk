#include "cuda_test_gate.h"
#include <cuda/atomic>
#include <cuda_runtime.h>
#include <stdexcept>
namespace mmltk::testsupport {
namespace {
__global__ void await_release(std::uint32_t* word) {
 cuda::atomic_ref<std::uint32_t, cuda::thread_scope_system> released(*word);
 while (!released.load(cuda::memory_order_acquire)) __nanosleep(1000);
}
}  // namespace
CudaTestGate::CudaTestGate() {
 if (cudaMallocManaged(&word_, sizeof(*word_)) != cudaSuccess) throw std::runtime_error("cannot allocate CUDA test gate");
 *word_ = 0;
}
CudaTestGate::~CudaTestGate() {
 release();
 if (stream_) (void)cudaStreamSynchronize(stream_);
 (void)cudaFree(word_);
}
void CudaTestGate::hold(cudaStream_t stream) {
 if (stream_) throw std::logic_error("CUDA test gate already holds a stream");
 await_release<<<1, 1, 0, stream>>>(word_);
 if (cudaGetLastError() != cudaSuccess) throw std::runtime_error("cannot hold CUDA test stream");
 stream_ = stream;
}
void CudaTestGate::release() noexcept { cuda::atomic_ref<std::uint32_t, cuda::thread_scope_system>(*word_).store(1U, cuda::memory_order_release); }
}  // namespace mmltk::testsupport
