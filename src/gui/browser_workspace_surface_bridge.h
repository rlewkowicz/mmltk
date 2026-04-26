#pragma once

#include "gui/browser_frame_slot_contract.h"
#include "gui/browser_retained_frame_registry.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mmltk::gui {

inline constexpr std::string_view kBrowserWorkspaceSurfaceId = "workspace";

[[nodiscard]] inline bool is_browser_workspace_surface_descriptor(
    const mmltk::browser::FrameSlotDescriptor& descriptor) noexcept {
    return descriptor.slot_name == kBrowserWorkspaceSurfaceId &&
           mmltk::browser::frame_slot_has_valid_byte_layout(descriptor);
}

[[nodiscard]] inline std::string browser_workspace_surface_revision(
    const mmltk::browser::FrameSlotDescriptor& descriptor, const std::uint64_t fallback_revision = 0) {
    const std::uint64_t revision_value = descriptor.frame_id.sequence != 0U ? descriptor.frame_id.sequence
                                         : fallback_revision != 0U          ? fallback_revision
                                                                            : descriptor.ready_ns;
    return revision_value == 0U ? std::string{} : std::to_string(revision_value);
}

[[nodiscard]] inline std::optional<mmltk::browser::WorkspaceSurfaceInfo> make_browser_workspace_surface_info(
    const mmltk::browser::FrameSlotDescriptor& descriptor, const std::uint64_t fallback_revision = 0) {
    if (!is_browser_workspace_surface_descriptor(descriptor)) {
        return std::nullopt;
    }

    mmltk::browser::WorkspaceSurfaceInfo surface_info;
    surface_info.surface_id = std::string(kBrowserWorkspaceSurfaceId);
    surface_info.revision = browser_workspace_surface_revision(descriptor, fallback_revision);
    if (surface_info.revision.empty()) {
        return std::nullopt;
    }
    surface_info.width = descriptor.width;
    surface_info.height = descriptor.height;
    surface_info.texture_format = "rgba8unorm";
    surface_info.opaque = true;
    surface_info.upright = true;
    return surface_info;
}

enum class BrowserWorkspaceSurfaceOwner : std::uint8_t {
    None = 0,
    Live = 1,
    PredictStatic = 2,
    Annotate = 3,
};

class BrowserWorkspaceSurfaceBridge {
   public:
    using ReleaseFn = RetainedBrowserFrameRegistry::ReleaseFn;

    struct PreviewRuntimeState {
        bool has_frame = false;
        mmltk::browser::CaptureRegion displayed_region{};
        std::uint64_t frame_id = 0;
        std::optional<mmltk::live::LiveFrameId> live_frame_id;

        void clear() noexcept {
            has_frame = false;
            displayed_region = {};
            frame_id = 0;
            live_frame_id.reset();
        }

        void publish(const mmltk::browser::FrameSlotDescriptor& descriptor,
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
        }
    };

    void publish(BrowserWorkspaceSurfaceOwner owner, RetainedBrowserImportedFrameSource source, ReleaseFn release_fn,
                 const std::optional<mmltk::live::LiveFrameId> live_frame_id = std::nullopt);

    void clear_if_owner(BrowserWorkspaceSurfaceOwner owner);

    void clear();

    void reset();

    [[nodiscard]] std::optional<mmltk::browser::WorkspaceSurfaceInfo> acquire_current_surface(
        std::string_view surface_id) const;

    [[nodiscard]] std::optional<RetainedBrowserImportedFrameSource> acquire_surface_source(
        std::string_view surface_id, std::string_view revision) const;

    bool release_surface(std::string_view surface_id, std::string_view revision);

    [[nodiscard]] const std::optional<mmltk::browser::WorkspaceSurfaceInfo>& surface_info() const noexcept {
        return surface_info_;
    }

    [[nodiscard]] const PreviewRuntimeState& preview() const noexcept {
        return preview_;
    }

    [[nodiscard]] BrowserWorkspaceSurfaceOwner owner() const noexcept {
        return owner_;
    }

   private:
    void discard_current_surface();

    BrowserWorkspaceSurfaceOwner owner_ = BrowserWorkspaceSurfaceOwner::None;
    std::optional<mmltk::browser::WorkspaceSurfaceInfo> surface_info_;
    PreviewRuntimeState preview_{};
    RetainedBrowserFrameRegistry registry_;
};

}  // namespace mmltk::gui
