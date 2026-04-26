#include "browser/host_api.h"

#include <array>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace mmltk::browser {

static_assert(kRoutedIntentPayloadAlternativeCount == 29U,
              "Update host_api intent routing for RoutedIntentPayload ABI changes.");

namespace {

template <typename T>
void get_optional(const nlohmann::json& j, const char* key, T& out) {
    if (j.contains(key)) {
        j.at(key).get_to(out);
    }
}

template <typename T>
void get_optional_alias(const nlohmann::json& j, const char* snake_key, const char* camel_key, T& out) {
    if (j.contains(snake_key)) {
        j.at(snake_key).get_to(out);
        return;
    }
    if (j.contains(camel_key)) {
        j.at(camel_key).get_to(out);
    }
}

[[nodiscard]] nlohmann::json::const_iterator find_optional_member(const nlohmann::json& j,
                                                                  const std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (const auto it = j.find(key); it != j.end() && !it->is_null()) {
            return it;
        }
    }
    return j.end();
}

[[nodiscard]] const nlohmann::json& require_object_payload(const IntentMessage& intent) {
    if (!intent.payload.is_object()) {
        throw std::runtime_error("browser host api intent payload must be a JSON object");
    }
    return intent.payload;
}

void validate_protocol_version(const std::uint32_t protocol_version) {
    if (protocol_version != kHostApiProtocolVersion) {
        throw std::runtime_error("browser host api protocol version mismatch");
    }
}

[[noreturn]] void throw_invalid_enum(const char* kind, const std::string_view value) {
    throw std::runtime_error("invalid browser host api " + std::string(kind) + ": " + std::string(value));
}

template <typename Enum>
struct BrowserEnumFactoryItem {
    std::string_view name;
    Enum value;
};

template <typename Enum, std::size_t N>
[[nodiscard]] std::string_view enum_factory_name(const Enum value,
                                                 const std::array<BrowserEnumFactoryItem<Enum>, N>& factory) noexcept {
    for (const BrowserEnumFactoryItem<Enum>& item : factory) {
        if (item.value == value) {
            return item.name;
        }
    }
    return factory.front().name;
}

template <typename Enum, std::size_t N>
[[nodiscard]] Enum enum_factory_value(const char* kind, const std::string_view name,
                                      const std::array<BrowserEnumFactoryItem<Enum>, N>& factory) {
    for (const BrowserEnumFactoryItem<Enum>& item : factory) {
        if (item.name == name) {
            return item.value;
        }
    }
    throw_invalid_enum(kind, name);
}

constexpr std::array<BrowserEnumFactoryItem<Workflow>, 6> kWorkflowFactory{{
    {"train", Workflow::Train},
    {"validate", Workflow::Validate},
    {"predict", Workflow::Predict},
    {"annotate", Workflow::Annotate},
    {"export", Workflow::Export},
    {"live", Workflow::Live},
}};
constexpr std::array<BrowserEnumFactoryItem<SourceKind>, 4> kSourceKindFactory{{
    {"compiled_dataset", SourceKind::CompiledDataset},
    {"single_image", SourceKind::SingleImage},
    {"image_folder", SourceKind::ImageFolder},
    {"video_stream", SourceKind::VideoStream},
}};
constexpr std::array<BrowserEnumFactoryItem<BrowserHostBackend>, 2> kBrowserHostBackendFactory{{
    {"unknown", BrowserHostBackend::Unknown},
    {"cef", BrowserHostBackend::Cef},
}};
constexpr std::array<BrowserEnumFactoryItem<BrowserRuntimeCapabilityStatus>, 3> kBrowserRuntimeCapabilityStatusFactory{{
    {"unknown", BrowserRuntimeCapabilityStatus::Unknown},
    {"available", BrowserRuntimeCapabilityStatus::Available},
    {"unavailable", BrowserRuntimeCapabilityStatus::Unavailable},
}};
constexpr std::array<BrowserEnumFactoryItem<BrowserBridgePhase>, 3> kBrowserBridgePhaseFactory{{
    {"idle", BrowserBridgePhase::Idle},
    {"polling", BrowserBridgePhase::Polling},
    {"dispatch", BrowserBridgePhase::Dispatch},
}};
constexpr std::array<BrowserEnumFactoryItem<FileDialogMode>, 3> kFileDialogModeFactory{{
    {"open_file", FileDialogMode::OpenFile},
    {"open_folder", FileDialogMode::OpenFolder},
    {"save_file", FileDialogMode::SaveFile},
}};

[[noreturn]] void throw_invalid_intent(const std::string_view intent_name) {
    throw std::runtime_error("invalid browser host api intent: " + std::string(intent_name));
}

[[noreturn]] void throw_intent_payload_error(const std::string_view intent_name, const std::string& message) {
    throw std::runtime_error("invalid browser host api intent `" + std::string(intent_name) + "`: " + message);
}

TrainStartIntent parse_train_start_intent(const nlohmann::json& payload) {
    return TrainStartIntent{payload};
}

int parse_required_int(const std::string_view intent_name, const nlohmann::json& payload, const char* key) {
    const auto value_it = payload.find(key);
    if (value_it == payload.end() || (!value_it->is_number_integer() && !value_it->is_number_unsigned())) {
        throw_intent_payload_error(intent_name, std::string("payload.") + key + " must be an integer");
    }
    return value_it->get<int>();
}

bool parse_required_bool(const std::string_view intent_name, const nlohmann::json& payload, const char* key) {
    const auto value_it = payload.find(key);
    if (value_it == payload.end() || !value_it->is_boolean()) {
        throw_intent_payload_error(intent_name, std::string("payload.") + key + " must be a boolean");
    }
    return value_it->get<bool>();
}

AnnotateSetupAction parse_annotate_setup_action(const std::string_view intent_name, const nlohmann::json& payload) {
    const auto action_it = payload.find("action");
    if (action_it == payload.end() || !action_it->is_string()) {
        throw_intent_payload_error(intent_name, "payload.action must be a string");
    }

    const std::string action = action_it->get<std::string>();
    if (action == "start_live" || action == "start-live" || action == "start_live_annotate" ||
        action == "start-live-annotate") {
        return AnnotateSetupAction::StartLive;
    }
    if (action == "stop_live" || action == "stop-live" || action == "stop_live_annotate" ||
        action == "stop-live-annotate") {
        return AnnotateSetupAction::StopLive;
    }
    if (action == "reload_frame" || action == "reload-frame" || action == "reload_current_frame" ||
        action == "reload-current-frame") {
        return AnnotateSetupAction::ReloadFrame;
    }
    if (action == "prev_frame" || action == "prev-frame" || action == "previous_frame" || action == "previous-frame") {
        return AnnotateSetupAction::PrevFrame;
    }
    if (action == "next_frame" || action == "next-frame") {
        return AnnotateSetupAction::NextFrame;
    }

    throw_intent_payload_error(intent_name,
                               "payload.action must be `start_live`, `stop_live`, `reload_frame`, "
                               "`prev_frame`, or `next_frame`");
}

AnnotateWorkspacePhase parse_annotate_workspace_phase(const std::string_view intent_name,
                                                      const nlohmann::json& payload) {
    const auto phase_it = payload.find("phase");
    if (phase_it == payload.end() || !phase_it->is_string()) {
        throw_intent_payload_error(intent_name, "payload.phase must be a string");
    }

    const std::string phase = phase_it->get<std::string>();
    if (phase == "begin") {
        return AnnotateWorkspacePhase::Begin;
    }
    if (phase == "update") {
        return AnnotateWorkspacePhase::Update;
    }
    if (phase == "end") {
        return AnnotateWorkspacePhase::End;
    }
    throw_intent_payload_error(intent_name, "payload.phase must be `begin`, `update`, or `end`");
}

AnnotateHandleRole parse_annotate_handle_role(const std::string_view intent_name, const nlohmann::json& payload) {
    const auto role_it = payload.find("role");
    if (role_it == payload.end() || !role_it->is_string()) {
        throw_intent_payload_error(intent_name, "payload.handle.role must be a string");
    }

    const std::string role = role_it->get<std::string>();
    if (role == "point") {
        return AnnotateHandleRole::Point;
    }
    if (role == "spline_knot") {
        return AnnotateHandleRole::SplineKnot;
    }
    if (role == "spline_in_handle") {
        return AnnotateHandleRole::SplineInHandle;
    }
    if (role == "spline_out_handle") {
        return AnnotateHandleRole::SplineOutHandle;
    }
    if (role == "skeleton_node") {
        return AnnotateHandleRole::SkeletonNode;
    }
    throw_intent_payload_error(intent_name,
                               "payload.handle.role must be `point`, `spline_knot`, "
                               "`spline_in_handle`, `spline_out_handle`, or `skeleton_node`");
}

AnnotateWorkspaceHandleRef parse_annotate_workspace_handle_ref(const std::string_view intent_name,
                                                               const nlohmann::json& payload) {
    if (!payload.is_object()) {
        throw_intent_payload_error(intent_name, "payload.handle must be a JSON object");
    }

    const int object_index = parse_required_int(intent_name, payload, "object_index");
    const int element_index = parse_required_int(intent_name, payload, "element_index");
    if (object_index < 0) {
        throw_intent_payload_error(intent_name, "payload.handle.object_index must be non-negative");
    }
    if (element_index < 0) {
        throw_intent_payload_error(intent_name, "payload.handle.element_index must be non-negative");
    }

    AnnotateWorkspaceHandleRef handle;
    handle.object_index = static_cast<std::uint32_t>(object_index);
    handle.element_index = static_cast<std::uint32_t>(element_index);
    handle.role = parse_annotate_handle_role(intent_name, payload);
    return handle;
}

TrainStopIntent parse_train_stop_intent(const nlohmann::json& payload) {
    TrainStopIntent routed;
    get_optional(payload, "force", routed.force);
    return routed;
}

TrainRemoteQueryIntent parse_train_remote_query_intent(const nlohmann::json&) {
    return TrainRemoteQueryIntent{};
}

TrainLocalGpuRefreshIntent parse_train_local_gpu_refresh_intent(const nlohmann::json&) {
    return TrainLocalGpuRefreshIntent{};
}

TrainRemoteOfferArmIntent parse_train_remote_offer_arm_intent(const std::string_view intent_name,
                                                              const nlohmann::json& payload) {
    const auto offer_id_it = payload.find("offer_id");
    if (offer_id_it == payload.end() || !offer_id_it->is_number_integer()) {
        throw_intent_payload_error(intent_name, "payload.offer_id must be an integer");
    }

    TrainRemoteOfferArmIntent routed;
    routed.offer_id = offer_id_it->get<int>();
    return routed;
}

TrainRemoteOfferClearIntent parse_train_remote_offer_clear_intent(const nlohmann::json&) {
    return TrainRemoteOfferClearIntent{};
}

TrainRemoteStartIntent parse_train_remote_start_intent(const nlohmann::json& payload) {
    return TrainRemoteStartIntent{payload};
}

TrainRemoteStopIntent parse_train_remote_stop_intent(const nlohmann::json&) {
    return TrainRemoteStopIntent{};
}

PredictStartIntent parse_predict_start_intent(const nlohmann::json& payload) {
    PredictStartIntent routed;
    routed.options = payload;
    if (const auto it = routed.options.find("selected_preset"); it != routed.options.end() && !it->is_null()) {
        routed.selected_preset = it->get<std::string>();
        routed.options.erase(it);
    }
    return routed;
}

PredictStopIntent parse_predict_stop_intent(const nlohmann::json&) {
    return PredictStopIntent{};
}

ValidateStartIntent parse_validate_start_intent(const nlohmann::json& payload) {
    return ValidateStartIntent{payload};
}

ExportStartIntent parse_export_start_intent(const nlohmann::json& payload) {
    return ExportStartIntent{payload};
}

AnnotateSaveIntent parse_annotate_save_intent(const nlohmann::json& payload) {
    return AnnotateSaveIntent{payload};
}

AnnotateSetupIntent parse_annotate_setup_intent(const std::string_view intent_name, const nlohmann::json& payload) {
    if (intent_name == "annotate.live.start") {
        return AnnotateSetupIntent{AnnotateSetupAction::StartLive};
    }
    if (intent_name == "annotate.live.stop") {
        return AnnotateSetupIntent{AnnotateSetupAction::StopLive};
    }
    if (intent_name == "annotate.setup_frame.reload" || intent_name == "annotate.frame.reload") {
        return AnnotateSetupIntent{AnnotateSetupAction::ReloadFrame};
    }
    if (intent_name == "annotate.setup_frame.prev" || intent_name == "annotate.frame.prev") {
        return AnnotateSetupIntent{AnnotateSetupAction::PrevFrame};
    }
    if (intent_name == "annotate.setup_frame.next" || intent_name == "annotate.frame.next") {
        return AnnotateSetupIntent{AnnotateSetupAction::NextFrame};
    }
    return AnnotateSetupIntent{
        parse_annotate_setup_action(intent_name, payload),
    };
}

AnnotateHoldSaveIntent parse_annotate_hold_save_intent(const std::string_view intent_name,
                                                       const nlohmann::json& payload) {
    AnnotateHoldSaveIntent routed;
    if (payload.contains("enabled")) {
        routed.enabled = parse_required_bool(intent_name, payload, "enabled");
        return routed;
    }
    if (payload.contains("active")) {
        routed.enabled = parse_required_bool(intent_name, payload, "active");
        return routed;
    }
    if (payload.contains("hold_save")) {
        routed.enabled = parse_required_bool(intent_name, payload, "hold_save");
        return routed;
    }
    throw_intent_payload_error(intent_name, "payload.enabled, payload.active, or payload.hold_save is required");
}

AnnotateBrushRadiusIntent parse_annotate_brush_radius_intent(const std::string_view intent_name,
                                                             const nlohmann::json& payload) {
    AnnotateBrushRadiusIntent routed;
    if (payload.contains("radius")) {
        routed.radius = parse_required_int(intent_name, payload, "radius");
    } else if (payload.contains("brush_radius")) {
        routed.radius = parse_required_int(intent_name, payload, "brush_radius");
    } else {
        throw_intent_payload_error(intent_name,
                                   "payload.radius or payload.brush_radius is "
                                   "required");
    }
    if (routed.radius <= 0) {
        throw_intent_payload_error(intent_name, "payload.radius must be greater than zero");
    }
    return routed;
}

AnnotateSidebarIntent parse_annotate_sidebar_intent(const std::string_view intent_name, const nlohmann::json& payload) {
    const auto action_it = payload.find("action");
    if (action_it == payload.end() || !action_it->is_string()) {
        throw_intent_payload_error(intent_name, "payload.action must be a string");
    }

    AnnotateSidebarIntent routed;
    routed.action = action_it->get<std::string>();

    const auto parse_optional_index = [&](const char* key, std::optional<std::uint32_t>& target) {
        if (const auto it = payload.find(key); it != payload.end() && !it->is_null()) {
            if (!it->is_number_unsigned() && !it->is_number_integer()) {
                throw_intent_payload_error(intent_name, std::string("payload.") + key + " must be an integer");
            }
            target = it->get<std::uint32_t>();
        }
    };

    parse_optional_index("object_index", routed.object_index);
    parse_optional_index("category_index", routed.category_index);
    parse_optional_index("spline_segment_index", routed.spline_segment_index);
    parse_optional_index("skeleton_joint_index", routed.skeleton_joint_index);

    if (const auto cleanup_radius_it = payload.find("cleanup_radius");
        cleanup_radius_it != payload.end() && !cleanup_radius_it->is_null()) {
        if (!cleanup_radius_it->is_number_integer()) {
            throw_intent_payload_error(intent_name, "payload.cleanup_radius must be an integer");
        }
        routed.cleanup_radius = cleanup_radius_it->get<int>();
    }

    if (const auto enabled_it = payload.find("enabled"); enabled_it != payload.end() && !enabled_it->is_null()) {
        if (!enabled_it->is_boolean()) {
            throw_intent_payload_error(intent_name, "payload.enabled must be a boolean");
        }
        routed.enabled = enabled_it->get<bool>();
    }

    if (const auto category_name_it = payload.find("category_name");
        category_name_it != payload.end() && !category_name_it->is_null()) {
        if (!category_name_it->is_string()) {
            throw_intent_payload_error(intent_name, "payload.category_name must be a string");
        }
        routed.category_name = category_name_it->get<std::string>();
    }

    if (const auto tool_it = payload.find("tool"); tool_it != payload.end() && !tool_it->is_null()) {
        if (!tool_it->is_string()) {
            throw_intent_payload_error(intent_name, "payload.tool must be a string");
        }
        routed.tool = tool_it->get<std::string>();
    }

    if (const auto handle_mode_it = payload.find("spline_handle_mode");
        handle_mode_it != payload.end() && !handle_mode_it->is_null()) {
        if (!handle_mode_it->is_string()) {
            throw_intent_payload_error(intent_name, "payload.spline_handle_mode must be a string");
        }
        routed.spline_handle_mode = handle_mode_it->get<std::string>();
    }

    if (const auto cleanup_op_it = payload.find("cleanup_op");
        cleanup_op_it != payload.end() && !cleanup_op_it->is_null()) {
        if (!cleanup_op_it->is_string()) {
            throw_intent_payload_error(intent_name, "payload.cleanup_op must be a string");
        }
        routed.cleanup_op = cleanup_op_it->get<std::string>();
    }

    if (const auto sup_it = payload.find("sup"); sup_it != payload.end() && !sup_it->is_null()) {
        if (!sup_it->is_object()) {
            throw_intent_payload_error(intent_name, "payload.sup must be a JSON object");
        }
        routed.sup = *sup_it;
    }

    if (const auto nosup_it = payload.find("nosup"); nosup_it != payload.end() && !nosup_it->is_null()) {
        if (!nosup_it->is_object()) {
            throw_intent_payload_error(intent_name, "payload.nosup must be a JSON object");
        }
        routed.nosup = *nosup_it;
    }

    if (routed.action == "select_object" || routed.action == "assist" || routed.action == "delete_selected" ||
        routed.action == "undo" || routed.action == "redo") {
        return routed;
    }
    if (routed.action == "add_category") {
        if (routed.category_name.empty()) {
            throw_intent_payload_error(intent_name, "payload.category_name must be a non-empty string");
        }
        return routed;
    }
    if (routed.action == "create_object") {
        if (routed.tool.empty()) {
            throw_intent_payload_error(intent_name, "payload.tool must be a non-empty string");
        }
        return routed;
    }
    if (routed.action == "update_selected") {
        if (!routed.enabled.has_value() && !routed.category_index.has_value()) {
            throw_intent_payload_error(intent_name, "payload.enabled or payload.category_index is required");
        }
        return routed;
    }
    if (routed.action == "redraw_box" || routed.action == "spline.insert_active_knot" ||
        routed.action == "spline.close" || routed.action == "spline.reopen" ||
        routed.action == "spline.delete_active_knot" || routed.action == "skeleton.skip_joint" ||
        routed.action == "skeleton.hide_joint" || routed.action == "skeleton.show_joint" ||
        routed.action == "skeleton.reseed_joint") {
        return routed;
    }
    if (routed.action == "spline.update_active_segment") {
        return routed;
    }
    if (routed.action == "spline.set_handle_mode") {
        if (routed.spline_handle_mode != "corner" && routed.spline_handle_mode != "smooth" &&
            routed.spline_handle_mode != "mirrored") {
            throw_intent_payload_error(intent_name,
                                       "payload.spline_handle_mode must be `corner`, "
                                       "`smooth`, or `mirrored`");
        }
        return routed;
    }
    if (routed.action == "skeleton.update_active_joint") {
        return routed;
    }
    if (routed.action == "mask.cleanup") {
        if (!routed.cleanup_radius.has_value()) {
            throw_intent_payload_error(intent_name, "payload.cleanup_radius is required");
        }
        if (routed.cleanup_op != "largest_component" && routed.cleanup_op != "fill_holes" &&
            routed.cleanup_op != "dilate" && routed.cleanup_op != "erode" && routed.cleanup_op != "open" &&
            routed.cleanup_op != "close") {
            throw_intent_payload_error(intent_name,
                                       "payload.cleanup_op must be `largest_component`, "
                                       "`fill_holes`, `dilate`, `erode`, `open`, or `close`");
        }
        return routed;
    }
    if (routed.action == "mask.update_color_ranges") {
        if (!routed.sup.is_object() || !routed.nosup.is_object()) {
            throw_intent_payload_error(intent_name, "payload.sup and payload.nosup are required");
        }
        return routed;
    }

    throw_intent_payload_error(intent_name, "payload.action is not supported");
}

AnnotateWorkspaceClickIntent parse_annotate_workspace_click_intent(const std::string_view intent_name,
                                                                   const nlohmann::json& payload) {
    AnnotateWorkspaceClickIntent routed;
    routed.capture_x = parse_required_int(intent_name, payload, "capture_x");
    routed.capture_y = parse_required_int(intent_name, payload, "capture_y");
    get_optional(payload, "double_click", routed.double_click);
    return routed;
}

AnnotateBoxDragKind parse_annotate_box_drag_kind(const std::string_view intent_name, const nlohmann::json& payload) {
    const auto drag_kind_it = payload.find("drag_kind");
    if (drag_kind_it == payload.end() || !drag_kind_it->is_string()) {
        throw_intent_payload_error(intent_name, "payload.drag_kind must be a string");
    }

    const std::string drag_kind = drag_kind_it->get<std::string>();
    if (drag_kind == "create") {
        return AnnotateBoxDragKind::Create;
    }
    if (drag_kind == "move") {
        return AnnotateBoxDragKind::Move;
    }
    if (drag_kind == "resize_top_left" || drag_kind == "resize-top-left") {
        return AnnotateBoxDragKind::ResizeTopLeft;
    }
    if (drag_kind == "resize_top_right" || drag_kind == "resize-top-right") {
        return AnnotateBoxDragKind::ResizeTopRight;
    }
    if (drag_kind == "resize_bottom_left" || drag_kind == "resize-bottom-left") {
        return AnnotateBoxDragKind::ResizeBottomLeft;
    }
    if (drag_kind == "resize_bottom_right" || drag_kind == "resize-bottom-right") {
        return AnnotateBoxDragKind::ResizeBottomRight;
    }

    throw_intent_payload_error(intent_name,
                               "payload.drag_kind must be `create`, `move`, "
                               "`resize_top_left`, `resize_top_right`, "
                               "`resize_bottom_left`, or `resize_bottom_right`");
}

AnnotateWorkspaceBoxDragIntent parse_annotate_workspace_box_drag_intent(const std::string_view intent_name,
                                                                        const nlohmann::json& payload) {
    AnnotateWorkspaceBoxDragIntent routed;
    routed.phase = parse_annotate_workspace_phase(intent_name, payload);
    routed.capture_x = parse_required_int(intent_name, payload, "capture_x");
    routed.capture_y = parse_required_int(intent_name, payload, "capture_y");

    if (const auto drag_kind_it = payload.find("drag_kind");
        drag_kind_it != payload.end() && !drag_kind_it->is_null()) {
        routed.drag_kind = parse_annotate_box_drag_kind(intent_name, payload);
    }
    if (const auto object_index_it = payload.find("object_index");
        object_index_it != payload.end() && !object_index_it->is_null()) {
        const int object_index = parse_required_int(intent_name, payload, "object_index");
        if (object_index < 0) {
            throw_intent_payload_error(intent_name, "payload.object_index must be non-negative");
        }
        routed.object_index = static_cast<std::uint32_t>(object_index);
    }

    if (routed.phase == AnnotateWorkspacePhase::Begin && !routed.drag_kind.has_value()) {
        throw_intent_payload_error(intent_name, "payload.drag_kind is required for phase `begin`");
    }
    if (routed.phase == AnnotateWorkspacePhase::Begin && routed.drag_kind.has_value() &&
        *routed.drag_kind != AnnotateBoxDragKind::Create && !routed.object_index.has_value()) {
        throw_intent_payload_error(intent_name, "payload.object_index is required for direct box drag begin");
    }
    return routed;
}

AnnotateWorkspaceHandleDragIntent parse_annotate_workspace_handle_drag_intent(const std::string_view intent_name,
                                                                              const nlohmann::json& payload) {
    AnnotateWorkspaceHandleDragIntent routed;
    routed.phase = parse_annotate_workspace_phase(intent_name, payload);
    routed.capture_x = parse_required_int(intent_name, payload, "capture_x");
    routed.capture_y = parse_required_int(intent_name, payload, "capture_y");
    if (const auto handle_it = payload.find("handle"); handle_it != payload.end() && !handle_it->is_null()) {
        routed.handle = parse_annotate_workspace_handle_ref(intent_name, *handle_it);
    }
    if (routed.phase == AnnotateWorkspacePhase::Begin && !routed.handle.has_value()) {
        throw_intent_payload_error(intent_name, "payload.handle is required for phase `begin`");
    }
    return routed;
}

AnnotateWorkspaceBrushIntent parse_annotate_workspace_brush_intent(const std::string_view intent_name,
                                                                   const nlohmann::json& payload) {
    AnnotateWorkspaceBrushIntent routed;
    routed.phase = parse_annotate_workspace_phase(intent_name, payload);
    routed.capture_x = parse_required_int(intent_name, payload, "capture_x");
    routed.capture_y = parse_required_int(intent_name, payload, "capture_y");
    routed.radius = parse_required_int(intent_name, payload, "radius");
    if (routed.radius <= 0) {
        throw_intent_payload_error(intent_name, "payload.radius must be greater than zero");
    }
    return routed;
}

AnnotateWorkspaceFillIntent parse_annotate_workspace_fill_intent(const std::string_view intent_name,
                                                                 const nlohmann::json& payload) {
    return AnnotateWorkspaceFillIntent{
        parse_required_int(intent_name, payload, "capture_x"),
        parse_required_int(intent_name, payload, "capture_y"),
    };
}

AnnotateWorkspaceColorSampleIntent parse_annotate_workspace_color_sample_intent(const std::string_view intent_name,
                                                                                const nlohmann::json& payload) {
    return AnnotateWorkspaceColorSampleIntent{
        parse_required_int(intent_name, payload, "capture_x"),
        parse_required_int(intent_name, payload, "capture_y"),
    };
}

ToolSelectIntent parse_tool_select_intent(const std::string_view intent_name, const nlohmann::json& payload) {
    const auto tool_it = payload.find("tool");
    if (tool_it == payload.end() || !tool_it->is_string()) {
        throw_intent_payload_error(intent_name, "payload.tool must be a string");
    }

    ToolSelectIntent routed;
    tool_it->get_to(routed.tool);
    routed.options = payload;
    routed.options.erase("tool");
    return routed;
}

LivePreviewControlIntent parse_live_preview_control_intent(const std::string_view intent_name,
                                                           const nlohmann::json& payload) {
    LivePreviewControlIntent routed;
    if (intent_name == "live.preview.fit_to_capture") {
        if (payload.contains("enabled")) {
            routed.fit_to_capture = parse_required_bool(intent_name, payload, "enabled");
            return routed;
        }
        if (payload.contains("value")) {
            routed.fit_to_capture = parse_required_bool(intent_name, payload, "value");
            return routed;
        }
        routed.fit_to_capture = parse_required_bool(intent_name, payload, "fit_to_capture");
        return routed;
    }
    if (intent_name == "live.preview.full_frame_display" || intent_name == "live.preview.full_frame" ||
        intent_name == "live.preview.crop_overlay_mode") {
        if (payload.contains("enabled")) {
            routed.crop_overlay_mode = parse_required_bool(intent_name, payload, "enabled");
            return routed;
        }
        if (payload.contains("value")) {
            routed.crop_overlay_mode = parse_required_bool(intent_name, payload, "value");
            return routed;
        }
        if (payload.contains("full_frame")) {
            routed.crop_overlay_mode = parse_required_bool(intent_name, payload, "full_frame");
            return routed;
        }
        routed.crop_overlay_mode = parse_required_bool(intent_name, payload, "crop_overlay_mode");
        return routed;
    }

    if (const auto fit_it = payload.find("fit_to_capture"); fit_it != payload.end() && !fit_it->is_null()) {
        if (!fit_it->is_boolean()) {
            throw_intent_payload_error(intent_name, "payload.fit_to_capture must be a boolean");
        }
        routed.fit_to_capture = fit_it->get<bool>();
    }

    std::optional<bool> crop_overlay_mode;
    if (const auto crop_it = payload.find("crop_overlay_mode"); crop_it != payload.end() && !crop_it->is_null()) {
        if (!crop_it->is_boolean()) {
            throw_intent_payload_error(intent_name, "payload.crop_overlay_mode must be a boolean");
        }
        crop_overlay_mode = crop_it->get<bool>();
    }

    if (const auto full_frame_it = payload.find("full_frame");
        full_frame_it != payload.end() && !full_frame_it->is_null()) {
        if (!full_frame_it->is_boolean()) {
            throw_intent_payload_error(intent_name, "payload.full_frame must be a boolean");
        }
        const bool full_frame = full_frame_it->get<bool>();
        if (crop_overlay_mode.has_value() && crop_overlay_mode.value() != full_frame) {
            throw_intent_payload_error(intent_name, "payload.full_frame must match payload.crop_overlay_mode");
        }
        crop_overlay_mode = full_frame;
    }

    routed.crop_overlay_mode = crop_overlay_mode;
    if (!routed.fit_to_capture.has_value() && !routed.crop_overlay_mode.has_value()) {
        throw_intent_payload_error(intent_name,
                                   "payload.fit_to_capture, payload.crop_overlay_mode, or "
                                   "payload.full_frame is required");
    }
    return routed;
}

SettingsUpdateIntent parse_settings_update_intent(const std::string_view intent_name, const nlohmann::json& payload) {
    SettingsUpdateIntent routed;
    const auto patch_it = payload.find("patch");
    if (patch_it != payload.end()) {
        if (!patch_it->is_object()) {
            throw_intent_payload_error(intent_name, "payload.patch must be a JSON object");
        }
        routed.patch = *patch_it;
        return routed;
    }
    routed.patch = payload;
    return routed;
}

CropCommitIntent parse_crop_commit_intent(const std::string_view intent_name, const nlohmann::json& payload) {
    CropCommitIntent routed;
    get_optional(payload, "has_crop", routed.has_crop);
    if (const auto crop_it = payload.find("crop"); crop_it != payload.end() && !crop_it->is_null()) {
        crop_it->get_to(routed.crop);
        if (!payload.contains("has_crop")) {
            routed.has_crop = routed.crop.width > 0U && routed.crop.height > 0U;
        }
    }
    if (routed.has_crop && !payload.contains("crop")) {
        throw_intent_payload_error(intent_name, "payload.crop is required when payload.has_crop is true");
    }
    if (!routed.has_crop) {
        routed.crop = CaptureRegion{};
    }
    return routed;
}

FileDialogRequestIntent parse_file_dialog_request_intent(const std::string_view intent_name,
                                                         const nlohmann::json& payload) {
    FileDialogRequestIntent routed;

    const auto dialog_id_it = payload.find("dialog_id");
    if (dialog_id_it == payload.end() || !dialog_id_it->is_string()) {
        throw_intent_payload_error(intent_name, "payload.dialog_id must be a string");
    }
    dialog_id_it->get_to(routed.dialog_id);

    const auto mode_it = payload.find("mode");
    if (mode_it == payload.end() || !mode_it->is_string()) {
        throw_intent_payload_error(intent_name, "payload.mode must be a string");
    }
    routed.mode = file_dialog_mode_from_name(mode_it->get<std::string>());

    get_optional(payload, "title", routed.title);
    get_optional(payload, "default_path", routed.default_path);
    if (const auto filters_it = payload.find("filters"); filters_it != payload.end()) {
        if (!filters_it->is_array()) {
            throw_intent_payload_error(intent_name, "payload.filters must be an array");
        }
        routed.filters.reserve(filters_it->size());
        for (const nlohmann::json& filter_json : *filters_it) {
            if (!filter_json.is_object()) {
                throw_intent_payload_error(intent_name, "payload.filters entries must be objects");
            }
            FileDialogFilter filter;
            get_optional(filter_json, "name", filter.name);
            if (const auto patterns_it = filter_json.find("patterns"); patterns_it != filter_json.end()) {
                if (!patterns_it->is_array()) {
                    throw_intent_payload_error(intent_name, "payload.filters[].patterns must be an array");
                }
                filter.patterns = patterns_it->get<std::vector<std::string>>();
            }
            routed.filters.push_back(std::move(filter));
        }
    }

    return routed;
}

ViewportCommitIntent parse_viewport_commit_intent(const std::string_view intent_name, const nlohmann::json& payload) {
    ViewportCommitIntent routed;
    get_optional(payload, "center_x", routed.center_x);
    get_optional(payload, "center_y", routed.center_y);
    get_optional(payload, "zoom", routed.zoom);
    if (routed.zoom <= 0.0) {
        throw_intent_payload_error(intent_name, "payload.zoom must be greater than zero");
    }
    if (const auto clip_it = payload.find("clip"); clip_it != payload.end() && !clip_it->is_null()) {
        routed.clip = clip_it->get<CaptureRegion>();
    }
    return routed;
}

using WorkflowMask = std::uint32_t;
using RoutedIntentPayloadFactory = RoutedIntentPayload (*)(std::string_view intent_name, const nlohmann::json& payload);

struct IntentRouteSpec {
    WorkflowMask workflows = 0U;
    const char* workflow_error = nullptr;
    RoutedIntentPayloadFactory make_payload = nullptr;
};

[[nodiscard]] constexpr WorkflowMask workflow_mask(const Workflow workflow) noexcept {
    return 1U << static_cast<std::uint32_t>(workflow);
}

[[nodiscard]] constexpr bool workflow_allowed(const WorkflowMask mask, const Workflow workflow) noexcept {
    return (mask & workflow_mask(workflow)) != 0U;
}

template <auto Parser>
[[nodiscard]] RoutedIntentPayload make_routed_payload(const std::string_view intent_name,
                                                      const nlohmann::json& payload) {
    if constexpr (std::is_invocable_v<decltype(Parser), std::string_view, const nlohmann::json&>) {
        return Parser(intent_name, payload);
    } else {
        return Parser(payload);
    }
}

template <auto Parser>
[[nodiscard]] constexpr IntentRouteSpec route_spec(const WorkflowMask workflows, const char* workflow_error) noexcept {
    return IntentRouteSpec{workflows, workflow_error, make_routed_payload<Parser>};
}

[[nodiscard]] const IntentRouteSpec& find_intent_route_spec(const std::string_view intent_name) {
    constexpr WorkflowMask kTrain = workflow_mask(Workflow::Train);
    constexpr WorkflowMask kValidate = workflow_mask(Workflow::Validate);
    constexpr WorkflowMask kPredict = workflow_mask(Workflow::Predict);
    constexpr WorkflowMask kAnnotate = workflow_mask(Workflow::Annotate);
    constexpr WorkflowMask kExport = workflow_mask(Workflow::Export);
    constexpr WorkflowMask kLive = workflow_mask(Workflow::Live);
    constexpr WorkflowMask kPredictOrLive = kPredict | kLive;
    constexpr WorkflowMask kPredictAnnotateOrLive = kPredict | kAnnotate | kLive;
    constexpr WorkflowMask kAnyWorkflow = kTrain | kValidate | kPredict | kAnnotate | kExport | kLive;
    constexpr const char* kTrainWorkflow = "workflow must be `train`";
    constexpr const char* kPredictLiveWorkflow = "workflow must be `predict` or `live`";
    constexpr const char* kValidateWorkflow = "workflow must be `validate`";
    constexpr const char* kExportWorkflow = "workflow must be `export`";
    constexpr const char* kAnnotateWorkflow = "workflow must be `annotate`";
    constexpr const char* kLiveWorkflow = "workflow must be `live`";
    constexpr const char* kPredictAnnotateLiveWorkflow = "workflow must be `predict`, `annotate`, or `live`";

    static const std::unordered_map<std::string_view, IntentRouteSpec> kFactory{
        {"train.start", route_spec<&parse_train_start_intent>(kTrain, kTrainWorkflow)},
        {"train.stop", route_spec<&parse_train_stop_intent>(kTrain, kTrainWorkflow)},
        {"train.remote.query", route_spec<&parse_train_remote_query_intent>(kTrain, kTrainWorkflow)},
        {"train.local_gpu.refresh", route_spec<&parse_train_local_gpu_refresh_intent>(kTrain, kTrainWorkflow)},
        {"train.remote.offer.arm", route_spec<&parse_train_remote_offer_arm_intent>(kTrain, kTrainWorkflow)},
        {"train.remote.offer.clear", route_spec<&parse_train_remote_offer_clear_intent>(kTrain, kTrainWorkflow)},
        {"train.remote.start", route_spec<&parse_train_remote_start_intent>(kTrain, kTrainWorkflow)},
        {"train.remote.stop", route_spec<&parse_train_remote_stop_intent>(kTrain, kTrainWorkflow)},
        {"predict.start", route_spec<&parse_predict_start_intent>(kPredictOrLive, kPredictLiveWorkflow)},
        {"predict.stop", route_spec<&parse_predict_stop_intent>(kPredictOrLive, kPredictLiveWorkflow)},
        {"validate.start", route_spec<&parse_validate_start_intent>(kValidate, kValidateWorkflow)},
        {"export.start", route_spec<&parse_export_start_intent>(kExport, kExportWorkflow)},
        {"annotate.save", route_spec<&parse_annotate_save_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.save_now", route_spec<&parse_annotate_save_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.setup", route_spec<&parse_annotate_setup_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.live.start", route_spec<&parse_annotate_setup_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.live.stop", route_spec<&parse_annotate_setup_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.setup_frame.reload", route_spec<&parse_annotate_setup_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.setup_frame.prev", route_spec<&parse_annotate_setup_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.setup_frame.next", route_spec<&parse_annotate_setup_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.frame.reload", route_spec<&parse_annotate_setup_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.frame.prev", route_spec<&parse_annotate_setup_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.frame.next", route_spec<&parse_annotate_setup_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.hold_save", route_spec<&parse_annotate_hold_save_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.hold_save.update", route_spec<&parse_annotate_hold_save_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.brush_radius", route_spec<&parse_annotate_brush_radius_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.brush_radius.update", route_spec<&parse_annotate_brush_radius_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.sidebar", route_spec<&parse_annotate_sidebar_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.workspace.click", route_spec<&parse_annotate_workspace_click_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.workspace.box_drag",
         route_spec<&parse_annotate_workspace_box_drag_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.workspace.handle_drag",
         route_spec<&parse_annotate_workspace_handle_drag_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.workspace.brush", route_spec<&parse_annotate_workspace_brush_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.workspace.fill", route_spec<&parse_annotate_workspace_fill_intent>(kAnnotate, kAnnotateWorkflow)},
        {"annotate.workspace.color_sample",
         route_spec<&parse_annotate_workspace_color_sample_intent>(kAnnotate, kAnnotateWorkflow)},
        {"tool.select", route_spec<&parse_tool_select_intent>(kPredictAnnotateOrLive, kPredictAnnotateLiveWorkflow)},
        {"live.preview.update", route_spec<&parse_live_preview_control_intent>(kLive, kLiveWorkflow)},
        {"live.preview.controls", route_spec<&parse_live_preview_control_intent>(kLive, kLiveWorkflow)},
        {"live.preview.fit_to_capture", route_spec<&parse_live_preview_control_intent>(kLive, kLiveWorkflow)},
        {"live.preview.full_frame_display", route_spec<&parse_live_preview_control_intent>(kLive, kLiveWorkflow)},
        {"live.preview.full_frame", route_spec<&parse_live_preview_control_intent>(kLive, kLiveWorkflow)},
        {"live.preview.crop_overlay_mode", route_spec<&parse_live_preview_control_intent>(kLive, kLiveWorkflow)},
        {"crop.commit", route_spec<&parse_crop_commit_intent>(kPredictAnnotateOrLive, kPredictAnnotateLiveWorkflow)},
        {"viewport.commit",
         route_spec<&parse_viewport_commit_intent>(kPredictAnnotateOrLive, kPredictAnnotateLiveWorkflow)},
        {"settings.update", route_spec<&parse_settings_update_intent>(kAnyWorkflow, nullptr)},
        {"file_dialog.request", route_spec<&parse_file_dialog_request_intent>(kAnyWorkflow, nullptr)},
        {"file-dialog.request", route_spec<&parse_file_dialog_request_intent>(kAnyWorkflow, nullptr)},
    };
    const auto spec = kFactory.find(intent_name);
    if (spec == kFactory.end()) {
        throw_invalid_intent(intent_name);
    }
    return spec->second;
}

}  // namespace

std::string_view workflow_name(const Workflow workflow) noexcept {
    return enum_factory_name(workflow, kWorkflowFactory);
}

Workflow workflow_from_name(const std::string_view name) {
    return enum_factory_value("workflow", name, kWorkflowFactory);
}

std::string_view source_kind_name(const SourceKind kind) noexcept {
    return enum_factory_name(kind, kSourceKindFactory);
}

SourceKind source_kind_from_name(const std::string_view name) {
    return enum_factory_value("source kind", name, kSourceKindFactory);
}

std::string_view browser_host_backend_name(const BrowserHostBackend backend) noexcept {
    return enum_factory_name(backend, kBrowserHostBackendFactory);
}

BrowserHostBackend browser_host_backend_from_name(const std::string_view name) {
    return enum_factory_value("browser host backend", name, kBrowserHostBackendFactory);
}

std::string_view browser_runtime_capability_status_name(const BrowserRuntimeCapabilityStatus status) noexcept {
    return enum_factory_name(status, kBrowserRuntimeCapabilityStatusFactory);
}

BrowserRuntimeCapabilityStatus browser_runtime_capability_status_from_name(const std::string_view name) {
    return enum_factory_value("browser runtime capability status", name, kBrowserRuntimeCapabilityStatusFactory);
}

std::string_view browser_bridge_phase_name(const BrowserBridgePhase phase) noexcept {
    return enum_factory_name(phase, kBrowserBridgePhaseFactory);
}

BrowserBridgePhase browser_bridge_phase_from_name(const std::string_view name) {
    return enum_factory_value("browser bridge phase", name, kBrowserBridgePhaseFactory);
}

std::string_view file_dialog_mode_name(const FileDialogMode mode) noexcept {
    return enum_factory_name(mode, kFileDialogModeFactory);
}

FileDialogMode file_dialog_mode_from_name(const std::string_view name) {
    return enum_factory_value("file dialog mode", name, kFileDialogModeFactory);
}

void to_json(nlohmann::json& j, const CaptureRegion& region) {
    j = nlohmann::json{
        {"x", region.x},
        {"y", region.y},
        {"width", region.width},
        {"height", region.height},
    };
}

void from_json(const nlohmann::json& j, CaptureRegion& region) {
    j.at("x").get_to(region.x);
    j.at("y").get_to(region.y);
    j.at("width").get_to(region.width);
    j.at("height").get_to(region.height);
}

void to_json(nlohmann::json& j, const JobLogEntry& entry) {
    j = nlohmann::json{
        {"sequence", entry.sequence},
        {"level", entry.level},
        {"message", entry.message},
    };
}

void from_json(const nlohmann::json& j, JobLogEntry& entry) {
    j.at("sequence").get_to(entry.sequence);
    j.at("level").get_to(entry.level);
    j.at("message").get_to(entry.message);
}

void to_json(nlohmann::json& j, const JobState& state) {
    j = nlohmann::json{
        {"running", state.running},         {"label", state.label},
        {"summary", state.summary},         {"error", state.error},
        {"output_tail", state.output_tail}, {"recent_logs", state.recent_logs},
    };
}

void from_json(const nlohmann::json& j, JobState& state) {
    get_optional(j, "running", state.running);
    get_optional(j, "label", state.label);
    get_optional(j, "summary", state.summary);
    get_optional(j, "error", state.error);
    get_optional(j, "output_tail", state.output_tail);
    get_optional(j, "recent_logs", state.recent_logs);
}

void to_json(nlohmann::json& j, const SourceMetadata& metadata) {
    j = nlohmann::json{
        {"kind", source_kind_name(metadata.kind)}, {"locator", metadata.locator},
        {"recursive", metadata.recursive},         {"device_index", metadata.device_index},
        {"capture_width", metadata.capture_width}, {"capture_height", metadata.capture_height},
        {"capture_fps", metadata.capture_fps},     {"v4l2_buffer_count", metadata.v4l2_buffer_count},
        {"has_crop", metadata.has_crop},           {"crop", metadata.crop},
    };
}

void from_json(const nlohmann::json& j, SourceMetadata& metadata) {
    metadata.kind = source_kind_from_name(j.at("kind").get<std::string>());
    get_optional(j, "locator", metadata.locator);
    get_optional(j, "recursive", metadata.recursive);
    get_optional(j, "device_index", metadata.device_index);
    get_optional(j, "capture_width", metadata.capture_width);
    get_optional(j, "capture_height", metadata.capture_height);
    get_optional(j, "capture_fps", metadata.capture_fps);
    get_optional(j, "v4l2_buffer_count", metadata.v4l2_buffer_count);
    get_optional(j, "has_crop", metadata.has_crop);
    if (j.contains("crop")) {
        j.at("crop").get_to(metadata.crop);
    }
}

void to_json(nlohmann::json& j, const AnnotationDocumentState& state) {
    j = nlohmann::json{
        {"document_generation", state.document_generation},
        {"session_revision", state.session_revision},
        {"capture_width", state.capture_width},
        {"capture_height", state.capture_height},
        {"instance_count", state.instance_count},
        {"selected_instance",
         state.selected_instance.has_value() ? nlohmann::json(*state.selected_instance) : nlohmann::json(nullptr)},
    };
}

void from_json(const nlohmann::json& j, AnnotationDocumentState& state) {
    get_optional(j, "document_generation", state.document_generation);
    get_optional(j, "session_revision", state.session_revision);
    get_optional(j, "capture_width", state.capture_width);
    get_optional(j, "capture_height", state.capture_height);
    get_optional(j, "instance_count", state.instance_count);
    if (j.contains("selected_instance") && !j.at("selected_instance").is_null()) {
        state.selected_instance = j.at("selected_instance").get<std::uint32_t>();
    } else {
        state.selected_instance.reset();
    }
}

void to_json(nlohmann::json& j, const WorkspaceSurfaceInfo& surface_info) {
    j = nlohmann::json{
        {"surfaceId", surface_info.surface_id},
        {"revision", surface_info.revision},
        {"width", surface_info.width},
        {"height", surface_info.height},
        {"textureFormat", surface_info.texture_format},
        {"opaque", surface_info.opaque},
        {"upright", surface_info.upright},
    };
}

void from_json(const nlohmann::json& j, WorkspaceSurfaceInfo& surface_info) {
    surface_info = WorkspaceSurfaceInfo{};
    get_optional_alias(j, "surface_id", "surfaceId", surface_info.surface_id);
    get_optional_alias(j, "revision", "revision", surface_info.revision);
    get_optional_alias(j, "width", "width", surface_info.width);
    get_optional_alias(j, "height", "height", surface_info.height);
    get_optional_alias(j, "texture_format", "textureFormat", surface_info.texture_format);
    get_optional_alias(j, "opaque", "opaque", surface_info.opaque);
    get_optional_alias(j, "upright", "upright", surface_info.upright);
}

void to_json(nlohmann::json& j, const BrowserRuntimeCapabilities& capabilities) {
    j = nlohmann::json{
        {"host_backend", browser_host_backend_name(capabilities.host_backend)},
        {"navigator_gpu", browser_runtime_capability_status_name(capabilities.navigator_gpu)},
        {"workspace_surface_bridge", browser_runtime_capability_status_name(capabilities.workspace_surface_bridge)},
        {"workspace_surface_zero_copy",
         browser_runtime_capability_status_name(capabilities.workspace_surface_zero_copy)},
    };
}

void from_json(const nlohmann::json& j, BrowserRuntimeCapabilities& capabilities) {
    if (const auto it = j.find("host_backend"); it != j.end() && !it->is_null()) {
        capabilities.host_backend = browser_host_backend_from_name(it->get<std::string>());
    }
    if (const auto it = j.find("navigator_gpu"); it != j.end() && !it->is_null()) {
        capabilities.navigator_gpu = browser_runtime_capability_status_from_name(it->get<std::string>());
    }
    if (const auto it = find_optional_member(j, {"workspace_surface_bridge", "workspaceSurfaceBridge"});
        it != j.end() && !it->is_null()) {
        capabilities.workspace_surface_bridge = browser_runtime_capability_status_from_name(it->get<std::string>());
    }
    if (const auto it = find_optional_member(j, {"workspace_surface_zero_copy", "workspaceSurfaceZeroCopy"});
        it != j.end() && !it->is_null()) {
        capabilities.workspace_surface_zero_copy = browser_runtime_capability_status_from_name(it->get<std::string>());
    }
}

void to_json(nlohmann::json& j, const StateSnapshot& snapshot) {
    j = nlohmann::json{
        {"type", kStateSnapshotMessageType},
        {"protocol_version", snapshot.protocol_version},
        {"state_revision", snapshot.state_revision},
        {"active_workflow", workflow_name(snapshot.active_workflow)},
        {"workflow_state", snapshot.workflow_state},
        {"settings_state", snapshot.settings_state},
        {"job", snapshot.job},
        {"source", snapshot.source},
        {"annotation", snapshot.annotation},
        {"runtime_capabilities", snapshot.runtime_capabilities},
    };
    if (snapshot.workspace_surface.has_value()) {
        j["workspace_surface"] = *snapshot.workspace_surface;
    }
}

void from_json(const nlohmann::json& j, StateSnapshot& snapshot) {
    const std::string type = j.at("type").get<std::string>();
    if (type != kStateSnapshotMessageType) {
        throw std::runtime_error("browser host api state snapshot type mismatch");
    }

    get_optional(j, "protocol_version", snapshot.protocol_version);
    get_optional(j, "state_revision", snapshot.state_revision);
    snapshot.active_workflow = workflow_from_name(j.at("active_workflow").get<std::string>());
    if (j.contains("workflow_state")) {
        snapshot.workflow_state = j.at("workflow_state");
    }
    if (j.contains("settings_state")) {
        snapshot.settings_state = j.at("settings_state");
    }
    if (j.contains("job")) {
        j.at("job").get_to(snapshot.job);
    }
    if (j.contains("source")) {
        j.at("source").get_to(snapshot.source);
    }
    if (j.contains("annotation")) {
        j.at("annotation").get_to(snapshot.annotation);
    }
    if (j.contains("runtime_capabilities")) {
        j.at("runtime_capabilities").get_to(snapshot.runtime_capabilities);
    }
    if (const auto it = find_optional_member(j, {"workspace_surface", "workspaceSurface"});
        it != j.end() && !it->is_null()) {
        WorkspaceSurfaceInfo workspace_surface;
        it->get_to(workspace_surface);
        snapshot.workspace_surface = std::move(workspace_surface);
    } else {
        snapshot.workspace_surface.reset();
    }
}

void to_json(nlohmann::json& j, const IntentMessage& intent) {
    j = nlohmann::json{
        {"type", kIntentMessageType},      {"protocol_version", intent.protocol_version},
        {"request_id", intent.request_id}, {"workflow", workflow_name(intent.workflow)},
        {"intent", intent.intent},         {"payload", intent.payload},
    };
}

void from_json(const nlohmann::json& j, IntentMessage& intent) {
    const std::string type = j.at("type").get<std::string>();
    if (type != kIntentMessageType) {
        throw std::runtime_error("browser host api intent type mismatch");
    }

    get_optional(j, "protocol_version", intent.protocol_version);
    get_optional(j, "request_id", intent.request_id);
    intent.workflow = workflow_from_name(j.at("workflow").get<std::string>());
    get_optional(j, "intent", intent.intent);
    if (j.contains("payload")) {
        intent.payload = j.at("payload");
    }
}

std::string_view routed_intent_name(const RoutedIntentPayload& payload) noexcept {
    static constexpr std::array<std::string_view, kRoutedIntentPayloadAlternativeCount> kNames{
        "train.start",
        "train.stop",
        "train.remote.query",
        "train.local_gpu.refresh",
        "train.remote.offer.arm",
        "train.remote.offer.clear",
        "train.remote.start",
        "train.remote.stop",
        "predict.start",
        "predict.stop",
        "validate.start",
        "export.start",
        "annotate.save",
        "annotate.setup",
        "annotate.hold_save",
        "annotate.brush_radius",
        "annotate.sidebar",
        "annotate.workspace.click",
        "annotate.workspace.box_drag",
        "annotate.workspace.handle_drag",
        "annotate.workspace.brush",
        "annotate.workspace.fill",
        "annotate.workspace.color_sample",
        "tool.select",
        "live.preview.update",
        "settings.update",
        "crop.commit",
        "file_dialog.request",
        "viewport.commit",
    };
    const std::size_t index = payload.index();
    return index < kNames.size() ? kNames[index] : std::string_view{};
}

RoutedIntent route_intent(const IntentMessage& intent) {
    validate_protocol_version(intent.protocol_version);
    const std::string_view intent_name = intent.intent;
    const IntentRouteSpec& spec = find_intent_route_spec(intent_name);
    if (!workflow_allowed(spec.workflows, intent.workflow)) {
        throw_intent_payload_error(intent_name, spec.workflow_error);
    }
    const nlohmann::json& payload = require_object_payload(intent);

    RoutedIntent routed;
    routed.protocol_version = intent.protocol_version;
    routed.request_id = intent.request_id;
    routed.workflow = intent.workflow;
    routed.payload = spec.make_payload(intent_name, payload);
    return routed;
}

}  // namespace mmltk::browser
