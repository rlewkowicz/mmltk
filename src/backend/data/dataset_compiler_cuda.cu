#include "detail/dataset_compiler_cuda.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/common/system/execution_policy.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "detail/dataset_compiler_cuda_bridge.h"

namespace mmltk::backend::data::compiler_internal {

namespace {

__global__ void resize_binary_masks_kernel(const uint8_t* input_masks, uint8_t* output_masks, const uint32_t* source_widths,
                                           const uint32_t* source_heights, const uint64_t* source_offsets, uint32_t target_width,
                                           uint32_t target_height, uint32_t batch_size, uint64_t target_pixels) {
    const uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
    const uint32_t mask_index = blockIdx.z;
    if (mask_index >= batch_size || x >= target_width || y >= target_height) { return; }

    const uint32_t source_width = source_widths[mask_index];
    const uint32_t source_height = source_heights[mask_index];
    const uint32_t source_y = min(source_height - 1, static_cast<uint32_t>(((static_cast<uint64_t>(2) * y + 1) * source_height) /
                                                                           (static_cast<uint64_t>(2) * target_height)));
    const uint32_t source_x = min(source_width - 1, static_cast<uint32_t>(((static_cast<uint64_t>(2) * x + 1) * source_width) /
                                                                          (static_cast<uint64_t>(2) * target_width)));
    const uint64_t source_offset = source_offsets[mask_index];
    const uint64_t source_index = source_offset + static_cast<uint64_t>(source_y) * source_width + source_x;
    const uint64_t output_index = static_cast<uint64_t>(mask_index) * target_pixels + static_cast<uint64_t>(y) * target_width + x;
    output_masks[output_index] = input_masks[source_index] == 0 ? uint8_t{0} : uint8_t{1};
}

}  // namespace

struct CudaBinaryMaskResizer::Impl {
    explicit Impl(int requested_device_id) : device_id(requested_device_id) {}

    int device_id = 0;
    mmltk::frameworks::gpu::DeviceExecution execution;
    std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> input_storage, output_storage, widths_storage, heights_storage,
        offsets_storage;
    cudaStream_t stream = nullptr;
    uint8_t* device_input = nullptr;
    uint8_t* device_output = nullptr;
    uint32_t* device_widths = nullptr;
    uint32_t* device_heights = nullptr;
    uint64_t* device_offsets = nullptr;
    uint8_t* host_input = nullptr;
    uint8_t* host_output = nullptr;
    uint32_t* host_widths = nullptr;
    uint32_t* host_heights = nullptr;
    uint64_t* host_offsets = nullptr;
    size_t device_input_capacity = 0;
    size_t device_output_capacity = 0;
    size_t device_widths_capacity = 0;
    size_t device_heights_capacity = 0;
    size_t device_offsets_capacity = 0;
    size_t host_input_capacity = 0;
    size_t host_output_capacity = 0;
    size_t host_widths_capacity = 0;
    size_t host_heights_capacity = 0;
    size_t host_offsets_capacity = 0;
};

void CudaBinaryMaskResizer::reset() noexcept {
    if (impl_ == nullptr) { return; }
    (void)cudaSetDevice(impl_->device_id);
    if (impl_->stream != nullptr) { (void)cudaStreamSynchronize(impl_->stream); }
    if (impl_->device_input != nullptr) { (void)cudaFree(impl_->device_input); }
    if (impl_->device_output != nullptr) { (void)cudaFree(impl_->device_output); }
    if (impl_->device_widths != nullptr) { (void)cudaFree(impl_->device_widths); }
    if (impl_->device_heights != nullptr) { (void)cudaFree(impl_->device_heights); }
    if (impl_->device_offsets != nullptr) { (void)cudaFree(impl_->device_offsets); }
    if (impl_->stream != nullptr) { (void)cudaStreamDestroy(impl_->stream); }
    delete impl_;
    impl_ = nullptr;
}

bool CudaBinaryMaskResizer::available() noexcept {
    int device_count = 0;
    return cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0;
}

CudaBinaryMaskResizer::CudaBinaryMaskResizer(int device_id) {
    auto candidate = std::make_unique<Impl>(device_id);
    int device_count = 0;
    cuda_bridge::require_cuda_success(cudaGetDeviceCount(&device_count), "cudaGetDeviceCount for mask resize");
    if (device_count <= 0) { throw std::runtime_error("CUDA mask resize requested but no CUDA devices are visible"); }
    if (device_id < 0 || device_id >= device_count) { throw std::runtime_error("CUDA mask resize device id is out of range"); }

    cuda_bridge::require_cuda_success(cudaSetDevice(device_id), "cudaSetDevice for mask resize");
    candidate->execution = mmltk::frameworks::gpu::resolve_device_execution(
        device_id, mmltk::common::system::NumaTopology::Capture(),
        mmltk::common::system::bound_memory_node(mmltk::common::system::capture_memory_policy()));
    const cudaError_t stream_status = cuda_bridge::create_compiler_stream(&candidate->stream, cudaStreamNonBlocking);
    if (stream_status != cudaSuccess) {
        if (candidate->stream != nullptr) {
            (void)cudaStreamDestroy(candidate->stream);
            candidate->stream = nullptr;
        }
        cuda_bridge::require_cuda_success(stream_status, "cudaStreamCreateWithPriority for mask resize");
    }
    impl_ = candidate.release();
}

CudaBinaryMaskResizer::~CudaBinaryMaskResizer() { reset(); }

CudaBinaryMaskResizer::CudaBinaryMaskResizer(CudaBinaryMaskResizer&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }

CudaBinaryMaskResizer& CudaBinaryMaskResizer::operator=(CudaBinaryMaskResizer&& other) noexcept {
    if (this == &other) { return *this; }
    reset();
    impl_ = other.impl_;
    other.impl_ = nullptr;
    return *this;
}

void CudaBinaryMaskResizer::resize_batch(const std::vector<uint8_t>& source_masks, const std::vector<uint32_t>& source_widths,
                                         const std::vector<uint32_t>& source_heights, const std::vector<size_t>& source_offsets,
                                         uint32_t target_width, uint32_t target_height, std::vector<uint8_t>& output_masks) {
    if (impl_ == nullptr) { throw std::runtime_error("CUDA mask resizer is not initialized"); }
    const size_t batch_size = source_widths.size();
    if (source_heights.size() != batch_size || source_offsets.size() != batch_size) {
        throw std::runtime_error("CUDA mask resize batch metadata is inconsistent");
    }
    if (target_width == 0 || target_height == 0) { throw std::runtime_error("CUDA mask resize target dimensions must be positive"); }
    if (batch_size == 0) {
        output_masks.clear();
        return;
    }
    if (batch_size > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error("CUDA mask resize batch size exceeds uint32_t range");
    }
    if (batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("CUDA mask resize batch is too large");
    }
    if (batch_size > 65535u) { throw std::runtime_error("CUDA mask resize batch exceeds maximum supported grid depth"); }

    const size_t target_pixels = static_cast<size_t>(target_width) * target_height;
    if (target_pixels == 0) { throw std::runtime_error("CUDA mask resize target pixel count must be positive"); }
    if (target_pixels > std::numeric_limits<size_t>::max() / batch_size) {
        throw std::runtime_error("CUDA mask resize output buffer size overflow");
    }
    const size_t output_bytes = target_pixels * batch_size;

    for (size_t index = 0; index < batch_size; ++index) {
        const size_t source_width = source_widths[index];
        const size_t source_height = source_heights[index];
        if (source_width == 0 || source_height == 0) { throw std::runtime_error("CUDA mask resize source dimensions must be positive"); }
        const size_t source_pixels = source_width * source_height;
        if (source_offsets[index] > source_masks.size() || source_pixels > source_masks.size() - source_offsets[index]) {
            throw std::runtime_error("CUDA mask resize source span is out of bounds");
        }
    }

    const auto& placement = impl_->execution.placement;
    mmltk::common::system::ScopedExecutionPolicy policy({placement.cpus, "mask-resize", 0, placement.numa_node, -10, false});
    cuda_bridge::require_cuda_success(cudaSetDevice(impl_->device_id), "cudaSetDevice for mask resize batch");

    auto ensure_host_buffer = [](std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer>& storage, void** pointer, size_t& capacity,
                                 size_t required) {
        if (!storage) storage = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
        storage->ensure_bytes(required);
        *pointer = storage->data();
        capacity = storage->capacity_bytes();
    };
    auto ensure_device_buffer = [&](void** pointer, size_t& capacity, size_t required, const char* context) {
        if (required <= capacity) { return; }
        if (*pointer != nullptr) {
            cuda_bridge::require_cuda_success(cudaFree(*pointer), context);
            *pointer = nullptr;
        }
        cuda_bridge::require_cuda_success(cudaMalloc(pointer, required), context);
        capacity = required;
    };

    ensure_host_buffer(impl_->input_storage, reinterpret_cast<void**>(&impl_->host_input), impl_->host_input_capacity, source_masks.size());
    ensure_host_buffer(impl_->output_storage, reinterpret_cast<void**>(&impl_->host_output), impl_->host_output_capacity, output_bytes);
    ensure_host_buffer(impl_->widths_storage, reinterpret_cast<void**>(&impl_->host_widths), impl_->host_widths_capacity,
                       batch_size * sizeof(uint32_t));
    ensure_host_buffer(impl_->heights_storage, reinterpret_cast<void**>(&impl_->host_heights), impl_->host_heights_capacity,
                       batch_size * sizeof(uint32_t));
    ensure_host_buffer(impl_->offsets_storage, reinterpret_cast<void**>(&impl_->host_offsets), impl_->host_offsets_capacity,
                       batch_size * sizeof(uint64_t));

    ensure_device_buffer(reinterpret_cast<void**>(&impl_->device_input), impl_->device_input_capacity, source_masks.size(),
                         "cudaMalloc for mask resize input");
    ensure_device_buffer(reinterpret_cast<void**>(&impl_->device_output), impl_->device_output_capacity, output_bytes,
                         "cudaMalloc for mask resize output");
    ensure_device_buffer(reinterpret_cast<void**>(&impl_->device_widths), impl_->device_widths_capacity, batch_size * sizeof(uint32_t),
                         "cudaMalloc for mask resize widths");
    ensure_device_buffer(reinterpret_cast<void**>(&impl_->device_heights), impl_->device_heights_capacity, batch_size * sizeof(uint32_t),
                         "cudaMalloc for mask resize heights");
    ensure_device_buffer(reinterpret_cast<void**>(&impl_->device_offsets), impl_->device_offsets_capacity, batch_size * sizeof(uint64_t),
                         "cudaMalloc for mask resize offsets");

    std::memcpy(impl_->host_input, source_masks.data(), source_masks.size());
    std::memcpy(impl_->host_widths, source_widths.data(), batch_size * sizeof(uint32_t));
    std::memcpy(impl_->host_heights, source_heights.data(), batch_size * sizeof(uint32_t));
    for (size_t index = 0; index < batch_size; ++index) {
        impl_->host_offsets[index] = static_cast<uint64_t>(source_offsets[index]);
    }

    cuda_bridge::require_cuda_success(
        cudaMemcpyAsync(impl_->device_input, impl_->host_input, source_masks.size(), cudaMemcpyHostToDevice, impl_->stream),
        "cudaMemcpyAsync H2D mask resize input");
    cuda_bridge::require_cuda_success(
        cudaMemcpyAsync(impl_->device_widths, impl_->host_widths, batch_size * sizeof(uint32_t), cudaMemcpyHostToDevice, impl_->stream),
        "cudaMemcpyAsync H2D mask resize widths");
    cuda_bridge::require_cuda_success(
        cudaMemcpyAsync(impl_->device_heights, impl_->host_heights, batch_size * sizeof(uint32_t), cudaMemcpyHostToDevice, impl_->stream),
        "cudaMemcpyAsync H2D mask resize heights");
    cuda_bridge::require_cuda_success(
        cudaMemcpyAsync(impl_->device_offsets, impl_->host_offsets, batch_size * sizeof(uint64_t), cudaMemcpyHostToDevice, impl_->stream),
        "cudaMemcpyAsync H2D mask resize offsets");

    const dim3 block(16, 16, 1);
    const dim3 grid((target_width + block.x - 1) / block.x, (target_height + block.y - 1) / block.y, static_cast<unsigned int>(batch_size));
    resize_binary_masks_kernel<<<grid, block, 0, impl_->stream>>>(impl_->device_input, impl_->device_output, impl_->device_widths,
                                                                  impl_->device_heights, impl_->device_offsets, target_width, target_height,
                                                                  static_cast<uint32_t>(batch_size), static_cast<uint64_t>(target_pixels));
    cuda_bridge::require_cuda_success(cudaGetLastError(), "resize_binary_masks_kernel launch");

    cuda_bridge::require_cuda_success(
        cudaMemcpyAsync(impl_->host_output, impl_->device_output, output_bytes, cudaMemcpyDeviceToHost, impl_->stream),
        "cudaMemcpyAsync D2H mask resize output");
    cuda_bridge::require_cuda_success(cudaStreamSynchronize(impl_->stream), "cudaStreamSynchronize for mask resize batch");

    output_masks.resize(output_bytes);
    std::memcpy(output_masks.data(), impl_->host_output, output_bytes);
}

}  // namespace mmltk::backend::data::compiler_internal
