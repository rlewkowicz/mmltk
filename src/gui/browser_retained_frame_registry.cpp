#include "browser_retained_frame_registry.h"
#include "browser_workspace_surface_bridge.h"

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

namespace mmltk::gui {

namespace {

enum class ImportedDiscardDisposition : std::uint8_t {
    NotFound,
    Released,
    RetainedUntilRelease,
};

constexpr std::uint32_t kDrmFormatAbgr8888 = 0x34324241U;
constexpr std::uint64_t kDrmFormatModInvalid = (1ULL << 56U) - 1U;

[[nodiscard]] std::string workspace_surface_key(const std::string_view surface_id, const std::string_view revision) {
    return std::string(surface_id) + '\n' + std::string(revision);
}

void validate_workspace_surface_info(const mmltk::browser::WorkspaceSurfaceInfo& surface_info,
                                     const mmltk::browser::FrameSlotDescriptor& descriptor) {
    if (surface_info.surface_id != kBrowserWorkspaceSurfaceId) {
        throw std::runtime_error(
            "retained browser workspace surface publication requires the "
            "`workspace` surface id");
    }
    if (surface_info.revision.empty()) {
        throw std::runtime_error("retained browser workspace surface publication requires a revision");
    }
    if (!is_browser_workspace_surface_descriptor(descriptor)) {
        throw std::runtime_error(
            "retained browser workspace surface publication requires a "
            "workspace descriptor");
    }
    if (surface_info.width != descriptor.width || surface_info.height != descriptor.height) {
        throw std::runtime_error(
            "retained browser workspace surface publication dimensions must "
            "match the descriptor");
    }
}

[[nodiscard]] const mmltk::browser::WorkspaceSurfaceInfo* workspace_surface_ptr(
    const std::optional<mmltk::browser::WorkspaceSurfaceInfo>& surface) noexcept {
    if (!surface.has_value()) {
        return nullptr;
    }
    return std::addressof(*surface);
}

void validate_publication_descriptor(const mmltk::browser::FrameSlotDescriptor& descriptor,
                                     const std::string_view publication_kind) {
    if (descriptor.transport != mmltk::browser::FrameTransportKind::CudaDeviceBuffer) {
        throw std::runtime_error(std::string(publication_kind) + " requires CUDA device-buffer transport");
    }
    if (descriptor.pixel_format != mmltk::browser::FramePixelFormat::Rgba8) {
        throw std::runtime_error(std::string(publication_kind) + " requires RGBA8 pixel format");
    }
    if (!mmltk::browser::frame_slot_has_valid_byte_layout(descriptor)) {
        throw std::runtime_error(std::string(publication_kind) + " requires a valid frame byte layout");
    }
    if (descriptor.lifecycle != mmltk::browser::FrameSlotLifecycle::ExplicitRelease) {
        throw std::runtime_error(std::string(publication_kind) + " requires explicit release semantics");
    }
    if (descriptor.ownership != mmltk::browser::FrameSlotOwnership::NativeHost) {
        throw std::runtime_error(std::string(publication_kind) + " requires native-host ownership");
    }
    if (!mmltk::browser::frame_slot_native_import_metadata(descriptor).has_value()) {
        throw std::runtime_error(std::string(publication_kind) + " requires native import metadata");
    }
}

void validate_imported_frame_source(const mmltk::browser::FrameSlotDescriptor& descriptor,
                                    const RetainedBrowserImportedFrameSource& imported_frame_source) {
    const std::optional<LinuxImportedFrameSourceContract> linux_import =
        linux_imported_frame_source_contract(imported_frame_source);
    if (!linux_import.has_value() || linux_import->image.kind != LinuxImportedImageSourceKind::DmaBufRgba ||
        linux_import->image.fd == 0U) {
        throw std::runtime_error(
            "retained browser imported frame source requires "
            "a DMA-BUF RGBA image");
    }
    if (linux_import->image.cuda_device_index < 0) {
        throw std::runtime_error(
            "retained browser imported frame source requires "
            "a non-negative CUDA device index");
    }
    const std::uint64_t min_rgba_row_bytes = static_cast<std::uint64_t>(descriptor.width) * 4U;
    const bool modifier_valid =
        (linux_import->image.modifier_mode == LinuxDmaBufModifierMode::Implicit &&
         linux_import->image.drm_modifier == kDrmFormatModInvalid) ||
        (linux_import->image.modifier_mode == LinuxDmaBufModifierMode::Explicit &&
         linux_import->image.drm_modifier != kDrmFormatModInvalid);
    if (linux_import->image.width != descriptor.width || linux_import->image.height != descriptor.height ||
        linux_import->image.stride_bytes != descriptor.row_stride_bytes ||
        linux_import->image.stride_bytes < min_rgba_row_bytes ||
        linux_import->image.allocation_size < descriptor.byte_length ||
        linux_import->image.drm_format != kDrmFormatAbgr8888 || !modifier_valid) {
        throw std::runtime_error(
            "retained browser imported frame source requires valid DMA-BUF "
            "layout and modifier metadata");
    }
    if (linux_import->image.offset > linux_import->image.allocation_size ||
        descriptor.byte_length > linux_import->image.allocation_size - linux_import->image.offset) {
        throw std::runtime_error(
            "retained browser imported frame source DMA-BUF layout is smaller "
            "than the published workspace frame");
    }
    if (linux_import->ready_sync.kind != LinuxImportedSyncHandleKind::CudaEvent ||
        linux_import->ready_sync.handle == 0U) {
        throw std::runtime_error(
            "retained browser imported frame source requires "
            "a CUDA event for readiness");
    }

    const std::optional<LinuxImportedFrameLifecycleContract> linux_lifecycle =
        linux_imported_frame_lifecycle_contract(imported_frame_source);
    if (!linux_lifecycle.has_value()) {
        return;
    }
    if (linux_lifecycle->release_sync.kind == LinuxImportedSyncHandleKind::None &&
        linux_lifecycle->release_sync.handle != 0U) {
        throw std::runtime_error(
            "retained browser imported frame lifecycle "
            "carried a release sync handle "
            "without a concrete sync kind");
    }
    if (linux_lifecycle->release_sync.kind != LinuxImportedSyncHandleKind::None &&
        linux_lifecycle->release_sync.handle == 0U) {
        throw std::runtime_error(
            "retained browser imported frame lifecycle "
            "carried an empty release sync handle");
    }
}

struct RetainedEntry {
    std::optional<mmltk::browser::WorkspaceSurfaceInfo> workspace_surface;
    std::optional<RetainedBrowserImportedFrameSource> imported_frame_source;
    RetainedBrowserFrameRegistry::ReleaseFn release_fn;

    [[nodiscard]] std::optional<RetainedBrowserImportedFrameSource> imported_frame() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (released || !imported_frame_source.has_value()) {
            return std::nullopt;
        }
        acquired = true;
        return imported_frame_source;
    }

    [[nodiscard]] ImportedDiscardDisposition discard_surface() {
        std::lock_guard<std::mutex> lock(mutex);
        if (released || retired || !imported_frame_source.has_value()) {
            return ImportedDiscardDisposition::NotFound;
        }
        if (!acquired) {
            return ImportedDiscardDisposition::Released;
        }
        retired = true;
        return ImportedDiscardDisposition::RetainedUntilRelease;
    }

    bool release_once() {
        RetainedBrowserFrameRegistry::ReleaseFn release_fn_local;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (released) {
                return false;
            }
            released = true;
            release_fn_local = std::move(release_fn);
        }
        if (release_fn_local) {
            release_fn_local();
        }
        return true;
    }

    mutable std::mutex mutex;
    bool released = false;
    mutable bool acquired = false;
    bool retired = false;
};

}  // namespace

struct RetainedBrowserFrameRegistry::Impl {
    template <typename Map>
    [[nodiscard]] static std::shared_ptr<RetainedEntry> find_in_map(const Map& entries, const std::string_view key) {
        const auto entry_it = entries.find(std::string(key));
        if (entry_it == entries.end()) {
            return nullptr;
        }
        return entry_it->second;
    }

    [[nodiscard]] bool contains_workspace_surface(const std::string_view surface_id,
                                                  const std::string_view revision) const {
        return find_in_map(entries_by_surface_key, workspace_surface_key(surface_id, revision)) != nullptr;
    }

    void erase_entry_locked(const std::shared_ptr<RetainedEntry>& entry) {
        if (entry == nullptr) {
            return;
        }

        if (const auto* surface_info = workspace_surface_ptr(entry->workspace_surface); surface_info != nullptr) {
            entries_by_surface_key.erase(workspace_surface_key(surface_info->surface_id, surface_info->revision));
        }
    }

    void retain_workspace_surface(std::shared_ptr<RetainedEntry> entry) {
        if (entry == nullptr) {
            throw std::runtime_error(
                "retained browser workspace surface publication requires "
                "workspace surface metadata");
        }

        const auto* surface_info = workspace_surface_ptr(entry->workspace_surface);
        if (surface_info == nullptr) {
            throw std::runtime_error(
                "retained browser workspace surface publication requires "
                "workspace surface metadata");
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (contains_workspace_surface(surface_info->surface_id, surface_info->revision)) {
            throw std::runtime_error("retained browser workspace surface collision while publishing");
        }

        entries_by_surface_key.emplace(workspace_surface_key(surface_info->surface_id, surface_info->revision),
                                       std::move(entry));
    }

    [[nodiscard]] std::shared_ptr<RetainedEntry> find_workspace_surface(std::string_view surface_id,
                                                                        std::string_view revision) const {
        std::lock_guard<std::mutex> lock(mutex);
        return find_in_map(entries_by_surface_key, workspace_surface_key(surface_id, revision));
    }

    [[nodiscard]] std::shared_ptr<RetainedEntry> erase_workspace_surface(std::string_view surface_id,
                                                                         std::string_view revision) {
        std::lock_guard<std::mutex> lock(mutex);
        std::shared_ptr<RetainedEntry> entry =
            find_in_map(entries_by_surface_key, workspace_surface_key(surface_id, revision));
        if (entry == nullptr) {
            return nullptr;
        }
        erase_entry_locked(entry);
        return entry;
    }

    [[nodiscard]] std::vector<std::shared_ptr<RetainedEntry>> erase_all() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::shared_ptr<RetainedEntry>> entries;
        entries.reserve(entries_by_surface_key.size());
        for (const auto& [key, entry] : entries_by_surface_key) {
            (void)key;
            if (entry != nullptr) {
                entries.push_back(entry);
            }
        }
        entries_by_surface_key.clear();
        return entries;
    }

    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<RetainedEntry>> entries_by_surface_key;
};

RetainedLinuxImageFd::~RetainedLinuxImageFd() {
    reset();
}

RetainedLinuxImageFd::RetainedLinuxImageFd(RetainedLinuxImageFd&& other) noexcept : fd_(other.release()) {}

RetainedLinuxImageFd& RetainedLinuxImageFd::operator=(RetainedLinuxImageFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

void RetainedLinuxImageFd::reset(const int fd) noexcept {
    if (fd_ >= 0) {
        (void)::close(fd_);
    }
    fd_ = fd;
}

RetainedBrowserFrameRegistry::RetainedBrowserFrameRegistry() : impl_(std::make_unique<Impl>()) {}

RetainedBrowserFrameRegistry::~RetainedBrowserFrameRegistry() {
    release_all();
}

RetainedBrowserFrameRegistry::RetainedBrowserFrameRegistry(RetainedBrowserFrameRegistry&& other) noexcept = default;

RetainedBrowserFrameRegistry& RetainedBrowserFrameRegistry::operator=(RetainedBrowserFrameRegistry&& other) noexcept =
    default;

void RetainedBrowserFrameRegistry::retain_workspace_surface(const mmltk::browser::WorkspaceSurfaceInfo& surface_info,
                                                            RetainedBrowserImportedFrameSource imported_frame_source,
                                                            ReleaseFn release_fn) {
    const mmltk::browser::FrameSlotDescriptor& descriptor = imported_frame_source.descriptor;
    validate_workspace_surface_info(surface_info, descriptor);
    validate_publication_descriptor(descriptor, "retained browser workspace surface publication");

    normalize_retained_browser_imported_frame_source(imported_frame_source);
    validate_imported_frame_source(descriptor, imported_frame_source);

    auto entry = std::make_shared<RetainedEntry>();
    entry->workspace_surface = surface_info;
    entry->imported_frame_source = std::move(imported_frame_source);
    entry->release_fn = std::move(release_fn);
    impl_->retain_workspace_surface(std::move(entry));
}

std::optional<RetainedBrowserImportedFrameSource> RetainedBrowserFrameRegistry::imported_frame_source_for_surface(
    const std::string_view surface_id, const std::string_view revision) const {
    if (surface_id.empty() || revision.empty()) {
        return std::nullopt;
    }

    const std::shared_ptr<RetainedEntry> entry = impl_->find_workspace_surface(surface_id, revision);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return entry->imported_frame();
}

bool RetainedBrowserFrameRegistry::release_workspace_surface(const std::string_view surface_id,
                                                             const std::string_view revision) {
    if (surface_id.empty() || revision.empty()) {
        return false;
    }

    const std::shared_ptr<RetainedEntry> entry = impl_->erase_workspace_surface(surface_id, revision);
    return entry != nullptr && entry->release_once();
}

bool RetainedBrowserFrameRegistry::discard_workspace_surface(const mmltk::browser::WorkspaceSurfaceInfo& surface_info) {
    if (surface_info.surface_id.empty() || surface_info.revision.empty()) {
        return false;
    }

    const std::shared_ptr<RetainedEntry> entry =
        impl_->find_workspace_surface(surface_info.surface_id, surface_info.revision);
    if (entry == nullptr) {
        return false;
    }

    switch (entry->discard_surface()) {
        case ImportedDiscardDisposition::NotFound:
            return false;
        case ImportedDiscardDisposition::Released: {
            const std::shared_ptr<RetainedEntry> erased_entry =
                impl_->erase_workspace_surface(surface_info.surface_id, surface_info.revision);
            return erased_entry != nullptr && erased_entry->release_once();
        }
        case ImportedDiscardDisposition::RetainedUntilRelease:
            return true;
    }
    return false;
}

void RetainedBrowserFrameRegistry::release_all() {
    if (impl_ == nullptr) {
        return;
    }
    for (const std::shared_ptr<RetainedEntry>& entry : impl_->erase_all()) {
        if (entry != nullptr) {
            (void)entry->release_once();
        }
    }
}

}  // namespace mmltk::gui
