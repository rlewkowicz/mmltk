#include "src/frameworks/gpu/imported_image_buffer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <string_view>
#include <sys/types.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include "src/common/io/noexcept_io.h"
namespace mmltk::frameworks::gpu {
namespace {
// Only the enabled import formatter calls this. Observe actual loaded mappings
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
    if (!maps)
        read_errno = errno;
    else {
        char line[8192];
        while (read_bytes < kMapsBytes && std::fgets(line, static_cast<int>(std::min(sizeof(line), kMapsBytes - read_bytes + 1U)), maps)) {
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
            if (std::any_of(paths.begin(), paths.begin() + path_count, [&](const auto& previous) { return std::strcmp(previous.data(), path) == 0; })) continue;
            const std::string_view path_view{path};
            if (path_count == paths.size() || path_view.size() >= paths.front().size()) {
                bounded = true;
                continue;
            }
            // A non-ASCII or JSON-sensitive mapping path is explicitly omitted,
            // never emitted as malformed JSON or a guessed resolved pathname.
            if (!std::ranges::all_of(path_view, [](const unsigned char byte) { return byte >= 32U && byte < 127U && byte != '"' && byte != '\\'; })) {
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
        const int length = std::snprintf(output, sizeof(output),
                                         "{\"event\":\"cuda.workspace.loaded_library\",\"native_process_id\":%ld,\"library_path\":\"%s\","
                                         "\"library_provenance\":\"proc_self_maps_at_first_memory_import\"}\n",
                                         static_cast<long>(process), paths[index].data());
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(output))
            mmltk::common::io::write_all_noexcept(STDERR_FILENO, {output, static_cast<std::size_t>(length)});
    }
    const int length = std::snprintf(output, sizeof(output),
                                     "{\"event\":\"cuda.workspace.library_inventory\",\"native_process_id\":%ld,\"inventory_status\":\"%s\","
                                     "\"inventory_errno\":%d,\"library_count\":%zu,\"maps_bytes\":%zu,\"maps_byte_limit\":%zu,"
                                     "\"inventory_bounded\":%s,\"path_encoding_unavailable\":%s}\n",
                                     static_cast<long>(process),
                                     read_errno != 0                   ? "unavailable"
                                     : bounded || encoding_unavailable ? "partial"
                                                                       : "complete",
                                     read_errno, path_count, read_bytes, kMapsBytes, bounded ? "true" : "false", encoding_unavailable ? "true" : "false");
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(output))
        mmltk::common::io::write_all_noexcept(STDERR_FILENO, {output, static_cast<std::size_t>(length)});
}
void observe(const char* operation, std::uint64_t identity, int retained_fd, CUexternalMemory memory, CUdeviceptr base, const ImageWorkspaceLayout& layout,
             CUresult status, int importing_fd = -1) noexcept {
    const auto* trace = std::getenv("MMLTK_GUI_TRACE_FILE");
    if (!trace || !*trace) return;
    const int saved_errno = errno;
    trace_loaded_libraries();
    char device_uuid[33]{};
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0U; index != layout.device_uuid.size(); ++index) {
        device_uuid[index * 2U] = hex[layout.device_uuid[index] >> 4U];
        device_uuid[index * 2U + 1U] = hex[layout.device_uuid[index] & 15U];
    }
    char output[1024];
    const int length =
        std::snprintf(output, sizeof(output),
                      "{\"event\":\"cuda.workspace.%s\",\"native_process_id\":%ld,\"workspace_allocation\":%llu,"
                      "\"retained_memory_descriptor\":%d,\"import_descriptor\":%d,\"cuda_external_memory\":%llu,\"cuda_mapped_base\":%llu,"
                      "\"device_uuid\":\"%s\",\"device\":%d,\"device_incarnation\":%llu,\"capacity_width\":%u,\"capacity_height\":%u,"
                      "\"fd_consumed\":%s,\"memory_size\":%zu,\"image_offset\":%zu,\"row_pitch\":%zu,\"dedicated\":%s,\"cuda_status\":%d}\n",
                      operation, static_cast<long>(::getpid()), static_cast<unsigned long long>(identity), retained_fd, importing_fd,
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(memory)), static_cast<unsigned long long>(base), device_uuid,
                      layout.device, static_cast<unsigned long long>(layout.device_incarnation), layout.width, layout.height,
                      std::strcmp(operation, "memory_import") == 0 && status == CUDA_SUCCESS ? "true" : "false", layout.required_allocation_bytes,
                      layout.offset_bytes, layout.pitch_bytes, layout.dedicated ? "true" : "false", static_cast<int>(status));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(output))
        mmltk::common::io::write_all_noexcept(STDERR_FILENO, {output, static_cast<std::size_t>(length)});
    errno = saved_errno;
}
}  // namespace
ImportedImageBuffer::ImportedImageBuffer() : resources_(std::make_shared<Resources>()) {}
ImportedImageBuffer::~ImportedImageBuffer() noexcept { static_cast<void>(Release()); }
CUdeviceptr ImportedImageBuffer::data() const noexcept { return resources_ ? resources_->data : 0U; }
std::size_t ImportedImageBuffer::pitch_bytes() const noexcept { return resources_ ? resources_->layout.pitch_bytes : 0U; }
std::size_t ImportedImageBuffer::allocation_size() const noexcept { return resources_ ? resources_->layout.required_allocation_bytes : 0U; }
bool ImportedImageBuffer::owns_resources() const noexcept {
    return resources_ && (resources_->memory || resources_->mapped_base || resources_->backing.get() >= 0);
}
bool ImportedImageBuffer::Import(DeviceContext context, mmltk::common::io::ScopedFd backing, const ImageWorkspaceLayout& layout, std::uint64_t identity,
                                 std::string* error) {
    const auto fail = [&](const char* operation, CUresult status) {
        observe("memory_import_rejected", identity, resources_ && resources_->backing.get() >= 0 ? resources_->backing.get() : backing.get(),
                resources_ ? resources_->memory : nullptr, resources_ ? resources_->mapped_base : 0U, layout, status);
        if (error) {
            const char* detail = nullptr;
            static_cast<void>(cuGetErrorString(status, &detail));
            *error = std::string(operation) + ": " + (detail ? detail : "invalid import");
        }
        return false;
    };
    if (!resources_ || owns_resources() || !layout.valid() || context.device() != layout.device || backing.get() < 0 || !identity)
        return fail("workspace memory metadata", CUDA_ERROR_INVALID_VALUE);
    context.Bind();
    CUuuid uuid{};
    auto status = cuDeviceGetUuid(&uuid, layout.device);
    if (status != CUDA_SUCCESS) return fail("workspace physical device UUID lookup", status);
    if (std::memcmp(uuid.bytes, layout.device_uuid.data(), layout.device_uuid.size()) != 0)
        return fail(("workspace physical device UUID mismatch for CUDA device " + std::to_string(layout.device)).c_str(), CUDA_ERROR_INVALID_DEVICE);
    mmltk::common::io::ScopedFd consumed;
    int duplicate;
    do { duplicate = ::fcntl(backing.get(), F_DUPFD_CLOEXEC, 0); } while (duplicate < 0 && errno == EINTR);
    consumed.reset(duplicate);
    if (duplicate < 0) return fail("workspace memory descriptor duplication", CUDA_ERROR_OPERATING_SYSTEM);
    auto& resource = *resources_;
    resource.context.emplace(std::move(context));
    resource.backing = std::move(backing);
    resource.identity = identity;
    resource.layout = layout;
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC descriptor{};
    descriptor.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    descriptor.handle.fd = consumed.get();
    descriptor.size = layout.required_allocation_bytes;
    descriptor.flags = layout.dedicated ? CUDA_EXTERNAL_MEMORY_DEDICATED : 0U;
    observe("memory_import_started", identity, resource.backing.get(), nullptr, 0U, layout, CUDA_SUCCESS, consumed.get());
    status = cuImportExternalMemory(&resource.memory, &descriptor);
    if (status == CUDA_SUCCESS) static_cast<void>(consumed.release());
    observe("memory_import", identity, resource.backing.get(), resource.memory, 0U, layout, status);
    if (status != CUDA_SUCCESS) return fail("cuImportExternalMemory", status);
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC mapping{};
    mapping.offset = 0U;
    mapping.size = layout.required_allocation_bytes;
    status = cuExternalMemoryGetMappedBuffer(&resource.mapped_base, resource.memory, &mapping);
    observe("memory_map", identity, resource.backing.get(), resource.memory, resource.mapped_base, layout, status);
    if (status != CUDA_SUCCESS) return fail("cuExternalMemoryGetMappedBuffer", status);
    resource.data = resource.mapped_base + layout.offset_bytes;
    if (error) error->clear();
    return true;
}
cudaError_t ImportedImageBuffer::Release() noexcept { return Release({&cuMemFree, &cuDestroyExternalMemory}); }
std::shared_ptr<ImportedImageBuffer> ImportedImageBuffer::ImportAlias(DeviceContext context) const {
    if (!resources_ || resources_->backing.get() < 0 || empty()) throw std::runtime_error("workspace alias has no imported backing");
    int duplicate;
    do { duplicate = ::fcntl(resources_->backing.get(), F_DUPFD_CLOEXEC, 0); } while (duplicate < 0 && errno == EINTR);
    if (duplicate < 0) throw std::runtime_error("workspace alias descriptor duplication failed");
    mmltk::common::io::ScopedFd backing(duplicate);
    auto alias = std::make_shared<ImportedImageBuffer>();
    std::string error;
    if (!alias->Import(std::move(context), std::move(backing), resources_->layout, resources_->identity, &error)) throw std::runtime_error(error);
    return alias;
}
cudaError_t ImportedImageBuffer::Release(const ReleaseOperations& operations) noexcept {
    if (!resources_) return cudaSuccess;
    if (!owns_resources()) {
        resources_.reset();
        return cudaSuccess;
    }
    auto& resource = *resources_;
    CUresult status = CUDA_SUCCESS;
    observe("memory_retirement_started", resource.identity, resource.backing.get(), resource.memory, resource.mapped_base, resource.layout, status);
    try {
        if (resource.context) resource.context->Bind();
        if (resource.mapped_base) {
            status = operations.free_mapping(resource.mapped_base);
            if (status == CUDA_SUCCESS) resource.mapped_base = resource.data = 0U;
        }
        if (status == CUDA_SUCCESS && resource.memory) {
            status = operations.destroy_memory(resource.memory);
            if (status == CUDA_SUCCESS) resource.memory = nullptr;
        }
    } catch (...) { status = CUDA_ERROR_UNKNOWN; }
    observe("memory_retirement", resource.identity, resource.backing.get(), resource.memory, resource.mapped_base, resource.layout, status);
    if (status != CUDA_SUCCESS) {
        release_failure_ = cudaErrorUnknown;
        std::move(retention_).Install(TerminalCudaCustody::Share(std::move(resources_)), release_failure_);
        return release_failure_;
    }
    // Only now may the FD's independent backing reference and context retire.
    resources_.reset();
    return cudaSuccess;
}
}  // namespace mmltk::frameworks::gpu
