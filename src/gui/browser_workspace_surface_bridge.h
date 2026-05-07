#pragma once

#include "gui/browser_retained_frame_registry.h"
#include "gui/browser_dmabuf_fd.h"
#include "gui/browser_workspace_surface_contract.h"
#include "mmltk/live/live_types.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mmltk::gui {

enum class BrowserWorkspaceSurfaceOwner : std::uint8_t {
    None = 0,
    Live = 1,
    PredictStatic = 2,
    Annotate = 3,
};

[[nodiscard]] inline const char* browser_workspace_surface_owner_name(
    const BrowserWorkspaceSurfaceOwner owner) noexcept {
    switch (owner) {
        case BrowserWorkspaceSurfaceOwner::Live:
            return "live";
        case BrowserWorkspaceSurfaceOwner::PredictStatic:
            return "predict_static";
        case BrowserWorkspaceSurfaceOwner::Annotate:
            return "annotate";
        case BrowserWorkspaceSurfaceOwner::None:
            break;
    }
    return "none";
}

[[nodiscard]] RetainedBrowserImportedFrameSource make_retained_workspace_surface_source(
    const mmltk::browser::FrameSlotDescriptor& descriptor, const mmltk::live::WorkspaceOutputBundle& bundle,
    int cuda_device_index, const std::optional<LinuxImportedFrameLifecycleContract>& lifecycle_contract = std::nullopt);

template <typename Bundle>
[[nodiscard]] RetainedBrowserImportedFrameSource make_retained_workspace_dmabuf_source(
    const mmltk::browser::FrameSlotDescriptor& descriptor, const Bundle& bundle, const int cuda_device_index,
    const std::optional<LinuxImportedFrameLifecycleContract>& lifecycle_contract = std::nullopt) {
    RetainedBrowserImportedFrameSource source;
    source.descriptor = descriptor;
    mmltk::browser::normalize_frame_slot_native_import_metadata(source.descriptor);

    const mmltk::live::WorkspaceDmaBufImage& dmabuf = bundle.dmabuf_image;
    std::shared_ptr<RetainedLinuxImageFd> owned_fd;
    std::uintptr_t fd_handle = 0U;
    if (dmabuf.valid()) {
        const int duplicate_fd =
            duplicate_workspace_dmabuf_fd(dmabuf.fd, "fcntl(F_DUPFD_CLOEXEC) for retained workspace DMA-BUF");
        owned_fd = std::make_shared<RetainedLinuxImageFd>(duplicate_fd);
        fd_handle = static_cast<std::uintptr_t>(duplicate_fd);
    }

    apply_linux_imported_frame_source_contract(
        source,
        LinuxImportedFrameSourceContract{
            make_workspace_dmabuf_image_source(dmabuf, fd_handle, cuda_device_index),
            LinuxImportedSyncSource{
                bundle.ready_event == nullptr ? LinuxImportedSyncHandleKind::None
                                              : LinuxImportedSyncHandleKind::CudaEvent,
                reinterpret_cast<std::uintptr_t>(bundle.ready_event),
                descriptor.ready_sync.value,
            },
            LinuxImportedSyncSource{
                bundle.stream == nullptr ? LinuxImportedSyncHandleKind::None : LinuxImportedSyncHandleKind::CudaStream,
                reinterpret_cast<std::uintptr_t>(bundle.stream),
                0U,
            },
        });
    source.owned_image_fd = std::move(owned_fd);
    if (lifecycle_contract.has_value()) {
        apply_linux_imported_frame_lifecycle_contract(source, *lifecycle_contract);
    }
    normalize_retained_browser_imported_frame_source(source);
    return source;
}

class BrowserWorkspaceSurfaceBridge {
   public:
    using ReleaseFn = RetainedBrowserFrameRegistry::ReleaseFn;

    struct PreviewRuntimeState {
        bool has_frame = false;
        mmltk::browser::CaptureRegion displayed_region{};
        std::uint64_t frame_id = 0;
        std::optional<mmltk::live::LiveFrameId> live_frame_id;
        std::string surface_revision;
        BrowserWorkspaceSurfaceOwner owner = BrowserWorkspaceSurfaceOwner::None;
        std::uint64_t publish_ns = 0;
        std::string last_error;

        void clear() noexcept {
            has_frame = false;
            displayed_region = {};
            frame_id = 0;
            live_frame_id.reset();
            surface_revision.clear();
            owner = BrowserWorkspaceSurfaceOwner::None;
            publish_ns = 0;
            last_error.clear();
        }

        void publish(BrowserWorkspaceSurfaceOwner next_owner, const mmltk::browser::FrameSlotDescriptor& descriptor,
                     std::string next_surface_revision, std::uint64_t next_publish_ns,
                     const std::optional<mmltk::live::LiveFrameId> next_live_frame_id = std::nullopt) noexcept {
            has_frame = true;
            displayed_region = descriptor.capture_region;
            if (displayed_region.width == 0U) {
                displayed_region.width = descriptor.width;
            }
            if (displayed_region.height == 0U) {
                displayed_region.height = descriptor.height;
            }
            frame_id = descriptor.frame_id.sequence;
            live_frame_id = next_live_frame_id;
            surface_revision = std::move(next_surface_revision);
            owner = next_owner;
            publish_ns = next_publish_ns;
            last_error.clear();
        }

        void publish_present(BrowserWorkspaceSurfaceOwner next_owner,
                             const mmltk::live::WorkspacePresentSnapshot& present,
                             std::uint64_t next_publish_ns) noexcept {
            has_frame = present.valid;
            displayed_region = mmltk::browser::CaptureRegion{present.source_region.x, present.source_region.y,
                                                              present.source_region.width, present.source_region.height};
            if (displayed_region.width == 0U) {
                displayed_region.width = present.dims.width;
            }
            if (displayed_region.height == 0U) {
                displayed_region.height = present.dims.height;
            }
            frame_id = present.frame_id.sequence;
            live_frame_id = present.frame_id;
            surface_revision = present.revision == 0U ? std::string{} : std::to_string(present.revision);
            owner = next_owner;
            publish_ns = next_publish_ns;
            last_error.clear();
        }

        void record_error(BrowserWorkspaceSurfaceOwner next_owner, std::string next_surface_revision,
                          std::uint64_t next_publish_ns, std::string error_message) {
            if (next_owner != BrowserWorkspaceSurfaceOwner::None) {
                owner = next_owner;
            }
            if (!next_surface_revision.empty()) {
                surface_revision = std::move(next_surface_revision);
            }
            publish_ns = next_publish_ns;
            last_error = std::move(error_message);
        }

        void clear_error() noexcept {
            last_error.clear();
        }
    };

    void publish(BrowserWorkspaceSurfaceOwner owner, RetainedBrowserImportedFrameSource source, ReleaseFn release_fn,
                 const std::optional<mmltk::live::LiveFrameId> live_frame_id = std::nullopt);

    void configure_swapchain(BrowserWorkspaceSurfaceOwner owner,
                             mmltk::live::WorkspaceSwapchainDescriptor descriptor);

    bool present_swapchain(BrowserWorkspaceSurfaceOwner owner, const mmltk::live::WorkspacePresentSnapshot& present);

    void clear_if_owner(BrowserWorkspaceSurfaceOwner owner);

    void clear();

    void reset();

    [[nodiscard]] std::optional<mmltk::browser::WorkspaceSurfaceInfo> acquire_current_surface(
        std::string_view surface_id) const;

    [[nodiscard]] std::optional<RetainedBrowserImportedFrameSource> acquire_surface_source(
        std::string_view surface_id, std::string_view revision) const;

    bool release_surface(std::string_view surface_id, std::string_view revision);
    void record_error(BrowserWorkspaceSurfaceOwner owner, std::string surface_revision, std::string error_message);
    void clear_error() noexcept;

    [[nodiscard]] const std::optional<mmltk::browser::WorkspaceSurfaceInfo>& surface_info() const noexcept {
        return surface_info_;
    }

    [[nodiscard]] const PreviewRuntimeState& preview() const noexcept {
        return preview_;
    }

    [[nodiscard]] const std::optional<mmltk::live::WorkspaceSwapchainDescriptor>& swapchain_descriptor()
        const noexcept {
        return swapchain_descriptor_;
    }

    [[nodiscard]] const mmltk::live::WorkspacePresentSnapshot& latest_present() const noexcept {
        return latest_present_;
    }

    [[nodiscard]] bool native_presented() const noexcept {
        return swapchain_descriptor_.has_value() && latest_present_.valid;
    }

    [[nodiscard]] BrowserWorkspaceSurfaceOwner owner() const noexcept {
        return owner_;
    }

   private:
    void discard_current_surface();

    BrowserWorkspaceSurfaceOwner owner_ = BrowserWorkspaceSurfaceOwner::None;
    std::optional<mmltk::browser::WorkspaceSurfaceInfo> surface_info_;
    std::optional<mmltk::live::WorkspaceSwapchainDescriptor> swapchain_descriptor_;
    mmltk::live::WorkspacePresentSnapshot latest_present_{};
    PreviewRuntimeState preview_{};
    RetainedBrowserFrameRegistry registry_;
};

}  // namespace mmltk::gui
