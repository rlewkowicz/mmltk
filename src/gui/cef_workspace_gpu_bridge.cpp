#include "gui/cef_workspace_gpu_bridge.h"

#include "gui/browser_runtime_shared.h"
#include "gui/browser_workspace_surface_bridge.h"
#include "gui/cef_subprocess_app.h"

#include "include/internal/cef_types.h"
#include "include/wrapper/cef_helpers.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace mmltk::gui {

namespace {

using namespace browser_runtime_shared;

constexpr auto kWorkspaceGpuBridgePropertyName = "__MMLTK_WORKSPACE_GPU_BRIDGE__";

extern "C" bool mmltk_cef_workspace_gpu_bridge_renderer_helper_present() noexcept __attribute__((weak));

extern "C" bool mmltk_cef_workspace_gpu_bridge_export_shared_image(
    const char* surface_id, std::size_t surface_id_length, const char* revision, std::size_t revision_length,
    const char* publication_source_json, std::size_t publication_source_json_length,
    CefString* exported_shared_image_json_out, CefString* exception) noexcept __attribute__((weak));

extern "C" bool mmltk_cef_workspace_gpu_bridge_import_texture(
    CefRefPtr<CefV8Value> device, const char* surface_id, std::size_t surface_id_length, const char* revision,
    std::size_t revision_length, const char* exported_shared_image_json, std::size_t exported_shared_image_json_length,
    CefRefPtr<CefV8Value>* texture_out, CefString* exception) noexcept __attribute__((weak));

[[nodiscard]] bool exported_shared_image_helper_present() noexcept {
    if (mmltk_cef_workspace_gpu_bridge_import_texture == nullptr) {
        return false;
    }
    return mmltk_cef_workspace_gpu_bridge_renderer_helper_present == nullptr ||
           mmltk_cef_workspace_gpu_bridge_renderer_helper_present();
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
        {"handle", std::to_string(source.handle)},
        {"cuda_device_index", source.cuda_device_index},
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
    const mmltk::browser::WorkspaceSurfaceInfo& surface_info, const RetainedBrowserImportedFrameSource& source) {
    if (mmltk_cef_workspace_gpu_bridge_export_shared_image == nullptr) {
        return std::nullopt;
    }

    const std::optional<LinuxImportedFrameSourceContract> linux_import = linux_imported_frame_source_contract(source);
    const std::optional<LinuxImportedFrameLifecycleContract> linux_lifecycle =
        linux_imported_frame_lifecycle_contract(source);
    if (!linux_import.has_value()) {
        return std::nullopt;
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
    CefString helper_exception;
    const bool exported = mmltk_cef_workspace_gpu_bridge_export_shared_image(
        surface_info.surface_id.data(), surface_info.surface_id.size(), surface_info.revision.data(),
        surface_info.revision.size(), publication_source_payload.data(), publication_source_payload.size(),
        &exported_shared_image_json, &helper_exception);
    if (!exported || exported_shared_image_json.ToString().empty()) {
        (void)helper_exception;
        return std::nullopt;
    }

    WorkspaceGpuBridgeExportedSharedImage exported_shared_image;
    workspace_gpu_bridge_exported_shared_image_from_json(nlohmann::json::parse(exported_shared_image_json.ToString()),
                                                         exported_shared_image);
    if (!exported_shared_image.is_valid()) {
        return std::nullopt;
    }
    return exported_shared_image;
}

[[nodiscard]] nlohmann::json workspace_gpu_bridge_publication_to_json(
    const WorkspaceGpuBridgeSurfacePublication& publication) {
    return {
        {"workspace_surface", publication.surface_info},
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

void send_runtime_capabilities_update(const CefRefPtr<CefBrowser>& browser, const bool state_known,
                                      const std::optional<WorkspaceGpuBridgeSurfacePublication>& publication) {
    const bool has_publication = publication.has_value();
    const bool has_live_exported_shared_image = has_publication && publication->has_live_exported_shared_image();
    nlohmann::json message = {
        {"type", kRuntimeCapabilitiesMessageType},
        {"workspace_surface_bridge", state_known ? "available" : "unknown"},
        {"workspace_surface_zero_copy",
         !state_known                                                                              ? "unknown"
         : !has_publication                                                                        ? "unknown"
         : has_live_exported_shared_image && cef_workspace_gpu_bridge_zero_copy_import_supported() ? "available"
                                                                                                   : "unavailable"},
    };
    send_renderer_bridge_json_message(browser, message);
}

[[nodiscard]] bool import_texture_via_renderer_helper(CefRefPtr<CefV8Value> device,
                                                      const WorkspaceGpuBridgeSurfacePublication& publication,
                                                      CefRefPtr<CefV8Value>& texture, CefString& exception) {
    const auto& exported_shared_image = publication.exported_shared_image;
    if (!exported_shared_image.has_value() || !exported_shared_image->has_live_image()) {
        exception =
            "workspace GPU bridge current publication has no live exported "
            "SharedImage payload";
        texture = CefV8Value::CreateNull();
        return false;
    }
    if (mmltk_cef_workspace_gpu_bridge_import_texture == nullptr) {
        exception =
            "workspace GPU bridge importTexture requires the renderer GPU bridge "
            "patch set";
        texture = CefV8Value::CreateNull();
        return false;
    }

    CefRefPtr<CefV8Value> imported_texture;
    CefString helper_exception;
    const bool imported = mmltk_cef_workspace_gpu_bridge_import_texture(
        std::move(device), publication.surface_info.surface_id.data(), publication.surface_info.surface_id.size(),
        publication.surface_info.revision.data(), publication.surface_info.revision.size(),
        exported_shared_image->serialized_json.data(), exported_shared_image->serialized_json.size(), &imported_texture,
        &helper_exception);
    if (!imported || imported_texture == nullptr || imported_texture->IsNull() || imported_texture->IsUndefined()) {
        exception = !helper_exception.ToString().empty() ? helper_exception
                                                         : CefString(
                                                               "workspace GPU bridge renderer helper did not return a "
                                                               "GPUTexture");
        texture = CefV8Value::CreateNull();
        return false;
    }

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

class WorkspaceGpuBridgeFunctionHandler : public CefV8Handler {
   public:
    enum class Operation : std::uint8_t {
        AcquireCurrentSurface = 0,
        ImportTexture = 1,
        ReleaseSurface = 2,
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
    publication.exported_shared_image = export_shared_image_publication(surface_info, source);
    return publication_is_valid(publication)
               ? std::optional<WorkspaceGpuBridgeSurfacePublication>(std::move(publication))
               : std::nullopt;
}

bool cef_workspace_gpu_bridge_zero_copy_import_supported() noexcept {
    return exported_shared_image_helper_present();
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

    context->GetGlobal()->SetValue(kWorkspaceGpuBridgePropertyName, bridge, attributes);
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
    send_runtime_capabilities_update(browser, state.state_known, state.publication);
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
            if (surface_state == nullptr || !surface_state->state_known || !surface_state->publication.has_value() ||
                surface_state->publication->surface_info.surface_id != surface_id ||
                !surface_state->publication->has_live_exported_shared_image() ||
                !cef_workspace_gpu_bridge_zero_copy_import_supported()) {
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
            if (surface_state == nullptr || !surface_state->state_known) {
                exception = "workspace GPU bridge surface state is unavailable";
                retval = CefV8Value::CreateNull();
                return true;
            }
            if (!surface_state->publication.has_value() ||
                surface_state->publication->surface_info.surface_id != surface_id ||
                surface_state->publication->surface_info.revision != revision) {
                exception = "workspace GPU bridge revision is unavailable";
                retval = CefV8Value::CreateNull();
                return true;
            }
            if (!surface_state->publication->has_live_exported_shared_image()) {
                exception =
                    "workspace GPU bridge importTexture requires a live exported "
                    "SharedImage payload";
                retval = CefV8Value::CreateNull();
                return true;
            }
            if (!cef_workspace_gpu_bridge_zero_copy_import_supported()) {
                exception =
                    "workspace GPU bridge importTexture requires the renderer GPU bridge "
                    "patch set";
                retval = CefV8Value::CreateNull();
                return true;
            }
            if (!import_texture_via_renderer_helper(arguments[0], *surface_state->publication, retval, exception)) {
                retval = CefV8Value::CreateNull();
            }
            return true;
        }
        case Operation::ReleaseSurface: {
            if (arguments.size() != 2U || !arguments[0]->IsString() || !arguments[1]->IsString()) {
                exception =
                    "workspace GPU bridge releaseSurface requires a surfaceId and "
                    "revision";
                retval = CefV8Value::CreateUndefined();
                return true;
            }
            const std::string surface_id = arguments[0]->GetStringValue().ToString();
            const std::string revision = arguments[1]->GetStringValue().ToString();
            CefRefPtr<CefBrowser> browser = current_browser();
            if (browser != nullptr) {
                bridge_->forget_surface_revision(browser->GetIdentifier(), surface_id, revision);
                if (const CefWorkspaceGpuBridge::SurfaceState* updated_surface_state =
                        bridge_->surface_state_for_browser(browser->GetIdentifier());
                    updated_surface_state != nullptr) {
                    send_runtime_capabilities_update(browser, updated_surface_state->state_known,
                                                     updated_surface_state->publication);
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
            retval = CefV8Value::CreateUndefined();
            return true;
        }
    }

    exception = "workspace GPU bridge operation is unsupported";
    retval = CefV8Value::CreateNull();
    return true;
}

}  // namespace mmltk::gui
