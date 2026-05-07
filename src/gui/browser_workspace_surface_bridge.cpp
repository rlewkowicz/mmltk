#include "browser_workspace_surface_bridge.h"

#include "browser_dmabuf_fd.h"

#include <chrono>
#include <memory>
#include <utility>

namespace mmltk::gui {

namespace {

[[nodiscard]] std::uint64_t browser_bridge_now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] std::optional<mmltk::browser::WorkspaceSurfaceInfo> workspace_surface_info_from_present(
    const mmltk::live::WorkspacePresentSnapshot& present) {
    if (!present.valid || present.revision == 0U || present.dims.width == 0U || present.dims.height == 0U) {
        return std::nullopt;
    }
    mmltk::browser::WorkspaceSurfaceInfo surface_info;
    surface_info.surface_id = std::string(kBrowserWorkspaceSurfaceId);
    surface_info.revision = std::to_string(present.revision);
    surface_info.width = present.dims.width;
    surface_info.height = present.dims.height;
    surface_info.texture_format = "rgba8unorm";
    surface_info.opaque = true;
    surface_info.upright = true;
    return surface_info;
}

}  // namespace

RetainedBrowserImportedFrameSource make_retained_workspace_surface_source(
    const mmltk::browser::FrameSlotDescriptor& descriptor, const mmltk::live::WorkspaceOutputBundle& bundle,
    const int cuda_device_index, const std::optional<LinuxImportedFrameLifecycleContract>& lifecycle_contract) {
    return make_retained_workspace_dmabuf_source(descriptor, bundle, cuda_device_index, lifecycle_contract);
}

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
    preview_.publish(owner, descriptor, surface_info_->revision, browser_bridge_now_ns(), live_frame_id);
}

void BrowserWorkspaceSurfaceBridge::configure_swapchain(
    const BrowserWorkspaceSurfaceOwner owner, mmltk::live::WorkspaceSwapchainDescriptor descriptor) {
    if (!descriptor.valid()) {
        throw std::runtime_error("browser workspace surface bridge requires a valid persistent swapchain descriptor");
    }
    discard_current_surface();
    owner_ = owner;
    swapchain_descriptor_ = std::move(descriptor);
    latest_present_ = {};
}

bool BrowserWorkspaceSurfaceBridge::present_swapchain(const BrowserWorkspaceSurfaceOwner owner,
                                                      const mmltk::live::WorkspacePresentSnapshot& present) {
    if (!swapchain_descriptor_.has_value() || owner_ != owner || !present.valid ||
        swapchain_descriptor_->generation != present.swapchain_generation) {
        return false;
    }
    if (present.front_slot_index >= swapchain_descriptor_->slots.size()) {
        return false;
    }
    const std::optional<mmltk::browser::WorkspaceSurfaceInfo> next_surface_info =
        workspace_surface_info_from_present(present);
    if (!next_surface_info.has_value()) {
        return false;
    }

    owner_ = owner;
    latest_present_ = present;
    surface_info_ = *next_surface_info;
    preview_.publish_present(owner, present, browser_bridge_now_ns());
    return true;
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
    swapchain_descriptor_.reset();
    latest_present_ = {};
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

void BrowserWorkspaceSurfaceBridge::record_error(const BrowserWorkspaceSurfaceOwner owner, std::string surface_revision,
                                                 std::string error_message) {
    preview_.record_error(owner, std::move(surface_revision), browser_bridge_now_ns(), std::move(error_message));
}

void BrowserWorkspaceSurfaceBridge::clear_error() noexcept {
    preview_.clear_error();
}

void BrowserWorkspaceSurfaceBridge::discard_current_surface() {
    if (surface_info_.has_value()) {
        (void)registry_.discard_workspace_surface(*surface_info_);
    }
    owner_ = BrowserWorkspaceSurfaceOwner::None;
    surface_info_.reset();
    swapchain_descriptor_.reset();
    latest_present_ = {};
    preview_.clear();
}

}  // namespace mmltk::gui
