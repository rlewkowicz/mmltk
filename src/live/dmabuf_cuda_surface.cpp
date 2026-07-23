#include "live/live_helpers.h"

#include "mmltk_logging.h"

#include <cuda.h>
#include <drm_fourcc.h>
#include <gbm.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace mmltk::live {

namespace {

constexpr std::uint32_t kDmaBufRgbaFormat = kWorkspaceDmaBufDrmFormatAbgr8888;

#if defined(GBM_BO_USE_TEXTURING)
constexpr std::uint32_t kGbmTextureUsage = GBM_BO_USE_TEXTURING;
constexpr const char* kExplicitModifierTextureProfileLabel = "explicit-modifier-texture";
#else
constexpr std::uint32_t kGbmTextureUsage = GBM_BO_USE_RENDERING;
constexpr const char* kExplicitModifierTextureProfileLabel = "explicit-modifier-rendering";
#endif
constexpr std::uint32_t kGbmTextureRenderingUsage = kGbmTextureUsage | GBM_BO_USE_RENDERING;
constexpr bool kGbmTextureRenderingUsageIsDistinct = kGbmTextureRenderingUsage != kGbmTextureUsage;

using GbmBoCreateWithModifiersFn = gbm_bo* (*)(gbm_device*, std::uint32_t, std::uint32_t, std::uint32_t,
                                               const std::uint64_t*, unsigned int);
using GbmBoCreateWithModifiers2Fn = gbm_bo* (*)(gbm_device*, std::uint32_t, std::uint32_t, std::uint32_t,
                                                const std::uint64_t*, unsigned int, std::uint32_t);
using GbmBoGetFdForPlaneFn = int (*)(gbm_bo*, int);
using GbmBoGetStrideForPlaneFn = std::uint32_t (*)(gbm_bo*, int);
using GbmDeviceGetBackendNameFn = const char* (*)(gbm_device*);

struct RenderNodeOpenResult {
    int fd = -1;
    std::string path;
};

[[nodiscard]] std::string errno_message(const char* context, const std::string& path = {}) {
    std::string message(context);
    if (!path.empty()) {
        message += " ";
        message += path;
    }
    message += ": ";
    message += std::strerror(errno);
    return message;
}

void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
    }
}

[[nodiscard]] int duplicate_fd_minimum(const int fd, const char* context) {
    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0) {
        throw std::runtime_error(errno_message(context));
    }
    return duplicate;
}

[[nodiscard]] void* load_symbol(const char* name) noexcept {
    return ::dlsym(RTLD_DEFAULT, name);
}

template <typename Fn>
[[nodiscard]] Fn load_symbol_as(const char* name) noexcept {
    return reinterpret_cast<Fn>(load_symbol(name));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

[[nodiscard]] std::string hex_u64(const std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::nouppercase << value;
    return out.str();
}

[[nodiscard]] std::string hex_u32(const std::uint32_t value) {
    return hex_u64(value);
}

[[nodiscard]] std::string modifier_list_summary(const std::vector<std::uint64_t>& modifiers) {
    if (modifiers.empty()) {
        return "[]";
    }
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < modifiers.size(); ++index) {
        if (index != 0U) {
            out << ",";
        }
        out << hex_u64(modifiers[index]);
    }
    out << "]";
    return out.str();
}

void log_dmabuf_capabilities(const LinuxGpuInteropDevice& device) {
    mmltk::logging::logger("live.dmabuf")
        ->info(
            "workspace DMA-BUF capabilities: cuda_device={} render_node={} gbm_backend={} format={} "
            "modifier_query={} modifier_count={} modifiers={}",
            device.cuda_device_index(), device.render_node_path(),
            device.gbm_backend_name().empty() ? "<unknown>" : device.gbm_backend_name(), hex_u32(kDmaBufRgbaFormat),
            device.has_dma_buf_import_modifiers() ? "available" : "unavailable", device.dma_buf_modifiers().size(),
            modifier_list_summary(device.dma_buf_modifiers()));
}

[[nodiscard]] std::string egl_error_name(const EGLint error) {
    switch (error) {
        case EGL_SUCCESS:
            return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:
            return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:
            return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC:
            return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE:
            return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT:
            return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG:
            return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE:
            return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:
            return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE:
            return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH:
            return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER:
            return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP:
            return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW:
            return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST:
            return "EGL_CONTEXT_LOST";
        default:
            return "EGL_ERROR_" + hex_u32(static_cast<std::uint32_t>(error));
    }
}

[[nodiscard]] std::string allocation_probe_label(const LinuxGpuInteropDevice& device,
                                                 const GpuInteropAllocationProfile& profile, const char* context) {
    std::ostringstream message;
    message << context << ": cuda_device=" << device.cuda_device_index()
            << " render_node=" << (device.render_node_path().empty() ? "<unknown>" : device.render_node_path())
            << " gbm_backend=" << (device.gbm_backend_name().empty() ? "<unknown>" : device.gbm_backend_name())
            << " allocation=" << profile.label << " format=" << hex_u32(kDmaBufRgbaFormat)
            << " modifier=" << hex_u64(profile.modifier) << " usage_flags=" << hex_u32(profile.usage_flags);
    return message.str();
}

[[nodiscard]] std::string allocation_errno_message(const LinuxGpuInteropDevice& device,
                                                   const GpuInteropAllocationProfile& profile, const char* context,
                                                   const int saved_errno) {
    std::ostringstream message;
    message << allocation_probe_label(device, profile, context) << " failed: " << std::strerror(saved_errno)
            << " (errno=" << saved_errno << ")";
    return message.str();
}

[[nodiscard]] std::string egl_failure_message(const LinuxGpuInteropDevice& device,
                                              const GpuInteropAllocationProfile& profile, const char* context,
                                              const char* operation, const EGLint error) {
    return allocation_probe_label(device, profile, context) + " " + operation + " failed: " + egl_error_name(error);
}

[[nodiscard]] std::string cuda_device_pci_bus_id(const int cuda_device_index, const char* context) {
    std::array<char, 32> pci_bus_id{};
    ensure_cuda_ok(cudaDeviceGetPCIBusId(pci_bus_id.data(), static_cast<int>(pci_bus_id.size()), cuda_device_index),
                   context);
    if (pci_bus_id[0] == '\0') {
        throw std::runtime_error(std::string(context) + ": CUDA device has no PCI bus id");
    }
    return std::string(pci_bus_id.data());
}

[[nodiscard]] bool render_node_matches_pci_bus_id(const int render_index, const std::string& pci_bus_id) {
    std::error_code error;
    const std::filesystem::path device_path =
        std::filesystem::path("/sys/class/drm") / ("renderD" + std::to_string(render_index)) / "device";
    const std::filesystem::path canonical_device = std::filesystem::canonical(device_path, error);
    return !error && canonical_device.filename().string() == pci_bus_id;
}

[[nodiscard]] RenderNodeOpenResult open_render_node(const int cuda_device_index, const char* context) {
    const std::string pci_bus_id = cuda_device_pci_bus_id(cuda_device_index, context);
    for (int index = 128; index <= 191; ++index) {
        if (!render_node_matches_pci_bus_id(index, pci_bus_id)) {
            continue;
        }
        const std::string path = "/dev/dri/renderD" + std::to_string(index);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            return RenderNodeOpenResult{fd, path};
        }
    }
    throw std::runtime_error(std::string(context) + ": no readable DRM render node matched CUDA device PCI bus id " +
                             pci_bus_id);
}

[[nodiscard]] std::uint64_t dma_buf_allocation_size(const int fd, const std::uint64_t required_bytes) {
    const off_t size = ::lseek(fd, 0, SEEK_END);
    if (size > 0) {
        return static_cast<std::uint64_t>(size);
    }
    return required_bytes;
}

[[nodiscard]] std::uint64_t checked_surface_bytes(const std::uint64_t offset, const std::uint64_t stride_bytes,
                                                  const std::uint32_t height, const char* context) {
    if (height == 0U || stride_bytes == 0U) {
        return 0U;
    }
    if (stride_bytes > (std::numeric_limits<std::uint64_t>::max() - offset) / height) {
        throw std::overflow_error(context);
    }
    return offset + stride_bytes * static_cast<std::uint64_t>(height);
}

[[nodiscard]] bool extension_list_contains(const char* extensions, const std::string_view name) noexcept {
    if (extensions == nullptr || name.empty()) {
        return false;
    }
    std::string_view list(extensions);
    std::size_t offset = 0;
    while (offset < list.size()) {
        const std::size_t end = list.find(' ', offset);
        const std::string_view token = list.substr(offset, end == std::string_view::npos ? end : end - offset);
        if (token == name) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1U;
    }
    return false;
}

[[nodiscard]] EGLDisplay create_egl_display(gbm_device* device, const char* context) {
    auto* get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = EGL_NO_DISPLAY;
    if (get_platform_display != nullptr) {
        display = get_platform_display(EGL_PLATFORM_GBM_KHR, device, nullptr);
    }
    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(device));
    }
    if (display == EGL_NO_DISPLAY) {
        throw std::runtime_error(std::string(context) + ": failed to create an EGL display for the GBM device");
    }
    return display;
}

[[nodiscard]] EGLContext create_egl_context(EGLDisplay display, const char* extensions, const char* context) {
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        throw std::runtime_error(std::string(context) +
                                 ": eglBindAPI(EGL_OPENGL_ES_API) failed: " + egl_error_name(eglGetError()));
    }

    const EGLint context_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION,
        2,
        EGL_NONE,
    };
    if (extension_list_contains(extensions, "EGL_KHR_no_config_context")) {
        EGLContext context_handle = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attrs);
        if (context_handle != EGL_NO_CONTEXT) {
            return context_handle;
        }
    }

    EGLConfig config = nullptr;
    EGLint config_count = 0;
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE,
    };
    if (eglChooseConfig(display, config_attrs, &config, 1, &config_count) != EGL_TRUE || config_count <= 0 ||
        config == nullptr) {
        throw std::runtime_error(std::string(context) +
                                 ": failed to choose an EGL config for GBM interop: " + egl_error_name(eglGetError()));
    }
    EGLContext context_handle = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (context_handle == EGL_NO_CONTEXT) {
        throw std::runtime_error(std::string(context) +
                                 ": failed to create an EGL context for GBM interop: " + egl_error_name(eglGetError()));
    }
    return context_handle;
}

[[nodiscard]] bool egl_display_supports_format(PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats, EGLDisplay display,
                                               const std::uint32_t format) {
    EGLint format_count = 0;
    if (query_formats(display, 0, nullptr, &format_count) != EGL_TRUE || format_count <= 0) {
        return false;
    }
    std::vector<EGLint> formats(static_cast<std::size_t>(format_count));
    EGLint returned_count = 0;
    if (query_formats(display, format_count, formats.data(), &returned_count) != EGL_TRUE || returned_count <= 0) {
        return false;
    }
    formats.resize(static_cast<std::size_t>(returned_count));
    return std::find(formats.begin(), formats.end(), static_cast<EGLint>(format)) != formats.end();
}

[[nodiscard]] std::vector<std::uint64_t> query_egl_modifiers(PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers,
                                                             EGLDisplay display, const std::uint32_t format) {
    EGLint modifier_count = 0;
    if (query_modifiers(display, static_cast<EGLint>(format), 0, nullptr, nullptr, &modifier_count) != EGL_TRUE ||
        modifier_count <= 0) {
        return {};
    }

    std::vector<EGLuint64KHR> raw_modifiers(static_cast<std::size_t>(modifier_count));
    std::vector<EGLBoolean> external_only(static_cast<std::size_t>(modifier_count));
    EGLint returned_count = 0;
    if (query_modifiers(display, static_cast<EGLint>(format), modifier_count, raw_modifiers.data(),
                        external_only.data(), &returned_count) != EGL_TRUE ||
        returned_count <= 0) {
        return {};
    }

    std::vector<std::uint64_t> modifiers;
    modifiers.reserve(static_cast<std::size_t>(returned_count));
    for (EGLint index = 0; index < returned_count; ++index) {
        const std::uint64_t modifier = static_cast<std::uint64_t>(raw_modifiers[static_cast<std::size_t>(index)]);
        if (modifier == DRM_FORMAT_MOD_INVALID ||
            std::find(modifiers.begin(), modifiers.end(), modifier) != modifiers.end()) {
            continue;
        }
        modifiers.push_back(modifier);
    }
    const auto linear_it = std::find(modifiers.begin(), modifiers.end(), DRM_FORMAT_MOD_LINEAR);
    if (linear_it != modifiers.end() && linear_it != modifiers.begin()) {
        std::rotate(modifiers.begin(), linear_it, linear_it + 1);
    }
    return modifiers;
}

[[nodiscard]] std::vector<GpuInteropAllocationProfile> build_allocation_profiles(
    const LinuxGpuInteropDevice& interop_device, const std::uint64_t required_modifier) {
    std::vector<GpuInteropAllocationProfile> profiles;
    if (required_modifier == DRM_FORMAT_MOD_INVALID ||
        std::find(interop_device.dma_buf_modifiers().begin(), interop_device.dma_buf_modifiers().end(),
                  required_modifier) == interop_device.dma_buf_modifiers().end()) {
        return profiles;
    }
    profiles.reserve(kGbmTextureRenderingUsageIsDistinct ? 2U : 1U);
    profiles.push_back(GpuInteropAllocationProfile{
        kGbmTextureUsage,
        required_modifier,
        kExplicitModifierTextureProfileLabel,
    });
    if constexpr (kGbmTextureRenderingUsageIsDistinct) {
        profiles.push_back(GpuInteropAllocationProfile{
            kGbmTextureRenderingUsage,
            required_modifier,
            "explicit-modifier-texture-rendering",
        });
    }
    return profiles;
}

[[nodiscard]] gbm_bo* allocate_gbm_bo(gbm_device* device, const GpuInteropAllocationProfile& profile,
                                      const std::uint32_t width, const std::uint32_t height) {
    errno = 0;
    const std::uint64_t modifier = profile.modifier;
    if (auto create_with_modifiers2 = load_symbol_as<GbmBoCreateWithModifiers2Fn>("gbm_bo_create_with_modifiers2");
        create_with_modifiers2 != nullptr) {
        // cppcheck-suppress returnDanglingLifetime
        return create_with_modifiers2(device, width, height, kDmaBufRgbaFormat, &modifier, 1U, profile.usage_flags);
    }
    if (auto create_with_modifiers = load_symbol_as<GbmBoCreateWithModifiersFn>("gbm_bo_create_with_modifiers");
        create_with_modifiers != nullptr) {
        // cppcheck-suppress returnDanglingLifetime
        return create_with_modifiers(device, width, height, kDmaBufRgbaFormat, &modifier, 1U);
    }
    errno = ENOSYS;
    return nullptr;
}

[[nodiscard]] int export_plane_fd(gbm_bo* bo, const char* context) {
    int exported_fd = -1;
    if (auto get_fd_for_plane = load_symbol_as<GbmBoGetFdForPlaneFn>("gbm_bo_get_fd_for_plane");
        get_fd_for_plane != nullptr) {
        exported_fd = get_fd_for_plane(bo, 0);
    }
    if (exported_fd < 0) {
        exported_fd = gbm_bo_get_fd(bo);
    }
    if (exported_fd < 0) {
        throw std::runtime_error(errno_message(context));
    }
    if (exported_fd >= 3) {
        return exported_fd;
    }
    int duplicate = -1;
    try {
        duplicate = duplicate_fd_minimum(exported_fd, context);
    } catch (...) {
        close_fd(exported_fd);
        throw;
    }
    close_fd(exported_fd);
    return duplicate;
}

[[nodiscard]] std::uint32_t plane_stride(gbm_bo* bo) {
    if (auto get_stride_for_plane = load_symbol_as<GbmBoGetStrideForPlaneFn>("gbm_bo_get_stride_for_plane");
        get_stride_for_plane != nullptr) {
        return get_stride_for_plane(bo, 0);
    }
    return gbm_bo_get_stride(bo);
}

[[nodiscard]] EGLImageKHR create_dma_buf_egl_image(const LinuxGpuInteropDevice& interop_device,
                                                   const GpuInteropAllocationProfile& profile, const int fd,
                                                   const std::uint32_t width, const std::uint32_t height,
                                                   const std::uint64_t stride_bytes, const std::uint64_t offset,
                                                   const char* context) {
    if (stride_bytes > static_cast<std::uint64_t>(std::numeric_limits<EGLint>::max()) ||
        offset > static_cast<std::uint64_t>(std::numeric_limits<EGLint>::max())) {
        throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                 " DMA-BUF plane layout is too large for EGL import attributes");
    }

    std::array<EGLint, 32> attrs{};
    std::size_t index = 0;
    auto push_attr = [&](const EGLint key, const EGLint value) {
        attrs[index++] = key;
        attrs[index++] = value;
    };
    push_attr(EGL_WIDTH, static_cast<EGLint>(width));
    push_attr(EGL_HEIGHT, static_cast<EGLint>(height));
    push_attr(EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(kDmaBufRgbaFormat));
    push_attr(EGL_DMA_BUF_PLANE0_FD_EXT, fd);
    push_attr(EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(offset));
    push_attr(EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(stride_bytes));
    push_attr(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(profile.modifier & 0xffffffffU));
    push_attr(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>(profile.modifier >> 32U));
    attrs[index] = EGL_NONE;

    EGLImageKHR image = interop_device.egl_create_image()(interop_device.egl_display(), EGL_NO_CONTEXT,
                                                          EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    if (image == EGL_NO_IMAGE_KHR) {
        throw std::runtime_error(
            egl_failure_message(interop_device, profile, context, "eglCreateImageKHR", eglGetError()));
    }
    return image;
}

[[nodiscard]] bool frame_has_supported_cuda_layout(const CUeglFrame& frame) noexcept {
    return frame.planeCount == 1U && frame.width > 0U && frame.height > 0U && frame.numChannels >= 4U &&
           frame.cuFormat == CU_AD_FORMAT_UNSIGNED_INT8 &&
           (frame.frameType == CU_EGL_FRAME_TYPE_PITCH || frame.frameType == CU_EGL_FRAME_TYPE_ARRAY);
}

}  

void ensure_cuda_driver_ok(const CUresult status, const char* context) {
    if (status == CUDA_SUCCESS) {
        return;
    }

    const char* name = nullptr;
    const char* description = nullptr;
    (void)cuGetErrorName(status, &name);
    (void)cuGetErrorString(status, &description);

    std::string message(context);
    message += ": ";
    message += name != nullptr ? name : "unknown CUDA driver error";
    if (description != nullptr) {
        message += " (";
        message += description;
        message += ")";
    }
    throw std::runtime_error(message);
}

LinuxGpuInteropDevice::~LinuxGpuInteropDevice() {
    reset();
}

LinuxGpuInteropDevice::LinuxGpuInteropDevice(LinuxGpuInteropDevice&& other) noexcept
    : fd_(other.fd_),
      cuda_device_index_(other.cuda_device_index_),
      render_node_path_(std::move(other.render_node_path_)),
      gbm_backend_name_(std::move(other.gbm_backend_name_)),
      device_(other.device_),
      egl_display_(other.egl_display_),
      egl_context_(other.egl_context_),
      egl_create_image_(other.egl_create_image_),
      egl_destroy_image_(other.egl_destroy_image_),
      egl_query_dma_buf_formats_(other.egl_query_dma_buf_formats_),
      egl_query_dma_buf_modifiers_(other.egl_query_dma_buf_modifiers_),
      dma_buf_modifiers_(std::move(other.dma_buf_modifiers_)),
      has_dma_buf_import_modifiers_(other.has_dma_buf_import_modifiers_),
      cached_allocation_profile_(other.cached_allocation_profile_) {
    other.clear_after_move();
}

LinuxGpuInteropDevice& LinuxGpuInteropDevice::operator=(LinuxGpuInteropDevice&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        cuda_device_index_ = other.cuda_device_index_;
        render_node_path_ = std::move(other.render_node_path_);
        gbm_backend_name_ = std::move(other.gbm_backend_name_);
        device_ = other.device_;
        egl_display_ = other.egl_display_;
        egl_context_ = other.egl_context_;
        egl_create_image_ = other.egl_create_image_;
        egl_destroy_image_ = other.egl_destroy_image_;
        egl_query_dma_buf_formats_ = other.egl_query_dma_buf_formats_;
        egl_query_dma_buf_modifiers_ = other.egl_query_dma_buf_modifiers_;
        dma_buf_modifiers_ = std::move(other.dma_buf_modifiers_);
        has_dma_buf_import_modifiers_ = other.has_dma_buf_import_modifiers_;
        cached_allocation_profile_ = other.cached_allocation_profile_;

        other.clear_after_move();
    }
    return *this;
}

void LinuxGpuInteropDevice::clear_after_move() noexcept {
    fd_ = -1;
    cuda_device_index_ = -1;
    device_ = nullptr;
    egl_display_ = EGL_NO_DISPLAY;
    egl_context_ = EGL_NO_CONTEXT;
    egl_create_image_ = nullptr;
    egl_destroy_image_ = nullptr;
    egl_query_dma_buf_formats_ = nullptr;
    egl_query_dma_buf_modifiers_ = nullptr;
    has_dma_buf_import_modifiers_ = false;
    cached_allocation_profile_.reset();
}

void LinuxGpuInteropDevice::ensure_open(const int cuda_device_index, const char* context) {
    if (device_ != nullptr) {
        if (cuda_device_index_ != cuda_device_index) {
            throw std::runtime_error(std::string(context) +
                                     ": Linux GPU interop device is already open for a different CUDA device");
        }
        return;
    }

    ensure_cuda_driver_ok(cuInit(0), context);
    RenderNodeOpenResult render_node = open_render_node(cuda_device_index, context);
    gbm_device* device = gbm_create_device(render_node.fd);
    if (device == nullptr) {
        const std::string error = errno_message(context, render_node.path);
        close_fd(render_node.fd);
        throw std::runtime_error(error);
    }

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    try {
        display = create_egl_display(device, context);
        if (eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
            throw std::runtime_error(std::string(context) + ": eglInitialize failed: " + egl_error_name(eglGetError()));
        }

        const char* egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
        if (!extension_list_contains(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
            throw std::runtime_error(std::string(context) +
                                     ": EGL display does not support EGL_EXT_image_dma_buf_import");
        }
        egl_create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        egl_destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        egl_query_dma_buf_formats_ =
            reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
        egl_query_dma_buf_modifiers_ =
            reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
        if (egl_create_image_ == nullptr || egl_destroy_image_ == nullptr || egl_query_dma_buf_formats_ == nullptr) {
            throw std::runtime_error(std::string(context) +
                                     ": EGL display is missing required DMA-BUF import extension entry points");
        }
        if (!egl_display_supports_format(egl_query_dma_buf_formats_, display, kDmaBufRgbaFormat)) {
            throw std::runtime_error(std::string(context) +
                                     ": EGL display does not advertise DRM_FORMAT_ABGR8888 DMA-BUF import support");
        }

        has_dma_buf_import_modifiers_ =
            extension_list_contains(egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers") &&
            egl_query_dma_buf_modifiers_ != nullptr;
        if (has_dma_buf_import_modifiers_) {
            dma_buf_modifiers_ = query_egl_modifiers(egl_query_dma_buf_modifiers_, display, kDmaBufRgbaFormat);
        }
        if (dma_buf_modifiers_.empty()) {
            throw std::runtime_error(std::string(context) +
                                     ": EGL display has no explicit DRM modifier for DRM_FORMAT_ABGR8888");
        }
        egl_context = create_egl_context(display, egl_extensions, context);
    } catch (...) {
        if (egl_context != EGL_NO_CONTEXT) {
            (void)eglDestroyContext(display, egl_context);
        }
        if (display != EGL_NO_DISPLAY) {
            (void)eglTerminate(display);
        }
        gbm_device_destroy(device);
        close_fd(render_node.fd);
        throw;
    }

    fd_ = render_node.fd;
    cuda_device_index_ = cuda_device_index;
    render_node_path_ = std::move(render_node.path);
    device_ = device;
    egl_display_ = display;
    egl_context_ = egl_context;

    if (auto backend_name_fn = load_symbol_as<GbmDeviceGetBackendNameFn>("gbm_device_get_backend_name");
        backend_name_fn != nullptr) {
        if (const char* backend = backend_name_fn(device_); backend != nullptr) {
            gbm_backend_name_ = backend;
        }
    }
    log_dmabuf_capabilities(*this);
}

void LinuxGpuInteropDevice::reset() noexcept {
    cached_allocation_profile_.reset();
    dma_buf_modifiers_.clear();
    has_dma_buf_import_modifiers_ = false;
    egl_query_dma_buf_modifiers_ = nullptr;
    egl_query_dma_buf_formats_ = nullptr;
    egl_create_image_ = nullptr;
    egl_destroy_image_ = nullptr;
    if (egl_context_ != EGL_NO_CONTEXT) {
        (void)eglDestroyContext(egl_display_, egl_context_);
        egl_context_ = EGL_NO_CONTEXT;
    }
    if (egl_display_ != EGL_NO_DISPLAY) {
        (void)eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }
    if (device_ != nullptr) {
        gbm_device_destroy(device_);
        device_ = nullptr;
    }
    close_fd(fd_);
    cuda_device_index_ = -1;
    render_node_path_.clear();
    gbm_backend_name_.clear();
}

DmaBufCudaRgbaSurface::~DmaBufCudaRgbaSurface() {
    reset();
}

DmaBufCudaRgbaSurface::DmaBufCudaRgbaSurface(DmaBufCudaRgbaSurface&& other) noexcept
    : bo_(other.bo_),
      egl_display_(other.egl_display_),
      egl_destroy_image_(other.egl_destroy_image_),
      egl_image_(other.egl_image_),
      cuda_resource_(other.cuda_resource_),
      surface_object_(other.surface_object_),
      array_(other.array_),
      device_ptr_(other.device_ptr_),
      fd_(other.fd_),
      width_(other.width_),
      height_(other.height_),
      stride_bytes_(other.stride_bytes_),
      offset_(other.offset_),
      allocation_size_(other.allocation_size_),
      drm_format_(other.drm_format_),
      drm_modifier_(other.drm_modifier_),
      frame_type_(other.frame_type_),
      channel_count_(other.channel_count_),
      cuda_format_(other.cuda_format_) {
    other.clear_after_move();
}

DmaBufCudaRgbaSurface& DmaBufCudaRgbaSurface::operator=(DmaBufCudaRgbaSurface&& other) noexcept {
    if (this != &other) {
        reset();
        bo_ = other.bo_;
        egl_display_ = other.egl_display_;
        egl_destroy_image_ = other.egl_destroy_image_;
        egl_image_ = other.egl_image_;
        cuda_resource_ = other.cuda_resource_;
        surface_object_ = other.surface_object_;
        array_ = other.array_;
        device_ptr_ = other.device_ptr_;
        fd_ = other.fd_;
        width_ = other.width_;
        height_ = other.height_;
        stride_bytes_ = other.stride_bytes_;
        offset_ = other.offset_;
        allocation_size_ = other.allocation_size_;
        drm_format_ = other.drm_format_;
        drm_modifier_ = other.drm_modifier_;
        frame_type_ = other.frame_type_;
        channel_count_ = other.channel_count_;
        cuda_format_ = other.cuda_format_;

        other.clear_after_move();
    }
    return *this;
}

void DmaBufCudaRgbaSurface::clear_after_move() noexcept {
    bo_ = nullptr;
    egl_display_ = EGL_NO_DISPLAY;
    egl_destroy_image_ = nullptr;
    egl_image_ = EGL_NO_IMAGE_KHR;
    cuda_resource_ = nullptr;
    surface_object_ = 0;
    array_ = nullptr;
    device_ptr_ = 0;
    fd_ = -1;
    width_ = 0;
    height_ = 0;
    stride_bytes_ = 0;
    offset_ = 0;
    allocation_size_ = 0;
    drm_format_ = 0;
    drm_modifier_ = kWorkspaceDmaBufDrmFormatModInvalid;
    frame_type_ = CU_EGL_FRAME_TYPE_PITCH;
    channel_count_ = 0;
    cuda_format_ = CU_AD_FORMAT_UNSIGNED_INT8;
}

void DmaBufCudaRgbaSurface::ensure_dimensions(LinuxGpuInteropDevice& interop_device, const int cuda_device_index,
                                              const std::uint32_t width, const std::uint32_t height,
                                              const std::uint64_t required_modifier, const char* context) {
    if (width == 0U || height == 0U) {
        return;
    }
    if (cuda_resource_ != nullptr && width_ >= width && height_ >= height && drm_modifier_ == required_modifier) {
        return;
    }

    ensure_cuda_ok(cudaSetDevice(cuda_device_index), context);
    ensure_cuda_driver_ok(cuInit(0), context);
    interop_device.ensure_open(cuda_device_index, context);
    if (interop_device.get() == nullptr || interop_device.egl_display() == EGL_NO_DISPLAY ||
        interop_device.egl_create_image() == nullptr || interop_device.egl_destroy_image() == nullptr) {
        throw std::runtime_error(std::string(context) + ": Linux GBM/EGL interop device is not initialized");
    }

    const std::optional<GpuInteropAllocationProfile>& cached_profile = interop_device.cached_allocation_profile();
    if (cached_profile.has_value()) {
        if (cached_profile->modifier != required_modifier) {
            throw std::runtime_error(std::string(context) +
                                     ": cached allocation modifier differs from the negotiated modifier");
        }
        DmaBufCudaRgbaSurface next;
        next.initialize(interop_device, *cached_profile, width, height, context);
        *this = std::move(next);
        return;
    }

    std::string probe_errors;
    const std::vector<GpuInteropAllocationProfile> profiles =
        build_allocation_profiles(interop_device, required_modifier);
    if (profiles.empty()) {
        throw std::runtime_error(std::string(context) + ": negotiated DRM modifier " + hex_u64(required_modifier) +
                                 " is not supported by the CUDA/EGL/GBM device");
    }
    for (const GpuInteropAllocationProfile& profile : profiles) {
        DmaBufCudaRgbaSurface next;
        try {
            next.initialize(interop_device, profile, width, height, context);
            interop_device.cache_allocation_profile(profile);
            *this = std::move(next);
            return;
        } catch (const std::exception& error) {
            if (!probe_errors.empty()) {
                probe_errors += "\n";
            }
            probe_errors += error.what();
        }
    }

    throw std::runtime_error(std::string(context) + ": no GBM/EGL/CUDA workspace allocation profile succeeded\n" +
                             probe_errors);
}

void DmaBufCudaRgbaSurface::initialize(LinuxGpuInteropDevice& interop_device,
                                       const GpuInteropAllocationProfile& profile, const std::uint32_t width,
                                       const std::uint32_t height, const char* context) {
    reset();
    bo_ = allocate_gbm_bo(interop_device.get(), profile, width, height);
    if (bo_ == nullptr) {
        const int saved_errno = errno == 0 ? EINVAL : errno;
        throw std::runtime_error(allocation_errno_message(interop_device, profile, context, saved_errno));
    }

    try {
        if (gbm_bo_get_plane_count(bo_) != 1) {
            throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                     " GBM allocated a multi-plane workspace buffer");
        }

        fd_ = export_plane_fd(bo_, context);
        width_ = width;
        height_ = height;
        stride_bytes_ = plane_stride(bo_);
        offset_ = gbm_bo_get_offset(bo_, 0);
        drm_format_ = gbm_bo_get_format(bo_);
        if (drm_format_ != kDmaBufRgbaFormat || stride_bytes_ == 0U) {
            throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                     " GBM returned incompatible DMA-BUF RGBA layout metadata");
        }

        drm_modifier_ = gbm_bo_get_modifier(bo_);
        if (drm_modifier_ == DRM_FORMAT_MOD_INVALID || drm_modifier_ != profile.modifier) {
            throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                     " explicit-modifier allocation returned an unexpected modifier " +
                                     hex_u64(drm_modifier_));
        }

        const std::uint64_t min_row_bytes = static_cast<std::uint64_t>(width) * sizeof(Rgba32Pixel);
        const std::uint64_t required_bytes =
            checked_surface_bytes(offset_, static_cast<std::uint64_t>(stride_bytes_), height, context);
        allocation_size_ = dma_buf_allocation_size(fd_, required_bytes);
        if (stride_bytes_ < min_row_bytes || allocation_size_ < required_bytes) {
            throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                     " DMA-BUF plane byte coverage is smaller than the workspace image");
        }

        egl_image_ =
            create_dma_buf_egl_image(interop_device, profile, fd_, width, height, stride_bytes_, offset_, context);
        egl_display_ = interop_device.egl_display();
        egl_destroy_image_ = interop_device.egl_destroy_image();
        ensure_cuda_driver_ok(
            cuGraphicsEGLRegisterImage(&cuda_resource_, egl_image_, CU_GRAPHICS_MAP_RESOURCE_FLAGS_WRITE_DISCARD),
            allocation_probe_label(interop_device, profile, context).c_str());
        CUeglFrame egl_frame{};
        ensure_cuda_driver_ok(cuGraphicsResourceGetMappedEglFrame(&egl_frame, cuda_resource_, 0, 0),
                              allocation_probe_label(interop_device, profile, context).c_str());
        if (!frame_has_supported_cuda_layout(egl_frame)) {
            throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                     " CUDA returned an unsupported EGL frame layout");
        }
        if (egl_frame.width != width || egl_frame.height != height) {
            throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                     " CUDA returned EGL frame dimensions that do not match the DMA-BUF allocation");
        }
        width_ = egl_frame.width;
        height_ = egl_frame.height;
        frame_type_ = egl_frame.frameType;
        channel_count_ = egl_frame.numChannels;
        cuda_format_ = egl_frame.cuFormat;
        if (frame_type_ == CU_EGL_FRAME_TYPE_PITCH) {
            if (egl_frame.frame.pPitch[0] == nullptr) {
                throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                         " CUDA returned an empty pitched EGL frame");
            }
            device_ptr_ =
                reinterpret_cast<CUdeviceptr>(egl_frame.frame.pPitch[0]);  // NOLINT(performance-no-int-to-ptr)
            if (egl_frame.pitch > 0U) {
                stride_bytes_ = egl_frame.pitch;
            }
            if (stride_bytes_ < min_row_bytes) {
                throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                         " CUDA returned a pitched EGL frame with too small a pitch");
            }
            const std::uint64_t cuda_required_bytes =
                checked_surface_bytes(offset_, static_cast<std::uint64_t>(stride_bytes_), height, context);
            if (allocation_size_ < cuda_required_bytes) {
                throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                         " CUDA returned a pitched EGL frame larger than the DMA-BUF allocation");
            }
        } else {
            array_ = egl_frame.frame.pArray[0];
            if (array_ == nullptr) {
                throw std::runtime_error(allocation_probe_label(interop_device, profile, context) +
                                         " CUDA returned an empty array EGL frame");
            }
            CUDA_RESOURCE_DESC surface_desc{};
            surface_desc.resType = CU_RESOURCE_TYPE_ARRAY;
            surface_desc.res.array.hArray = array_;
            ensure_cuda_driver_ok(cuSurfObjectCreate(&surface_object_, &surface_desc),
                                  allocation_probe_label(interop_device, profile, context).c_str());
        }
    } catch (...) {
        reset();
        throw;
    }
}

void DmaBufCudaRgbaSurface::reset() noexcept {
    if (surface_object_ != 0) {
        (void)cuSurfObjectDestroy(surface_object_);
        surface_object_ = 0;
    }
    if (cuda_resource_ != nullptr) {
        (void)cuGraphicsUnregisterResource(cuda_resource_);
        cuda_resource_ = nullptr;
    }
    if (egl_image_ != EGL_NO_IMAGE_KHR) {
        if (egl_display_ != EGL_NO_DISPLAY && egl_destroy_image_ != nullptr) {
            (void)egl_destroy_image_(egl_display_, egl_image_);
        }
        egl_image_ = EGL_NO_IMAGE_KHR;
    }
    egl_display_ = EGL_NO_DISPLAY;
    egl_destroy_image_ = nullptr;
    close_fd(fd_);
    if (bo_ != nullptr) {
        gbm_bo_destroy(bo_);
        bo_ = nullptr;
    }
    clear_after_move();
}

int DmaBufCudaRgbaSurface::duplicate_fd(const char* context) const {
    if (fd_ < 0) {
        throw std::runtime_error(std::string(context) + ": DMA-BUF surface has no file descriptor");
    }
    return duplicate_fd_minimum(fd_, context);
}

WorkspaceDmaBufImage DmaBufCudaRgbaSurface::dmabuf_image(const std::uint32_t published_width,
                                                         const std::uint32_t published_height) const {
    const std::uint64_t stride_bytes = static_cast<std::uint64_t>(stride_bytes_);
    if (fd_ < 0 || published_width == 0U || published_height == 0U || published_width > width_ ||
        published_height > height_ || stride_bytes < static_cast<std::uint64_t>(published_width) * 4U ||
        published_height > std::numeric_limits<std::uint64_t>::max() / stride_bytes || allocation_size_ < offset_ ||
        allocation_size_ - offset_ < stride_bytes * published_height || drm_format_ != kDmaBufRgbaFormat ||
        drm_modifier_ == DRM_FORMAT_MOD_INVALID) {
        return {};
    }
    return WorkspaceDmaBufImage{
        fd_, published_width, published_height, stride_bytes, offset_, allocation_size_, drm_format_, drm_modifier_,
    };
}

}  
