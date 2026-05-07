#pragma once

#include "gui/browser_retained_frame_registry.h"
#include "gui/workspace_gpu_bridge_result.h"
#include "mmltk/live/live_types.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mmltk::gui {

inline constexpr std::string_view kRendererWorkspaceGpuBridgeStateMessageName = "mmltk.workspace_gpu_bridge.state";
inline constexpr std::string_view kRendererWorkspaceGpuBridgeReleaseMessageName = "mmltk.workspace_gpu_bridge.release";

struct WorkspaceGpuBridgeExportedSharedImage {
    std::string serialized_json;
    std::string mailbox_json;
    std::string result_code;
    std::string result_detail;
    bool has_metadata = false;
    bool has_creation_sync_token = false;
    std::uint32_t texture_target = 0;
    bool is_software = false;

    [[nodiscard]] bool is_valid() const noexcept {
        return !serialized_json.empty() && !mailbox_json.empty() && has_metadata && has_creation_sync_token;
    }

    [[nodiscard]] bool has_live_image() const noexcept {
        return is_valid() && texture_target != 0U && !is_software;
    }

    bool operator==(const WorkspaceGpuBridgeExportedSharedImage&) const noexcept = default;
};

struct WorkspaceGpuBridgeSurfacePublication {
    mmltk::browser::WorkspaceSurfaceInfo surface_info;
    std::optional<WorkspaceGpuBridgeExportedSharedImage> exported_shared_image;
    std::string export_result_code;
    std::string export_result_detail;
    std::string export_error;

    [[nodiscard]] bool has_live_exported_shared_image() const noexcept {
        return exported_shared_image.has_value() && exported_shared_image->has_live_image();
    }

    bool operator==(const WorkspaceGpuBridgeSurfacePublication&) const noexcept = default;
};

struct WorkspaceGpuBridgeStructuralCapability {
    bool available = false;
    std::string detail;

    bool operator==(const WorkspaceGpuBridgeStructuralCapability&) const noexcept = default;
};

struct WorkspaceGpuBridgeZeroCopyCapability {
    bool attempted = false;
    bool available = false;
    std::string result_code;
    std::string result_detail;

    bool operator==(const WorkspaceGpuBridgeZeroCopyCapability&) const noexcept = default;
};

struct WorkspaceGpuBridgeCefResult {
    bool ok = false;
    std::string result_code;
    std::string result_detail;

    [[nodiscard]] std::string message(std::string_view fallback) const;
};

struct WorkspaceGpuBridgePresentRects {
    std::int32_t bounds_x = 0;
    std::int32_t bounds_y = 0;
    std::int32_t bounds_width = 0;
    std::int32_t bounds_height = 0;
    std::int32_t damage_x = 0;
    std::int32_t damage_y = 0;
    std::int32_t damage_width = 0;
    std::int32_t damage_height = 0;
};

[[nodiscard]] std::optional<WorkspaceGpuBridgeSurfacePublication> make_workspace_gpu_bridge_surface_publication(
    const mmltk::browser::WorkspaceSurfaceInfo& surface_info, const RetainedBrowserImportedFrameSource& source);

[[nodiscard]] bool cef_workspace_gpu_bridge_zero_copy_import_supported() noexcept;

[[nodiscard]] bool cef_workspace_gpu_bridge_swapchain_present_supported() noexcept;

[[nodiscard]] WorkspaceGpuBridgeCefResult configure_cef_workspace_gpu_bridge_swapchain(
    std::string_view surface_id, const mmltk::live::WorkspaceSwapchainDescriptor& descriptor,
    std::optional<int> cuda_device_index);

[[nodiscard]] WorkspaceGpuBridgeCefResult present_cef_workspace_gpu_bridge_front_slot(
    std::string_view surface_id, const mmltk::live::WorkspacePresentSnapshot& present,
    const WorkspaceGpuBridgePresentRects& rects);

[[nodiscard]] WorkspaceGpuBridgeCefResult destroy_cef_workspace_gpu_bridge_swapchain(std::string_view surface_id);

void release_workspace_gpu_bridge_publication(std::string_view surface_id, std::string_view revision);

void send_workspace_gpu_bridge_publication(const CefRefPtr<CefBrowser>& browser,
                                           const std::optional<WorkspaceGpuBridgeSurfacePublication>& publication);

class CefWorkspaceGpuBridge {
   public:
    struct SurfaceState {
        bool state_known = false;
        std::optional<WorkspaceGpuBridgeSurfacePublication> publication;
        WorkspaceGpuBridgeStructuralCapability structural_capability;
        WorkspaceGpuBridgeZeroCopyCapability zero_copy;
    };

    void install_in_context(const CefRefPtr<CefBrowser>& browser, const CefRefPtr<CefFrame>& frame,
                            const CefRefPtr<CefV8Context>& context);

    [[nodiscard]] bool on_process_message_received(const CefRefPtr<CefBrowser>& browser,
                                                   const CefRefPtr<CefFrame>& frame, CefProcessId source_process,
                                                   const CefRefPtr<CefProcessMessage>& message);

    void on_browser_destroyed(int browser_identifier);

    [[nodiscard]] const SurfaceState* surface_state_for_browser(int browser_identifier) const noexcept;

    void forget_surface_revision(int browser_identifier, std::string_view surface_id, std::string_view revision);

    bool record_zero_copy_result(int browser_identifier, bool available, std::string result_code,
                                 std::string result_detail);

   private:
    std::unordered_map<int, SurfaceState> surfaces_by_browser_id_;
};

}  // namespace mmltk::gui
