#include "src/frameworks/gpu/exported_image_buffer.h"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <string_view>

namespace mmltk::frameworks::gpu {
namespace {

void set_driver_error(std::string* const error_message, const char* const operation, const CUresult status) {
    if (error_message == nullptr) return;
    const char* detail = nullptr;
    if (cuGetErrorString(status, &detail) != CUDA_SUCCESS || detail == nullptr) detail = "unknown CUDA driver error";
    error_message->assign(operation);
    error_message->append(": ");
    error_message->append(std::string_view{detail});
}

[[nodiscard]] CUmemAllocationProp exportable_property(const int device_id) noexcept {
    CUmemAllocationProp property{};
    property.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    property.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    property.location.id = device_id;
    property.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    return property;
}

}  // namespace

ExportedImageBuffer::~ExportedImageBuffer() noexcept { static_cast<void>(Release()); }

bool ExportedImageBuffer::allocate(const int device_id, const std::uint32_t width, const std::uint32_t height,
                                   const std::size_t pitch_bytes, const std::size_t minimum_allocation_bytes,
                                   std::string* const error_message, const std::size_t offset_bytes) {
    if (!empty() || owns_resources()) {
        if (error_message != nullptr) *error_message = "presentation allocation was already made";
        return false;
    }
    if (device_id < 0 || width == 0U || height == 0U || pitch_bytes < static_cast<std::size_t>(width) * 4U ||
        height > (std::numeric_limits<std::size_t>::max() - offset_bytes) / pitch_bytes) {
        if (error_message != nullptr) *error_message = "presentation allocation metadata is invalid";
        return false;
    }
    const std::size_t requested = std::max(offset_bytes + pitch_bytes * static_cast<std::size_t>(height), minimum_allocation_bytes);
    const CUmemAllocationProp property = exportable_property(device_id);
    std::size_t granularity = 0U;
    CUresult status = cuMemGetAllocationGranularity(&granularity, &property, CU_MEM_ALLOC_GRANULARITY_RECOMMENDED);
    if (status != CUDA_SUCCESS || granularity == 0U) {
        set_driver_error(error_message, "cuMemGetAllocationGranularity failed", status);
        return false;
    }
    if (requested > std::numeric_limits<std::size_t>::max() - (granularity - 1U)) {
        if (error_message != nullptr) *error_message = "presentation allocation size overflows granularity";
        return false;
    }
    const std::size_t rounded = ((requested + granularity - 1U) / granularity) * granularity;
    status = cuMemCreate(&allocation_, rounded, &property, 0U);
    if (status != CUDA_SUCCESS) {
        set_driver_error(error_message, "cuMemCreate failed", status);
        return false;
    }
    allocation_size_ = rounded;
    status = cuMemAddressReserve(&address_, rounded, granularity, 0U, 0U);
    if (status != CUDA_SUCCESS) {
        set_driver_error(error_message, "cuMemAddressReserve failed", status);
        static_cast<void>(Release());
        return false;
    }
    reserved_bytes_ = rounded;
    status = cuMemMap(address_, rounded, 0U, allocation_, 0U);
    if (status != CUDA_SUCCESS) {
        set_driver_error(error_message, "cuMemMap failed", status);
        static_cast<void>(Release());
        return false;
    }
    mapping_active_ = true;
    CUmemAccessDesc access{};
    access.location = property.location;
    access.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    status = cuMemSetAccess(address_, rounded, &access, 1U);
    if (status != CUDA_SUCCESS) {
        set_driver_error(error_message, "cuMemSetAccess failed", status);
        static_cast<void>(Release());
        return false;
    }
    device_ptr_ = address_ + offset_bytes;
    pitch_bytes_ = pitch_bytes;
    width_ = width;
    height_ = height;
    if (error_message != nullptr) error_message->clear();
    return true;
}

int ExportedImageBuffer::export_descriptor(std::string* const error_message) const {
    if (empty()) {
        if (error_message != nullptr) *error_message = "presentation allocation has nothing to export";
        return -1;
    }
    int descriptor = -1;
    const CUresult status = cuMemExportToShareableHandle(&descriptor, allocation_, CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0U);
    if (status != CUDA_SUCCESS || descriptor < 0) {
        set_driver_error(error_message, "cuMemExportToShareableHandle failed", status);
        return -1;
    }
    if (error_message != nullptr) error_message->clear();
    return descriptor;
}

cudaError_t ExportedImageBuffer::Release() noexcept {
    return Release({
        .unmap = &cuMemUnmap,
        .free_address = &cuMemAddressFree,
        .release_allocation = &cuMemRelease,
    });
}

cudaError_t ExportedImageBuffer::Release(const ReleaseOperations& operations) noexcept {
    const CUmemGenericAllocationHandle allocation = allocation_;
    const CUdeviceptr address = address_;
    const std::size_t reserved_bytes = reserved_bytes_;
    const bool mapping_active = mapping_active_;

    allocation_ = 0U;
    address_ = 0U;
    device_ptr_ = 0U;
    allocation_size_ = 0U;
    reserved_bytes_ = 0U;
    pitch_bytes_ = 0U;
    width_ = 0U;
    height_ = 0U;
    mapping_active_ = false;

    cudaError_t first_failure = cudaSuccess;
    bool mapping_released = !mapping_active;
    if (mapping_active && address != 0U && reserved_bytes != 0U) {
        mapping_released = operations.unmap != nullptr && operations.unmap(address, reserved_bytes) == CUDA_SUCCESS;
        if (!mapping_released) first_failure = cudaErrorUnknown;
    }
    if (address != 0U && reserved_bytes != 0U && mapping_released &&
        (operations.free_address == nullptr || operations.free_address(address, reserved_bytes) != CUDA_SUCCESS) &&
        first_failure == cudaSuccess)
        first_failure = cudaErrorUnknown;
    if (allocation != 0U && (operations.release_allocation == nullptr || operations.release_allocation(allocation) != CUDA_SUCCESS) &&
        first_failure == cudaSuccess)
        first_failure = cudaErrorUnknown;
    if (release_failure_ == cudaSuccess) release_failure_ = first_failure;
    return first_failure;
}

}  // namespace mmltk::frameworks::gpu
