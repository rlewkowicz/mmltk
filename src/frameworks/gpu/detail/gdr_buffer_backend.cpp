#include "src/frameworks/gpu/detail/gdr_buffer_backend.h"
#include "src/frameworks/gpu/gdr_mapped_buffer.h"
#include <gdrapi.h>
#include <cerrno>
#include <system_error>
#include <stdexcept>
#include <string>
namespace mmltk::frameworks::gpu::detail {
namespace {
void check_cuda(CUresult result, const char* operation) {
    if (result == CUDA_SUCCESS) return;
    const char* text = nullptr;
    (void)cuGetErrorString(result, &text);
    throw std::runtime_error(std::string(operation) + ": " + (text ? text : "CUDA driver failure"));
}
void check_gdr(int result, const char* operation) {
    if (result != 0) throw std::runtime_error(std::string(operation) + ": GDR status " + std::to_string(result));
}
class NativeGdrBackend final : public GdrBufferBackend {
   public:
    CUcontext current_context() override {
        CUcontext result{};
        check_cuda(cuCtxGetCurrent(&result), "get GDR owner context");
        return result;
    }
    void push_context(CUcontext context) override { check_cuda(cuCtxPushCurrent(context), "bind GDR owner context"); }
    int pop_context() noexcept override {
        CUcontext popped{};
        return static_cast<int>(cuCtxPopCurrent(&popped));
    }
    int current_device() override {
        CUdevice device{};
        check_cuda(cuCtxGetDevice(&device), "get GDR owner device");
        return device;
    }
    void* open() override {
        auto* handle = gdr_open();
        if (!handle) {
            const int failure = errno;
            if (failure == ENOTSUP)
                throw GdrTransportUnavailable(
                    "GDRCopy requires gdrdrv or CUDA 13.3+ DMA-BUF mmap on the selected GPU; omit --gdrcopy "
                    "for H2D");
            throw std::system_error(failure, std::generic_category(), "open GDR backend");
        }
        return handle;
    }
    bool uses_dmabuf(void* handle) override {
        int result{};
        check_gdr(gdr_get_attribute(static_cast<gdr_t>(handle), GDR_ATTR_USING_DMA_BUF_MMAP, &result), "query GDR backend");
        return result != 0;
    }
    void require_device_support(bool dmabuf, int device) override {
        int supported{};
        if (dmabuf) {
            int version{};
            check_cuda(cuDriverGetVersion(&version), "query DMA-BUF driver version");
            if (version < 13030) throw GdrTransportUnavailable("GDR DMA-BUF mmap requires driver 13.3+");
            check_cuda(cuDeviceGetAttribute(&supported, CU_DEVICE_ATTRIBUTE_DMA_BUF_MMAP_SUPPORTED, device), "query selected device DMA-BUF mmap support");
        } else {
            check_cuda(cuDeviceGetAttribute(&supported, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED, device), "query selected device GPUDirect support");
        }
        if (!supported) throw GdrTransportUnavailable("selected GPU does not support the required GDR backend");
    }
    int close(void* handle) noexcept override { return gdr_close(static_cast<gdr_t>(handle)); }
    CUdeviceptr allocate(std::size_t bytes) override {
        CUdeviceptr result{};
        check_cuda(cuMemAlloc(&result, bytes), "allocate GDR device storage");
        return result;
    }
    void sync_memops(CUdeviceptr allocation) override {
        unsigned value = 1;
        check_cuda(cuPointerSetAttribute(&value, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS, allocation), "enable synchronous memops on original GDR allocation");
    }
    int free(CUdeviceptr allocation) noexcept override { return static_cast<int>(cuMemFree(allocation)); }
    std::uintptr_t pin(void* handle, CUdeviceptr data, std::size_t bytes) override {
        gdr_mh_t mapping{};
        check_gdr(gdr_pin_buffer(static_cast<gdr_t>(handle), data, bytes, 0, 0, &mapping), "register GDR storage");
        return mapping.h;
    }
    int unpin(void* handle, std::uintptr_t mapping) noexcept override { return gdr_unpin_buffer(static_cast<gdr_t>(handle), {mapping}); }
    void* map(void* handle, std::uintptr_t mapping, std::size_t bytes) override {
        void* result{};
        check_gdr(gdr_map(static_cast<gdr_t>(handle), {mapping}, &result, bytes), "map GDR storage");
        return result;
    }
    GdrMappingInfo info(void* handle, std::uintptr_t mapping) override {
        gdr_info_t result{};
        check_gdr(gdr_get_info(static_cast<gdr_t>(handle), {mapping}, &result), "inspect GDR mapping");
        return {.base = result.va,
                .bytes = result.mapped_size,
                .page_size = result.page_size,
                .mapping_type = static_cast<int>(result.mapping_type),
                .mapped = result.mapped != 0 && result.mapping_type > GDR_MAPPING_TYPE_NONE && result.mapping_type < GDR_MAPPING_TYPE_MAX};
    }
    int unmap(void* handle, std::uintptr_t mapping, void* address, std::size_t bytes) noexcept override {
        return gdr_unmap(static_cast<gdr_t>(handle), {mapping}, address, bytes);
    }
    void copy(std::uintptr_t mapping, void* destination, const void* source, std::size_t bytes) override {
        check_gdr(gdr_copy_to_mapping({mapping}, destination, source, bytes), "copy into GDR mapping");
    }
    CUevent create_event() override {
        CUevent event{};
        check_cuda(cuEventCreate(&event, CU_EVENT_DISABLE_TIMING), "create GDR consumer event");
        return event;
    }
    void record_event(CUevent event, CUstream stream) override { check_cuda(cuEventRecord(event, stream), "record GDR consumption"); }
    void wait_event(CUevent event) override { check_cuda(cuEventSynchronize(event), "wait for GDR consumption"); }
    int destroy_event(CUevent event) noexcept override { return static_cast<int>(cuEventDestroy(event)); }
    void synchronize_context() override { check_cuda(cuCtxSynchronize(), "settle unrecorded GDR consumption"); }
};
}  // namespace
std::shared_ptr<GdrBufferBackend> gdr_buffer_backend() {
    // Immutable adapter, not a shared resource pool or CUDA context.
    static const auto backend = std::make_shared<NativeGdrBackend>();
    return backend;
}
}  // namespace mmltk::frameworks::gpu::detail
