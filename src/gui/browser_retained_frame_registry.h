#pragma once

#include "gui/browser_frame_slot_contract.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace mmltk::gui {

enum class LinuxImportedImageSourceKind : std::uint8_t {
    Unknown = 0,
    CudaDevicePointer = 1,
    DmaBufRgba = 2,
};

enum class LinuxDmaBufModifierMode : std::uint8_t {
    Unknown = 0,
    Implicit = 1,
    Explicit = 2,
};

enum class LinuxImportedSyncHandleKind : std::uint8_t {
    None = 0,
    CudaEvent = 1,
    CudaStream = 2,
};

struct LinuxImportedImageSource {
    LinuxImportedImageSourceKind kind = LinuxImportedImageSourceKind::Unknown;
    std::uintptr_t fd = 0;
    int cuda_device_index = -1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t stride_bytes = 0;
    std::uint64_t offset = 0;
    std::uint64_t allocation_size = 0;
    std::uint32_t drm_format = 0;
    std::uint64_t drm_modifier = 0;
    LinuxDmaBufModifierMode modifier_mode = LinuxDmaBufModifierMode::Unknown;

    bool operator==(const LinuxImportedImageSource&) const noexcept = default;
};

struct LinuxImportedSyncSource {
    LinuxImportedSyncHandleKind kind = LinuxImportedSyncHandleKind::None;
    std::uintptr_t handle = 0;
    std::uint64_t value = 0;

    bool operator==(const LinuxImportedSyncSource&) const noexcept = default;
};

struct LinuxImportedFrameSourceContract {
    LinuxImportedImageSource image{};
    LinuxImportedSyncSource ready_sync{};
    LinuxImportedSyncSource producer_stream{};

    bool operator==(const LinuxImportedFrameSourceContract&) const noexcept = default;
};

struct LinuxImportedFrameLifecycleContract {
    LinuxImportedSyncSource release_sync{};

    bool operator==(const LinuxImportedFrameLifecycleContract&) const noexcept = default;
};

class RetainedLinuxImageFd;

struct RetainedBrowserImportedFrameSource {
    mmltk::browser::FrameSlotDescriptor descriptor;
    std::optional<LinuxImportedFrameSourceContract> linux_import;
    std::optional<LinuxImportedFrameLifecycleContract> linux_lifecycle;
    std::shared_ptr<RetainedLinuxImageFd> owned_image_fd;
};

class RetainedLinuxImageFd {
   public:
    explicit RetainedLinuxImageFd(int fd) noexcept : fd_(fd) {}
    ~RetainedLinuxImageFd();

    RetainedLinuxImageFd(const RetainedLinuxImageFd&) = delete;
    RetainedLinuxImageFd& operator=(const RetainedLinuxImageFd&) = delete;

    RetainedLinuxImageFd(RetainedLinuxImageFd&& other) noexcept;
    RetainedLinuxImageFd& operator=(RetainedLinuxImageFd&& other) noexcept;

    [[nodiscard]] int fd() const noexcept {
        return fd_;
    }
    [[nodiscard]] int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) noexcept;

   private:
    int fd_ = -1;
};

[[nodiscard]] inline bool linux_imported_frame_source_contract_is_empty(
    const LinuxImportedFrameSourceContract& contract) noexcept {
    return contract.image.fd == 0U && contract.image.cuda_device_index < 0 &&
           contract.image.width == 0U && contract.image.height == 0U && contract.image.stride_bytes == 0U &&
           contract.image.offset == 0U && contract.image.allocation_size == 0U && contract.image.drm_format == 0U &&
           contract.image.drm_modifier == 0U && contract.image.modifier_mode == LinuxDmaBufModifierMode::Unknown &&
           contract.ready_sync.kind == LinuxImportedSyncHandleKind::None && contract.ready_sync.handle == 0U &&
           contract.ready_sync.value == 0U && contract.producer_stream.kind == LinuxImportedSyncHandleKind::None &&
           contract.producer_stream.handle == 0U && contract.producer_stream.value == 0U;
}

inline void canonicalize_linux_imported_frame_source_contract(LinuxImportedFrameSourceContract& contract,
                                                              std::uint64_t ready_sync_value) noexcept {
    if (contract.ready_sync.kind == LinuxImportedSyncHandleKind::None && contract.ready_sync.handle != 0U) {
        contract.ready_sync.kind = LinuxImportedSyncHandleKind::CudaEvent;
    }
    if (contract.producer_stream.kind == LinuxImportedSyncHandleKind::None && contract.producer_stream.handle != 0U) {
        contract.producer_stream.kind = LinuxImportedSyncHandleKind::CudaStream;
    }
    if (contract.ready_sync.value == 0U && ready_sync_value != 0U) {
        contract.ready_sync.value = ready_sync_value;
    }
}

[[nodiscard]] inline std::optional<LinuxImportedFrameSourceContract> linux_imported_frame_source_contract(
    const RetainedBrowserImportedFrameSource& source) noexcept {
    if (!source.linux_import.has_value() || linux_imported_frame_source_contract_is_empty(*source.linux_import)) {
        return std::nullopt;
    }
    LinuxImportedFrameSourceContract contract = *source.linux_import;
    canonicalize_linux_imported_frame_source_contract(contract, source.descriptor.ready_sync.value);
    return contract;
}

[[nodiscard]] inline bool linux_imported_frame_lifecycle_contract_is_empty(
    const LinuxImportedFrameLifecycleContract& contract) noexcept {
    return contract.release_sync.kind == LinuxImportedSyncHandleKind::None && contract.release_sync.handle == 0U &&
           contract.release_sync.value == 0U;
}

[[nodiscard]] inline std::optional<LinuxImportedFrameLifecycleContract> linux_imported_frame_lifecycle_contract(
    const RetainedBrowserImportedFrameSource& source) noexcept {
    if (!source.linux_lifecycle.has_value() ||
        linux_imported_frame_lifecycle_contract_is_empty(*source.linux_lifecycle)) {
        return std::nullopt;
    }
    return source.linux_lifecycle;
}

inline void apply_linux_imported_frame_lifecycle_contract(RetainedBrowserImportedFrameSource& source,
                                                          LinuxImportedFrameLifecycleContract contract) noexcept {
    if (linux_imported_frame_lifecycle_contract_is_empty(contract)) {
        source.linux_lifecycle.reset();
        return;
    }
    source.linux_lifecycle = contract;
}

inline void apply_linux_imported_frame_source_contract(RetainedBrowserImportedFrameSource& source,
                                                       LinuxImportedFrameSourceContract contract) noexcept {
    canonicalize_linux_imported_frame_source_contract(contract, source.descriptor.ready_sync.value);

    if (linux_imported_frame_source_contract_is_empty(contract)) {
        source.linux_import.reset();
        return;
    }
    source.linux_import = contract;
}

inline void normalize_linux_imported_frame_source_contract(RetainedBrowserImportedFrameSource& source) noexcept {
    if (std::optional<LinuxImportedFrameSourceContract> contract = linux_imported_frame_source_contract(source);
        contract.has_value()) {
        apply_linux_imported_frame_source_contract(source, *contract);
    } else {
        source.linux_import.reset();
    }
}

inline void normalize_linux_imported_frame_lifecycle_contract(RetainedBrowserImportedFrameSource& source) noexcept {
    if (std::optional<LinuxImportedFrameLifecycleContract> contract = linux_imported_frame_lifecycle_contract(source);
        contract.has_value()) {
        apply_linux_imported_frame_lifecycle_contract(source, *contract);
    } else {
        source.linux_lifecycle.reset();
    }
}

inline void normalize_retained_browser_imported_frame_source(RetainedBrowserImportedFrameSource& source) noexcept {
    normalize_linux_imported_frame_source_contract(source);
    normalize_linux_imported_frame_lifecycle_contract(source);
}

class RetainedBrowserFrameRegistry {
   public:
    using ReleaseFn = std::function<void()>;

    RetainedBrowserFrameRegistry();
    ~RetainedBrowserFrameRegistry();

    RetainedBrowserFrameRegistry(const RetainedBrowserFrameRegistry&) = delete;
    RetainedBrowserFrameRegistry& operator=(const RetainedBrowserFrameRegistry&) = delete;
    RetainedBrowserFrameRegistry(RetainedBrowserFrameRegistry&&) noexcept;
    RetainedBrowserFrameRegistry& operator=(RetainedBrowserFrameRegistry&&) noexcept;

    void retain_workspace_surface(const mmltk::browser::WorkspaceSurfaceInfo& surface_info,
                                  RetainedBrowserImportedFrameSource imported_frame_source, ReleaseFn release_fn);

    [[nodiscard]] std::optional<RetainedBrowserImportedFrameSource> imported_frame_source_for_surface(
        std::string_view surface_id, std::string_view revision) const;

    bool release_workspace_surface(std::string_view surface_id, std::string_view revision);
    bool discard_workspace_surface(const mmltk::browser::WorkspaceSurfaceInfo& surface_info);
    void release_all();

   private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::gui
