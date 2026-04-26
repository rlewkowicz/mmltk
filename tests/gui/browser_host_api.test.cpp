#include "browser/host_api.h"
#include "gui/browser_host_adapters.h"
#include "gui/annotation/common.h"
#include "gui/annotation/controller.h"
#include "gui/annotation/document/document.h"
#include "gui/annotation/document/types.h"
#include "gui/annotation/session.h"
#include "gui/annotation_core.h"
#include "gui/gui_settings.h"
#include "gui/view_state.h"
#include "gui/browser_host_helpers.h"
#include "mmltk/live/live_types.h"
#include "mmltk/live/manual_overlay_document.h"
#include "support/filesystem_test_utils.hpp"
#include "support/catch2_compat.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace mmltk::browser;
using namespace mmltk::gui;

template <typename T>
[[nodiscard]] T& require_optional(std::optional<T>& value) {
    assert(value.has_value());
    if (!value.has_value()) {
        throw std::runtime_error("expected optional value");
    }
    return *value;
}

template <typename T>
[[nodiscard]] const T& require_optional(const std::optional<T>& value) {
    assert(value.has_value());
    if (!value.has_value()) {
        throw std::runtime_error("expected optional value");
    }
    return *value;
}

static_assert(kRoutedIntentPayloadAlternativeCount == 29U,
              "Update browser host API tests for RoutedIntentPayload ABI changes.");

mmltk::gui::GuiSettingsSnapshot make_gui_snapshot(mmltk::gui::View current_view, const std::string& selected_preset,
                                                  mmltk::gui::UiSettingsState& ui, mmltk::gui::TrainViewState& train,
                                                  mmltk::gui::ValidateViewState& validate,
                                                  mmltk::gui::PredictViewState& predict,
                                                  mmltk::gui::AnnotateViewState& annotate,
                                                  mmltk::gui::ExportViewState& export_state) {
    return mmltk::gui::GuiSettingsSnapshot{
        current_view, selected_preset, &ui, &train, &validate, &predict, &annotate, &export_state,
    };
}

struct GuiSnapshotFixture {
    mmltk::gui::UiSettingsState ui;
    mmltk::gui::TrainViewState train;
    mmltk::gui::ValidateViewState validate;
    mmltk::gui::PredictViewState predict;
    mmltk::gui::AnnotateViewState annotate;
    mmltk::gui::ExportViewState export_state;
    mmltk::gui::View current_view = mmltk::gui::View::Predict;
    std::string selected_preset = "rf-detr-seg-small";

    [[nodiscard]] mmltk::gui::GuiSettingsSnapshot make_snapshot() {
        return make_gui_snapshot(current_view, selected_preset, ui, train, validate, predict, annotate, export_state);
    }
};

struct BrowserHostSettingsFixture {
    GuiSnapshotFixture gui;
    mmltk::gui::GuiSettingsSnapshot snapshot;

    BrowserHostSettingsFixture() : gui(), snapshot(gui.make_snapshot()) {}

    void refresh_snapshot() {
        snapshot = gui.make_snapshot();
    }
};

IntentMessage make_intent_message(Workflow workflow, const char* intent_name,
                                  nlohmann::json payload = nlohmann::json::object()) {
    IntentMessage intent;
    intent.workflow = workflow;
    intent.intent = intent_name;
    intent.payload = std::move(payload);
    return intent;
}

void set_available_runtime_capabilities(BrowserRuntimeCapabilities& capabilities) {
    capabilities.host_backend = BrowserHostBackend::Cef;
    capabilities.navigator_gpu = BrowserRuntimeCapabilityStatus::Available;
    capabilities.workspace_surface_bridge = BrowserRuntimeCapabilityStatus::Available;
    capabilities.workspace_surface_zero_copy = BrowserRuntimeCapabilityStatus::Available;
}

[[nodiscard]] WorkspaceSurfaceInfo make_workspace_surface_info(std::string revision, const std::uint32_t width = 640U,
                                                               const std::uint32_t height = 360U) {
    return WorkspaceSurfaceInfo{
        "workspace", std::move(revision), width, height, "rgba8unorm", true, true,
    };
}

void attach_available_workspace_surface(StateSnapshot& snapshot, std::string revision, const std::uint32_t width = 640U,
                                        const std::uint32_t height = 360U) {
    set_available_runtime_capabilities(snapshot.runtime_capabilities);
    snapshot.workspace_surface = make_workspace_surface_info(std::move(revision), width, height);
}

BrowserHostIntentContext make_fixture_intent_context(BrowserHostSettingsFixture& fixture,
                                                     const bool bind_view_and_preset = false) {
    auto& gui = fixture.gui;
    return BrowserHostIntentContext{
        bind_view_and_preset ? &gui.current_view : nullptr,
        bind_view_and_preset ? &gui.selected_preset : nullptr,
        &fixture.snapshot,
        &gui.predict,
        &gui.annotate,
    };
}

void apply_fixture_routed_intent(BrowserHostSettingsFixture& fixture, const IntentMessage& intent,
                                 const bool bind_view_and_preset = false) {
    apply_routed_intent(route_intent(intent), make_fixture_intent_context(fixture, bind_view_and_preset));
}

BrowserHostIntentContext make_annotation_intent_context(
    mmltk::gui::AnnotationDocument& document, mmltk::gui::AnnotationSession& session,
    mmltk::gui::AnnotationController& controller, const mmltk::gui::AnnotationFrame* frame = nullptr,
    mmltk::gui::AnnotationCategories* categories = nullptr, mmltk::gui::View* current_view = nullptr,
    std::function<bool(std::string* error_message)> ensure_frame_pixels = {}) {
    return BrowserHostIntentContext{
        current_view,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &document,
        &session,
        &controller,
        frame,
        categories,
        std::move(ensure_frame_pixels),
    };
}

void apply_annotation_routed_intent(const IntentMessage& intent, mmltk::gui::AnnotationDocument& document,
                                    mmltk::gui::AnnotationSession& session,
                                    mmltk::gui::AnnotationController& controller,
                                    const mmltk::gui::AnnotationFrame* frame = nullptr,
                                    mmltk::gui::AnnotationCategories* categories = nullptr,
                                    mmltk::gui::View* current_view = nullptr,
                                    std::function<bool(std::string* error_message)> ensure_frame_pixels = {}) {
    apply_routed_intent(route_intent(intent),
                        make_annotation_intent_context(document, session, controller, frame, categories, current_view,
                                                       std::move(ensure_frame_pixels)));
}

[[nodiscard]] mmltk::gui::AnnotationObject make_test_annotation_object() {
    mmltk::gui::AnnotationObject object;
    object.object_id = "manual-1";
    object.category_index = 0U;
    object.shape = mmltk::gui::AnnotationBoxShape{mmltk::gui::AnnotationBox{4, 5, 12, 18}};
    return object;
}

[[nodiscard]] mmltk::gui::AnnotationObject make_test_point_object(const float x, const float y) {
    mmltk::gui::AnnotationObject object;
    object.object_id = "manual-1";
    object.category_index = 0U;
    object.shape = mmltk::gui::AnnotationPointShape{mmltk::gui::AnnotationPoint{x, y}};
    return object;
}

[[nodiscard]] mmltk::gui::AnnotationObject make_test_mask_object() {
    mmltk::gui::AnnotationObject object;
    object.object_id = "manual-1";
    object.category_index = 0U;
    object.shape = mmltk::gui::AnnotationMaskShape{
        mmltk::gui::AnnotationBox{0, 0, 8, 8},
        mmltk::gui::AnnotationMaskRegion{0U, 0U, 8U, 8U},
        std::vector<std::uint8_t>(64U, 0U),
        0U,
        std::nullopt,
    };
    return object;
}

[[nodiscard]] mmltk::gui::AnnotationCategories make_test_annotation_categories() {
    mmltk::gui::AnnotationCategories categories;
    categories.items.emplace_back(1, "object");
    return categories;
}

[[nodiscard]] mmltk::gui::AnnotationFrame make_test_annotation_frame(const std::uint32_t width = 32U,
                                                                     const std::uint32_t height = 24U) {
    mmltk::gui::AnnotationFrame frame;
    frame.width = width;
    frame.height = height;
    frame.capture_width = width;
    frame.capture_height = height;
    frame.pixels_bgr.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U, std::uint8_t{0});
    return frame;
}

[[nodiscard]] std::string make_browser_frame_token_prefix(const std::string_view slot_name,
                                                          const std::uint32_t slot_index,
                                                          const mmltk::live::LiveFrameId& frame_id) {
    return std::format("browser-frame:{}:{}:{}:{}", slot_name, slot_index, frame_id.session_nonce, frame_id.sequence);
}

[[nodiscard]] FrameSlotDescriptor make_direct_bridge_frame_slot(
    std::string slot_name, const std::uint32_t slot_index, const mmltk::live::LiveFrameId& frame_id,
    const CaptureRegion& capture_region, const std::uint32_t width, const std::uint32_t height,
    const std::uint64_t row_stride_bytes, const std::uint64_t ready_ns,
    const FrameSlotLifecycle lifecycle = FrameSlotLifecycle::ExplicitRelease, const bool short_frame = false) {
    FrameSlotDescriptor descriptor;
    descriptor.slot_name = std::move(slot_name);
    descriptor.transport = FrameTransportKind::CudaDeviceBuffer;
    descriptor.pixel_format = FramePixelFormat::Bgr8;
    descriptor.slot_index = slot_index;
    descriptor.frame_id = frame_id;
    descriptor.capture_region = capture_region;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.row_stride_bytes = row_stride_bytes;
    descriptor.byte_length = frame_byte_length(row_stride_bytes, height);
    descriptor.ready_ns = ready_ns;
    descriptor.short_frame = short_frame;
    descriptor.lifecycle = lifecycle;
    descriptor.ownership = FrameSlotOwnership::NativeHost;

    const std::string token_prefix =
        make_browser_frame_token_prefix(descriptor.slot_name, descriptor.slot_index, descriptor.frame_id);
    apply_linux_importable_frame_slot_contract(
        descriptor, LinuxImportableFrameSlotContract{
                        LinuxImportableImageHandle{
                            LinuxImportableImageHandleKind::OpaqueToken,
                            std::string(kLinuxImportableImageHandleTypeNativeToken),
                            token_prefix + ":image",
                        },
                        FrameSlotReadySync{
                            FrameSlotSyncKind::TimelinePoint,
                            token_prefix + ":ready",
                            ready_ns,
                        },
                        lifecycle == FrameSlotLifecycle::ExplicitRelease ? token_prefix + ":release" : std::string(),
                    });

    return descriptor;
}

void assert_linux_import_slot_contract(const FrameSlotDescriptor& descriptor, const std::string_view image_handle,
                                       const std::string_view ready_handle,
                                       const std::optional<std::string_view> release_token = std::nullopt) {
    const auto& linux_import = require_optional(descriptor.linux_import);
    assert(linux_import.image.kind == LinuxImportableImageHandleKind::OpaqueToken);
    assert(linux_import.image.handle_type == "native_token");
    assert(linux_import.image.handle == image_handle);
    assert(linux_import.ready_sync.kind == FrameSlotSyncKind::TimelinePoint);
    assert(linux_import.ready_sync.handle == ready_handle);
    assert(linux_import.ready_sync.value == descriptor.ready_ns);
    if (release_token.has_value()) {
        assert(linux_import.release_token == *release_token);
    } else {
        assert(linux_import.release_token.empty());
    }
}

void assert_native_import_metadata(const FrameSlotDescriptor& descriptor,
                                   FrameSlotNativeImportReleaseBehavior expected_release_behavior,
                                   std::optional<std::string_view> texture_format);

void assert_transport_message_native_import_metadata(
    const FrameSlotDescriptor& descriptor, const std::optional<std::string_view> texture_format = std::nullopt) {
    assert_native_import_metadata(descriptor, FrameSlotNativeImportReleaseBehavior::TransportMessage, texture_format);
}

void assert_contract_release_native_import_metadata(
    const FrameSlotDescriptor& descriptor, const std::optional<std::string_view> texture_format = std::nullopt) {
    assert_native_import_metadata(descriptor, FrameSlotNativeImportReleaseBehavior::ContractRelease, texture_format);
}

void assert_native_import_metadata(const FrameSlotDescriptor& descriptor,
                                   const FrameSlotNativeImportReleaseBehavior expected_release_behavior,
                                   const std::optional<std::string_view> texture_format = std::nullopt) {
    const auto metadata = frame_slot_native_import_metadata(descriptor);
    const auto& metadata_value = require_optional(metadata);
    const auto& linux_import = require_optional(descriptor.linux_import);
    const auto& linux_metadata = require_optional(linux_import.metadata);
    assert(metadata_value.release_behavior == expected_release_behavior);
    assert(linux_metadata.release_behavior == expected_release_behavior);
    if (texture_format.has_value()) {
        assert(metadata_value.texture_format == *texture_format);
        assert(linux_metadata.texture_format == *texture_format);
    } else {
        assert(metadata_value.texture_format.empty());
        assert(linux_metadata.texture_format.empty());
    }
}

void assert_transport_message_native_import_metadata_json(
    const nlohmann::json& json, const std::optional<std::string_view> texture_format = std::nullopt) {
    assert(json.contains("linux_import"));
    assert(json.at("linux_import").contains("metadata"));
    assert(json.at("linux_import").at("metadata").at("release_behavior") == "transport_message");
    if (texture_format.has_value()) {
        assert(json.at("linux_import").at("metadata").at("texture_format") == *texture_format);
    } else {
        assert(!json.at("linux_import").at("metadata").contains("texture_format"));
    }
}

struct DirectAnnotationFixture {
    mmltk::gui::AnnotationController controller;
    mmltk::gui::AnnotationDocument document;
    mmltk::gui::AnnotationSession session;
    mmltk::gui::AnnotationFrame frame = make_test_annotation_frame();
    mmltk::gui::AnnotationCategories categories = make_test_annotation_categories();

    DirectAnnotationFixture() {
        document.set_objects({make_test_annotation_object()});
        const bool activated = controller.set_active_tool(mmltk::gui::AnnotationToolKind::Direct, document, session);
        assert(activated);
    }
};

template <typename DragIntent>
void assert_annotation_drag_payload(const DragIntent& drag, const AnnotateWorkspacePhase expected_phase,
                                    const int expected_capture_x, const int expected_capture_y) {
    assert(drag.phase == expected_phase);
    assert(drag.capture_x == expected_capture_x);
    assert(drag.capture_y == expected_capture_y);
}

void test_state_snapshot_round_trips_through_json() {
    StateSnapshot snapshot;
    snapshot.protocol_version = kHostApiProtocolVersion;
    snapshot.state_revision = 42U;
    snapshot.active_workflow = Workflow::Annotate;
    snapshot.workflow_state = nlohmann::json{
        {"annotate", {{"output_dir", "/tmp/annotated-scenes"}, {"threshold", 0.33}}},
        {"annotate_runtime",
         {{"setup",
           {{"can_start_live", false},
            {"can_stop_live", true},
            {"can_reload_frame", true},
            {"can_prev_frame", true},
            {"can_next_frame", false},
            {"current_input_index", 1},
            {"input_count", 2}}},
          {"hold_save", {{"enabled", true}, {"active", false}, {"blocked", false}}},
          {"brush", {{"radius", 16}, {"enabled", true}}}}},
        {"annotate_runtime_controls",
         {{"setup_frame_navigation",
           {{"current_index", 1},
            {"input_count", 2},
            {"next", {{"enabled", false}, {"intent", "annotate.setup_frame.next"}}}}},
          {"live_annotate", {{"start", {{"enabled", true}, {"intent", "annotate.live.start"}}}}},
          {"save",
           {{"save_now", {{"enabled", true}, {"intent", "annotate.save_now"}}},
            {"hold_save",
             {{"enabled", true}, {"value", false}, {"payload_key", "active"}, {"intent", "annotate.hold_save"}}}}},
          {"brush",
           {{"radius",
             {{"value", 16},
              {"min", 1},
              {"max", 128},
              {"payload_key", "radius"},
              {"intent", "annotate.brush_radius"}}}}}}},
        {"annotate_workspace_scene",
         {{"document_generation", 9U},
          {"selected_object_index", 1U},
          {"capture_width", 640U},
          {"capture_height", 360U},
          {"visible_objects", nlohmann::json::array()},
          {"editable_handles", nlohmann::json::array()},
          {"selected_dense_mask",
           {{"object_index", 1U},
            {"category_index", 2U},
            {"capture_box", {{"x1", 144}, {"y1", 92}, {"x2", 196}, {"y2", 154}}},
            {"mask_rle_encoding", "row_major_start_length"},
            {"mask_rle", "0:3 58:2"}}},
          {"brush_preview",
           {{"visible", true}, {"capture_x", 213}, {"capture_y", 97}, {"radius", 18}, {"erase", true}}}}},
        {"live_runtime",
         {{"preview",
           {{"fit_to_capture", true},
            {"crop_overlay_mode", false},
            {"full_frame", false},
            {"show_controls", true},
            {"can_toggle_fit_to_capture", true},
            {"can_toggle_full_frame", true}}}}},
        {"live_preview_controls",
         {{"fit_to_capture",
           {{"value", true}, {"enabled", true}, {"payload_key", "enabled"}, {"intent", "live.preview.fit_to_capture"}}},
          {"full_frame_display",
           {{"value", false},
            {"enabled", true},
            {"payload_key", "enabled"},
            {"intent", "live.preview.full_frame_display"}}}}},
    };
    snapshot.settings_state = nlohmann::json{
        {"ui_scale", 1.25},
        {"density", "balanced"},
    };
    snapshot.job.running = true;
    snapshot.job.label = "predict";
    snapshot.job.summary = "running";
    snapshot.job.output_tail = "frame 8";
    snapshot.job.recent_logs = {
        JobLogEntry{7U, "info", "predict launched"},
        JobLogEntry{8U, "debug", "frame published"},
    };
    snapshot.source.kind = mmltk::browser::SourceKind::VideoStream;
    snapshot.source.locator = "device:2";
    snapshot.source.device_index = 2;
    snapshot.source.capture_width = 1920;
    snapshot.source.capture_height = 1080;
    snapshot.source.capture_fps = 120;
    snapshot.source.v4l2_buffer_count = 4;
    snapshot.source.has_crop = true;
    snapshot.source.crop = CaptureRegion{100U, 200U, 640U, 360U};
    snapshot.annotation.document_generation = 9U;
    snapshot.annotation.session_revision = 12U;
    snapshot.annotation.capture_width = 640U;
    snapshot.annotation.capture_height = 360U;
    snapshot.annotation.instance_count = 3U;
    snapshot.annotation.selected_instance = 1U;
    attach_available_workspace_surface(snapshot, "89");

    const nlohmann::json json = snapshot;
    assert(json.at("type") == kStateSnapshotMessageType);
    assert(json.at("active_workflow") == "annotate");
    assert(json.at("source").at("v4l2_buffer_count") == 4);
    assert(json.at("runtime_capabilities").at("host_backend") == "cef");
    assert(json.at("runtime_capabilities").at("navigator_gpu") == "available");
    assert(json.at("runtime_capabilities").at("workspace_surface_bridge") == "available");
    assert(json.at("runtime_capabilities").at("workspace_surface_zero_copy") == "available");
    assert(json.at("runtime_capabilities").size() == 4U);
    assert(json.at("workspace_surface").at("surfaceId") == "workspace");
    assert(json.at("workspace_surface").at("revision") == "89");
    assert(json.at("workspace_surface").at("width") == 640U);
    assert(json.at("workspace_surface").at("height") == 360U);
    assert(json.at("workspace_surface").at("textureFormat") == "rgba8unorm");

    const StateSnapshot round_trip = json.get<StateSnapshot>();
    assert(round_trip.protocol_version == kHostApiProtocolVersion);
    assert(round_trip.state_revision == 42U);
    assert(round_trip.active_workflow == Workflow::Annotate);
    assert(round_trip.workflow_state.at("annotate").at("threshold") == 0.33);
    assert(round_trip.workflow_state.at("annotate_runtime").at("brush").at("radius") == 16);
    assert(round_trip.workflow_state.at("annotate_runtime").at("hold_save").at("enabled") == true);
    assert(round_trip.workflow_state.at("annotate_runtime_controls")
               .at("setup_frame_navigation")
               .at("next")
               .at("intent") == "annotate.setup_frame.next");
    assert(round_trip.workflow_state.at("annotate_runtime_controls").at("save").at("save_now").at("intent") ==
           "annotate.save_now");
    assert(round_trip.workflow_state.at("annotate_runtime_controls").at("save").at("hold_save").at("payload_key") ==
           "active");
    assert(
        round_trip.workflow_state.at("annotate_workspace_scene").at("selected_dense_mask").at("capture_box").at("x1") ==
        144);
    assert(round_trip.workflow_state.at("annotate_workspace_scene").at("selected_dense_mask").at("mask_rle") ==
           "0:3 58:2");
    assert(round_trip.workflow_state.at("annotate_workspace_scene").at("brush_preview").at("visible") == true);
    assert(round_trip.workflow_state.at("annotate_workspace_scene").at("brush_preview").at("capture_x") == 213);
    assert(round_trip.workflow_state.at("annotate_workspace_scene").at("brush_preview").at("capture_y") == 97);
    assert(round_trip.workflow_state.at("annotate_workspace_scene").at("brush_preview").at("radius") == 18);
    assert(round_trip.workflow_state.at("annotate_workspace_scene").at("brush_preview").at("erase") == true);
    assert(round_trip.workflow_state.at("live_runtime").at("preview").at("show_controls") == true);
    assert(round_trip.workflow_state.at("live_preview_controls").at("fit_to_capture").at("intent") ==
           "live.preview.fit_to_capture");
    assert(round_trip.workflow_state.at("live_preview_controls").at("full_frame_display").at("payload_key") ==
           "enabled");
    assert(round_trip.settings_state.at("ui_scale") == 1.25);
    assert(round_trip.job.recent_logs.size() == 2U);
    assert(round_trip.source.kind == mmltk::browser::SourceKind::VideoStream);
    assert(round_trip.source.crop.width == 640U);
    assert(round_trip.source.v4l2_buffer_count == 4);
    assert(round_trip.annotation.selected_instance == 1U);
    assert(round_trip.runtime_capabilities.host_backend == BrowserHostBackend::Cef);
    assert(round_trip.runtime_capabilities.navigator_gpu == BrowserRuntimeCapabilityStatus::Available);
    assert(round_trip.runtime_capabilities.workspace_surface_bridge == BrowserRuntimeCapabilityStatus::Available);
    assert(round_trip.runtime_capabilities.workspace_surface_zero_copy == BrowserRuntimeCapabilityStatus::Available);
    assert(round_trip.workspace_surface.has_value());
    const WorkspaceSurfaceInfo& round_trip_surface = require_optional(round_trip.workspace_surface);
    assert(round_trip_surface.surface_id == "workspace");
    assert(round_trip_surface.revision == "89");
    assert(round_trip_surface.width == 640U);
    assert(round_trip_surface.height == 360U);
}

void test_state_snapshot_round_trips_live_runtime_metadata_with_workspace_surface() {
    StateSnapshot snapshot;
    snapshot.protocol_version = kHostApiProtocolVersion;
    snapshot.state_revision = 43U;
    snapshot.active_workflow = Workflow::Live;
    attach_available_workspace_surface(snapshot, "43");
    snapshot.workflow_state =
        nlohmann::json{{"live_runtime",
                        {{"active_mode", "predict"},
                         {"preview",
                          {{"initialized", true},
                           {"has_frame", true},
                           {"frame_id", 77U},
                           {"live_frame_id", {{"session_nonce", 10U}, {"sequence", 11U}}},
                           {"displayed_region", {{"x", 4U}, {"y", 8U}, {"width", 640U}, {"height", 360U}}},
                           {"fit_to_capture", true},
                           {"crop_overlay_mode", false},
                           {"error", std::string{}}}},
                         {"compositor",
                          {{"running", true},
                           {"last_frame_id", {{"session_nonce", 10U}, {"sequence", 12U}}},
                           {"frames_composited", 19U},
                           {"frames_dropped", 0U}}}}}};

    const nlohmann::json json = snapshot;
    assert(!json.contains("frame_slots"));
    assert(json.at("workflow_state").at("live_runtime").at("preview").at("frame_id") == 77U);
    assert(json.at("workflow_state").at("live_runtime").at("preview").at("displayed_region").at("width") == 640U);
    assert(json.at("workspace_surface").at("revision") == "43");

    const StateSnapshot round_trip = json.get<StateSnapshot>();
    const WorkspaceSurfaceInfo& round_trip_surface = require_optional(round_trip.workspace_surface);
    assert(!round_trip_surface.revision.empty());
    assert(round_trip.runtime_capabilities.workspace_surface_bridge == BrowserRuntimeCapabilityStatus::Available);
    assert(round_trip.runtime_capabilities.workspace_surface_zero_copy == BrowserRuntimeCapabilityStatus::Available);
    assert(round_trip.workflow_state.at("live_runtime").at("preview").at("live_frame_id").at("sequence") == 11U);
    assert(round_trip.workflow_state.at("live_runtime").at("preview").at("displayed_region").at("height") == 360U);
    assert(round_trip.workflow_state.at("live_runtime").at("compositor").at("last_frame_id").at("sequence") == 12U);
}

void test_browser_runtime_capabilities_round_trip_accepts_cef_backend() {
    BrowserRuntimeCapabilities capabilities;
    capabilities.host_backend = BrowserHostBackend::Cef;
    capabilities.navigator_gpu = BrowserRuntimeCapabilityStatus::Available;
    capabilities.workspace_surface_bridge = BrowserRuntimeCapabilityStatus::Available;
    capabilities.workspace_surface_zero_copy = BrowserRuntimeCapabilityStatus::Unavailable;

    const nlohmann::json json = capabilities;
    assert(json.at("host_backend") == "cef");
    assert(json.at("navigator_gpu") == "available");
    assert(json.at("workspace_surface_bridge") == "available");
    assert(json.at("workspace_surface_zero_copy") == "unavailable");
    assert(json.size() == 4U);

    const BrowserRuntimeCapabilities round_trip = json.get<BrowserRuntimeCapabilities>();
    assert(round_trip.host_backend == BrowserHostBackend::Cef);
    assert(round_trip.navigator_gpu == BrowserRuntimeCapabilityStatus::Available);
    assert(round_trip.workspace_surface_bridge == BrowserRuntimeCapabilityStatus::Available);
    assert(round_trip.workspace_surface_zero_copy == BrowserRuntimeCapabilityStatus::Unavailable);
}

void test_intent_round_trips_through_json() {
    IntentMessage intent = make_intent_message(Workflow::Predict, "predict.start",
                                               nlohmann::json{
                                                   {"backend", "onnx"},
                                                   {"threshold", 0.25},
                                               });
    intent.request_id = 77U;

    const nlohmann::json json = intent;
    assert(json.at("type") == kIntentMessageType);
    assert(json.at("intent") == "predict.start");
    assert(json.at("workflow") == "predict");

    const IntentMessage parsed = json.get<IntentMessage>();
    assert(parsed.request_id == 77U);
    assert(parsed.workflow == Workflow::Predict);
    assert(parsed.intent == "predict.start");
    assert(parsed.payload.at("backend") == "onnx");
}

void test_state_and_intent_type_validation_is_strict() {
    bool state_threw = false;
    try {
        const nlohmann::json invalid_state = {
            {"type", kIntentMessageType},
            {"workflow", "annotate"},
            {"intent", "crop.commit"},
        };
        (void)invalid_state.get<StateSnapshot>();
    } catch (const std::runtime_error&) {
        state_threw = true;
    }
    assert(state_threw);

    bool intent_threw = false;
    try {
        const nlohmann::json invalid_intent = {
            {"type", kStateSnapshotMessageType},
            {"active_workflow", "annotate"},
        };
        (void)invalid_intent.get<IntentMessage>();
    } catch (const std::runtime_error&) {
        intent_threw = true;
    }
    assert(intent_threw);
}

void test_route_predict_start_intent_preserves_request_metadata_and_options() {
    IntentMessage intent = make_intent_message(Workflow::Predict, "predict.start",
                                               nlohmann::json{
                                                   {"selected_preset", "rf-detr-seg-medium"},
                                                   {"backend", "onnx"},
                                                   {"threshold", 0.2},
                                               });
    intent.protocol_version = kHostApiProtocolVersion;
    intent.request_id = 91U;

    const RoutedIntent routed = route_intent(intent);
    assert(routed.protocol_version == kHostApiProtocolVersion);
    assert(routed.request_id == 91U);
    assert(routed.workflow == Workflow::Predict);
    assert(routed_intent_name(routed.payload) == "predict.start");
    assert(std::holds_alternative<PredictStartIntent>(routed.payload));

    const auto& predict = std::get<PredictStartIntent>(routed.payload);
    assert(require_optional(predict.selected_preset) == "rf-detr-seg-medium");
    assert(predict.options.at("backend") == "onnx");
    assert(predict.options.at("threshold") == 0.2);
    assert(!predict.options.contains("selected_preset"));
}

void test_predict_start_settings_patch_updates_predict_workflow_state() {
    BrowserHostSettingsFixture fixture;

    PredictStartIntent intent;
    intent.selected_preset = "rf-detr-seg-medium";
    intent.options = nlohmann::json{
        {"source",
         {{"kind", static_cast<int>(mmltk::gui::SourceKind::SingleImage)}, {"single_image_path", "/tmp/input.png"}}},
        {"model_artifacts", {{"onnx_path", "/tmp/model.onnx"}}},
        {"execution", {{"device_id", 2}, {"allow_fp16", false}}},
        {"backend", "onnx"},
        {"threshold", 0.4},
        {"batch_size", 1},
    };

    apply_settings_update(SettingsUpdateIntent{make_predict_start_settings_patch(intent)}, fixture.snapshot);
    assert(fixture.snapshot.selected_preset == "rf-detr-seg-medium");
    assert(fixture.snapshot.workflows.predict != nullptr);
    assert(fixture.snapshot.workflows.predict->source.kind == mmltk::gui::SourceKind::SingleImage);
    assert(fixture.snapshot.workflows.predict->source.single_image_path == "/tmp/input.png");
    assert(fixture.snapshot.workflows.predict->onnx_path == "/tmp/model.onnx");
    assert(fixture.snapshot.workflows.predict->device_id == 2);
    assert(!fixture.snapshot.workflows.predict->allow_fp16);
    assert(fixture.snapshot.workflows.predict->backend == "onnx");
    assert(fixture.snapshot.workflows.predict->threshold == 0.4F);
    assert(fixture.snapshot.workflows.predict->batch_size == 1);
}

void test_annotate_save_settings_patch_updates_annotate_workflow_state() {
    BrowserHostSettingsFixture fixture;
    fixture.gui.current_view = mmltk::gui::View::Annotate;
    fixture.refresh_snapshot();

    AnnotateSaveIntent intent;
    intent.options = nlohmann::json{
        {"source",
         {{"kind", static_cast<int>(mmltk::gui::SourceKind::ImageFolder)}, {"image_directory", "/tmp/images"}}},
        {"output_dir", "/tmp/annotated"},
        {"split", "val"},
        {"backend", "onnx"},
        {"threshold", 0.5},
        {"full_frame", true},
    };

    apply_settings_update(SettingsUpdateIntent{make_annotate_save_settings_patch(intent)}, fixture.snapshot);
    assert(fixture.snapshot.workflows.annotate != nullptr);
    assert(fixture.snapshot.workflows.annotate->source.kind == mmltk::gui::SourceKind::ImageFolder);
    assert(fixture.snapshot.workflows.annotate->source.image_directory == "/tmp/images");
    assert(fixture.snapshot.workflows.annotate->output_dir == "/tmp/annotated");
    assert(fixture.snapshot.workflows.annotate->split == "val");
    assert(fixture.snapshot.workflows.annotate->backend == "onnx");
    assert(fixture.snapshot.workflows.annotate->threshold == 0.5F);
    assert(fixture.snapshot.workflows.annotate->full_frame);
}

void test_train_details_settings_patch_updates_train_workflow_state() {
    BrowserHostSettingsFixture fixture;
    fixture.gui.current_view = mmltk::gui::View::Train;
    fixture.refresh_snapshot();

    apply_settings_update(
        SettingsUpdateIntent{nlohmann::json{{"selected_preset", "rf-detr-seg-medium"},
                                            {"workflows",
                                             {{"train",
                                               {{"dataset_paths",
                                                 {{"train_compiled_path", "/tmp/train.bin"},
                                                  {"val_compiled_path", "/tmp/val.bin"},
                                                  {"test_compiled_path", "/tmp/test.bin"}}},
                                                {"model_artifacts", {{"weights_path", "/tmp/base.pt"}}},
                                                {"execution", {{"workers", 6}, {"lanes", 2}}},
                                                {"training",
                                                 {{"output_dir", "/tmp/train-out"},
                                                  {"resume_path", "/tmp/checkpoint.pt"},
                                                  {"input_mode", 1},
                                                  {"batch_size", 8},
                                                  {"epochs", 24},
                                                  {"optimizer", 1},
                                                  {"amp", false},
                                                  {"use_ema", true},
                                                  {"freeze_encoder", true}}}}}}}}},
        fixture.snapshot);

    assert(fixture.snapshot.selected_preset == "rf-detr-seg-medium");
    assert(fixture.snapshot.workflows.train != nullptr);
    assert(fixture.snapshot.workflows.train->train_compiled_path == "/tmp/train.bin");
    assert(fixture.snapshot.workflows.train->val_compiled_path == "/tmp/val.bin");
    assert(fixture.snapshot.workflows.train->test_compiled_path == "/tmp/test.bin");
    assert(fixture.snapshot.workflows.train->weights_path == "/tmp/base.pt");
    assert(fixture.snapshot.workflows.train->workers == 6);
    assert(fixture.snapshot.workflows.train->lanes == 2);
    assert(fixture.snapshot.workflows.train->output_dir == "/tmp/train-out");
    assert(fixture.snapshot.workflows.train->resume_path == "/tmp/checkpoint.pt");
    assert(fixture.snapshot.workflows.train->input_mode == mmltk::gui::TrainInputMode::Resume);
    assert(fixture.snapshot.workflows.train->batch_size == 8);
    assert(fixture.snapshot.workflows.train->epochs == 24);
    assert(fixture.snapshot.workflows.train->optimizer == mmltk::gui::TrainOptimizerMode::Muon);
    assert(!fixture.snapshot.workflows.train->amp);
    assert(fixture.snapshot.workflows.train->use_ema);
    assert(fixture.snapshot.workflows.train->freeze_encoder);
}

void test_route_settings_update_applies_json_merge_patch_to_gui_snapshot() {
    BrowserHostSettingsFixture fixture;
    fixture.gui.ui.dark_mode = false;
    fixture.gui.ui.ui_scale = 1.0F;
    fixture.gui.ui.font_size = 16.0F;
    fixture.gui.predict.output_path = "/tmp/original.json";
    fixture.refresh_snapshot();

    const IntentMessage intent = make_intent_message(
        Workflow::Predict, "settings.update",
        nlohmann::json{
            {"selected_preset", "rf-detr-seg-medium"},
            {"ui", {{"dark_mode", true}, {"ui_scale", 1.75}}},
            {"workflows", {{"predict", {{"predict", {{"output_path", "/tmp/updated.json"}, {"threshold", 0.4}}}}}}},
        });

    const RoutedIntent routed = route_intent(intent);
    assert(std::holds_alternative<SettingsUpdateIntent>(routed.payload));

    apply_settings_update(std::get<SettingsUpdateIntent>(routed.payload), fixture.snapshot);
    assert(fixture.snapshot.selected_preset == "rf-detr-seg-medium");
    assert(fixture.snapshot.ui_settings != nullptr);
    assert(fixture.snapshot.ui_settings->dark_mode);
    assert(fixture.snapshot.ui_settings->ui_scale == 1.75F);
    assert(fixture.snapshot.ui_settings->font_size == 16.0F);
    assert(fixture.snapshot.workflows.predict != nullptr);
    assert(fixture.snapshot.workflows.predict->output_path == "/tmp/updated.json");
    assert(fixture.snapshot.workflows.predict->threshold == 0.4F);
}

void test_route_crop_commit_updates_source_selection_state() {
    const IntentMessage intent =
        make_intent_message(Workflow::Live, "crop.commit",
                            nlohmann::json{
                                {"has_crop", true},
                                {"crop", {{"x", 40}, {"y", 24}, {"width", 800}, {"height", 600}}},
                            });

    mmltk::gui::SourceSelectionState source;
    source.capture_width = 1920;
    source.capture_height = 1080;
    source.crop_x = 0;
    source.crop_y = 0;
    source.crop_width = 0;
    source.crop_height = 0;

    const RoutedIntent routed = route_intent(intent);
    assert(std::holds_alternative<CropCommitIntent>(routed.payload));
    apply_crop_commit(std::get<CropCommitIntent>(routed.payload), source);
    assert(source.crop_x == 40);
    assert(source.crop_y == 24);
    assert(source.crop_width == 800);
    assert(source.crop_height == 600);

    const IntentMessage clear_intent =
        make_intent_message(Workflow::Live, "crop.commit", nlohmann::json{{"has_crop", false}});
    const RoutedIntent cleared = route_intent(clear_intent);
    apply_crop_commit(std::get<CropCommitIntent>(cleared.payload), source);
    assert(source.crop_x == 0);
    assert(source.crop_y == 0);
    assert(source.crop_width == 0);
    assert(source.crop_height == 0);
}

void test_route_file_dialog_and_viewport_commit_intents() {
    const IntentMessage file_dialog_intent =
        make_intent_message(Workflow::Annotate, "file_dialog.request",
                            nlohmann::json{
                                {"dialog_id", "project-open"},
                                {"mode", "open_folder"},
                                {"title", "Choose Project"},
                                {"default_path", "/tmp/project"},
                                {"filters", {{{"name", "Projects"}, {"patterns", {"*.mmltk", "*.json"}}}}},
                            });

    const RoutedIntent file_dialog_routed = route_intent(file_dialog_intent);
    assert(std::holds_alternative<FileDialogRequestIntent>(file_dialog_routed.payload));
    const auto& dialog = std::get<FileDialogRequestIntent>(file_dialog_routed.payload);
    assert(dialog.dialog_id == "project-open");
    assert(dialog.mode == FileDialogMode::OpenFolder);
    assert(dialog.title == "Choose Project");
    assert(dialog.default_path == "/tmp/project");
    assert(dialog.filters.size() == 1U);
    assert(dialog.filters.front().patterns.size() == 2U);

    const IntentMessage viewport_intent =
        make_intent_message(Workflow::Annotate, "viewport.commit",
                            nlohmann::json{
                                {"center_x", 320.5},
                                {"center_y", 180.25},
                                {"zoom", 2.0},
                                {"clip", {{"x", 10}, {"y", 12}, {"width", 640}, {"height", 360}}},
                            });

    const RoutedIntent viewport_routed = route_intent(viewport_intent);
    assert(std::holds_alternative<ViewportCommitIntent>(viewport_routed.payload));
    const auto& viewport = std::get<ViewportCommitIntent>(viewport_routed.payload);
    assert(viewport.center_x == 320.5);
    assert(viewport.center_y == 180.25);
    assert(viewport.zoom == 2.0);
    assert(require_optional(viewport.clip).width == 640U);
}

void test_route_annotate_sidebar_intents() {
    IntentMessage select_intent;
    select_intent.workflow = Workflow::Annotate;
    select_intent.intent = "annotate.sidebar";
    select_intent.payload = nlohmann::json{
        {"action", "select_object"},
        {"object_index", 2},
    };

    const RoutedIntent select_routed = route_intent(select_intent);
    assert(std::holds_alternative<AnnotateSidebarIntent>(select_routed.payload));
    const auto& select = std::get<AnnotateSidebarIntent>(select_routed.payload);
    assert(select.action == "select_object");
    assert(select.object_index == 2U);

    IntentMessage update_intent;
    update_intent.workflow = Workflow::Annotate;
    update_intent.intent = "annotate.sidebar";
    update_intent.payload = nlohmann::json{
        {"action", "update_selected"},
        {"enabled", false},
        {"category_index", 1},
    };

    const RoutedIntent update_routed = route_intent(update_intent);
    assert(std::holds_alternative<AnnotateSidebarIntent>(update_routed.payload));
    const auto& update = std::get<AnnotateSidebarIntent>(update_routed.payload);
    assert(update.action == "update_selected");
    assert(!require_optional(update.enabled));
    assert(update.category_index == 1U);

    IntentMessage assist_intent;
    assist_intent.workflow = Workflow::Annotate;
    assist_intent.intent = "annotate.sidebar";
    assist_intent.payload = nlohmann::json{
        {"action", "assist"},
    };

    const RoutedIntent assist_routed = route_intent(assist_intent);
    assert(std::holds_alternative<AnnotateSidebarIntent>(assist_routed.payload));
    const auto& assist = std::get<AnnotateSidebarIntent>(assist_routed.payload);
    assert(assist.action == "assist");

    IntentMessage spline_intent;
    spline_intent.workflow = Workflow::Annotate;
    spline_intent.intent = "annotate.sidebar";
    spline_intent.payload = nlohmann::json{
        {"action", "spline.set_handle_mode"},
        {"spline_handle_mode", "mirrored"},
    };

    const RoutedIntent spline_routed = route_intent(spline_intent);
    assert(std::holds_alternative<AnnotateSidebarIntent>(spline_routed.payload));
    const auto& spline = std::get<AnnotateSidebarIntent>(spline_routed.payload);
    assert(spline.action == "spline.set_handle_mode");
    assert(spline.spline_handle_mode == "mirrored");

    IntentMessage skeleton_intent;
    skeleton_intent.workflow = Workflow::Annotate;
    skeleton_intent.intent = "annotate.sidebar";
    skeleton_intent.payload = nlohmann::json{
        {"action", "skeleton.update_active_joint"},
        {"skeleton_joint_index", 3},
    };

    const RoutedIntent skeleton_routed = route_intent(skeleton_intent);
    assert(std::holds_alternative<AnnotateSidebarIntent>(skeleton_routed.payload));
    const auto& skeleton = std::get<AnnotateSidebarIntent>(skeleton_routed.payload);
    assert(skeleton.action == "skeleton.update_active_joint");
    assert(skeleton.skeleton_joint_index == 3U);

    IntentMessage cleanup_intent;
    cleanup_intent.workflow = Workflow::Annotate;
    cleanup_intent.intent = "annotate.sidebar";
    cleanup_intent.payload = nlohmann::json{
        {"action", "mask.cleanup"},
        {"cleanup_radius", 6},
        {"cleanup_op", "fill_holes"},
    };

    const RoutedIntent cleanup_routed = route_intent(cleanup_intent);
    assert(std::holds_alternative<AnnotateSidebarIntent>(cleanup_routed.payload));
    const auto& cleanup = std::get<AnnotateSidebarIntent>(cleanup_routed.payload);
    assert(cleanup.action == "mask.cleanup");
    assert(cleanup.cleanup_radius == 6);
    assert(cleanup.cleanup_op == "fill_holes");

    IntentMessage color_range_intent;
    color_range_intent.workflow = Workflow::Annotate;
    color_range_intent.intent = "annotate.sidebar";
    color_range_intent.payload = nlohmann::json{
        {"action", "mask.update_color_ranges"},
        {"sup",
         {{"center", {{"hue_degrees", 120.0}, {"saturation", 0.6}, {"value", 0.5}}},
          {"tolerance", {{"hue_plus_pct", 5.0}}},
          {"sampling", false}}},
        {"nosup",
         {{"center", {{"hue_degrees", 240.0}, {"saturation", 0.2}, {"value", 0.3}}},
          {"tolerance", {{"value_minus_pct", 11.0}}},
          {"sampling", true}}},
    };

    const RoutedIntent color_range_routed = route_intent(color_range_intent);
    assert(std::holds_alternative<AnnotateSidebarIntent>(color_range_routed.payload));
    const auto& color_ranges = std::get<AnnotateSidebarIntent>(color_range_routed.payload);
    assert(color_ranges.action == "mask.update_color_ranges");
    assert(color_ranges.sup.at("center").at("hue_degrees") == 120.0);
    assert(color_ranges.nosup.at("tolerance").at("value_minus_pct") == 11.0);
    assert(color_ranges.nosup.at("sampling") == true);
}

void test_route_annotate_workspace_intents() {
    const IntentMessage click_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.click",
                                                           nlohmann::json{
                                                               {"capture_x", 9},
                                                               {"capture_y", 11},
                                                               {"double_click", true},
                                                           });

    const RoutedIntent click_routed = route_intent(click_intent);
    assert(std::holds_alternative<AnnotateWorkspaceClickIntent>(click_routed.payload));
    assert(routed_intent_name(click_routed.payload) == "annotate.workspace.click");
    const auto& click = std::get<AnnotateWorkspaceClickIntent>(click_routed.payload);
    assert(click.capture_x == 9);
    assert(click.capture_y == 11);
    assert(click.double_click);

    const IntentMessage box_drag_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.box_drag",
                                                              nlohmann::json{
                                                                  {"phase", "begin"},
                                                                  {"drag_kind", "resize-bottom-right"},
                                                                  {"object_index", 7},
                                                                  {"capture_x", 12},
                                                                  {"capture_y", 14},
                                                              });

    const RoutedIntent box_drag_routed = route_intent(box_drag_intent);
    assert(std::holds_alternative<AnnotateWorkspaceBoxDragIntent>(box_drag_routed.payload));
    assert(routed_intent_name(box_drag_routed.payload) == "annotate.workspace.box_drag");
    const auto& box_drag = std::get<AnnotateWorkspaceBoxDragIntent>(box_drag_routed.payload);
    assert_annotation_drag_payload(box_drag, AnnotateWorkspacePhase::Begin, 12, 14);
    assert(box_drag.drag_kind == AnnotateBoxDragKind::ResizeBottomRight);
    assert(box_drag.object_index == 7U);

    const IntentMessage handle_drag_intent =
        make_intent_message(Workflow::Annotate, "annotate.workspace.handle_drag",
                            nlohmann::json{
                                {"phase", "begin"},
                                {"capture_x", 12},
                                {"capture_y", 14},
                                {"handle", {{"object_index", 2}, {"element_index", 1}, {"role", "spline_out_handle"}}},
                            });

    const RoutedIntent handle_drag_routed = route_intent(handle_drag_intent);
    assert(std::holds_alternative<AnnotateWorkspaceHandleDragIntent>(handle_drag_routed.payload));
    const auto& handle_drag = std::get<AnnotateWorkspaceHandleDragIntent>(handle_drag_routed.payload);
    assert_annotation_drag_payload(handle_drag, AnnotateWorkspacePhase::Begin, 12, 14);
    const auto& handle = require_optional(handle_drag.handle);
    assert(handle.object_index == 2U);
    assert(handle.element_index == 1U);
    assert(handle.role == AnnotateHandleRole::SplineOutHandle);

    const IntentMessage brush_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.brush",
                                                           nlohmann::json{
                                                               {"phase", "update"},
                                                               {"capture_x", 18},
                                                               {"capture_y", 7},
                                                               {"radius", 5},
                                                           });

    const RoutedIntent brush_routed = route_intent(brush_intent);
    assert(std::holds_alternative<AnnotateWorkspaceBrushIntent>(brush_routed.payload));
    const auto& brush = std::get<AnnotateWorkspaceBrushIntent>(brush_routed.payload);
    assert(brush.phase == AnnotateWorkspacePhase::Update);
    assert(brush.capture_x == 18);
    assert(brush.capture_y == 7);
    assert(brush.radius == 5);

    const IntentMessage fill_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.fill",
                                                          nlohmann::json{
                                                              {"capture_x", 3},
                                                              {"capture_y", 4},
                                                          });

    const RoutedIntent fill_routed = route_intent(fill_intent);
    assert(std::holds_alternative<AnnotateWorkspaceFillIntent>(fill_routed.payload));
    const auto& fill = std::get<AnnotateWorkspaceFillIntent>(fill_routed.payload);
    assert(fill.capture_x == 3);
    assert(fill.capture_y == 4);

    const IntentMessage color_sample_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.color_sample",
                                                                  nlohmann::json{
                                                                      {"capture_x", 13},
                                                                      {"capture_y", 17},
                                                                  });

    const RoutedIntent color_sample_routed = route_intent(color_sample_intent);
    assert(std::holds_alternative<AnnotateWorkspaceColorSampleIntent>(color_sample_routed.payload));
    assert(routed_intent_name(color_sample_routed.payload) == "annotate.workspace.color_sample");
    const auto& color_sample = std::get<AnnotateWorkspaceColorSampleIntent>(color_sample_routed.payload);
    assert(color_sample.capture_x == 13);
    assert(color_sample.capture_y == 17);
}

void test_route_annotate_and_live_native_shell_control_intents() {
    const IntentMessage annotate_save_now_intent = make_intent_message(Workflow::Annotate, "annotate.save_now");
    const RoutedIntent annotate_save_now_routed = route_intent(annotate_save_now_intent);
    assert(std::holds_alternative<AnnotateSaveIntent>(annotate_save_now_routed.payload));
    assert(routed_intent_name(annotate_save_now_routed.payload) == "annotate.save");

    const IntentMessage predict_stop_intent = make_intent_message(Workflow::Live, "predict.stop");
    const RoutedIntent predict_stop_routed = route_intent(predict_stop_intent);
    assert(std::holds_alternative<PredictStopIntent>(predict_stop_routed.payload));
    assert(routed_intent_name(predict_stop_routed.payload) == "predict.stop");

    const IntentMessage annotate_setup_intent = make_intent_message(Workflow::Annotate, "annotate.setup_frame.next");

    const RoutedIntent annotate_setup_routed = route_intent(annotate_setup_intent);
    assert(std::holds_alternative<AnnotateSetupIntent>(annotate_setup_routed.payload));
    assert(routed_intent_name(annotate_setup_routed.payload) == "annotate.setup");
    const auto& annotate_setup = std::get<AnnotateSetupIntent>(annotate_setup_routed.payload);
    assert(annotate_setup.action == AnnotateSetupAction::NextFrame);

    const IntentMessage annotate_live_start_intent = make_intent_message(Workflow::Annotate, "annotate.live.start");
    const RoutedIntent annotate_live_start_routed = route_intent(annotate_live_start_intent);
    assert(std::holds_alternative<AnnotateSetupIntent>(annotate_live_start_routed.payload));
    assert(std::get<AnnotateSetupIntent>(annotate_live_start_routed.payload).action == AnnotateSetupAction::StartLive);

    const IntentMessage annotate_live_stop_intent = make_intent_message(Workflow::Annotate, "annotate.live.stop");
    const RoutedIntent annotate_live_stop_routed = route_intent(annotate_live_stop_intent);
    assert(std::holds_alternative<AnnotateSetupIntent>(annotate_live_stop_routed.payload));
    assert(std::get<AnnotateSetupIntent>(annotate_live_stop_routed.payload).action == AnnotateSetupAction::StopLive);

    const IntentMessage hold_save_intent = make_intent_message(Workflow::Annotate, "annotate.hold_save",
                                                               nlohmann::json{
                                                                   {"active", true},
                                                               });

    const RoutedIntent hold_save_routed = route_intent(hold_save_intent);
    assert(std::holds_alternative<AnnotateHoldSaveIntent>(hold_save_routed.payload));
    assert(routed_intent_name(hold_save_routed.payload) == "annotate.hold_save");
    const auto& hold_save = std::get<AnnotateHoldSaveIntent>(hold_save_routed.payload);
    assert(hold_save.enabled == true);

    const IntentMessage brush_radius_intent = make_intent_message(Workflow::Annotate, "annotate.brush_radius",
                                                                  nlohmann::json{
                                                                      {"radius", 19},
                                                                  });

    const RoutedIntent brush_radius_routed = route_intent(brush_radius_intent);
    assert(std::holds_alternative<AnnotateBrushRadiusIntent>(brush_radius_routed.payload));
    assert(routed_intent_name(brush_radius_routed.payload) == "annotate.brush_radius");
    const auto& brush_radius = std::get<AnnotateBrushRadiusIntent>(brush_radius_routed.payload);
    assert(brush_radius.radius == 19);

    const IntentMessage fit_to_capture_intent = make_intent_message(Workflow::Live, "live.preview.fit_to_capture",
                                                                    nlohmann::json{
                                                                        {"enabled", false},
                                                                    });

    const RoutedIntent fit_to_capture_routed = route_intent(fit_to_capture_intent);
    assert(std::holds_alternative<LivePreviewControlIntent>(fit_to_capture_routed.payload));
    assert(routed_intent_name(fit_to_capture_routed.payload) == "live.preview.update");
    const auto& fit_to_capture = std::get<LivePreviewControlIntent>(fit_to_capture_routed.payload);
    assert(fit_to_capture.fit_to_capture == false);
    assert(!fit_to_capture.crop_overlay_mode.has_value());

    const IntentMessage live_preview_intent = make_intent_message(Workflow::Live, "live.preview.full_frame_display",
                                                                  nlohmann::json{
                                                                      {"enabled", true},
                                                                  });
    const RoutedIntent live_preview_routed = route_intent(live_preview_intent);
    assert(std::holds_alternative<LivePreviewControlIntent>(live_preview_routed.payload));
    const auto& live_preview = std::get<LivePreviewControlIntent>(live_preview_routed.payload);
    assert(!live_preview.fit_to_capture.has_value());
    assert(live_preview.crop_overlay_mode == true);
}

void test_route_validate_and_export_start_intents() {
    IntentMessage validate_intent;
    validate_intent.workflow = Workflow::Validate;
    validate_intent.intent = "validate.start";
    validate_intent.payload = nlohmann::json{
        {"workflows",
         {{"validate",
           {{"dataset_paths", {{"compiled_path", "/tmp/compiled"}}},
            {"validation", {{"resolution", 960}, {"batch_size", 2}}},
            {"execution", {{"device_id", 1}, {"workers", 4}, {"allow_fp16", true}}}}}}},
    };

    const RoutedIntent validate_routed = route_intent(validate_intent);
    assert(std::holds_alternative<ValidateStartIntent>(validate_routed.payload));
    const auto& validate = std::get<ValidateStartIntent>(validate_routed.payload);
    assert(validate.options.at("workflows").at("validate").at("validation").at("resolution") == 960);

    IntentMessage export_intent;
    export_intent.workflow = Workflow::Export;
    export_intent.intent = "export.start";
    export_intent.payload = nlohmann::json{
        {"workflows",
         {{"export",
           {{"model_artifacts", {{"weights_path", "/tmp/model.pt"}}},
            {"execution", {{"device_id", 2}, {"allow_fp16", false}}},
            {"export", {{"output_path", "/tmp/model.engine"}, {"opset_version", 19}}}}}}},
    };

    const RoutedIntent export_routed = route_intent(export_intent);
    assert(std::holds_alternative<ExportStartIntent>(export_routed.payload));
    const auto& export_start = std::get<ExportStartIntent>(export_routed.payload);
    assert(export_start.options.at("workflows").at("export").at("export").at("output_path") == "/tmp/model.engine");
}

void test_route_train_start_and_stop_intents() {
    IntentMessage train_start_intent;
    train_start_intent.workflow = Workflow::Train;
    train_start_intent.intent = "train.start";
    train_start_intent.payload = nlohmann::json{
        {"selected_preset", "rf-detr-seg-medium"},
        {"workflows",
         {{"train",
           {{"dataset_paths", {{"train_compiled_path", "/tmp/train.bin"}}},
            {"training", {{"output_dir", "/tmp/train-output"}, {"execution_target", 0}}}}}}},
    };

    const RoutedIntent train_start_routed = route_intent(train_start_intent);
    assert(std::holds_alternative<TrainStartIntent>(train_start_routed.payload));
    const auto& train_start = std::get<TrainStartIntent>(train_start_routed.payload);
    assert(train_start.options.at("selected_preset") == "rf-detr-seg-medium");
    assert(train_start.options.at("workflows").at("train").at("training").at("output_dir") == "/tmp/train-output");

    IntentMessage train_stop_intent;
    train_stop_intent.workflow = Workflow::Train;
    train_stop_intent.intent = "train.stop";
    train_stop_intent.payload = nlohmann::json{
        {"force", true},
    };

    const RoutedIntent train_stop_routed = route_intent(train_stop_intent);
    assert(std::holds_alternative<TrainStopIntent>(train_stop_routed.payload));
    const auto& train_stop = std::get<TrainStopIntent>(train_stop_routed.payload);
    assert(train_stop.force);
}

void test_route_train_remote_query_and_offer_intents() {
    IntentMessage query_intent;
    query_intent.workflow = Workflow::Train;
    query_intent.intent = "train.remote.query";
    query_intent.payload = nlohmann::json::object();

    const RoutedIntent query_routed = route_intent(query_intent);
    assert(std::holds_alternative<TrainRemoteQueryIntent>(query_routed.payload));

    IntentMessage arm_intent;
    arm_intent.workflow = Workflow::Train;
    arm_intent.intent = "train.remote.offer.arm";
    arm_intent.payload = nlohmann::json{
        {"offer_id", 4201},
    };

    const RoutedIntent arm_routed = route_intent(arm_intent);
    assert(std::holds_alternative<TrainRemoteOfferArmIntent>(arm_routed.payload));
    const auto& arm = std::get<TrainRemoteOfferArmIntent>(arm_routed.payload);
    assert(arm.offer_id == 4201);

    IntentMessage clear_intent;
    clear_intent.workflow = Workflow::Train;
    clear_intent.intent = "train.remote.offer.clear";
    clear_intent.payload = nlohmann::json::object();

    const RoutedIntent clear_routed = route_intent(clear_intent);
    assert(std::holds_alternative<TrainRemoteOfferClearIntent>(clear_routed.payload));
}

void test_route_train_local_gpu_refresh_intent() {
    IntentMessage refresh_intent;
    refresh_intent.workflow = Workflow::Train;
    refresh_intent.intent = "train.local_gpu.refresh";
    refresh_intent.payload = nlohmann::json::object();

    const RoutedIntent routed = route_intent(refresh_intent);
    assert(std::holds_alternative<TrainLocalGpuRefreshIntent>(routed.payload));
    assert(routed_intent_name(routed.payload) == "train.local_gpu.refresh");
}

void test_file_dialog_binding_resolves_known_browser_dialog_ids() {
    assert(file_dialog_binding_from_id("train.dataset.train_compiled_path") ==
           BrowserFileDialogBinding::TrainTrainCompiledPath);
    assert(file_dialog_binding_from_id("train.dataset.val_compiled_path") ==
           BrowserFileDialogBinding::TrainValCompiledPath);
    assert(file_dialog_binding_from_id("train.dataset.test_compiled_path") ==
           BrowserFileDialogBinding::TrainTestCompiledPath);
    assert(file_dialog_binding_from_id("train.model.weights") == BrowserFileDialogBinding::TrainWeights);
    assert(file_dialog_binding_from_id("train.training.resume_path") == BrowserFileDialogBinding::TrainResumePath);
    assert(file_dialog_binding_from_id("train.training.output_dir") == BrowserFileDialogBinding::TrainOutputDir);
    assert(file_dialog_binding_from_id("predict.source.file") == BrowserFileDialogBinding::PredictSingleImage);
    assert(file_dialog_binding_from_id("predict.output_path") == BrowserFileDialogBinding::PredictOutputPath);
    assert(file_dialog_binding_from_id("annotate.source.folder") == BrowserFileDialogBinding::AnnotateImageFolder);
    assert(file_dialog_binding_from_id("annotate.output_dir") == BrowserFileDialogBinding::AnnotateOutputDir);
    assert(file_dialog_binding_from_id("validate.dataset.compiled_path") ==
           BrowserFileDialogBinding::ValidateCompiledPath);
    assert(file_dialog_binding_from_id("validate.dataset.source_dir") == BrowserFileDialogBinding::ValidateSourceRoot);
    assert(file_dialog_binding_from_id("validate.model.onnx") == BrowserFileDialogBinding::ValidateOnnx);
    assert(file_dialog_binding_from_id("validate.model.tensorrt") == BrowserFileDialogBinding::ValidateTensorRt);
    assert(file_dialog_binding_from_id("validate.output.save_engine_path") ==
           BrowserFileDialogBinding::ValidateSaveEngine);
    assert(file_dialog_binding_from_id("validate.report_json_path") == BrowserFileDialogBinding::ValidateReportJson);
    assert(file_dialog_binding_from_id("export.model.weights") == BrowserFileDialogBinding::ExportWeights);
    assert(file_dialog_binding_from_id("export.model.onnx") == BrowserFileDialogBinding::ExportOnnx);
    assert(file_dialog_binding_from_id("export.output_path") == BrowserFileDialogBinding::ExportOutputPath);
    assert(file_dialog_binding_from_id("unknown.binding") == BrowserFileDialogBinding::Unknown);
}

void test_route_intent_validation_rejects_invalid_workflow_and_payloads() {
    bool annotate_save_threw = false;
    try {
        IntentMessage invalid_workflow;
        invalid_workflow.workflow = Workflow::Predict;
        invalid_workflow.intent = "annotate.save";
        invalid_workflow.payload = nlohmann::json::object();
        (void)route_intent(invalid_workflow);
    } catch (const std::runtime_error&) {
        annotate_save_threw = true;
    }
    assert(annotate_save_threw);

    bool tool_select_threw = false;
    try {
        IntentMessage missing_tool;
        missing_tool.workflow = Workflow::Annotate;
        missing_tool.intent = "tool.select";
        missing_tool.payload = nlohmann::json::object();
        (void)route_intent(missing_tool);
    } catch (const std::runtime_error&) {
        tool_select_threw = true;
    }
    assert(tool_select_threw);

    bool remote_offer_arm_threw = false;
    try {
        IntentMessage missing_offer_id;
        missing_offer_id.workflow = Workflow::Train;
        missing_offer_id.intent = "train.remote.offer.arm";
        missing_offer_id.payload = nlohmann::json::object();
        (void)route_intent(missing_offer_id);
    } catch (const std::runtime_error&) {
        remote_offer_arm_threw = true;
    }
    assert(remote_offer_arm_threw);

    bool protocol_threw = false;
    try {
        IntentMessage invalid_protocol;
        invalid_protocol.protocol_version = kHostApiProtocolVersion + 1U;
        invalid_protocol.workflow = Workflow::Predict;
        invalid_protocol.intent = "predict.start";
        invalid_protocol.payload = nlohmann::json::object();
        (void)route_intent(invalid_protocol);
    } catch (const std::runtime_error&) {
        protocol_threw = true;
    }
    assert(protocol_threw);

    bool annotate_sidebar_threw = false;
    try {
        IntentMessage invalid_annotate_sidebar;
        invalid_annotate_sidebar.workflow = Workflow::Predict;
        invalid_annotate_sidebar.intent = "annotate.sidebar";
        invalid_annotate_sidebar.payload = nlohmann::json{
            {"action", "select_object"},
            {"object_index", 0},
        };
        (void)route_intent(invalid_annotate_sidebar);
    } catch (const std::runtime_error&) {
        annotate_sidebar_threw = true;
    }
    assert(annotate_sidebar_threw);

    bool annotate_handle_mode_threw = false;
    try {
        IntentMessage invalid_annotate_handle_mode;
        invalid_annotate_handle_mode.workflow = Workflow::Annotate;
        invalid_annotate_handle_mode.intent = "annotate.sidebar";
        invalid_annotate_handle_mode.payload = nlohmann::json{
            {"action", "spline.set_handle_mode"},
            {"spline_handle_mode", "bezier"},
        };
        (void)route_intent(invalid_annotate_handle_mode);
    } catch (const std::runtime_error&) {
        annotate_handle_mode_threw = true;
    }
    assert(annotate_handle_mode_threw);

    bool workspace_handle_begin_threw = false;
    try {
        IntentMessage invalid_workspace_handle;
        invalid_workspace_handle.workflow = Workflow::Annotate;
        invalid_workspace_handle.intent = "annotate.workspace.handle_drag";
        invalid_workspace_handle.payload = nlohmann::json{
            {"phase", "begin"},
            {"capture_x", 4},
            {"capture_y", 6},
        };
        (void)route_intent(invalid_workspace_handle);
    } catch (const std::runtime_error&) {
        workspace_handle_begin_threw = true;
    }
    assert(workspace_handle_begin_threw);

    bool workspace_box_drag_begin_threw = false;
    try {
        IntentMessage invalid_workspace_box_drag;
        invalid_workspace_box_drag.workflow = Workflow::Annotate;
        invalid_workspace_box_drag.intent = "annotate.workspace.box_drag";
        invalid_workspace_box_drag.payload = nlohmann::json{
            {"phase", "begin"},
            {"capture_x", 4},
            {"capture_y", 6},
        };
        (void)route_intent(invalid_workspace_box_drag);
    } catch (const std::runtime_error&) {
        workspace_box_drag_begin_threw = true;
    }
    assert(workspace_box_drag_begin_threw);

    bool workspace_box_drag_object_index_threw = false;
    try {
        IntentMessage invalid_workspace_box_drag_object_index;
        invalid_workspace_box_drag_object_index.workflow = Workflow::Annotate;
        invalid_workspace_box_drag_object_index.intent = "annotate.workspace.box_drag";
        invalid_workspace_box_drag_object_index.payload = nlohmann::json{
            {"phase", "begin"},
            {"drag_kind", "move"},
            {"capture_x", 4},
            {"capture_y", 6},
        };
        (void)route_intent(invalid_workspace_box_drag_object_index);
    } catch (const std::runtime_error&) {
        workspace_box_drag_object_index_threw = true;
    }
    assert(workspace_box_drag_object_index_threw);

    bool workspace_brush_radius_threw = false;
    try {
        IntentMessage invalid_workspace_brush;
        invalid_workspace_brush.workflow = Workflow::Annotate;
        invalid_workspace_brush.intent = "annotate.workspace.brush";
        invalid_workspace_brush.payload = nlohmann::json{
            {"phase", "begin"},
            {"capture_x", 4},
            {"capture_y", 6},
            {"radius", 0},
        };
        (void)route_intent(invalid_workspace_brush);
    } catch (const std::runtime_error&) {
        workspace_brush_radius_threw = true;
    }
    assert(workspace_brush_radius_threw);

    bool annotate_setup_missing_action_threw = false;
    try {
        IntentMessage invalid_annotate_setup;
        invalid_annotate_setup.workflow = Workflow::Annotate;
        invalid_annotate_setup.intent = "annotate.setup";
        invalid_annotate_setup.payload = nlohmann::json::object();
        (void)route_intent(invalid_annotate_setup);
    } catch (const std::runtime_error&) {
        annotate_setup_missing_action_threw = true;
    }
    assert(annotate_setup_missing_action_threw);

    bool annotate_live_workflow_threw = false;
    try {
        IntentMessage invalid_annotate_live;
        invalid_annotate_live.workflow = Workflow::Live;
        invalid_annotate_live.intent = "annotate.live.start";
        invalid_annotate_live.payload = nlohmann::json::object();
        (void)route_intent(invalid_annotate_live);
    } catch (const std::runtime_error&) {
        annotate_live_workflow_threw = true;
    }
    assert(annotate_live_workflow_threw);

    bool annotate_hold_save_threw = false;
    try {
        IntentMessage invalid_hold_save;
        invalid_hold_save.workflow = Workflow::Annotate;
        invalid_hold_save.intent = "annotate.hold_save";
        invalid_hold_save.payload = nlohmann::json::object();
        (void)route_intent(invalid_hold_save);
    } catch (const std::runtime_error&) {
        annotate_hold_save_threw = true;
    }
    assert(annotate_hold_save_threw);

    bool annotate_brush_radius_threw = false;
    try {
        IntentMessage invalid_brush_radius;
        invalid_brush_radius.workflow = Workflow::Annotate;
        invalid_brush_radius.intent = "annotate.brush_radius";
        invalid_brush_radius.payload = nlohmann::json{
            {"radius", 0},
        };
        (void)route_intent(invalid_brush_radius);
    } catch (const std::runtime_error&) {
        annotate_brush_radius_threw = true;
    }
    assert(annotate_brush_radius_threw);

    bool live_preview_enabled_threw = false;
    try {
        IntentMessage invalid_live_preview;
        invalid_live_preview.workflow = Workflow::Live;
        invalid_live_preview.intent = "live.preview.fit_to_capture";
        invalid_live_preview.payload = nlohmann::json::object();
        (void)route_intent(invalid_live_preview);
    } catch (const std::runtime_error&) {
        live_preview_enabled_threw = true;
    }
    assert(live_preview_enabled_threw);

    bool live_preview_conflict_threw = false;
    try {
        IntentMessage invalid_live_preview_conflict;
        invalid_live_preview_conflict.workflow = Workflow::Live;
        invalid_live_preview_conflict.intent = "live.preview.update";
        invalid_live_preview_conflict.payload = nlohmann::json{
            {"crop_overlay_mode", false},
            {"full_frame", true},
        };
        (void)route_intent(invalid_live_preview_conflict);
    } catch (const std::runtime_error&) {
        live_preview_conflict_threw = true;
    }
    assert(live_preview_conflict_threw);
}

void test_gui_source_selection_maps_to_browser_source_metadata() {
    mmltk::gui::SourceSelectionState source;
    source.kind = mmltk::gui::SourceKind::ImageFolder;
    source.image_directory = "/tmp/images";
    source.recursive = true;
    source.capture_width = 1280;
    source.capture_height = 720;
    source.capture_fps = 60;
    source.v4l2_buffer_count = 6;
    source.crop_x = 12;
    source.crop_y = 24;
    source.crop_width = 640;
    source.crop_height = 360;

    const SourceMetadata metadata = source_metadata_from_selection(source);
    assert(metadata.kind == mmltk::browser::SourceKind::ImageFolder);
    assert(metadata.locator == "/tmp/images");
    assert(metadata.recursive);
    assert(metadata.capture_width == 1280);
    assert(metadata.capture_height == 720);
    assert(metadata.capture_fps == 60);
    assert(metadata.v4l2_buffer_count == 6);
    assert(metadata.has_crop);
    assert(metadata.crop.x == 12U);
    assert(metadata.crop.height == 360U);
}

void test_manual_overlay_snapshot_maps_to_annotation_document_state() {
    mmltk::live::ManualOverlayDocumentSnapshot snapshot;
    snapshot.document_generation = 19U;
    snapshot.session_revision = 23U;
    snapshot.capture_width = 800U;
    snapshot.capture_height = 450U;
    snapshot.selected_instance = 2U;
    snapshot.instances.resize(4U);

    const AnnotationDocumentState state = annotation_document_state_from_snapshot(snapshot);
    assert(state.document_generation == 19U);
    assert(state.session_revision == 23U);
    assert(state.capture_width == 800U);
    assert(state.capture_height == 450U);
    assert(state.instance_count == 4U);
    assert(state.selected_instance == 2U);
}

void test_annotation_editor_state_maps_to_browser_annotation_state() {
    mmltk::gui::AnnotationFrame frame;
    frame.width = 640U;
    frame.height = 360U;
    frame.capture_width = 1280U;
    frame.capture_height = 720U;

    mmltk::gui::AnnotationDocument document;
    document.set_objects({make_test_annotation_object()});

    mmltk::gui::AnnotationSession session;
    session.select_object(0U);

    const std::optional<AnnotationDocumentState> state =
        annotation_document_state_from_editor(&frame, &document, &session);
    const auto& state_value = require_optional(state);
    assert(state_value.document_generation == document.generation());
    assert(state_value.session_revision == session.overlay_revision());
    assert(state_value.capture_width == 1280U);
    assert(state_value.capture_height == 720U);
    assert(state_value.instance_count == 1U);
    assert(state_value.selected_instance == 0U);
}

void test_train_local_gpu_state_to_json_surfaces_inventory_and_selection() {
    const std::vector<mmltk::gui::LocalGpuInfo> gpus = {
        mmltk::gui::LocalGpuInfo{0, "RTX 4090", 24ULL * 1024ULL * 1024ULL * 1024ULL},
        mmltk::gui::LocalGpuInfo{2, "RTX 6000 Ada", 48ULL * 1024ULL * 1024ULL * 1024ULL},
    };
    const std::vector<bool> selected = {true, false};
    const std::vector<int> configured_device_ids = {0, 2};

    const nlohmann::json state = train_local_gpu_state_to_json(gpus, selected, configured_device_ids, "", true);
    assert(state.at("refresh_running") == true);
    assert(state.at("refresh_action_label") == "Refreshing GPUs...");
    assert(state.at("selection_summary") == "1 / 2 GPUs selected");
    assert(state.at("configured_device_ids").at(0) == 0);
    assert(state.at("configured_device_ids").at(1) == 2);
    assert(state.at("selected_device_ids").size() == 1U);
    assert(state.at("selected_device_ids").at(0) == 0);
    assert(state.at("devices").size() == 2U);
    assert(state.at("devices").at(0).at("label") == "GPU 0 · RTX 4090 · 24.0 GiB");
    assert(state.at("devices").at(1).at("selected") == false);
    assert(state.at("banner").is_null());
}

void test_train_local_gpu_state_to_json_surfaces_error_and_selection_banners() {
    const std::vector<mmltk::gui::LocalGpuInfo> gpus = {
        mmltk::gui::LocalGpuInfo{1, "A100", 40ULL * 1024ULL * 1024ULL * 1024ULL},
    };
    const std::vector<int> configured_device_ids = {1};

    const nlohmann::json selection_required =
        train_local_gpu_state_to_json(gpus, std::vector<bool>{false}, configured_device_ids, "", false);
    assert(selection_required.at("has_selection") == false);
    assert(selection_required.at("banner").at("kind") == "selection_required");
    assert(selection_required.at("banner").at("message") == "Select at least one local GPU before running training.");

    const nlohmann::json error_state =
        train_local_gpu_state_to_json(std::span<const mmltk::gui::LocalGpuInfo>{}, std::vector<bool>{},
                                      configured_device_ids, "cuda enumeration failed", false);
    assert(error_state.at("error") == "cuda enumeration failed");
    assert(error_state.at("banner").at("kind") == "error");
    assert(error_state.at("banner").at("message") == "cuda enumeration failed");
}

void test_live_runtime_state_to_json_surfaces_preview_and_pipeline_status() {
    BrowserLiveRuntimeState state;
    state.active_mode = "predict";
    state.starting = false;
    state.stopping = true;
    state.show_running_section = true;
    state.show_static_preview = false;
    state.show_idle_start_error = false;
    state.video_stream_source = true;
    set_available_runtime_capabilities(state.runtime_capabilities);
    state.preview.initialized = false;
    state.preview.has_frame = true;
    state.preview.displayed_region = CaptureRegion{4U, 8U, 640U, 360U};
    state.preview.frame_id = 77U;
    state.preview.live_frame_id = mmltk::live::LiveFrameId{10U, 11U};
    state.preview.fit_to_capture = true;
    state.preview.crop_overlay_mode = false;
    state.preview.static_source_name.clear();
    state.preview.error.clear();
    state.preview.interop_failed = false;
    state.controller.present = true;
    state.controller.running = true;
    state.controller.last_error = "controller warning";
    state.analyzer.attached = true;
    state.analyzer.model_hot = true;
    state.analyzer.running = true;
    state.analyzer.frames_analyzed = 120U;
    state.analyzer.frames_skipped = 7U;
    state.analyzer.last_latency_ms = 3.25;
    state.analyzer.backend_name = "tensorrt";
    state.analyzer.last_error = "analyzer warning";
    state.manual_overlay.running = false;
    state.manual_overlay.generations_rendered = 4U;
    state.manual_overlay.last_generation = 9U;
    state.manual_overlay.last_error = "manual warning";
    state.compositor.running = true;
    state.compositor.frames_composited = 118U;
    state.compositor.frames_dropped = 2U;
    state.compositor.last_frame_id = mmltk::live::LiveFrameId{10U, 12U};
    state.compositor.manual_overlay_active = true;
    state.compositor.analysis_overlay_active = false;
    state.compositor.last_error = "compositor warning";
    state.start_error = "start failed";
    state.action_error = "stop failed";

    const nlohmann::json runtime = live_runtime_state_to_json(state);
    assert(runtime.at("active_mode") == "predict");
    assert(runtime.at("stopping") == true);
    assert(runtime.at("runtime_capabilities").at("host_backend") == "cef");
    assert(runtime.at("runtime_capabilities").at("navigator_gpu") == "available");
    assert(runtime.at("runtime_capabilities").at("workspace_surface_bridge") == "available");
    assert(runtime.at("runtime_capabilities").at("workspace_surface_zero_copy") == "available");
    assert(runtime.at("runtime_capabilities").size() == 4U);
    assert(runtime.at("preview").at("initialized") == false);
    assert(runtime.at("preview").at("has_frame") == true);
    assert(runtime.at("preview").at("frame_id") == 77U);
    assert(runtime.at("preview").at("displayed_region").at("width") == 640U);
    assert(runtime.at("preview").at("displayed_region").at("x") == 4U);
    assert(runtime.at("preview").at("live_frame_id").at("sequence") == 11U);
    assert(runtime.at("preview").at("interop_failed") == false);
    assert(runtime.at("controller").at("present") == true);
    assert(runtime.at("analyzer").at("backend_name") == "tensorrt");
    assert(runtime.at("analyzer").at("frames_analyzed") == 120U);
    assert(runtime.at("manual_overlay").at("last_generation") == 9U);
    assert(runtime.at("compositor").at("frames_dropped") == 2U);
    assert(runtime.at("compositor").at("last_frame_id").at("sequence") == 12U);
    assert(runtime.at("start_error") == "start failed");
    assert(runtime.at("action_error") == "stop failed");
}

void test_apply_routed_settings_update_updates_bound_gui_state() {
    BrowserHostSettingsFixture fixture;
    auto& gui = fixture.gui;
    gui.ui.ui_scale = 1.0F;
    gui.predict.threshold = 0.25F;
    fixture.refresh_snapshot();

    const IntentMessage intent =
        make_intent_message(Workflow::Predict, "settings.update",
                            nlohmann::json{
                                {"current_view", "annotate"},
                                {"selected_preset", "rf-detr-seg-medium"},
                                {"ui", {{"ui_scale", 1.5}}},
                                {"workflows", {{"predict", {{"predict", {{"threshold", 0.4}}}}}}},
                            });

    apply_fixture_routed_intent(fixture, intent, true);

    assert(gui.current_view == mmltk::gui::View::Annotate);
    assert(gui.selected_preset == "rf-detr-seg-medium");
    assert(gui.ui.ui_scale == 1.5F);
    assert(gui.predict.threshold == 0.4F);
}

void test_apply_routed_crop_commit_updates_workflow_source_selection() {
    BrowserHostSettingsFixture fixture;
    auto& gui = fixture.gui;
    gui.predict.source.capture_width = 1920;
    gui.predict.source.capture_height = 1080;
    gui.annotate.source.capture_width = 1280;
    gui.annotate.source.capture_height = 720;
    fixture.refresh_snapshot();

    const IntentMessage predict_crop =
        make_intent_message(Workflow::Live, "crop.commit",
                            nlohmann::json{
                                {"has_crop", true},
                                {"crop", {{"x", 32}, {"y", 48}, {"width", 800}, {"height", 600}}},
                            });

    apply_fixture_routed_intent(fixture, predict_crop);
    assert(gui.predict.source.crop_x == 32);
    assert(gui.predict.source.crop_y == 48);
    assert(gui.predict.source.crop_width == 800);
    assert(gui.predict.source.crop_height == 600);

    const IntentMessage annotate_crop =
        make_intent_message(Workflow::Annotate, "crop.commit", nlohmann::json{{"has_crop", false}});

    gui.annotate.source.crop_x = 9;
    gui.annotate.source.crop_y = 7;
    gui.annotate.source.crop_width = 320;
    gui.annotate.source.crop_height = 200;
    apply_fixture_routed_intent(fixture, annotate_crop);
    assert(gui.annotate.source.crop_x == 0);
    assert(gui.annotate.source.crop_y == 0);
    assert(gui.annotate.source.crop_width == 0);
    assert(gui.annotate.source.crop_height == 0);
}

void test_apply_routed_tool_select_updates_annotation_session() {
    mmltk::gui::AnnotationController controller;
    mmltk::gui::AnnotationDocument document;
    document.set_objects({make_test_annotation_object()});
    mmltk::gui::AnnotationSession session;

    mmltk::gui::View current_view = mmltk::gui::View::Predict;
    IntentMessage intent;
    intent.workflow = Workflow::Annotate;
    intent.intent = "tool.select";
    intent.payload = nlohmann::json{{"tool", "mask.paint"}};

    apply_annotation_routed_intent(intent, document, session, controller, nullptr, nullptr, &current_view);

    assert(current_view == mmltk::gui::View::Annotate);
    assert(session.active_tool() == mmltk::gui::AnnotationToolKind::MaskPaint);
}

void test_apply_routed_annotate_workspace_click_routes_canvas_click_to_active_tool() {
    mmltk::gui::AnnotationController controller;
    mmltk::gui::AnnotationDocument document;
    mmltk::gui::AnnotationSession session;
    mmltk::gui::AnnotationFrame frame = make_test_annotation_frame();
    mmltk::gui::AnnotationCategories categories = make_test_annotation_categories();
    const bool activated = controller.set_active_tool(mmltk::gui::AnnotationToolKind::Point, document, session);
    assert(activated);

    mmltk::gui::View current_view = mmltk::gui::View::Predict;
    const IntentMessage intent = make_intent_message(Workflow::Annotate, "annotate.workspace.click",
                                                     nlohmann::json{
                                                         {"capture_x", 10},
                                                         {"capture_y", 6},
                                                     });

    apply_annotation_routed_intent(intent, document, session, controller, &frame, &categories, &current_view);

    assert(current_view == mmltk::gui::View::Annotate);
    assert(document.size() == 1U);
    assert(session.selected_object_index() == std::optional<std::size_t>{0U});
    const auto* point = std::get_if<mmltk::gui::AnnotationPointShape>(&document.object(0U)->shape);
    assert(point != nullptr);
    assert(point->point.x == 10.0F);
    assert(point->point.y == 6.0F);
}

void test_apply_routed_annotate_workspace_box_drag_creates_box_object() {
    mmltk::gui::AnnotationController controller;
    mmltk::gui::AnnotationDocument document;
    mmltk::gui::AnnotationSession session;
    mmltk::gui::AnnotationFrame frame = make_test_annotation_frame();
    mmltk::gui::AnnotationCategories categories;
    const bool activated = controller.set_active_tool(mmltk::gui::AnnotationToolKind::Box, document, session);
    assert(activated);

    const IntentMessage begin_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.box_drag",
                                                           nlohmann::json{
                                                               {"phase", "begin"},
                                                               {"drag_kind", "create"},
                                                               {"capture_x", 3},
                                                               {"capture_y", 4},
                                                           });
    apply_annotation_routed_intent(begin_intent, document, session, controller, &frame, &categories);
    assert(session.create_drag_session().active);
    assert(session.pointer_captured());

    const IntentMessage update_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.box_drag",
                                                            nlohmann::json{
                                                                {"phase", "update"},
                                                                {"capture_x", 11},
                                                                {"capture_y", 15},
                                                            });
    apply_annotation_routed_intent(update_intent, document, session, controller, &frame, &categories);

    const IntentMessage end_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.box_drag",
                                                         nlohmann::json{
                                                             {"phase", "end"},
                                                             {"capture_x", 11},
                                                             {"capture_y", 15},
                                                         });
    apply_annotation_routed_intent(end_intent, document, session, controller, &frame, &categories);

    assert(document.size() == 1U);
    assert(!categories.items.empty());
    assert(session.selected_object_index() == std::optional<std::size_t>{0U});
    assert(!session.create_drag_session().active);
    assert(!session.pointer_captured());
    const auto* box = std::get_if<mmltk::gui::AnnotationBoxShape>(&document.object(0U)->shape);
    assert(box != nullptr);
    assert(box->box.x1 == 3);
    assert(box->box.y1 == 4);
    assert(box->box.x2 == 12);
    assert(box->box.y2 == 16);
}

void test_apply_routed_annotate_workspace_box_drag_moves_selected_box() {
    DirectAnnotationFixture fixture;

    const IntentMessage begin_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.box_drag",
                                                           nlohmann::json{
                                                               {"phase", "begin"},
                                                               {"drag_kind", "move"},
                                                               {"object_index", 0},
                                                               {"capture_x", 6},
                                                               {"capture_y", 7},
                                                           });
    apply_annotation_routed_intent(begin_intent, fixture.document, fixture.session, fixture.controller, &fixture.frame,
                                   &fixture.categories);
    assert(fixture.session.direct_drag_session().active);
    assert(fixture.session.direct_drag_index() == std::optional<std::size_t>{0U});
    assert(fixture.session.pointer_captured());

    const IntentMessage update_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.box_drag",
                                                            nlohmann::json{
                                                                {"phase", "update"},
                                                                {"capture_x", 8},
                                                                {"capture_y", 9},
                                                            });
    apply_annotation_routed_intent(update_intent, fixture.document, fixture.session, fixture.controller, &fixture.frame,
                                   &fixture.categories);

    const IntentMessage end_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.box_drag",
                                                         nlohmann::json{
                                                             {"phase", "end"},
                                                             {"capture_x", 8},
                                                             {"capture_y", 9},
                                                         });
    apply_annotation_routed_intent(end_intent, fixture.document, fixture.session, fixture.controller, &fixture.frame,
                                   &fixture.categories);

    const auto* box = std::get_if<mmltk::gui::AnnotationBoxShape>(&fixture.document.object(0U)->shape);
    assert(box != nullptr);
    assert(box->box.x1 == 6);
    assert(box->box.y1 == 7);
    assert(box->box.x2 == 14);
    assert(box->box.y2 == 20);
    assert(!fixture.session.direct_drag_session().active);
    assert(!fixture.session.direct_drag_index().has_value());
    assert(!fixture.session.pointer_captured());
}

void test_apply_routed_annotate_workspace_box_drag_resizes_selected_box() {
    DirectAnnotationFixture fixture;

    const IntentMessage begin_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.box_drag",
                                                           nlohmann::json{
                                                               {"phase", "begin"},
                                                               {"drag_kind", "resize_bottom_right"},
                                                               {"object_index", 0},
                                                               {"capture_x", 12},
                                                               {"capture_y", 18},
                                                           });
    apply_annotation_routed_intent(begin_intent, fixture.document, fixture.session, fixture.controller, &fixture.frame,
                                   &fixture.categories);

    const IntentMessage end_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.box_drag",
                                                         nlohmann::json{
                                                             {"phase", "end"},
                                                             {"capture_x", 16},
                                                             {"capture_y", 20},
                                                         });
    apply_annotation_routed_intent(end_intent, fixture.document, fixture.session, fixture.controller, &fixture.frame,
                                   &fixture.categories);

    const auto* box = std::get_if<mmltk::gui::AnnotationBoxShape>(&fixture.document.object(0U)->shape);
    assert(box != nullptr);
    assert(box->box.x1 == 4);
    assert(box->box.y1 == 5);
    assert(box->box.x2 == 16);
    assert(box->box.y2 == 20);
    assert(!fixture.session.direct_drag_session().active);
    assert(!fixture.session.direct_drag_index().has_value());
}

void test_apply_routed_annotate_workspace_color_sample_updates_selected_ranges() {
    mmltk::gui::AnnotationController controller;
    mmltk::gui::AnnotationDocument document;
    mmltk::gui::AnnotationObject object = make_test_mask_object();
    object.sup.sampling = true;
    object.nosup.center = mmltk::gui::AnnotationHsv{210.0F, 0.2F, 0.3F};
    document.set_objects({object});
    mmltk::gui::AnnotationSession session;
    session.select_object(0U);
    mmltk::gui::AnnotationFrame frame = make_test_annotation_frame(4U, 4U);
    frame.capture_width = 8U;
    frame.capture_height = 8U;
    frame.view_x = 2U;
    frame.view_y = 1U;
    frame.pixels_bgr.clear();
    mmltk::gui::AnnotationCategories categories = make_test_annotation_categories();
    bool ensure_called = false;

    const IntentMessage intent = make_intent_message(Workflow::Annotate, "annotate.workspace.color_sample",
                                                     nlohmann::json{
                                                         {"capture_x", 3},
                                                         {"capture_y", 3},
                                                     });
    apply_annotation_routed_intent(
        intent, document, session, controller, &frame, &categories, nullptr, [&](std::string*) {
            ensure_called = true;
            const auto pixel_count = static_cast<std::vector<std::uint8_t>::size_type>(4U) * 4U * 3U;
            frame.pixels_bgr.assign(pixel_count, std::uint8_t{0});
            const std::size_t sample_offset = (static_cast<std::size_t>(2U) * 4U + 1U) * 3U;
            frame.pixels_bgr[sample_offset + 0U] = 32U;
            frame.pixels_bgr[sample_offset + 1U] = 96U;
            frame.pixels_bgr[sample_offset + 2U] = 224U;
            return true;
        });

    assert(ensure_called);
    const mmltk::gui::AnnotationHsv expected = mmltk::gui::annotation_bgr_to_hsv(32U, 96U, 224U);
    const auto* updated = document.object(0U);
    assert(updated != nullptr);
    assert(updated->sup.center.hue_degrees == expected.hue_degrees);
    assert(updated->sup.center.saturation == expected.saturation);
    assert(updated->sup.center.value == expected.value);
    assert(!updated->sup.sampling);
    assert(!updated->nosup.sampling);
    assert(updated->nosup.center.hue_degrees == 210.0F);
}

void test_apply_routed_annotate_workspace_handle_drag_moves_point_handle() {
    mmltk::gui::AnnotationController controller;
    mmltk::gui::AnnotationDocument document;
    document.set_objects({make_test_point_object(4.0F, 5.0F)});
    mmltk::gui::AnnotationSession session;
    session.select_object(0U);
    const bool activated = controller.set_active_tool(mmltk::gui::AnnotationToolKind::Direct, document, session);
    assert(activated);

    mmltk::gui::AnnotationFrame frame = make_test_annotation_frame();
    mmltk::gui::AnnotationCategories categories = make_test_annotation_categories();
    mmltk::gui::View current_view = mmltk::gui::View::Predict;

    const IntentMessage begin_intent =
        make_intent_message(Workflow::Annotate, "annotate.workspace.handle_drag",
                            nlohmann::json{
                                {"phase", "begin"},
                                {"capture_x", 4},
                                {"capture_y", 5},
                                {"handle", {{"object_index", 0}, {"element_index", 0}, {"role", "point"}}},
                            });
    apply_annotation_routed_intent(begin_intent, document, session, controller, &frame, &categories, &current_view);
    assert(document.transaction_active());
    assert(session.handle_drag().has_value());
    assert(session.pointer_captured());

    const IntentMessage update_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.handle_drag",
                                                            nlohmann::json{
                                                                {"phase", "update"},
                                                                {"capture_x", 12},
                                                                {"capture_y", 14},
                                                            });
    apply_annotation_routed_intent(update_intent, document, session, controller, &frame, &categories, &current_view);

    const IntentMessage end_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.handle_drag",
                                                         nlohmann::json{
                                                             {"phase", "end"},
                                                             {"capture_x", 16},
                                                             {"capture_y", 18},
                                                         });
    apply_annotation_routed_intent(end_intent, document, session, controller, &frame, &categories, &current_view);

    const auto* point = std::get_if<mmltk::gui::AnnotationPointShape>(&document.object(0U)->shape);
    assert(point != nullptr);
    assert(point->point.x == 16.0F);
    assert(point->point.y == 18.0F);
    assert(!document.transaction_active());
    assert(!session.handle_drag().has_value());
    assert(!session.pointer_captured());
}

void test_apply_routed_annotate_workspace_brush_tracks_stroke_and_updates_mask() {
    mmltk::gui::AnnotationController controller;
    mmltk::gui::AnnotationDocument document;
    document.set_objects({make_test_mask_object()});
    mmltk::gui::AnnotationSession session;
    session.select_object(0U);
    const bool activated = controller.set_active_tool(mmltk::gui::AnnotationToolKind::MaskPaint, document, session);
    assert(activated);

    mmltk::gui::AnnotationFrame frame = make_test_annotation_frame();
    mmltk::gui::AnnotationCategories categories = make_test_annotation_categories();

    const IntentMessage begin_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.brush",
                                                           nlohmann::json{
                                                               {"phase", "begin"},
                                                               {"capture_x", 1},
                                                               {"capture_y", 1},
                                                               {"radius", 2},
                                                           });
    apply_annotation_routed_intent(begin_intent, document, session, controller, &frame, &categories);
    assert(document.transaction_active());
    assert(session.paint_stroke().active);
    assert(session.pointer_captured());
    assert(session.brush_preview().visible);

    const IntentMessage update_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.brush",
                                                            nlohmann::json{
                                                                {"phase", "update"},
                                                                {"capture_x", 5},
                                                                {"capture_y", 1},
                                                                {"radius", 2},
                                                            });
    apply_annotation_routed_intent(update_intent, document, session, controller, &frame, &categories);

    const IntentMessage end_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.brush",
                                                         nlohmann::json{
                                                             {"phase", "end"},
                                                             {"capture_x", 5},
                                                             {"capture_y", 4},
                                                             {"radius", 2},
                                                         });
    apply_annotation_routed_intent(end_intent, document, session, controller, &frame, &categories);

    const auto* mask = std::get_if<mmltk::gui::AnnotationMaskShape>(&document.object(0U)->shape);
    assert(mask != nullptr);
    assert(std::any_of(mask->mask.begin(), mask->mask.end(), [](const std::uint8_t value) { return value != 0U; }));
    assert(!document.transaction_active());
    assert(!session.paint_stroke().active);
    assert(!session.pointer_captured());
    assert(!session.brush_preview().visible);
}

void test_apply_routed_annotate_workspace_fill_updates_mask_object() {
    mmltk::gui::AnnotationController controller;
    mmltk::gui::AnnotationDocument document;
    document.set_objects({make_test_mask_object()});
    mmltk::gui::AnnotationSession session;
    session.select_object(0U);
    const bool activated = controller.set_active_tool(mmltk::gui::AnnotationToolKind::MaskFill, document, session);
    assert(activated);

    mmltk::gui::AnnotationFrame frame = make_test_annotation_frame();
    mmltk::gui::AnnotationCategories categories = make_test_annotation_categories();

    const IntentMessage fill_intent = make_intent_message(Workflow::Annotate, "annotate.workspace.fill",
                                                          nlohmann::json{
                                                              {"capture_x", 3},
                                                              {"capture_y", 3},
                                                          });
    apply_annotation_routed_intent(fill_intent, document, session, controller, &frame, &categories);

    const auto* mask = std::get_if<mmltk::gui::AnnotationMaskShape>(&document.object(0U)->shape);
    assert(mask != nullptr);
    assert(mask->mask.size() == 64U);
    assert(std::all_of(mask->mask.begin(), mask->mask.end(), [](const std::uint8_t value) { return value == 1U; }));
}

void test_live_output_bundle_maps_to_direct_bridge_frame_slot_descriptor() {
    mmltk::live::OutputBundle bundle;
    bundle.slot_index = 3U;
    bundle.frame_id = mmltk::live::LiveFrameId{100U, 101U};
    bundle.dims = mmltk::live::OutputDimensions{640U, 360U, 2048U};
    bundle.region = mmltk::live::LiveCaptureRegion{8U, 16U, 640U, 360U};
    bundle.ready_ns = 999U;
    bundle.short_frame = true;

    const FrameSlotDescriptor descriptor = frame_slot_from_output_bundle(
        "workspace", FrameTransportKind::CudaDeviceBuffer, FramePixelFormat::Bgr8, bundle);
    assert(descriptor.slot_name == "workspace");
    assert(descriptor.transport == FrameTransportKind::CudaDeviceBuffer);
    assert(descriptor.pixel_format == FramePixelFormat::Bgr8);
    assert(descriptor.slot_index == 3U);
    assert(descriptor.frame_id.session_nonce == 100U);
    assert(descriptor.capture_region.x == 8U);
    assert(descriptor.width == 640U);
    assert(descriptor.height == 360U);
    assert(descriptor.row_stride_bytes == 2048U);
    assert(descriptor.byte_length == frame_byte_length(2048U, 360U));
    assert(descriptor.ready_ns == 999U);
    assert(descriptor.short_frame);
    assert(require_optional(descriptor.linux_import).image.handle.empty());
    assert(require_optional(descriptor.linux_import).image.handle_type.empty());
    assert(require_optional(descriptor.linux_import).release_token.empty());
    assert_contract_release_native_import_metadata(descriptor);
    assert(descriptor.lifecycle == FrameSlotLifecycle::ExplicitRelease);
    assert(descriptor.ownership == FrameSlotOwnership::NativeHost);
    assert(descriptor.ready_sync.kind == FrameSlotSyncKind::TimelinePoint);
    assert(descriptor.ready_sync.handle.empty());
    assert(descriptor.ready_sync.value == 999U);
    assert(!frame_slot_supports_imported_acquire(descriptor));
}

void test_workspace_output_bundle_reissues_named_browser_frame_slot() {
    mmltk::live::WorkspaceOutputBundle output_bundle;
    output_bundle.slot_index = 4U;
    output_bundle.frame_id = mmltk::live::LiveFrameId{7U, 8U};
    output_bundle.dims = mmltk::live::WorkspaceDimensions{640U, 360U, 2560U};
    output_bundle.region = mmltk::live::LiveCaptureRegion{16U, 24U, 640U, 360U};
    output_bundle.ready_ns = 22U;
    output_bundle.short_frame = true;

    const FrameSlotDescriptor descriptor = frame_slot_from_workspace_output_bundle(
        "workspace", FrameTransportKind::CudaDeviceBuffer, FramePixelFormat::Rgba8, output_bundle);
    assert(descriptor.slot_name == "workspace");
    assert(descriptor.transport == FrameTransportKind::CudaDeviceBuffer);
    assert(descriptor.pixel_format == FramePixelFormat::Rgba8);
    assert(descriptor.frame_id.sequence == 8U);
    assert(descriptor.capture_region.x == 16U);
    assert(descriptor.row_stride_bytes == 2560U);
    assert(descriptor.short_frame);
    assert(require_optional(descriptor.linux_import).image.handle.empty());
    assert(require_optional(descriptor.linux_import).release_token.empty());
    assert_contract_release_native_import_metadata(descriptor, "rgba8unorm");
    assert(descriptor.ready_sync.kind == FrameSlotSyncKind::TimelinePoint);
    assert(descriptor.ready_sync.handle.empty());
    assert(descriptor.ready_sync.value == 22U);
    assert(!frame_slot_supports_imported_acquire(descriptor));

    mmltk::live::WorkspaceOutputBundle next_output_bundle = output_bundle;
    next_output_bundle.frame_id = mmltk::live::LiveFrameId{7U, 9U};
    next_output_bundle.ready_ns = 23U;
    const FrameSlotDescriptor next_workspace_descriptor = frame_slot_from_workspace_output_bundle(
        "workspace", FrameTransportKind::CudaDeviceBuffer, FramePixelFormat::Rgba8, next_output_bundle);
    assert(next_workspace_descriptor.pixel_format == FramePixelFormat::Rgba8);
    assert(require_optional(next_workspace_descriptor.linux_import).image.handle.empty());
    assert(require_optional(next_workspace_descriptor.linux_import).release_token.empty());
    assert_contract_release_native_import_metadata(next_workspace_descriptor, "rgba8unorm");
    assert(next_workspace_descriptor.ready_sync.handle.empty());
    assert(next_workspace_descriptor.ready_sync.value == 23U);
    assert(next_workspace_descriptor.frame_id.sequence != descriptor.frame_id.sequence);
    assert(next_workspace_descriptor.ready_sync.value != descriptor.ready_sync.value);
}

void test_cuda_frame_slot_round_trips_import_contract_through_json() {
    FrameSlotDescriptor descriptor =
        make_direct_bridge_frame_slot("workspace", 3U, mmltk::live::LiveFrameId{100U, 101U},
                                      CaptureRegion{8U, 16U, 640U, 360U}, 640U, 360U, 2048U, 999U);

    const nlohmann::json json = descriptor;
    assert(json.at("linux_import").at("image").at("kind") == "opaque_token");
    assert(json.at("linux_import").at("image").at("handle") == "browser-frame:workspace:3:100:101:image");
    assert_transport_message_native_import_metadata_json(json);

    const FrameSlotDescriptor round_trip = json.get<FrameSlotDescriptor>();
    assert_linux_import_slot_contract(round_trip, "browser-frame:workspace:3:100:101:image",
                                      "browser-frame:workspace:3:100:101:ready",
                                      "browser-frame:workspace:3:100:101:release");
    assert_transport_message_native_import_metadata(round_trip);
    assert(round_trip.ready_sync.kind == FrameSlotSyncKind::TimelinePoint);
    assert(round_trip.ready_sync.handle == "browser-frame:workspace:3:100:101:ready");
    assert(round_trip.ready_sync.value == 999U);
    assert(frame_slot_supports_imported_acquire(round_trip));
}

void test_structured_linux_import_contract_controls_descriptor_ready_sync() {
    FrameSlotDescriptor descriptor = make_direct_bridge_frame_slot(
        "workspace", 3U, mmltk::live::LiveFrameId{100U, 101U}, CaptureRegion{8U, 16U, 640U, 360U}, 640U, 360U, 2048U,
        999U, FrameSlotLifecycle::ExplicitRelease);
    descriptor.linux_import = LinuxImportableFrameSlotContract{
        LinuxImportableImageHandle{
            LinuxImportableImageHandleKind::OpaqueToken,
            std::string(kLinuxImportableImageHandleTypeNativeToken),
            "browser-frame:workspace:structured:image",
        },
        FrameSlotReadySync{
            FrameSlotSyncKind::TimelinePoint,
            "browser-frame:workspace:structured:ready",
            999U,
        },
        "browser-frame:workspace:structured:release",
    };
    descriptor.ready_sync = FrameSlotReadySync{
        FrameSlotSyncKind::BinaryFence,
        "browser-frame:workspace:stale:ready",
        123U,
    };

    normalize_linux_importable_frame_slot_contract(descriptor);

    assert(require_optional(descriptor.linux_import).image.handle == "browser-frame:workspace:structured:image");
    assert(require_optional(descriptor.linux_import).release_token == "browser-frame:workspace:structured:release");
    assert(descriptor.ready_sync.kind == FrameSlotSyncKind::TimelinePoint);
    assert(descriptor.ready_sync.handle == "browser-frame:workspace:structured:ready");
    assert(descriptor.ready_sync.value == 999U);
    assert_transport_message_native_import_metadata(descriptor);
}

void test_rgba_import_slot_derives_texture_format_metadata() {
    FrameSlotDescriptor descriptor = make_direct_bridge_frame_slot(
        "workspace", 3U, mmltk::live::LiveFrameId{100U, 101U}, CaptureRegion{8U, 16U, 640U, 360U}, 640U, 360U, 2560U,
        999U, FrameSlotLifecycle::ExplicitRelease);
    descriptor.pixel_format = FramePixelFormat::Rgba8;

    normalize_frame_slot_native_import_metadata(descriptor);

    const nlohmann::json json = descriptor;
    assert_transport_message_native_import_metadata(descriptor, "rgba8unorm");
    assert_transport_message_native_import_metadata_json(json, "rgba8unorm");

    const FrameSlotDescriptor round_trip = json.get<FrameSlotDescriptor>();
    assert_transport_message_native_import_metadata(round_trip, "rgba8unorm");
}

void test_structured_linux_import_contract_accepts_camelcase_nested_metadata() {
    FrameSlotDescriptor descriptor = make_direct_bridge_frame_slot(
        "workspace", 9U, mmltk::live::LiveFrameId{700U, 701U}, CaptureRegion{8U, 16U, 640U, 360U}, 640U, 360U, 2048U,
        999U, FrameSlotLifecycle::ExplicitRelease);

    const nlohmann::json json = {
        {"slot_name", "workspace"},
        {"transport", "cuda_device_buffer"},
        {"pixel_format", "rgba8"},
        {"slot_index", 3U},
        {"frame_id", {{"session_nonce", 100U}, {"sequence", 101U}}},
        {"capture_region", {{"x", 8U}, {"y", 16U}, {"width", 640U}, {"height", 360U}}},
        {"width", 640U},
        {"height", 360U},
        {"rowStrideBytes", 2560U},
        {"byteLength", frame_byte_length(2560U, 360U)},
        {"readyNs", 999U},
        {"shortFrame", false},
        {"lifecycle", "explicit_release"},
        {"ownership", "native_host"},
        {"linuxImport",
         {{"image",
           {{"kind", "opaque_token"},
            {"handleType", "native_token"},
            {"handle", "browser-frame:workspace:3:100:101:image"}}},
          {"readySync",
           {{"kind", "timeline_point"}, {"handle", "browser-frame:workspace:3:100:101:ready"}, {"value", 999U}}},
          {"releaseToken", "browser-frame:workspace:3:100:101:release"},
          {"metadata",
           {{"runtime", "cef"}, {"textureFormat", "rgba8unorm"}, {"releaseBehavior", "transport_message"}}}}}};

    json.get_to(descriptor);

    assert(descriptor.pixel_format == FramePixelFormat::Rgba8);
    assert_linux_import_slot_contract(descriptor, "browser-frame:workspace:3:100:101:image",
                                      "browser-frame:workspace:3:100:101:ready",
                                      "browser-frame:workspace:3:100:101:release");
    assert_transport_message_native_import_metadata(descriptor, "rgba8unorm");
    const auto native_import_metadata_value = frame_slot_native_import_metadata(descriptor);
    const auto& native_import_metadata = require_optional(native_import_metadata_value);
    const auto& linux_import = require_optional(descriptor.linux_import);
    const auto& linux_import_metadata = require_optional(linux_import.metadata);
    assert(native_import_metadata.runtime == "cef");
    assert(linux_import_metadata.runtime == "cef");
    assert(frame_slot_supports_imported_acquire(descriptor));

    const nlohmann::json normalized = descriptor;
    assert(normalized.at("linux_import").at("metadata").at("runtime") == "cef");
    assert_transport_message_native_import_metadata_json(normalized, "rgba8unorm");
}

void test_cuda_frame_slot_import_contract_rejects_invalid_import_contracts() {
    FrameSlotDescriptor descriptor = make_direct_bridge_frame_slot(
        "workspace", 3U, mmltk::live::LiveFrameId{100U, 101U}, CaptureRegion{8U, 16U, 640U, 360U}, 640U, 360U, 2048U,
        999U, FrameSlotLifecycle::ExplicitRelease);
    assert(frame_slot_supports_imported_acquire(descriptor));

    descriptor.byte_length -= 1U;
    assert(!frame_slot_supports_imported_acquire(descriptor));

    descriptor.byte_length = frame_byte_length(descriptor.row_stride_bytes, descriptor.height);
    auto& linux_import = require_optional(descriptor.linux_import);
    linux_import.release_token = linux_import.image.handle;
    assert(!frame_slot_supports_imported_acquire(descriptor));
}

void test_snapshot_retained_frame_slot_round_trips_without_release_token() {
    const FrameSlotDescriptor descriptor = make_direct_bridge_frame_slot(
        "workspace", 1U, mmltk::live::LiveFrameId{303U, 404U}, CaptureRegion{12U, 18U, 320U, 180U}, 320U, 180U, 1280U,
        777U, FrameSlotLifecycle::SnapshotRetained);

    const nlohmann::json json = descriptor;
    assert(json.at("linux_import").at("image").at("kind") == "opaque_token");
    assert(json.at("lifecycle") == "snapshot_retained");
    assert(json.at("ready_sync").at("kind") == "timeline_point");
    assert(!json.contains("release_token"));

    const FrameSlotDescriptor round_trip = json.get<FrameSlotDescriptor>();
    assert_linux_import_slot_contract(round_trip, "browser-frame:workspace:1:303:404:image",
                                      "browser-frame:workspace:1:303:404:ready");
    assert(round_trip.ready_sync.handle == "browser-frame:workspace:1:303:404:ready");
    assert(round_trip.ready_sync.value == 777U);
    assert(round_trip.lifecycle == FrameSlotLifecycle::SnapshotRetained);
    const auto& round_trip_linux_import = require_optional(round_trip.linux_import);
    assert(round_trip_linux_import.release_token.empty());
    assert(!round_trip_linux_import.metadata.has_value());
    assert(!frame_slot_supports_imported_acquire(round_trip));
}

void test_gui_settings_snapshot_builds_browser_state_snapshot() {
    BrowserHostSettingsFixture fixture;
    auto& gui = fixture.gui;
    gui.ui.dark_mode = true;
    gui.ui.ui_scale = 1.5F;
    gui.ui.density = mmltk::gui::UiDensity::Comfortable;
    gui.selected_preset = "rf-detr-seg-medium";
    gui.predict.source.kind = mmltk::gui::SourceKind::SingleImage;
    gui.predict.source.single_image_path = "/tmp/input.png";
    gui.predict.output_path = "/tmp/predictions.json";
    fixture.refresh_snapshot();
    const auto& gui_snapshot = fixture.snapshot;

    GuiStateSnapshotInputs inputs;
    inputs.gui = &gui_snapshot;
    inputs.state_revision = 81U;
    inputs.job.running = true;
    inputs.job.label = "predict";
    inputs.job.output_tail = "frame 3";

    const StateSnapshot snapshot = make_state_snapshot(inputs);
    assert(snapshot.state_revision == 81U);
    assert(snapshot.active_workflow == Workflow::Predict);
    assert(snapshot.workflow_state.at("selected_preset") == "rf-detr-seg-medium");
    assert(snapshot.workflow_state.at("workflows").at("predict").at("predict").at("output_path") ==
           "/tmp/predictions.json");
    assert(snapshot.settings_state.at("ui_scale") == 1.5);
    assert(snapshot.settings_state.at("dark_mode") == true);
    assert(snapshot.job.running);
    assert(snapshot.job.label == "predict");
    assert(snapshot.job.output_tail == "frame 3");
    assert(snapshot.source.kind == mmltk::browser::SourceKind::SingleImage);
    assert(snapshot.source.locator == "/tmp/input.png");
}

void test_live_view_snapshot_infers_source_and_attaches_annotation_and_workspace_surface() {
    BrowserHostSettingsFixture fixture;
    auto& gui = fixture.gui;
    gui.current_view = mmltk::gui::View::Live;
    gui.selected_preset = "rf-detr-seg-medium";
    gui.predict.source.kind = mmltk::gui::SourceKind::VideoStream;
    gui.predict.source.device_index = 4;
    gui.predict.source.capture_width = 1920;
    gui.predict.source.capture_height = 1080;
    gui.predict.source.capture_fps = 144;
    gui.predict.source.v4l2_buffer_count = 3;
    fixture.refresh_snapshot();
    const auto& gui_snapshot = fixture.snapshot;

    AnnotationDocumentState annotation;
    annotation.document_generation = 7U;
    annotation.session_revision = 9U;
    annotation.capture_width = 640U;
    annotation.capture_height = 360U;
    annotation.instance_count = 2U;

    GuiStateSnapshotInputs inputs;
    inputs.gui = &gui_snapshot;
    inputs.annotation = annotation;
    inputs.workspace_surface = WorkspaceSurfaceInfo{
        "workspace", "23", 640U, 360U, "rgba8unorm", true, true,
    };

    const StateSnapshot snapshot = make_state_snapshot(inputs);
    assert(snapshot.active_workflow == Workflow::Live);
    assert(snapshot.source.kind == mmltk::browser::SourceKind::VideoStream);
    assert(snapshot.source.locator == "device:4");
    assert(snapshot.source.capture_width == 1920);
    assert(snapshot.source.v4l2_buffer_count == 3);
    assert(snapshot.annotation.document_generation == 7U);
    assert(snapshot.annotation.instance_count == 2U);
    assert(snapshot.workspace_surface.has_value());
    const WorkspaceSurfaceInfo& workspace_surface = require_optional(snapshot.workspace_surface);
    assert(workspace_surface.surface_id == "workspace");
    assert(workspace_surface.revision == "23");
    assert(workspace_surface.width == 640U);
    assert(workspace_surface.height == 360U);
}

void test_browser_host_snapshot_cache_tracks_snapshot_delivery_revisions() {
    mmltk::gui::BrowserHostSnapshotCache cache;
    StateSnapshot state;
    state.active_workflow = Workflow::Predict;
    state.workflow_state = nlohmann::json{
        {"selected_preset", "rf-detr-seg-small"},
    };

    const auto encode_snapshot = [&]() { return cache.encode([&]() { return state; }); };

    const StateSnapshot first = nlohmann::json::parse(encode_snapshot()).get<StateSnapshot>();
    assert(first.state_revision == 1U);
    assert(first.workflow_state.at("selected_preset") == "rf-detr-seg-small");

    const StateSnapshot second = nlohmann::json::parse(encode_snapshot()).get<StateSnapshot>();
    assert(second.state_revision == 1U);

    state.job.running = true;
    state.job.summary = "frame published";
    const StateSnapshot third = nlohmann::json::parse(encode_snapshot()).get<StateSnapshot>();
    assert(third.state_revision == 2U);
    assert(third.job.running);
    assert(third.job.summary == "frame published");

    state.job = JobState{};
    const StateSnapshot fourth = nlohmann::json::parse(encode_snapshot()).get<StateSnapshot>();
    assert(fourth.state_revision == 3U);
    assert(!fourth.job.running);
    assert(fourth.job.summary.empty());
}

void test_browser_host_helper_resolves_bundle_root_browser_assets() {
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() /
        std::format("mmltk-browser-bundle-assets-{}", static_cast<long long>(::getpid()));
    const std::filesystem::path browser_root = temp_root / "browser";
    mmltk::testsupport::remove_path_recursively_best_effort(temp_root);
    std::filesystem::create_directories(browser_root);
    {
        std::ofstream index_file(browser_root / "index.html", std::ios::binary);
        index_file << "<html></html>";
    }

    const mmltk::gui::BrowserHostAssetPaths paths = mmltk::gui::resolve_browser_host_asset_paths(temp_root.string());
    assert(paths.bundle_root == temp_root);
    assert(paths.browser_root == browser_root);
    assert(paths.index_html == browser_root / "index.html");

    mmltk::testsupport::remove_path_recursively_best_effort(temp_root);
}

void test_browser_host_helper_resolves_explicit_browser_root() {
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() /
        std::format("mmltk-browser-host-assets-{}", static_cast<long long>(::getpid()));
    mmltk::testsupport::remove_path_recursively_best_effort(temp_root);
    std::filesystem::create_directories(temp_root);
    {
        std::ofstream index_file(temp_root / "index.html", std::ios::binary);
        index_file << "<html></html>";
    }

    const mmltk::gui::BrowserHostAssetPaths paths = mmltk::gui::resolve_browser_host_asset_paths(temp_root.string());
    assert(paths.browser_root == temp_root);
    assert(paths.index_html == temp_root / "index.html");

    mmltk::testsupport::remove_path_recursively_best_effort(temp_root);
}

MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_state_snapshot_round_trips_through_json);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_browser_runtime_capabilities_round_trip_accepts_cef_backend);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_intent_round_trips_through_json);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_state_and_intent_type_validation_is_strict);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_route_predict_start_intent_preserves_request_metadata_and_options);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_predict_start_settings_patch_updates_predict_workflow_state);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_annotate_save_settings_patch_updates_annotate_workflow_state);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_train_details_settings_patch_updates_train_workflow_state);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_route_settings_update_applies_json_merge_patch_to_gui_snapshot);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_crop_commit_updates_source_selection_state);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_file_dialog_and_viewport_commit_intents);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_annotate_sidebar_intents);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_annotate_workspace_intents);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_annotate_and_live_native_shell_control_intents);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_train_start_and_stop_intents);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_train_remote_query_and_offer_intents);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_train_local_gpu_refresh_intent);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_validate_and_export_start_intents);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_file_dialog_binding_resolves_known_browser_dialog_ids);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_route_intent_validation_rejects_invalid_workflow_and_payloads);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_gui_source_selection_maps_to_browser_source_metadata);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_manual_overlay_snapshot_maps_to_annotation_document_state);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_annotation_editor_state_maps_to_browser_annotation_state);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_train_local_gpu_state_to_json_surfaces_inventory_and_selection);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_train_local_gpu_state_to_json_surfaces_error_and_selection_banners);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_state_snapshot_round_trips_live_runtime_metadata_with_workspace_surface);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_live_runtime_state_to_json_surfaces_preview_and_pipeline_status);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_apply_routed_settings_update_updates_bound_gui_state);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_apply_routed_crop_commit_updates_workflow_source_selection);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_apply_routed_tool_select_updates_annotation_session);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_apply_routed_annotate_workspace_click_routes_canvas_click_to_active_tool);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_apply_routed_annotate_workspace_box_drag_creates_box_object);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_apply_routed_annotate_workspace_box_drag_moves_selected_box);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_apply_routed_annotate_workspace_box_drag_resizes_selected_box);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_apply_routed_annotate_workspace_color_sample_updates_selected_ranges);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_apply_routed_annotate_workspace_handle_drag_moves_point_handle);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_apply_routed_annotate_workspace_brush_tracks_stroke_and_updates_mask);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_apply_routed_annotate_workspace_fill_updates_mask_object);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_live_output_bundle_maps_to_direct_bridge_frame_slot_descriptor);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_workspace_output_bundle_reissues_named_browser_frame_slot);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_cuda_frame_slot_round_trips_import_contract_through_json);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_structured_linux_import_contract_controls_descriptor_ready_sync);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_rgba_import_slot_derives_texture_format_metadata);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_structured_linux_import_contract_accepts_camelcase_nested_metadata);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_cuda_frame_slot_import_contract_rejects_invalid_import_contracts);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_snapshot_retained_frame_slot_round_trips_without_release_token);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_gui_settings_snapshot_builds_browser_state_snapshot);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_live_view_snapshot_infers_source_and_attaches_annotation_and_workspace_surface);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]",
                         test_browser_host_snapshot_cache_tracks_snapshot_delivery_revisions);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_browser_host_helper_resolves_bundle_root_browser_assets);
MMLTK_REGISTER_TEST_CASE("[gui][browser_host_api]", test_browser_host_helper_resolves_explicit_browser_root);

}  // namespace
