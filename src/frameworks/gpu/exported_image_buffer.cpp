#include "src/frameworks/gpu/exported_image_buffer.h"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/common/io/noexcept_io.h"

namespace mmltk::frameworks::gpu {
namespace {

// Only the enabled export formatter calls this. Observe actual loaded mappings
// once per process, including after fork; do not load any additional library.
void trace_loaded_libraries() noexcept {
    static std::atomic<pid_t> observed_process{0};
    const pid_t process = ::getpid();
    if (observed_process.exchange(process, std::memory_order_relaxed) == process) return;
    constexpr std::size_t kMapsBytes = 1024U * 1024U;
    std::array<std::array<char, 1024U>, 32U> paths{};
    std::size_t path_count = 0U;
    std::size_t read_bytes = 0U;
    bool bounded = false;
    bool encoding_unavailable = false;
    bool discard_line = false;
    int read_errno = 0;
    FILE* const maps = std::fopen("/proc/self/maps", "re");
    if (!maps) read_errno = errno;
    else {
        char line[8192];
        while (read_bytes < kMapsBytes &&
               std::fgets(line, static_cast<int>(std::min(sizeof(line), kMapsBytes - read_bytes + 1U)), maps)) {
            const std::size_t length = std::strlen(line);
            read_bytes += length;
            const bool complete = length != 0U && line[length - 1U] == '\n';
            if (discard_line || !complete) {
                bounded = true;
                discard_line = !complete;
                continue;
            }
            line[length - 1U] = '\0';
            const char* const path = std::strchr(line, '/');
            if (!path) continue;
            const std::string_view name{std::strrchr(path, '/') + 1};
            if (!name.starts_with("libcuda.so") && !name.starts_with("libcudart.so") && !name.starts_with("libvulkan") &&
                !name.starts_with("libGLX_nvidia.so") && !name.starts_with("libnvidia-"))
                continue;
            if (std::any_of(paths.begin(), paths.begin() + path_count,
                            [&](const auto& previous) { return std::strcmp(previous.data(), path) == 0; }))
                continue;
            const std::string_view path_view{path};
            if (path_count == paths.size() || path_view.size() >= paths.front().size()) {
                bounded = true;
                continue;
            }
            // A non-ASCII or JSON-sensitive mapping path is explicitly omitted,
            // never emitted as malformed JSON or a guessed resolved pathname.
            if (!std::ranges::all_of(path_view, [](const unsigned char byte) {
                    return byte >= 32U && byte < 127U && byte != '"' && byte != '\\';
                })) {
                encoding_unavailable = true;
                continue;
            }
            std::memcpy(paths[path_count++].data(), path, path_view.size() + 1U);
        }
        if (std::ferror(maps)) read_errno = errno;
        if (std::fclose(maps) != 0 && read_errno == 0) read_errno = errno;
        bounded = bounded || read_bytes == kMapsBytes;
    }
    char output[1536];
    for (std::size_t index = 0U; index != path_count; ++index) {
        const int length = std::snprintf(
            output, sizeof(output),
            "{\"event\":\"cuda.workspace.loaded_library\",\"native_process_id\":%ld,\"library_path\":\"%s\","
            "\"library_provenance\":\"proc_self_maps_at_first_memory_export\"}\n",
            static_cast<long>(process), paths[index].data());
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(output))
            mmltk::common::io::write_all_noexcept(STDERR_FILENO, {output, static_cast<std::size_t>(length)});
    }
    const int length = std::snprintf(
        output, sizeof(output),
        "{\"event\":\"cuda.workspace.library_inventory\",\"native_process_id\":%ld,\"inventory_status\":\"%s\","
        "\"inventory_errno\":%d,\"library_count\":%zu,\"maps_bytes\":%zu,\"maps_byte_limit\":%zu,"
        "\"inventory_bounded\":%s,\"path_encoding_unavailable\":%s}\n",
        static_cast<long>(process), read_errno != 0 ? "unavailable" : bounded || encoding_unavailable ? "partial" : "complete",
        read_errno, path_count, read_bytes, kMapsBytes, bounded ? "true" : "false", encoding_unavailable ? "true" : "false");
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(output))
        mmltk::common::io::write_all_noexcept(STDERR_FILENO, {output, static_cast<std::size_t>(length)});
}

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
    // This framework has no application trace dependency. The existing native
    // stderr capture owns these bounded, opt-in records. No GPU allocation,
    // alias, or synchronization is introduced.
    if (const auto* trace = std::getenv("MMLTK_GUI_TRACE_FILE"); trace && *trace) {
        const int saved_errno = errno;
        trace_loaded_libraries();
        struct stat metadata{};
        const int stat_status = ::fstat(descriptor, &metadata);
        const int stat_errno = stat_status == 0 ? 0 : errno;
        CUmemGenericAllocationHandle mapped = 0U;
        // CUDA guarantees that a retained handle is the handle used by cuMemMap.
        // Compare it with the actual cuMemExportToShareableHandle argument.
        const auto retain_status = cuMemRetainAllocationHandle(&mapped, reinterpret_cast<void*>(device_ptr_));
        CUmemAllocationProp property{};
        const auto property_status = cuMemGetAllocationPropertiesFromHandle(&property, allocation_);
        CUuuid uuid{};
        const auto uuid_status = property_status == CUDA_SUCCESS && property.location.type == CU_MEM_LOCATION_TYPE_DEVICE
                                     ? cuDeviceGetUuid(&uuid, property.location.id)
                                     : CUDA_ERROR_NOT_SUPPORTED;
        const auto release_status = retain_status == CUDA_SUCCESS ? cuMemRelease(mapped) : CUDA_ERROR_NOT_SUPPORTED;
        char device_uuid[33]{};
        if (uuid_status == CUDA_SUCCESS) {
            constexpr char hex[] = "0123456789abcdef";
            for (std::size_t index = 0U; index != 16U; ++index) {
                const auto byte = static_cast<unsigned char>(uuid.bytes[index]);
                device_uuid[index * 2U] = hex[byte >> 4U];
                device_uuid[index * 2U + 1U] = hex[byte & 15U];
            }
        }
        char record[2048];
        const int length = std::snprintf(
            record, sizeof(record),
            "{\"event\":\"cuda.workspace.memory_export\",\"native_process_id\":%ld,\"workspace_descriptor\":%d,"
            "\"workspace_plane\":%llu,\"cuda_address\":%llu,\"cuda_export_allocation\":%llu,\"cuda_mapped_allocation\":%llu,"
            "\"cuda_retain_status\":%d,\"cuda_release_status\":%d,\"cuda_mapping_matches_export\":%s,"
            "\"cuda_property_status\":%d,\"cuda_allocation_type\":%u,\"cuda_location_type\":%u,\"cuda_location_id\":%d,"
            "\"cuda_requested_handle_types\":%u,\"cuda_compression_type\":%u,\"cuda_gpu_direct_rdma\":%u,"
            "\"cuda_allocation_usage\":%u,\"cuda_uuid_status\":%d,\"device_uuid\":\"%s\","
            "\"memory_size\":%zu,\"reserved_bytes\":%zu,\"row_pitch\":%zu,\"image_offset\":%llu,"
            "\"capacity_width\":%u,\"capacity_height\":%u,\"cuda_mapping_offset\":0,"
            "\"export_handle_type\":\"POSIX_FILE_DESCRIPTOR\",\"fd_stat_status\":%d,\"fd_stat_errno\":%d,"
            "\"fd_dev\":%llu,\"fd_ino\":%llu,\"fd_rdev\":%llu,\"fd_mode\":%u,\"fd_size\":%lld,"
            "\"fd_identity_scope\":\"metadata_only_not_gpu_allocation_identity\"}\n",
            static_cast<long>(::getpid()), descriptor, static_cast<unsigned long long>(device_ptr_),
            static_cast<unsigned long long>(address_), static_cast<unsigned long long>(allocation_),
            static_cast<unsigned long long>(mapped), static_cast<int>(retain_status), static_cast<int>(release_status),
            retain_status == CUDA_SUCCESS ? (mapped == allocation_ ? "true" : "false") : "null",
            static_cast<int>(property_status), static_cast<unsigned int>(property.type),
            static_cast<unsigned int>(property.location.type), property.location.id,
            static_cast<unsigned int>(property.requestedHandleTypes), static_cast<unsigned int>(property.allocFlags.compressionType),
            static_cast<unsigned int>(property.allocFlags.gpuDirectRDMACapable), static_cast<unsigned int>(property.allocFlags.usage),
            static_cast<int>(uuid_status), device_uuid, allocation_size_, reserved_bytes_, pitch_bytes_,
            static_cast<unsigned long long>(device_ptr_ - address_), width_, height_, stat_status, stat_errno,
            static_cast<unsigned long long>(metadata.st_dev), static_cast<unsigned long long>(metadata.st_ino),
            static_cast<unsigned long long>(metadata.st_rdev), static_cast<unsigned int>(metadata.st_mode),
            static_cast<long long>(metadata.st_size));
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(record))
            mmltk::common::io::write_all_noexcept(STDERR_FILENO, {record, static_cast<std::size_t>(length)});
        errno = saved_errno;
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
