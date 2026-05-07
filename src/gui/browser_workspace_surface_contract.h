#pragma once

#include "gui/browser_frame_slot_contract.h"
#include "gui/browser_retained_frame_registry.h"
#include "mmltk/live/live_types.h"

#include <cstdint>
#include <optional>
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

[[nodiscard]] inline LinuxDmaBufModifierMode linux_modifier_mode_from_live(
    const mmltk::live::DmaBufModifierMode mode) noexcept {
    switch (mode) {
        case mmltk::live::DmaBufModifierMode::Implicit:
            return LinuxDmaBufModifierMode::Implicit;
        case mmltk::live::DmaBufModifierMode::Explicit:
            return LinuxDmaBufModifierMode::Explicit;
        case mmltk::live::DmaBufModifierMode::Unknown:
            return LinuxDmaBufModifierMode::Unknown;
    }
    return LinuxDmaBufModifierMode::Unknown;
}

[[nodiscard]] inline LinuxImportedImageSource make_workspace_dmabuf_image_source(
    const mmltk::live::WorkspaceDmaBufImage& dmabuf, const std::uintptr_t fd_handle, const int cuda_device_index) {
    return LinuxImportedImageSource{
        dmabuf.valid() ? LinuxImportedImageSourceKind::DmaBufRgba : LinuxImportedImageSourceKind::Unknown,
        fd_handle,
        cuda_device_index,
        dmabuf.width,
        dmabuf.height,
        dmabuf.stride_bytes,
        dmabuf.offset,
        dmabuf.allocation_size,
        dmabuf.drm_format,
        dmabuf.drm_modifier,
        linux_modifier_mode_from_live(dmabuf.modifier_mode),
    };
}

}  // namespace mmltk::gui
