#include "gui/cef_workspace_gpu_bridge.h"

#include "gui/browser_runtime_shared.h"
#include "gui/browser_workspace_surface_contract.h"
#include "gui/cef_subprocess_app.h"

#include "include/capi/cef_v8_capi.h"
#include "include/internal/cef_types.h"
#include "include/wrapper/cef_helpers.h"
#ifndef WRAPPING_CEF_SHARED
#define MMLTK_DEFINED_WRAPPING_CEF_SHARED
#define WRAPPING_CEF_SHARED 1
#endif
#include "libcef_dll/ctocpp/v8_value_ctocpp.h"
#ifdef MMLTK_DEFINED_WRAPPING_CEF_SHARED
#undef WRAPPING_CEF_SHARED
#undef MMLTK_DEFINED_WRAPPING_CEF_SHARED
#endif

#include "mmltk_logging.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mmltk::gui {

namespace {

using namespace browser_runtime_shared;

constexpr auto kWorkspaceGpuBridgePropertyName = "__MMLTK_WORKSPACE_GPU_BRIDGE__";
constexpr std::string_view kWorkspaceGpuBridgeBlockedPrefix = "workspace GPU bridge blocked [";
constexpr std::string_view kSharedImageExportRejectedReason = "shared_image_export_rejected";
constexpr std::string_view kRendererImportRejectedReason = "renderer_import_rejected";

thread_local std::string g_last_workspace_gpu_bridge_export_result_code;
thread_local std::string g_last_workspace_gpu_bridge_export_result_detail;
thread_local std::uint64_t g_workspace_gpu_bridge_import_request_log_count = 0;

extern "C" bool mmltk_cef_workspace_gpu_bridge_renderer_helper_present() noexcept __attribute__((weak));

extern "C" bool mmltk_cef_workspace_gpu_bridge_export_shared_image(
    const char* surface_id, std::size_t surface_id_length, const char* revision, std::size_t revision_length,
    const char* publication_source_json, std::size_t publication_source_json_length,
    CefString* exported_shared_image_json_out, CefString* result_code, CefString* result_detail) noexcept
    __attribute__((weak));

extern "C" bool mmltk_cef_workspace_gpu_bridge_import_texture(
    void* device, const char* surface_id, std::size_t surface_id_length, const char* revision,
    std::size_t revision_length, const char* exported_shared_image_json, std::size_t exported_shared_image_json_length,
    void** texture_out, CefString* result_code, CefString* result_detail) noexcept __attribute__((weak));

extern "C" bool mmltk_cef_workspace_gpu_bridge_release_surface(const char* surface_id, std::size_t surface_id_length,
                                                               const char* revision, std::size_t revision_length,
                                                               bool renderer_dissociated, CefString* result_code,
                                                               CefString* result_detail) noexcept __attribute__((weak));

extern "C" bool mmltk_cef_workspace_gpu_bridge_release_texture(const char* surface_id, std::size_t surface_id_length,
                                                               const char* revision, std::size_t revision_length,
                                                               CefString* result_code,
                                                               CefString* result_detail) noexcept __attribute__((weak));

extern "C" bool mmltk_cef_workspace_gpu_bridge_configure_swapchain(
    const char* surface_id, std::size_t surface_id_length, const char* swapchain_json,
    std::size_t swapchain_json_length, CefString* result_code, CefString* result_detail) noexcept
    __attribute__((weak));

extern "C" bool mmltk_cef_workspace_gpu_bridge_present_front_slot(
    const char* surface_id, std::size_t surface_id_length, std::uint32_t slot_index, std::uint64_t revision,
    std::uint64_t swapchain_generation, std::int32_t bounds_x, std::int32_t bounds_y, std::int32_t bounds_width,
    std::int32_t bounds_height, std::int32_t damage_x, std::int32_t damage_y, std::int32_t damage_width,
    std::int32_t damage_height, std::uint32_t ready_sync_kind, std::uint64_t ready_sync_handle,
    std::uint64_t ready_sync_value, CefString* result_code, CefString* result_detail) noexcept __attribute__((weak));

extern "C" bool mmltk_cef_workspace_gpu_bridge_destroy_swapchain(const char* surface_id,
                                                                 std::size_t surface_id_length,
                                                                 CefString* result_code,
                                                                 CefString* result_detail) noexcept
    __attribute__((weak));

[[nodiscard]] bool exported_shared_image_helper_present() noexcept {
    if (mmltk_cef_workspace_gpu_bridge_import_texture == nullptr) {
        return false;
    }
    return mmltk_cef_workspace_gpu_bridge_renderer_helper_present == nullptr ||
           mmltk_cef_workspace_gpu_bridge_renderer_helper_present();
}

[[nodiscard]] WorkspaceGpuBridgeStructuralCapability structural_capability_rejected(std::string detail) {
    return WorkspaceGpuBridgeStructuralCapability{false, std::move(detail)};
}

[[nodiscard]] bool v8_object_has_function(const CefRefPtr<CefV8Value>& object, const char* name) {
    if (object == nullptr || !object->IsObject() || !object->HasValue(name)) {
        return false;
    }
    CefRefPtr<CefV8Value> value = object->GetValue(name);
    return value != nullptr && value->IsFunction();
}

[[nodiscard]] std::string exported_shared_image_payload_summary(const std::string_view serialized_json) {
    const nlohmann::json payload = nlohmann::json::parse(serialized_json, nullptr, false);
    if (!payload.is_object()) {
        return "payload=parse_failed";
    }
    const nlohmann::json& metadata = payload.value("metadata", nlohmann::json::object());
    const nlohmann::json& size = metadata.value("size", nlohmann::json::object());
    const nlohmann::json& dma_buf = payload.value("dma_buf", nlohmann::json::object());
    const nlohmann::json& sync_token = payload.value("creation_sync_token", nlohmann::json::object());
    std::ostringstream summary;
    summary << std::boolalpha << "payload_bytes=" << serialized_json.size()
            << " metadata_format=" << metadata.value("format", "unknown")
            << " metadata_usage=" << metadata.value("usage", -1)
            << " metadata_size=" << size.value("width", -1) << 'x' << size.value("height", -1)
            << " texture_target=" << payload.value("texture_target", -1)
            << " software=" << payload.value("is_software", false)
            << " dmabuf_fd=" << dma_buf.value("fd", "unknown")
            << " dmabuf_format=" << dma_buf.value("drm_format", -1)
            << " dmabuf_modifier=" << dma_buf.value("drm_modifier", "unknown")
            << " dmabuf_stride=" << dma_buf.value("stride_bytes", "unknown")
            << " sync_ns=" << sync_token.value("namespace_id", -1)
            << " sync_release=" << sync_token.value("release_count", "unknown")
            << " sync_verified=" << sync_token.value("verified_flush", false);
    return summary.str();
}

[[nodiscard]] WorkspaceGpuBridgeStructuralCapability workspace_gpu_bridge_structural_capability(
    const CefRefPtr<CefV8Value>& bridge) {
    if (bridge == nullptr || !bridge->IsObject()) {
        return structural_capability_rejected("__MMLTK_WORKSPACE_GPU_BRIDGE__ is not installed");
    }
    if (!v8_object_has_function(bridge, "acquireCurrentSurface")) {
        return structural_capability_rejected("__MMLTK_WORKSPACE_GPU_BRIDGE__.acquireCurrentSurface is absent");
    }
    if (!v8_object_has_function(bridge, "importTexture")) {
        return structural_capability_rejected("__MMLTK_WORKSPACE_GPU_BRIDGE__.importTexture is absent");
    }
    if (!v8_object_has_function(bridge, "releaseSurface")) {
        return structural_capability_rejected("__MMLTK_WORKSPACE_GPU_BRIDGE__.releaseSurface is absent");
    }
    if (!v8_object_has_function(bridge, "releaseRendererTexture")) {
        return structural_capability_rejected("__MMLTK_WORKSPACE_GPU_BRIDGE__.releaseRendererTexture is absent");
    }
    if (mmltk_cef_workspace_gpu_bridge_export_shared_image == nullptr) {
        return structural_capability_rejected("CEF workspace SharedImage export helper is absent");
    }
    if (mmltk_cef_workspace_gpu_bridge_import_texture == nullptr) {
        return structural_capability_rejected("CEF workspace texture import helper is absent");
    }
    if (mmltk_cef_workspace_gpu_bridge_release_texture == nullptr) {
        return structural_capability_rejected("CEF workspace texture release helper is absent");
    }
    if (mmltk_cef_workspace_gpu_bridge_release_surface == nullptr) {
        return structural_capability_rejected("CEF workspace browser surface release helper is absent");
    }
    if (mmltk_cef_workspace_gpu_bridge_renderer_helper_present != nullptr &&
        !mmltk_cef_workspace_gpu_bridge_renderer_helper_present()) {
        return structural_capability_rejected("CEF workspace renderer GPU bridge helper is absent");
    }
    return WorkspaceGpuBridgeStructuralCapability{true, {}};
}

[[nodiscard]] std::string workspace_gpu_bridge_blocked_message(const std::string_view reason,
                                                               const std::string_view detail) {
    std::string message;
    message.reserve(kWorkspaceGpuBridgeBlockedPrefix.size() + reason.size() + detail.size() + 3U);
    message.append(kWorkspaceGpuBridgeBlockedPrefix);
    message.append(reason);
    message.push_back(']');
    if (!detail.empty()) {
        message.append(": ");
        message.append(detail);
    }
    return message;
}

[[nodiscard]] WorkspaceGpuBridgeCefResult cef_result_from_strings(const bool ok, const CefString& result_code,
                                                                  const CefString& result_detail,
                                                                  const std::string_view fallback_code,
                                                                  const std::string_view fallback_detail) {
    WorkspaceGpuBridgeCefResult result;
    result.ok = ok;
    result.result_code = result_code.ToString().empty() ? std::string(fallback_code) : result_code.ToString();
    result.result_detail =
        result_detail.ToString().empty() ? std::string(fallback_detail) : result_detail.ToString();
    return result;
}

[[nodiscard]] LinuxImportedFrameSourceContract workspace_swapchain_slot_linux_import(
    const mmltk::live::WorkspaceSwapchainSlotDescriptor& slot, const std::optional<int> cuda_device_index) {
    const mmltk::live::WorkspaceDmaBufImage& dmabuf = slot.dmabuf_image;
    return LinuxImportedFrameSourceContract{
        make_workspace_dmabuf_image_source(dmabuf, dmabuf.fd >= 0 ? static_cast<std::uintptr_t>(dmabuf.fd) : 0U,
                                           cuda_device_index.value_or(-1)),
        LinuxImportedSyncSource{
            slot.ready_event_handle == 0U ? LinuxImportedSyncHandleKind::None : LinuxImportedSyncHandleKind::CudaEvent,
            slot.ready_event_handle,
            slot.revision,
        },
        LinuxImportedSyncSource{
            slot.producer_stream_handle == 0U ? LinuxImportedSyncHandleKind::None
                                              : LinuxImportedSyncHandleKind::CudaStream,
            slot.producer_stream_handle,
            0U,
        },
    };
}

[[nodiscard]] nlohmann::json linux_imported_frame_source_contract_to_json(
    const LinuxImportedFrameSourceContract& contract);

[[nodiscard]] nlohmann::json linux_imported_frame_lifecycle_contract_to_json(
    const LinuxImportedFrameLifecycleContract& contract);

[[nodiscard]] nlohmann::json workspace_swapchain_slot_to_json(
    const mmltk::live::WorkspaceSwapchainSlotDescriptor& slot, const std::optional<int> cuda_device_index) {
    const LinuxImportedFrameLifecycleContract lifecycle{
        LinuxImportedSyncSource{LinuxImportedSyncHandleKind::None, 0U, 0U},
    };
    return {
        {"slot_index", slot.slot_index},
        {"revision", std::to_string(slot.revision)},
        {"linux_import", linux_imported_frame_source_contract_to_json(
                             workspace_swapchain_slot_linux_import(slot, cuda_device_index))},
        {"linux_lifecycle", linux_imported_frame_lifecycle_contract_to_json(lifecycle)},
    };
}

[[nodiscard]] std::string workspace_swapchain_descriptor_json(
    const mmltk::live::WorkspaceSwapchainDescriptor& descriptor, const std::optional<int> cuda_device_index) {
    nlohmann::json slots = nlohmann::json::array();
    slots.get_ref<nlohmann::json::array_t&>().reserve(descriptor.slots.size());
    for (const mmltk::live::WorkspaceSwapchainSlotDescriptor& slot : descriptor.slots) {
        slots.push_back(workspace_swapchain_slot_to_json(slot, cuda_device_index));
    }
    return nlohmann::json{
        {"width", descriptor.width},
        {"height", descriptor.height},
        {"generation", std::to_string(descriptor.generation)},
        {"slots", std::move(slots)},
    }
        .dump();
}

[[nodiscard]] CefString shared_image_export_rejected_exception(
    const WorkspaceGpuBridgeSurfacePublication& publication) {
    const std::string detail =
        publication.export_error.empty() ? "SharedImage export rejected the current surface" : publication.export_error;
    const std::string reason = publication.export_result_code.empty() || publication.export_result_code == "ok"
                                   ? std::string(kSharedImageExportRejectedReason)
                                   : publication.export_result_code;
    return workspace_gpu_bridge_blocked_message(reason, detail);
}

[[nodiscard]] CefString renderer_import_rejected_exception(const std::string_view detail) {
    return workspace_gpu_bridge_blocked_message(kRendererImportRejectedReason, detail);
}

[[nodiscard]] std::string workspace_gpu_bridge_result_message(const std::string_view code,
                                                              const std::string_view detail) {
    if (code.empty()) {
        return std::string(detail);
    }
    if (detail.empty()) {
        return std::string(code);
    }
    std::string message(code);
    message.append(": ");
    message.append(detail);
    return message;
}

[[nodiscard]] nlohmann::json workspace_gpu_bridge_exported_shared_image_to_json(
    const WorkspaceGpuBridgeExportedSharedImage& exported_shared_image) {
    if (exported_shared_image.serialized_json.empty()) {
        return nullptr;
    }
    return nlohmann::json::parse(exported_shared_image.serialized_json);
}

void workspace_gpu_bridge_exported_shared_image_from_json(
    const nlohmann::json& json, WorkspaceGpuBridgeExportedSharedImage& exported_shared_image) {
    exported_shared_image = WorkspaceGpuBridgeExportedSharedImage{};
    if (!json.is_object()) {
        return;
    }

    exported_shared_image.serialized_json = json.dump();
    if (const auto it = json.find("mailbox"); it != json.end() && !it->is_null()) {
        exported_shared_image.mailbox_json = it->dump();
    }
    if (const auto it = json.find("metadata"); it != json.end() && it->is_object()) {
        exported_shared_image.has_metadata = true;
    }
    if (const auto it = json.find("creation_sync_token"); it != json.end() && it->is_object()) {
        exported_shared_image.has_creation_sync_token = true;
    }
    if (const auto texture_target_it = json.find("texture_target"); texture_target_it != json.end()) {
        if (texture_target_it->is_number_unsigned()) {
            exported_shared_image.texture_target = texture_target_it->get<std::uint32_t>();
        } else if (texture_target_it->is_number_integer() && texture_target_it->get<std::int64_t>() >= 0) {
            exported_shared_image.texture_target = static_cast<std::uint32_t>(texture_target_it->get<std::int64_t>());
        }
    }
    if (const auto it = json.find("is_software"); it != json.end() && it->is_boolean()) {
        exported_shared_image.is_software = it->get<bool>();
    }
}

[[nodiscard]] nlohmann::json linux_imported_sync_source_to_json(const LinuxImportedSyncSource& source) {
    return {
        {"kind", static_cast<std::uint32_t>(source.kind)},
        {"handle", std::to_string(source.handle)},
        {"value", source.value},
    };
}

[[nodiscard]] nlohmann::json linux_imported_image_source_to_json(const LinuxImportedImageSource& source) {
    return {
        {"kind", static_cast<std::uint32_t>(source.kind)},
        {"fd", std::to_string(source.fd)},
        {"cuda_device_index", source.cuda_device_index},
        {"width", source.width},
        {"height", source.height},
        {"stride_bytes", std::to_string(source.stride_bytes)},
        {"offset", std::to_string(source.offset)},
        {"allocation_size", std::to_string(source.allocation_size)},
        {"drm_format", source.drm_format},
        {"drm_modifier", std::to_string(source.drm_modifier)},
        {"modifier_mode", static_cast<std::uint32_t>(source.modifier_mode)},
    };
}

[[nodiscard]] nlohmann::json linux_imported_frame_source_contract_to_json(
    const LinuxImportedFrameSourceContract& contract) {
    return {
        {"image", linux_imported_image_source_to_json(contract.image)},
        {"ready_sync", linux_imported_sync_source_to_json(contract.ready_sync)},
        {"producer_stream", linux_imported_sync_source_to_json(contract.producer_stream)},
    };
}

[[nodiscard]] nlohmann::json linux_imported_frame_lifecycle_contract_to_json(
    const LinuxImportedFrameLifecycleContract& contract) {
    return {
        {"release_sync", linux_imported_sync_source_to_json(contract.release_sync)},
    };
}

[[nodiscard]] std::optional<WorkspaceGpuBridgeExportedSharedImage> export_shared_image_publication(
    const mmltk::browser::WorkspaceSurfaceInfo& surface_info, const RetainedBrowserImportedFrameSource& source,
    std::string* rejection_reason) {
    if (rejection_reason != nullptr) {
        rejection_reason->clear();
    }
    g_last_workspace_gpu_bridge_export_result_code.clear();
    g_last_workspace_gpu_bridge_export_result_detail.clear();
    const auto reject = [&surface_info, rejection_reason](std::string reason) -> std::nullopt_t {
        if (g_last_workspace_gpu_bridge_export_result_code.empty()) {
            g_last_workspace_gpu_bridge_export_result_code = "shared_image_export_rejected";
        }
        if (g_last_workspace_gpu_bridge_export_result_detail.empty()) {
            g_last_workspace_gpu_bridge_export_result_detail = reason;
        }
        if (rejection_reason != nullptr) {
            *rejection_reason = reason;
        }
        mmltk::logging::logger("gui")->warn("workspace GPU bridge export rejected surface {} revision {}: {}",
                                            surface_info.surface_id, surface_info.revision, reason);
        return std::nullopt;
    };

    if (mmltk_cef_workspace_gpu_bridge_export_shared_image == nullptr) {
        g_last_workspace_gpu_bridge_export_result_code = "shared_image_export_rejected";
        g_last_workspace_gpu_bridge_export_result_detail = "CEF export helper unavailable";
        return reject("CEF export helper unavailable");
    }

    const std::optional<LinuxImportedFrameSourceContract> linux_import = linux_imported_frame_source_contract(source);
    const std::optional<LinuxImportedFrameLifecycleContract> linux_lifecycle =
        linux_imported_frame_lifecycle_contract(source);
    if (!linux_import.has_value()) {
        return reject("missing Linux import metadata");
    }
    if (linux_import->image.kind != LinuxImportedImageSourceKind::DmaBufRgba) {
        return reject("unsupported Linux import image kind " +
                      std::to_string(static_cast<std::uint32_t>(linux_import->image.kind)));
    }

    nlohmann::json publication_source_json = {
        {"descriptor", source.descriptor},
        {"linux_import", linux_imported_frame_source_contract_to_json(*linux_import)},
        {"linux_lifecycle", linux_lifecycle.has_value()
                                ? nlohmann::json(linux_imported_frame_lifecycle_contract_to_json(*linux_lifecycle))
                                : nlohmann::json(nullptr)},
    };
    const std::string publication_source_payload = publication_source_json.dump();

    CefString exported_shared_image_json;
    CefString result_code;
    CefString result_detail;
    const bool exported = mmltk_cef_workspace_gpu_bridge_export_shared_image(
        surface_info.surface_id.data(), surface_info.surface_id.size(), surface_info.revision.data(),
        surface_info.revision.size(), publication_source_payload.data(), publication_source_payload.size(),
        &exported_shared_image_json, &result_code, &result_detail);
    if (!exported || exported_shared_image_json.ToString().empty()) {
        const std::string detail =
            workspace_gpu_bridge_result_message(result_code.ToString(), result_detail.ToString());
        const std::string reason =
            detail.empty() ? "CEF export helper returned no exported SharedImage payload" : detail;
        g_last_workspace_gpu_bridge_export_result_code =
            result_code.ToString().empty() ? "shared_image_export_rejected" : result_code.ToString();
        g_last_workspace_gpu_bridge_export_result_detail = reason;
        return reject(reason);
    }

    WorkspaceGpuBridgeExportedSharedImage exported_shared_image;
    try {
        workspace_gpu_bridge_exported_shared_image_from_json(
            nlohmann::json::parse(exported_shared_image_json.ToString()), exported_shared_image);
    } catch (const std::exception& error) {
        g_last_workspace_gpu_bridge_export_result_code = "invalid_shared_image_metadata";
        g_last_workspace_gpu_bridge_export_result_detail = error.what();
        return reject(std::string("invalid exported SharedImage JSON: ") + error.what());
    }
    if (!exported_shared_image.is_valid()) {
        g_last_workspace_gpu_bridge_export_result_code = "invalid_shared_image_metadata";
        g_last_workspace_gpu_bridge_export_result_detail = "invalid exported SharedImage metadata";
        return reject("invalid exported SharedImage metadata");
    }
    exported_shared_image.result_code = result_code.ToString().empty() ? "ok" : result_code.ToString();
    exported_shared_image.result_detail = result_detail.ToString();
    g_last_workspace_gpu_bridge_export_result_code = exported_shared_image.result_code;
    g_last_workspace_gpu_bridge_export_result_detail = exported_shared_image.result_detail;
    if (exported_shared_image.is_software) {
        g_last_workspace_gpu_bridge_export_result_code = "software_shared_image_rejected";
        g_last_workspace_gpu_bridge_export_result_detail = "exported SharedImage reported software backing";
        return reject("software_shared_image_rejected: exported SharedImage reported software backing");
    }
    mmltk::logging::logger("gui")->info(
        "workspace GPU bridge exported SharedImage surface {} revision {} texture_target={} software={}",
        surface_info.surface_id, surface_info.revision, exported_shared_image.texture_target,
        exported_shared_image.is_software);
    return exported_shared_image;
}

[[nodiscard]] nlohmann::json workspace_gpu_bridge_publication_to_json(
    const WorkspaceGpuBridgeSurfacePublication& publication) {
    return {
        {"workspace_surface", publication.surface_info},
        {"export_result_code", publication.export_result_code},
        {"export_result_detail", publication.export_result_detail},
        {"export_error", publication.export_error},
        {"exported_shared_image",
         publication.exported_shared_image.has_value()
             ? nlohmann::json(workspace_gpu_bridge_exported_shared_image_to_json(*publication.exported_shared_image))
             : nlohmann::json(nullptr)},
    };
}

void workspace_gpu_bridge_publication_from_json(const nlohmann::json& json,
                                                WorkspaceGpuBridgeSurfacePublication& publication) {
    publication = WorkspaceGpuBridgeSurfacePublication{};
    json.at("workspace_surface").get_to(publication.surface_info);
    if (const auto it = json.find("export_result_code"); it != json.end() && it->is_string()) {
        publication.export_result_code = it->get<std::string>();
    }
    if (const auto it = json.find("export_result_detail"); it != json.end() && it->is_string()) {
        publication.export_result_detail = it->get<std::string>();
    }
    if (const auto it = json.find("export_error"); it != json.end() && it->is_string()) {
        publication.export_error = it->get<std::string>();
    }
    if (const auto it = json.find("exported_shared_image"); it != json.end() && !it->is_null()) {
        WorkspaceGpuBridgeExportedSharedImage exported_shared_image;
        workspace_gpu_bridge_exported_shared_image_from_json(*it, exported_shared_image);
        publication.exported_shared_image = std::move(exported_shared_image);
    } else {
        publication.exported_shared_image.reset();
    }
}

[[nodiscard]] bool publication_is_valid(const WorkspaceGpuBridgeSurfacePublication& publication) {
    return publication.surface_info.surface_id == kBrowserWorkspaceSurfaceId &&
           !publication.surface_info.revision.empty() && publication.surface_info.width > 0U &&
           publication.surface_info.height > 0U &&
           (!publication.exported_shared_image.has_value() || publication.exported_shared_image->is_valid());
}

[[nodiscard]] std::optional<WorkspaceGpuBridgeSurfacePublication> decode_publication_payload(
    const std::string& payload) {
    if (payload.empty()) {
        return std::nullopt;
    }

    WorkspaceGpuBridgeSurfacePublication publication;
    workspace_gpu_bridge_publication_from_json(nlohmann::json::parse(payload), publication);
    if (!publication_is_valid(publication)) {
        throw std::runtime_error(
            "workspace GPU bridge publication carried an invalid exported "
            "SharedImage payload");
    }
    return publication;
}

void send_renderer_bridge_json_message(const CefRefPtr<CefBrowser>& browser, const nlohmann::json& message_json) {
    if (browser == nullptr) {
        return;
    }
    CefRefPtr<CefFrame> main_frame = browser->GetMainFrame();
    if (main_frame == nullptr) {
        return;
    }

    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kRendererBridgeMessageName.data());
    message->GetArgumentList()->SetString(0U, message_json.dump());
    main_frame->SendProcessMessage(PID_BROWSER, message);
}

void send_runtime_capabilities_update(const CefRefPtr<CefBrowser>& browser,
                                      const CefWorkspaceGpuBridge::SurfaceState& state) {
    const WorkspaceGpuBridgeStructuralCapability& structural_capability = state.structural_capability;
    const WorkspaceGpuBridgeZeroCopyCapability& zero_copy = state.zero_copy;
    const std::string zero_copy_status = !zero_copy.attempted ? "unknown"
                                        : zero_copy.available ? "available"
                                                              : "unavailable";
    nlohmann::json message = {
        {"type", kRuntimeCapabilitiesMessageType},
        {"workspace_surface_bridge", structural_capability.available ? "available" : "unavailable"},
        {"workspace_surface_zero_copy", zero_copy_status},
        {"capabilities",
         {{"workspace_surface_bridge",
           {{"status", structural_capability.available ? "ready" : "blocked"},
            {"summary", structural_capability.available ? "workspace surface bridge ready"
                                                        : "workspace surface bridge unavailable"},
            {"detail",
             structural_capability.available
                 ? "The CEF workspace GPU bridge functions and native helpers are installed."
                 : structural_capability.detail}}},
          {"workspace_surface_zero_copy",
           {{"status", zero_copy_status == "available" ? "ready"
                       : zero_copy_status == "unavailable" ? "blocked"
                                                            : "pending"},
            {"summary", zero_copy_status == "available" ? "workspace zero-copy available"
                        : zero_copy_status == "unavailable" ? "workspace zero-copy unavailable"
                                                             : "workspace zero-copy pending"},
            {"detail", zero_copy_status == "available"
                           ? "The current workspace surface has a live exported SharedImage payload and renderer import support."
                       : zero_copy_status == "unavailable" ? zero_copy.result_detail
                                                            : "No live workspace surface publication has been exported yet."}}}}},
    };
    if (!structural_capability.available && !structural_capability.detail.empty()) {
        message["workspace_surface_bridge_detail"] = structural_capability.detail;
    }
    if (zero_copy.attempted) {
        message["workspace_surface_zero_copy_result_code"] = zero_copy.result_code;
        message["workspace_surface_zero_copy_result_detail"] = zero_copy.result_detail;
    }
    send_renderer_bridge_json_message(browser, message);
}

[[nodiscard]] WorkspaceGpuBridgeZeroCopyCapability zero_copy_capability_from_publication(
    const WorkspaceGpuBridgeStructuralCapability& structural_capability,
    const std::optional<WorkspaceGpuBridgeSurfacePublication>& publication) {
    WorkspaceGpuBridgeZeroCopyCapability zero_copy;
    if (!publication.has_value()) {
        return zero_copy;
    }
    zero_copy.attempted = true;
    if (!structural_capability.available) {
        zero_copy.available = false;
        zero_copy.result_code = "workspace_surface_bridge_unavailable";
        zero_copy.result_detail = structural_capability.detail;
        return zero_copy;
    }
    if (publication->has_live_exported_shared_image() && cef_workspace_gpu_bridge_zero_copy_import_supported()) {
        zero_copy.available = true;
        zero_copy.result_code = "ok";
        zero_copy.result_detail = "workspace zero-copy publication ready";
        return zero_copy;
    }
    zero_copy.available = false;
    zero_copy.result_code =
        publication->export_result_code.empty() ? "workspace_zero_copy_unavailable" : publication->export_result_code;
    if (!publication->export_result_detail.empty()) {
        zero_copy.result_detail = publication->export_result_detail;
    } else if (!publication->export_error.empty()) {
        zero_copy.result_detail = publication->export_error;
    } else {
        zero_copy.result_detail = "workspace zero-copy publication was rejected";
    }
    return zero_copy;
}

[[nodiscard]] bool import_texture_via_renderer_helper(CefRefPtr<CefV8Value> device,
                                                      const WorkspaceGpuBridgeSurfacePublication& publication,
                                                      CefRefPtr<CefV8Value>& texture, CefString& exception,
                                                      std::string& result_code_out,
                                                      std::string& result_detail_out) {
    result_code_out = kRendererImportRejectedReason;
    result_detail_out.clear();
    const auto& exported_shared_image = publication.exported_shared_image;
    if (!exported_shared_image.has_value() || !exported_shared_image->has_live_image()) {
        exception =
            "workspace GPU bridge current publication has no live exported "
            "SharedImage payload";
        result_detail_out = exception.ToString();
        mmltk::logging::logger("gui")->warn("workspace GPU bridge import rejected surface {} revision {}: {}",
                                            publication.surface_info.surface_id, publication.surface_info.revision,
                                            exception.ToString());
        texture = CefV8Value::CreateNull();
        return false;
    }
    if (mmltk_cef_workspace_gpu_bridge_import_texture == nullptr) {
        exception =
            "workspace GPU bridge importTexture requires the renderer GPU bridge "
            "patch set";
        result_detail_out = exception.ToString();
        mmltk::logging::logger("gui")->warn("workspace GPU bridge import rejected surface {} revision {}: {}",
                                            publication.surface_info.surface_id, publication.surface_info.revision,
                                            exception.ToString());
        texture = CefV8Value::CreateNull();
        return false;
    }

    void* imported_texture_handle = nullptr;
    CefString result_code;
    CefString result_detail;
    void* device_handle = static_cast<void*>(CefV8ValueCToCpp_Unwrap(device));
    if (device_handle == nullptr) {
        exception = "workspace GPU bridge could not unwrap the GPUDevice handle";
        result_detail_out = exception.ToString();
        mmltk::logging::logger("gui")->warn("workspace GPU bridge import rejected surface {} revision {}: {}",
                                            publication.surface_info.surface_id, publication.surface_info.revision,
                                            exception.ToString());
        texture = CefV8Value::CreateNull();
        return false;
    }
    g_workspace_gpu_bridge_import_request_log_count += 1U;
    if (g_workspace_gpu_bridge_import_request_log_count <= 3U ||
        g_workspace_gpu_bridge_import_request_log_count % 300U == 0U) {
        mmltk::logging::logger("gui")->info("workspace GPU bridge importTexture request surface {} revision {} {}",
                                            publication.surface_info.surface_id, publication.surface_info.revision,
                                            exported_shared_image_payload_summary(
                                                exported_shared_image->serialized_json));
    }
    const bool imported = mmltk_cef_workspace_gpu_bridge_import_texture(
        device_handle, publication.surface_info.surface_id.data(), publication.surface_info.surface_id.size(),
        publication.surface_info.revision.data(), publication.surface_info.revision.size(),
        exported_shared_image->serialized_json.data(), exported_shared_image->serialized_json.size(),
        &imported_texture_handle, &result_code, &result_detail);
    CefRefPtr<CefV8Value> imported_texture =
        imported_texture_handle != nullptr ? CefV8ValueCToCpp_Wrap(static_cast<cef_v8_value_t*>(imported_texture_handle))
                                           : nullptr;
    if (!imported || imported_texture == nullptr || imported_texture->IsNull() || imported_texture->IsUndefined()) {
        const std::string code =
            result_code.ToString().empty() ? std::string(kRendererImportRejectedReason) : result_code.ToString();
        const std::string detail = result_detail.ToString().empty()
                                       ? "workspace GPU bridge renderer helper did not return a GPUTexture"
                                       : result_detail.ToString();
        result_code_out = code;
        result_detail_out = detail;
        exception = workspace_gpu_bridge_blocked_message(code, detail);
        mmltk::logging::logger("gui")->warn("workspace GPU bridge import rejected surface {} revision {}: {}",
                                            publication.surface_info.surface_id, publication.surface_info.revision,
                                            exception.ToString());
        texture = CefV8Value::CreateNull();
        return false;
    }

    mmltk::logging::logger("gui")->info("workspace GPU bridge importTexture succeeded surface {} revision {}",
                                        publication.surface_info.surface_id, publication.surface_info.revision);
    result_code_out = "ok";
    result_detail_out.clear();
    texture = imported_texture;
    return true;
}

[[nodiscard]] CefRefPtr<CefV8Value> create_workspace_surface_value(
    const mmltk::browser::WorkspaceSurfaceInfo& surface) {
    CefRefPtr<CefV8Value> value = CefV8Value::CreateObject(nullptr, nullptr);
    value->SetValue("surfaceId", CefV8Value::CreateString(surface.surface_id), V8_PROPERTY_ATTRIBUTE_READONLY);
    value->SetValue("revision", CefV8Value::CreateString(surface.revision), V8_PROPERTY_ATTRIBUTE_READONLY);
    value->SetValue("width", CefV8Value::CreateUInt(surface.width), V8_PROPERTY_ATTRIBUTE_READONLY);
    value->SetValue("height", CefV8Value::CreateUInt(surface.height), V8_PROPERTY_ATTRIBUTE_READONLY);
    value->SetValue("textureFormat", CefV8Value::CreateString(surface.texture_format), V8_PROPERTY_ATTRIBUTE_READONLY);
    value->SetValue("opaque", CefV8Value::CreateBool(surface.opaque), V8_PROPERTY_ATTRIBUTE_READONLY);
    value->SetValue("upright", CefV8Value::CreateBool(surface.upright), V8_PROPERTY_ATTRIBUTE_READONLY);
    return value;
}

[[nodiscard]] CefRefPtr<CefV8Value> create_workspace_release_result_value(const bool released,
                                                                          const std::string& result_code,
                                                                          const std::string& result_detail) {
    CefRefPtr<CefV8Value> value = CefV8Value::CreateObject(nullptr, nullptr);
    value->SetValue("released", CefV8Value::CreateBool(released), V8_PROPERTY_ATTRIBUTE_READONLY);
    value->SetValue("resultCode", CefV8Value::CreateString(result_code), V8_PROPERTY_ATTRIBUTE_READONLY);
    value->SetValue("resultDetail", CefV8Value::CreateString(result_detail), V8_PROPERTY_ATTRIBUTE_READONLY);
    return value;
}

struct RendererTextureReleaseResult {
    bool released = false;
    std::string result_code = "release_failure";
    std::string result_detail = "renderer GPU bridge release helper unavailable";
};

[[nodiscard]] RendererTextureReleaseResult release_renderer_workspace_texture(const std::string_view surface_id,
                                                                              const std::string_view revision) {
    RendererTextureReleaseResult release;
    if (mmltk_cef_workspace_gpu_bridge_release_texture == nullptr) {
        return release;
    }

    CefString result_code;
    CefString result_detail;
    if (!mmltk_cef_workspace_gpu_bridge_release_texture(surface_id.data(), surface_id.size(), revision.data(),
                                                        revision.size(), &result_code, &result_detail)) {
        release.result_code = result_code.ToString().empty() ? "release_failure" : result_code.ToString();
        release.result_detail = result_detail.ToString().empty() ? "renderer workspace texture release rejected"
                                                                 : result_detail.ToString();
        mmltk::logging::logger("gui")->warn(
            "workspace GPU bridge renderer texture release rejected surface {} revision {}: {}", surface_id, revision,
            workspace_gpu_bridge_result_message(release.result_code, release.result_detail));
        return release;
    }

    release.released = true;
    release.result_code = result_code.ToString().empty() ? "ok" : result_code.ToString();
    release.result_detail = result_detail.ToString();
    mmltk::logging::logger("gui")->info("workspace GPU bridge renderer texture released surface {} revision {}",
                                        surface_id, revision);
    return release;
}

class WorkspaceGpuBridgeFunctionHandler : public CefV8Handler {
   public:
    enum class Operation : std::uint8_t {
        AcquireCurrentSurface = 0,
        ImportTexture = 1,
        ReleaseSurface = 2,
        ReleaseRendererTexture = 3,
    };

    WorkspaceGpuBridgeFunctionHandler(CefWorkspaceGpuBridge* bridge, const Operation operation)
        : bridge_(bridge), operation_(operation) {}

    bool Execute(const CefString& name, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& retval, CefString& exception) override;

   private:
    [[nodiscard]] int current_browser_identifier() const {
        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        if (context == nullptr || !context->IsValid()) {
            return -1;
        }
        CefRefPtr<CefBrowser> browser = context->GetBrowser();
        return browser != nullptr ? browser->GetIdentifier() : -1;
    }

    [[nodiscard]] CefRefPtr<CefBrowser> current_browser() const {
        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        if (context == nullptr || !context->IsValid()) {
            return nullptr;
        }
        return context->GetBrowser();
    }

    CefWorkspaceGpuBridge* bridge_ = nullptr;
    Operation operation_ = Operation::AcquireCurrentSurface;

    IMPLEMENT_REFCOUNTING(WorkspaceGpuBridgeFunctionHandler);
};

}  // namespace

std::optional<WorkspaceGpuBridgeSurfacePublication> make_workspace_gpu_bridge_surface_publication(
    const mmltk::browser::WorkspaceSurfaceInfo& surface_info, const RetainedBrowserImportedFrameSource& source) {
    if (surface_info.surface_id != kBrowserWorkspaceSurfaceId || surface_info.revision.empty() ||
        !is_browser_workspace_surface_descriptor(source.descriptor) || source.descriptor.width != surface_info.width ||
        source.descriptor.height != surface_info.height) {
        return std::nullopt;
    }

    WorkspaceGpuBridgeSurfacePublication publication;
    publication.surface_info = surface_info;
    publication.exported_shared_image =
        export_shared_image_publication(surface_info, source, &publication.export_error);
    publication.export_result_code = g_last_workspace_gpu_bridge_export_result_code.empty()
                                         ? std::string{"shared_image_export_rejected"}
                                         : g_last_workspace_gpu_bridge_export_result_code;
    publication.export_result_detail = g_last_workspace_gpu_bridge_export_result_detail.empty()
                                           ? publication.export_error
                                           : g_last_workspace_gpu_bridge_export_result_detail;
    return publication_is_valid(publication)
               ? std::optional<WorkspaceGpuBridgeSurfacePublication>(std::move(publication))
               : std::nullopt;
}

std::string WorkspaceGpuBridgeCefResult::message(const std::string_view fallback) const {
    return workspace_gpu_bridge_result_message(result_code.empty() ? fallback : result_code, result_detail);
}

std::string workspace_gpu_bridge_last_export_result_code() noexcept {
    return g_last_workspace_gpu_bridge_export_result_code;
}

std::string workspace_gpu_bridge_last_export_result_detail() noexcept {
    return g_last_workspace_gpu_bridge_export_result_detail;
}

bool cef_workspace_gpu_bridge_zero_copy_import_supported() noexcept {
    return exported_shared_image_helper_present();
}

bool cef_workspace_gpu_bridge_swapchain_present_supported() noexcept {
    return mmltk_cef_workspace_gpu_bridge_configure_swapchain != nullptr &&
           mmltk_cef_workspace_gpu_bridge_present_front_slot != nullptr &&
           mmltk_cef_workspace_gpu_bridge_destroy_swapchain != nullptr;
}

std::string workspace_gpu_bridge_shared_image_export_rejected_message(const std::string_view detail) {
    return workspace_gpu_bridge_blocked_message(kSharedImageExportRejectedReason, detail);
}

WorkspaceGpuBridgeCefResult configure_cef_workspace_gpu_bridge_swapchain(
    const std::string_view surface_id, const mmltk::live::WorkspaceSwapchainDescriptor& descriptor,
    const std::optional<int> cuda_device_index) {
    if (surface_id.empty() || !descriptor.valid()) {
        return WorkspaceGpuBridgeCefResult{false, "invalid_swapchain_metadata",
                                           "workspace GPU bridge requires a valid persistent swapchain descriptor"};
    }
    if (!cuda_device_index.has_value() || *cuda_device_index < 0) {
        return WorkspaceGpuBridgeCefResult{false, "missing_cuda_device",
                                           "workspace GPU bridge swapchain configure requires a CUDA device index"};
    }
    if (mmltk_cef_workspace_gpu_bridge_configure_swapchain == nullptr) {
        return WorkspaceGpuBridgeCefResult{false, "swapchain_cabi_unavailable",
                                           "CEF workspace swapchain configure helper is absent"};
    }
    const std::string payload = workspace_swapchain_descriptor_json(descriptor, cuda_device_index);
    CefString result_code;
    CefString result_detail;
    const bool ok = mmltk_cef_workspace_gpu_bridge_configure_swapchain(
        surface_id.data(), surface_id.size(), payload.data(), payload.size(), &result_code, &result_detail);
    return cef_result_from_strings(ok, result_code, result_detail, ok ? "ok" : "swapchain_configure_failed",
                                   ok ? "configured workspace swapchain" : "CEF rejected workspace swapchain");
}

WorkspaceGpuBridgeCefResult present_cef_workspace_gpu_bridge_front_slot(
    const std::string_view surface_id, const mmltk::live::WorkspacePresentSnapshot& present,
    const WorkspaceGpuBridgePresentRects& rects) {
    if (surface_id.empty() || !present.valid || present.revision == 0U || present.ready_event_handle == 0U) {
        return WorkspaceGpuBridgeCefResult{false, "invalid_present",
                                           "workspace GPU bridge requires a valid front-slot present with ready sync"};
    }
    if (rects.bounds_width <= 0 || rects.bounds_height <= 0) {
        return WorkspaceGpuBridgeCefResult{false, "missing_workspace_bounds",
                                           "workspace GPU bridge present requires non-empty browser workspace bounds"};
    }
    if (mmltk_cef_workspace_gpu_bridge_present_front_slot == nullptr) {
        return WorkspaceGpuBridgeCefResult{false, "swapchain_cabi_unavailable",
                                           "CEF workspace front-slot present helper is absent"};
    }
    CefString result_code;
    CefString result_detail;
    constexpr std::uint32_t kReadySyncCudaEvent = static_cast<std::uint32_t>(LinuxImportedSyncHandleKind::CudaEvent);
    const bool ok = mmltk_cef_workspace_gpu_bridge_present_front_slot(
        surface_id.data(), surface_id.size(), present.front_slot_index, present.revision, present.swapchain_generation,
        rects.bounds_x, rects.bounds_y, rects.bounds_width, rects.bounds_height, rects.damage_x, rects.damage_y,
        rects.damage_width, rects.damage_height, kReadySyncCudaEvent,
        static_cast<std::uint64_t>(present.ready_event_handle), present.ready_ns, &result_code, &result_detail);
    return cef_result_from_strings(ok, result_code, result_detail, ok ? "ok" : "swapchain_present_failed",
                                   ok ? "presented workspace front slot" : "CEF rejected workspace front slot present");
}

WorkspaceGpuBridgeCefResult destroy_cef_workspace_gpu_bridge_swapchain(const std::string_view surface_id) {
    if (surface_id.empty()) {
        return WorkspaceGpuBridgeCefResult{false, "invalid_surface",
                                           "workspace GPU bridge destroy requires a workspace surface id"};
    }
    if (mmltk_cef_workspace_gpu_bridge_destroy_swapchain == nullptr) {
        return WorkspaceGpuBridgeCefResult{false, "swapchain_cabi_unavailable",
                                           "CEF workspace swapchain destroy helper is absent"};
    }
    CefString result_code;
    CefString result_detail;
    const bool ok = mmltk_cef_workspace_gpu_bridge_destroy_swapchain(surface_id.data(), surface_id.size(), &result_code,
                                                                     &result_detail);
    return cef_result_from_strings(ok, result_code, result_detail, ok ? "ok" : "swapchain_destroy_failed",
                                   ok ? "destroyed workspace swapchain" : "CEF rejected workspace swapchain destroy");
}

void release_workspace_gpu_bridge_publication(const std::string_view surface_id, const std::string_view revision) {
    if (mmltk_cef_workspace_gpu_bridge_release_surface == nullptr || surface_id.empty() || revision.empty()) {
        return;
    }
    CefString result_code;
    CefString result_detail;
    if (!mmltk_cef_workspace_gpu_bridge_release_surface(surface_id.data(), surface_id.size(), revision.data(),
                                                        revision.size(), false, &result_code, &result_detail)) {
        mmltk::logging::logger("gui")->warn(
            "workspace GPU bridge browser surface release failed {} revision {}: {}", surface_id, revision,
            workspace_gpu_bridge_result_message(result_code.ToString(), result_detail.ToString()));
    }
}

void send_workspace_gpu_bridge_publication(const CefRefPtr<CefBrowser>& browser,
                                           const std::optional<WorkspaceGpuBridgeSurfacePublication>& publication) {
    if (browser == nullptr) {
        return;
    }

    CefRefPtr<CefFrame> main_frame = browser->GetMainFrame();
    if (main_frame == nullptr) {
        return;
    }

    CefRefPtr<CefProcessMessage> message =
        CefProcessMessage::Create(kRendererWorkspaceGpuBridgeStateMessageName.data());
    message->GetArgumentList()->SetString(
        0U, publication.has_value() ? workspace_gpu_bridge_publication_to_json(*publication).dump() : std::string{});
    main_frame->SendProcessMessage(PID_RENDERER, message);
}

void CefWorkspaceGpuBridge::install_in_context(const CefRefPtr<CefBrowser>& browser, const CefRefPtr<CefFrame>& frame,
                                               const CefRefPtr<CefV8Context>& context) {
    CEF_REQUIRE_RENDERER_THREAD();
    if (browser == nullptr || frame == nullptr || !frame->IsMain() || context == nullptr || !context->IsValid()) {
        return;
    }

    CefRefPtr<CefV8Value> bridge = CefV8Value::CreateObject(nullptr, nullptr);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto attributes = static_cast<CefV8Value::PropertyAttribute>(
        V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTENUM | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
    bridge->SetValue(
        "acquireCurrentSurface",
        CefV8Value::CreateFunction("acquireCurrentSurface",
                                   new WorkspaceGpuBridgeFunctionHandler(
                                       this, WorkspaceGpuBridgeFunctionHandler::Operation::AcquireCurrentSurface)),
        attributes);
    bridge->SetValue("importTexture",
                     CefV8Value::CreateFunction("importTexture",
                                                new WorkspaceGpuBridgeFunctionHandler(
                                                    this, WorkspaceGpuBridgeFunctionHandler::Operation::ImportTexture)),
                     attributes);
    bridge->SetValue("releaseSurface",
                     CefV8Value::CreateFunction(
                         "releaseSurface", new WorkspaceGpuBridgeFunctionHandler(
                                               this, WorkspaceGpuBridgeFunctionHandler::Operation::ReleaseSurface)),
                     attributes);
    bridge->SetValue(
        "releaseRendererTexture",
        CefV8Value::CreateFunction("releaseRendererTexture",
                                   new WorkspaceGpuBridgeFunctionHandler(
                                       this, WorkspaceGpuBridgeFunctionHandler::Operation::ReleaseRendererTexture)),
        attributes);

    context->GetGlobal()->SetValue(kWorkspaceGpuBridgePropertyName, bridge, attributes);

    SurfaceState& state = surfaces_by_browser_id_[browser->GetIdentifier()];
    state.structural_capability =
        workspace_gpu_bridge_structural_capability(context->GetGlobal()->GetValue(kWorkspaceGpuBridgePropertyName));
    state.zero_copy = zero_copy_capability_from_publication(state.structural_capability, state.publication);
    send_runtime_capabilities_update(browser, state);
}

const CefWorkspaceGpuBridge::SurfaceState* CefWorkspaceGpuBridge::surface_state_for_browser(
    const int browser_identifier) const noexcept {
    const auto it = surfaces_by_browser_id_.find(browser_identifier);
    return it != surfaces_by_browser_id_.end() ? &it->second : nullptr;
}

void CefWorkspaceGpuBridge::forget_surface_revision(const int browser_identifier, const std::string_view surface_id,
                                                    const std::string_view revision) {
    const auto it = surfaces_by_browser_id_.find(browser_identifier);
    if (it == surfaces_by_browser_id_.end()) {
        return;
    }

    SurfaceState& state = it->second;
    if (!state.publication.has_value() || state.publication->surface_info.surface_id != surface_id ||
        state.publication->surface_info.revision != revision) {
        return;
    }

    state.publication.reset();
}

bool CefWorkspaceGpuBridge::record_zero_copy_result(const int browser_identifier, const bool available,
                                                    std::string result_code, std::string result_detail) {
    const auto it = surfaces_by_browser_id_.find(browser_identifier);
    if (it == surfaces_by_browser_id_.end()) {
        return false;
    }
    WorkspaceGpuBridgeZeroCopyCapability next;
    next.attempted = true;
    next.available = available;
    next.result_code = std::move(result_code);
    next.result_detail = std::move(result_detail);
    if (it->second.zero_copy == next) {
        return false;
    }
    it->second.zero_copy = std::move(next);
    return true;
}

bool CefWorkspaceGpuBridge::on_process_message_received(const CefRefPtr<CefBrowser>& browser,
                                                        const CefRefPtr<CefFrame>&, const CefProcessId source_process,
                                                        const CefRefPtr<CefProcessMessage>& message) {
    CEF_REQUIRE_RENDERER_THREAD();
    if (source_process != PID_BROWSER || browser == nullptr || message == nullptr ||
        message->GetName().ToString() != kRendererWorkspaceGpuBridgeStateMessageName) {
        return false;
    }

    SurfaceState& state = surfaces_by_browser_id_[browser->GetIdentifier()];
    state.state_known = true;
    state.publication = decode_publication_payload(message->GetArgumentList()->GetString(0U).ToString());
    if (state.publication.has_value()) {
        state.zero_copy = zero_copy_capability_from_publication(state.structural_capability, state.publication);
    }
    send_runtime_capabilities_update(browser, state);
    return true;
}

void CefWorkspaceGpuBridge::on_browser_destroyed(const int browser_identifier) {
    CEF_REQUIRE_RENDERER_THREAD();
    surfaces_by_browser_id_.erase(browser_identifier);
}

bool WorkspaceGpuBridgeFunctionHandler::Execute(const CefString& name, CefRefPtr<CefV8Value>,
                                                const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval,
                                                CefString& exception) {
    CEF_REQUIRE_RENDERER_THREAD();
    if (bridge_ == nullptr) {
        exception = "workspace GPU bridge is unavailable";
        return true;
    }

    const int browser_identifier = current_browser_identifier();
    const CefWorkspaceGpuBridge::SurfaceState* surface_state = bridge_->surface_state_for_browser(browser_identifier);
    const auto require_surface_id = [&arguments, &exception]() -> std::string {
        if (arguments.empty() || !arguments[0]->IsString()) {
            exception = "workspace GPU bridge requires a surfaceId string";
            return {};
        }
        return arguments[0]->GetStringValue().ToString();
    };

    switch (operation_) {
        case Operation::AcquireCurrentSurface: {
            const std::string surface_id = require_surface_id();
            if (surface_id.empty()) {
                retval = CefV8Value::CreateNull();
                return true;
            }
            if (surface_state != nullptr && !surface_state->structural_capability.available) {
                exception = workspace_gpu_bridge_blocked_message("workspace_surface_bridge_unavailable",
                                                                 surface_state->structural_capability.detail);
                retval = CefV8Value::CreateNull();
                return true;
            }
            if (surface_state == nullptr || !surface_state->state_known || !surface_state->publication.has_value() ||
                surface_state->publication->surface_info.surface_id != surface_id) {
                retval = CefV8Value::CreateNull();
                return true;
            }
            if (!surface_state->publication->has_live_exported_shared_image()) {
                exception = shared_image_export_rejected_exception(*surface_state->publication);
                retval = CefV8Value::CreateNull();
                return true;
            }
            if (!cef_workspace_gpu_bridge_zero_copy_import_supported()) {
                exception =
                    renderer_import_rejected_exception("workspace GPU bridge renderer import support is unavailable");
                retval = CefV8Value::CreateNull();
                return true;
            }
            retval = create_workspace_surface_value(surface_state->publication->surface_info);
            return true;
        }
        case Operation::ImportTexture: {
            if (name != "importTexture" || arguments.size() != 3U || arguments[0] == nullptr ||
                !arguments[0]->IsObject() || !arguments[1]->IsString() || !arguments[2]->IsString()) {
                exception =
                    "workspace GPU bridge importTexture requires a device, surfaceId, "
                    "and revision";
                retval = CefV8Value::CreateNull();
                return true;
            }
            const std::string surface_id = arguments[1]->GetStringValue().ToString();
            const std::string revision = arguments[2]->GetStringValue().ToString();
            const auto publish_import_result = [this](const bool available, std::string result_code,
                                                      std::string result_detail) {
                CefRefPtr<CefBrowser> browser = current_browser();
                if (browser == nullptr ||
                    !bridge_->record_zero_copy_result(browser->GetIdentifier(), available, std::move(result_code),
                                                      std::move(result_detail))) {
                    return;
                }
                if (const CefWorkspaceGpuBridge::SurfaceState* updated_surface_state =
                        bridge_->surface_state_for_browser(browser->GetIdentifier());
                    updated_surface_state != nullptr) {
                    send_runtime_capabilities_update(browser, *updated_surface_state);
                }
            };
            const auto reject_import = [&exception, &retval, &surface_id, &revision,
                                        &publish_import_result](const char* reason) {
                exception = renderer_import_rejected_exception(reason);
                mmltk::logging::logger("gui")->warn("workspace GPU bridge import rejected surface {} revision {}: {}",
                                                    surface_id, revision, reason);
                publish_import_result(false, std::string(kRendererImportRejectedReason), reason);
                retval = CefV8Value::CreateNull();
                return true;
            };
            if (surface_state == nullptr || !surface_state->state_known) {
                return reject_import("workspace GPU bridge surface state is unavailable");
            }
            if (!surface_state->structural_capability.available) {
                return reject_import(surface_state->structural_capability.detail.empty()
                                         ? "workspace GPU bridge structural capability is unavailable"
                                         : surface_state->structural_capability.detail.c_str());
            }
            if (!surface_state->publication.has_value() ||
                surface_state->publication->surface_info.surface_id != surface_id ||
                surface_state->publication->surface_info.revision != revision) {
                return reject_import("workspace GPU bridge revision is unavailable");
            }
            if (!surface_state->publication->has_live_exported_shared_image()) {
                return reject_import("workspace GPU bridge importTexture requires a live exported SharedImage payload");
            }
            if (!cef_workspace_gpu_bridge_zero_copy_import_supported()) {
                return reject_import("workspace GPU bridge importTexture requires the renderer GPU bridge patch set");
            }
            std::string import_result_code;
            std::string import_result_detail;
            if (!import_texture_via_renderer_helper(arguments[0], *surface_state->publication, retval, exception,
                                                    import_result_code, import_result_detail)) {
                const std::string import_exception = exception.ToString();
                if (import_exception.find(kWorkspaceGpuBridgeBlockedPrefix) == std::string::npos) {
                    exception = renderer_import_rejected_exception(import_exception);
                }
                if (CefRefPtr<CefBrowser> browser = current_browser(); browser != nullptr) {
                    bridge_->record_zero_copy_result(browser->GetIdentifier(), false, import_result_code,
                                                     import_result_detail);
                    if (const CefWorkspaceGpuBridge::SurfaceState* updated_surface_state =
                            bridge_->surface_state_for_browser(browser->GetIdentifier());
                        updated_surface_state != nullptr) {
                        send_runtime_capabilities_update(browser, *updated_surface_state);
                    }
                }
                retval = CefV8Value::CreateNull();
            } else {
                publish_import_result(true, "ok", "workspace zero-copy import succeeded");
            }
            return true;
        }
        case Operation::ReleaseRendererTexture: {
            if (arguments.size() != 2U || !arguments[0]->IsString() || !arguments[1]->IsString()) {
                exception =
                    "workspace GPU bridge releaseRendererTexture requires a surfaceId and "
                    "revision";
                retval = CefV8Value::CreateBool(false);
                return true;
            }
            const std::string surface_id = arguments[0]->GetStringValue().ToString();
            const std::string revision = arguments[1]->GetStringValue().ToString();
            const RendererTextureReleaseResult renderer_release =
                release_renderer_workspace_texture(surface_id, revision);
            retval = create_workspace_release_result_value(renderer_release.released, renderer_release.result_code,
                                                           renderer_release.result_detail);
            return true;
        }
        case Operation::ReleaseSurface: {
            if (arguments.size() != 2U || !arguments[0]->IsString() || !arguments[1]->IsString()) {
                exception =
                    "workspace GPU bridge releaseSurface requires a surfaceId and "
                    "revision";
                retval = CefV8Value::CreateBool(false);
                return true;
            }
            const std::string surface_id = arguments[0]->GetStringValue().ToString();
            const std::string revision = arguments[1]->GetStringValue().ToString();
            const RendererTextureReleaseResult renderer_release =
                release_renderer_workspace_texture(surface_id, revision);
            CefRefPtr<CefBrowser> browser = current_browser();
            if (browser != nullptr) {
                bridge_->forget_surface_revision(browser->GetIdentifier(), surface_id, revision);
                if (const CefWorkspaceGpuBridge::SurfaceState* updated_surface_state =
                        bridge_->surface_state_for_browser(browser->GetIdentifier());
                    updated_surface_state != nullptr) {
                    send_runtime_capabilities_update(browser, *updated_surface_state);
                }
            }
            CefRefPtr<CefFrame> main_frame = browser != nullptr ? browser->GetMainFrame() : nullptr;
            if (main_frame != nullptr) {
                CefRefPtr<CefProcessMessage> message =
                    CefProcessMessage::Create(kRendererWorkspaceGpuBridgeReleaseMessageName.data());
                message->GetArgumentList()->SetString(0U, surface_id);
                message->GetArgumentList()->SetString(1U, revision);
                main_frame->SendProcessMessage(PID_BROWSER, message);
            }
            retval = create_workspace_release_result_value(renderer_release.released, renderer_release.result_code,
                                                           renderer_release.result_detail);
            return true;
        }
    }

    exception = "workspace GPU bridge operation is unsupported";
    retval = CefV8Value::CreateNull();
    return true;
}

}  // namespace mmltk::gui
