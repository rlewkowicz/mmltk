#pragma once
#include <cuda.h>
#include <cstddef>
#include <cstdint>
#include <memory>
namespace mmltk::frameworks::gpu::detail {
struct GdrMappingInfo final {
 CUdeviceptr base = 0;
 std::size_t bytes = 0;
 std::uint32_t page_size = 0;
 int mapping_type = 0;
 bool mapped = false;
};
// Physical driver boundary, injected by standard ownership/failure tests.
// Acquisition throws; release reports failure without relinquishing ownership.
class GdrBufferBackend {
public:
 virtual ~GdrBufferBackend() = default;
 virtual CUcontext current_context() = 0;
 virtual void push_context(CUcontext) = 0;
 virtual int pop_context() noexcept = 0;
 virtual int current_device() = 0;
 virtual void* open() = 0;
 virtual bool uses_dmabuf(void*) = 0;
 virtual void require_device_support(bool dmabuf, int device) = 0;
 virtual int close(void*) noexcept = 0;
 virtual CUdeviceptr allocate(std::size_t) = 0;
 virtual void sync_memops(CUdeviceptr) = 0;
 virtual int free(CUdeviceptr) noexcept = 0;
 virtual std::uintptr_t pin(void*, CUdeviceptr, std::size_t) = 0;
 virtual int unpin(void*, std::uintptr_t) noexcept = 0;
 virtual void* map(void*, std::uintptr_t, std::size_t) = 0;
 virtual GdrMappingInfo info(void*, std::uintptr_t) = 0;
 virtual int unmap(void*, std::uintptr_t, void*, std::size_t) noexcept = 0;
 virtual void copy(std::uintptr_t, void*, const void*, std::size_t) = 0;
 virtual CUevent create_event() = 0;
 virtual void record_event(CUevent, CUstream) = 0;
 virtual void wait_event(CUevent) = 0;
 virtual int destroy_event(CUevent) noexcept = 0;
 virtual void synchronize_context() = 0;
};
[[nodiscard]] std::shared_ptr<GdrBufferBackend> gdr_buffer_backend();
}  // namespace mmltk::frameworks::gpu::detail
