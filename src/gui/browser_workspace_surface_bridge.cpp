#include "browser_workspace_surface_bridge.h"

#include <utility>

namespace mmltk::gui {

void BrowserWorkspaceSurfaceBridge::publish(const BrowserWorkspaceSurfaceOwner owner,
                                            RetainedBrowserImportedFrameSource source, ReleaseFn release_fn,
                                            const std::optional<mmltk::live::LiveFrameId> live_frame_id) {
    const mmltk::browser::FrameSlotDescriptor descriptor = source.descriptor;
    const std::optional<mmltk::browser::WorkspaceSurfaceInfo> surface_info =
        make_browser_workspace_surface_info(descriptor, descriptor.frame_id.sequence);
    if (!surface_info.has_value()) {
        throw std::runtime_error(
            "browser workspace surface bridge publish requires a workspace "
            "descriptor");
    }

    discard_current_surface();
    registry_.retain_workspace_surface(*surface_info, std::move(source), std::move(release_fn));
    owner_ = owner;
    surface_info_ = *surface_info;
    preview_.publish(descriptor, live_frame_id);
}

void BrowserWorkspaceSurfaceBridge::clear_if_owner(const BrowserWorkspaceSurfaceOwner owner) {
    if (owner_ == owner) {
        discard_current_surface();
    }
}

void BrowserWorkspaceSurfaceBridge::clear() {
    discard_current_surface();
}

void BrowserWorkspaceSurfaceBridge::reset() {
    owner_ = BrowserWorkspaceSurfaceOwner::None;
    surface_info_.reset();
    preview_.clear();
    registry_.release_all();
}

std::optional<mmltk::browser::WorkspaceSurfaceInfo> BrowserWorkspaceSurfaceBridge::acquire_current_surface(
    const std::string_view surface_id) const {
    if (surface_id != kBrowserWorkspaceSurfaceId || !surface_info_.has_value()) {
        return std::nullopt;
    }
    return surface_info_;
}

std::optional<RetainedBrowserImportedFrameSource> BrowserWorkspaceSurfaceBridge::acquire_surface_source(
    const std::string_view surface_id, const std::string_view revision) const {
    if (surface_id != kBrowserWorkspaceSurfaceId || revision.empty()) {
        return std::nullopt;
    }
    return registry_.imported_frame_source_for_surface(surface_id, revision);
}

bool BrowserWorkspaceSurfaceBridge::release_surface(const std::string_view surface_id,
                                                    const std::string_view revision) {
    if (!registry_.release_workspace_surface(surface_id, revision)) {
        return false;
    }
    if (surface_info_.has_value() && surface_info_->surface_id == surface_id && surface_info_->revision == revision) {
        owner_ = BrowserWorkspaceSurfaceOwner::None;
        surface_info_.reset();
        preview_.clear();
    }
    return true;
}

void BrowserWorkspaceSurfaceBridge::discard_current_surface() {
    if (surface_info_.has_value()) {
        (void)registry_.discard_workspace_surface(*surface_info_);
    }
    owner_ = BrowserWorkspaceSurfaceOwner::None;
    surface_info_.reset();
    preview_.clear();
}

}  // namespace mmltk::gui
