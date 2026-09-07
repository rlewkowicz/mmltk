#include "numa_host_tensor.h"
#include "detail/torch_cuda_utils.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime_api.h>
#include <limits>
#include <algorithm>
#include <stdexcept>
#include "src/frameworks/gpu/device_execution.h"

namespace mmltk::backend::ml::cuda {
NumaHostTensor::NumaHostTensor(int device) : device_(device) {
    if (device_ < 0 && cudaGetDevice(&device_) != cudaSuccess) throw std::runtime_error("resolve NUMA host tensor device");
    c10::cuda::CUDAGuard guard(checked_device_index(device_));
    CUcontext context{};
    if (cuCtxGetCurrent(&context) != CUDA_SUCCESS || !context) throw std::runtime_error("NUMA host tensor requires a current CUDA context");
    storage_ = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
}
at::Tensor NumaHostTensor::view(at::IntArrayRef shape, at::ScalarType dtype) {
    std::size_t bytes = c10::elementSize(dtype);
    for (const auto size : shape) {
        if (size < 0 || (size && bytes > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(size)))
            throw std::invalid_argument("NUMA host tensor shape overflows");
        bytes *= static_cast<std::size_t>(size);
    }
    // Existing tensor views must never be invalidated by high-water growth.
    if (bytes > storage_->capacity_bytes() && storage_.use_count() != 1) {
        NumaHostTensor replacement(device_);
        replacement.storage_->ensure_bytes(std::max<std::size_t>(bytes, 1));
        storage_.swap(replacement.storage_);
    } else {
        storage_->ensure_bytes(std::max<std::size_t>(bytes, 1));
    }
    return at::from_blob(storage_->data(), shape, [storage = storage_](void*) {}, at::TensorOptions().dtype(dtype).device(at::kCPU));
}
std::size_t NumaHostTensor::capacity_bytes() const noexcept { return storage_->capacity_bytes(); }
CUresult NumaHostTensor::ReleaseSettled() noexcept { return storage_.use_count() == 1 ? storage_->ReleaseSettled() : CUDA_ERROR_NOT_READY; }
at::Tensor numa_empty(at::IntArrayRef shape, at::ScalarType dtype, int device) {
    NumaHostTensor owner(device);
    return owner.view(shape, dtype);
}
at::Tensor numa_readback(const at::Tensor& source) {
    if (!source.is_cuda()) return source;
    c10::cuda::CUDAGuard guard(source.device());
    auto result = numa_empty(source.sizes(), source.scalar_type(), source.get_device());
    result.copy_(source, true);
    const auto stream = c10::cuda::getCurrentCUDAStream(source.get_device());
    if (cudaStreamSynchronize(stream.stream()) != cudaSuccess) throw std::runtime_error("complete NUMA tensor readback");
    return result;
}
}  // namespace mmltk::backend::ml::cuda
