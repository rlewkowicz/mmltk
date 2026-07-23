#include "browser/host_api.h"
#include "browser/browser_contract_metadata.h"
#include "gui/browser_file_dialog_contract.h"
#define MMLTK_BROWSER_SETTINGS_CONTRACT_METADATA_ONLY 1
#include "gui/browser_settings_contract.h"
#undef MMLTK_BROWSER_SETTINGS_CONTRACT_METADATA_ONLY

#include <algorithm>
#include <array>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mmltk::browser {

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

[[nodiscard]] consteval auto sorted_settings_patch_value_types() {
    auto values = kBrowserSettingsPatchValueTypes;
    std::sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) { return lhs.path < rhs.path; });
    return values;
}

inline constexpr auto kNativeSettingsPatchValueTypesByPath = sorted_settings_patch_value_types();

[[nodiscard]] consteval auto sorted_settings_patch_paths() {
    std::array<std::string_view, kNativeSettingsPatchValueTypesByPath.size()> paths{};
    for (std::size_t i = 0U; i < kNativeSettingsPatchValueTypesByPath.size(); ++i) {
        paths[i] = kNativeSettingsPatchValueTypesByPath[i].path;
    }
    return paths;
}

inline constexpr auto kNativeSettingsPatchPathsByPath = sorted_settings_patch_paths();

template <std::size_t N>
[[nodiscard]] consteval auto sorted_string_views(std::array<std::string_view, N> values) {
    std::sort(values.begin(), values.end());
    return values;
}

[[nodiscard]] consteval auto sorted_workflow_ids() {
    std::array<std::string_view, api::enum_traits<Workflow>::values().size()> values{};
    std::size_t index = 0U;
    for (const auto item : api::enum_traits<Workflow>::values()) {
        values[index++] = item.name;
    }
    return sorted_string_views(values);
}

inline constexpr auto kNativeWorkflowIdsByName = sorted_workflow_ids();

[[nodiscard]] consteval auto sorted_preset_names() {
    std::array<std::string_view, contract::kPresets.size()> values{};
    for (std::size_t i = 0U; i < contract::kPresets.size(); ++i) {
        values[i] = contract::kPresets[i].preset_name;
    }
    return sorted_string_views(values);
}

inline constexpr auto kNativePresetNamesByName = sorted_preset_names();

[[nodiscard]] consteval auto default_routed_intent_names_by_payload_index() {
    std::array<std::string_view, kRoutedIntentPayloadAlternativeCount> names{};
    std::size_t index = 0U;
    auto append_names = [&]<typename... Payloads>(api::type_list<Payloads...>) {
        ((names[index++] = Payloads::api_intent().id), ...);
    };
    append_names(BrowserIntentPayloadTypes{});
    return names;
}

inline constexpr auto kDefaultRoutedIntentNamesByPayloadIndex = default_routed_intent_names_by_payload_index();

template <std::size_t N>
[[nodiscard]] bool sorted_string_views_contains(const std::array<std::string_view, N>& values,
                                                const std::string_view needle) noexcept {
    return std::binary_search(values.begin(), values.end(), needle);
}

[[nodiscard]] bool numeric_enum_schema_contains_value(const BrowserNumericEnumSchemaSpec& schema,
                                                      const nlohmann::json& value) noexcept {
    if (const auto* signed_value = value.get_ptr<const nlohmann::json::number_integer_t*>()) {
        for (std::size_t i = 0U; i < schema.value_count; ++i) {
            if (static_cast<nlohmann::json::number_integer_t>(schema.values[i]) == *signed_value) {
                return true;
            }
        }
        return false;
    }

    if (const auto* unsigned_value = value.get_ptr<const nlohmann::json::number_unsigned_t*>()) {
        for (std::size_t i = 0U; i < schema.value_count; ++i) {
            if (schema.values[i] >= 0 &&
                static_cast<nlohmann::json::number_unsigned_t>(schema.values[i]) == *unsigned_value) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool settings_patch_path_is_known_prefix_or_leaf(const std::string_view path) noexcept {
    if (path.empty()) {
        return true;
    }
    const auto found =
        std::lower_bound(kNativeSettingsPatchPathsByPath.begin(), kNativeSettingsPatchPathsByPath.end(), path);
    return found != kNativeSettingsPatchPathsByPath.end() &&
           (*found == path ||
            (found->size() > path.size() && found->substr(0, path.size()) == path && (*found)[path.size()] == '.'));
}

[[nodiscard]] bool settings_patch_path_is_known_leaf(const std::string_view path) noexcept {
    return sorted_string_views_contains(kNativeSettingsPatchPathsByPath, path);
}

[[nodiscard]] std::string_view settings_patch_value_type(const std::string_view path) noexcept {
    const auto found =
        std::lower_bound(kNativeSettingsPatchValueTypesByPath.begin(), kNativeSettingsPatchValueTypesByPath.end(), path,
                         [](const BrowserSettingsPatchValueTypeSpec& value_type, const std::string_view needle) {
                             return value_type.path < needle;
                         });
    return found != kNativeSettingsPatchValueTypesByPath.end() && found->path == path ? found->value_type
                                                                                      : std::string_view{};
}

[[nodiscard]] bool workflow_id_is_known(const std::string_view value) noexcept {
    return sorted_string_views_contains(kNativeWorkflowIdsByName, value);
}

[[nodiscard]] bool preset_name_is_known(const std::string_view value) noexcept {
    return sorted_string_views_contains(kNativePresetNamesByName, value);
}

[[nodiscard]] bool settings_numeric_enum_value_is_allowed(const std::string_view value_type,
                                                          const nlohmann::json& value) noexcept {
    if (value.get_ptr<const nlohmann::json::number_integer_t*>() == nullptr &&
        value.get_ptr<const nlohmann::json::number_unsigned_t*>() == nullptr) {
        return false;
    }
    for (const BrowserNumericEnumSchemaSpec& schema : kBrowserSettingsNumericEnumSchemas) {
        if (schema.value_type != value_type) {
            continue;
        }
        return numeric_enum_schema_contains_value(schema, value);
    }
    return false;
}

[[nodiscard]] bool settings_numeric_enum_value_type_is_known(const std::string_view value_type) noexcept {
    for (const BrowserNumericEnumSchemaSpec& schema : kBrowserSettingsNumericEnumSchemas) {
        if (schema.value_type == value_type) {
            return true;
        }
    }
    return false;
}

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
    if (phase == "cancel") {
        return AnnotateWorkspacePhase::Cancel;
    }
    if (phase == "hover") {
        return AnnotateWorkspacePhase::Hover;
    }
    throw_intent_payload_error(intent_name, "payload.phase must be `begin`, `update`, `end`, `cancel`, or `hover`");
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

TrainRemoteStartIntent parse_train_remote_start_intent(const nlohmann::json& payload) {
    return TrainRemoteStartIntent{payload};
}

PredictStartIntent parse_predict_start_intent(const nlohmann::json& payload) {
    return PredictStartIntent{payload};
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
    for (const auto action : AnnotateSetupIntent::api_intent_actions()) {
        if (action.id == intent_name) {
            return AnnotateSetupIntent{action.action};
        }
    }
    (void)payload;
    throw_invalid_intent(intent_name);
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
    if (drag_kind == "resize_top_left") {
        return AnnotateBoxDragKind::ResizeTopLeft;
    }
    if (drag_kind == "resize_top_right") {
        return AnnotateBoxDragKind::ResizeTopRight;
    }
    if (drag_kind == "resize_bottom_left") {
        return AnnotateBoxDragKind::ResizeBottomLeft;
    }
    if (drag_kind == "resize_bottom_right") {
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

ToolSelectIntent parse_tool_select_intent(const std::string_view intent_name, const nlohmann::json& payload) {
    const auto tool_it = payload.find("tool");
    if (tool_it == payload.end() || !tool_it->is_string()) {
        throw_intent_payload_error(intent_name, "payload.tool must be a string");
    }

    ToolSelectIntent routed;
    tool_it->get_to(routed.tool);
    routed.options = nlohmann::json::object();
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        if (it.key() != "tool") {
            routed.options[it.key()] = *it;
        }
    }
    return routed;
}

LivePreviewControlIntent parse_live_preview_control_intent(const std::string_view intent_name,
                                                           const nlohmann::json& payload) {
    LivePreviewControlIntent routed;
    const bool enabled = parse_required_bool(intent_name, payload, "enabled");
    for (const auto target : LivePreviewControlIntent::api_intent_targets()) {
        if (target.id != intent_name) {
            continue;
        }
        switch (target.target) {
            case LivePreviewControlTarget::FitToCapture:
                routed.fit_to_capture = enabled;
                return routed;
            case LivePreviewControlTarget::CropOverlayMode:
                routed.crop_overlay_mode = enabled;
                return routed;
        }
    }
    throw_invalid_intent(intent_name);
}

void validate_settings_patch_value(const std::string_view intent_name, const nlohmann::json& value, std::string& path) {
    if (!settings_patch_path_is_known_prefix_or_leaf(path)) {
        throw_intent_payload_error(intent_name, "unsupported settings patch path `" + path + "`");
    }

    if (value.is_object()) {
        const std::size_t parent_size = path.size();
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (path.size() != 0U) {
                path.push_back('.');
            }
            path += it.key();
            validate_settings_patch_value(intent_name, *it, path);
            path.resize(parent_size);
        }
        return;
    }

    if (!settings_patch_path_is_known_leaf(path)) {
        throw_intent_payload_error(intent_name, "unsupported settings patch leaf `" + path + "`");
    }

    const std::string_view value_type = settings_patch_value_type(path);
    if (value_type.empty()) {
        throw_intent_payload_error(intent_name, "unsupported settings patch value type for `" + path + "`");
    }

    if (value_type == "workflow") {
        if (!value.is_string() || !workflow_id_is_known(value.get<std::string>())) {
            throw_intent_payload_error(intent_name, "payload.patch.current_view must be a generated workflow id");
        }
        return;
    }

    if (value_type == "preset") {
        if (!value.is_string() || !preset_name_is_known(value.get<std::string>())) {
            throw_intent_payload_error(intent_name, "payload.patch." + path + " must be a generated RF-DETR preset");
        }
        return;
    }

    if (value_type == "string") {
        if (!value.is_string()) {
            throw_intent_payload_error(intent_name, "payload.patch." + path + " must be a string");
        }
        return;
    }

    if (value_type == "number") {
        if (!value.is_number()) {
            throw_intent_payload_error(intent_name, "payload.patch." + path + " must be a number");
        }
        return;
    }

    if (settings_numeric_enum_value_type_is_known(value_type)) {
        if (!settings_numeric_enum_value_is_allowed(value_type, value)) {
            throw_intent_payload_error(
                intent_name, "payload.patch." + path + " must be a generated `" + std::string(value_type) + "` value");
        }
        return;
    }

    if (value_type == "boolean") {
        if (!value.is_boolean()) {
            throw_intent_payload_error(intent_name, "payload.patch." + path + " must be a boolean");
        }
        return;
    }

    if (value_type == "number_array") {
        if (!value.is_array()) {
            throw_intent_payload_error(intent_name, "payload.patch." + path + " must be a numeric array");
        }
        for (const nlohmann::json& entry : value) {
            if (!entry.is_number()) {
                throw_intent_payload_error(intent_name, "payload.patch." + path + " must be a numeric array");
            }
        }
        return;
    }

    if (value_type == "boolean_array") {
        if (!value.is_array()) {
            throw_intent_payload_error(intent_name, "payload.patch." + path + " must be a boolean array");
        }
        for (const nlohmann::json& entry : value) {
            if (!entry.is_boolean()) {
                throw_intent_payload_error(intent_name, "payload.patch." + path + " must be a boolean array");
            }
        }
        return;
    }

    throw_intent_payload_error(intent_name, "unsupported settings patch value type `" + std::string{value_type} + "`");
}

SettingsUpdateIntent parse_settings_update_intent(const std::string_view intent_name, const nlohmann::json& payload) {
    SettingsUpdateIntent routed;
    std::string path;
    const auto patch_it = payload.find("patch");
    if (patch_it != payload.end()) {
        if (!patch_it->is_object()) {
            throw_intent_payload_error(intent_name, "payload.patch must be a JSON object");
        }
        routed.patch = *patch_it;
        validate_settings_patch_value(intent_name, routed.patch, path);
        return routed;
    }
    routed.patch = payload;
    validate_settings_patch_value(intent_name, routed.patch, path);
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

[[nodiscard]] TrainStartIntent parse_native_intent_payload(std::type_identity<TrainStartIntent>, std::string_view,
                                                           const nlohmann::json& payload) {
    return parse_train_start_intent(payload);
}

[[nodiscard]] TrainRemoteStartIntent parse_native_intent_payload(std::type_identity<TrainRemoteStartIntent>,
                                                                 std::string_view, const nlohmann::json& payload) {
    return parse_train_remote_start_intent(payload);
}

[[nodiscard]] PredictStartIntent parse_native_intent_payload(std::type_identity<PredictStartIntent>, std::string_view,
                                                             const nlohmann::json& payload) {
    return parse_predict_start_intent(payload);
}

[[nodiscard]] ValidateStartIntent parse_native_intent_payload(std::type_identity<ValidateStartIntent>, std::string_view,
                                                              const nlohmann::json& payload) {
    return parse_validate_start_intent(payload);
}

[[nodiscard]] ExportStartIntent parse_native_intent_payload(std::type_identity<ExportStartIntent>, std::string_view,
                                                            const nlohmann::json& payload) {
    return parse_export_start_intent(payload);
}

[[nodiscard]] AnnotateSaveIntent parse_native_intent_payload(std::type_identity<AnnotateSaveIntent>, std::string_view,
                                                             const nlohmann::json& payload) {
    return parse_annotate_save_intent(payload);
}

[[nodiscard]] AnnotateSetupIntent parse_native_intent_payload(std::type_identity<AnnotateSetupIntent>,
                                                              const std::string_view intent_name,
                                                              const nlohmann::json& payload) {
    return parse_annotate_setup_intent(intent_name, payload);
}

[[nodiscard]] AnnotateSidebarIntent parse_native_intent_payload(std::type_identity<AnnotateSidebarIntent>,
                                                                const std::string_view intent_name,
                                                                const nlohmann::json& payload) {
    return parse_annotate_sidebar_intent(intent_name, payload);
}

[[nodiscard]] AnnotateWorkspaceBoxDragIntent parse_native_intent_payload(
    std::type_identity<AnnotateWorkspaceBoxDragIntent>, const std::string_view intent_name,
    const nlohmann::json& payload) {
    return parse_annotate_workspace_box_drag_intent(intent_name, payload);
}

[[nodiscard]] AnnotateWorkspaceHandleDragIntent parse_native_intent_payload(
    std::type_identity<AnnotateWorkspaceHandleDragIntent>, const std::string_view intent_name,
    const nlohmann::json& payload) {
    return parse_annotate_workspace_handle_drag_intent(intent_name, payload);
}

[[nodiscard]] ToolSelectIntent parse_native_intent_payload(std::type_identity<ToolSelectIntent>,
                                                           const std::string_view intent_name,
                                                           const nlohmann::json& payload) {
    return parse_tool_select_intent(intent_name, payload);
}

[[nodiscard]] LivePreviewControlIntent parse_native_intent_payload(std::type_identity<LivePreviewControlIntent>,
                                                                   const std::string_view intent_name,
                                                                   const nlohmann::json& payload) {
    return parse_live_preview_control_intent(intent_name, payload);
}

[[nodiscard]] SettingsUpdateIntent parse_native_intent_payload(std::type_identity<SettingsUpdateIntent>,
                                                               const std::string_view intent_name,
                                                               const nlohmann::json& payload) {
    return parse_settings_update_intent(intent_name, payload);
}

[[nodiscard]] CropCommitIntent parse_native_intent_payload(std::type_identity<CropCommitIntent>,
                                                           const std::string_view intent_name,
                                                           const nlohmann::json& payload) {
    return parse_crop_commit_intent(intent_name, payload);
}

[[nodiscard]] ViewportCommitIntent parse_native_intent_payload(std::type_identity<ViewportCommitIntent>,
                                                               const std::string_view intent_name,
                                                               const nlohmann::json& payload) {
    return parse_viewport_commit_intent(intent_name, payload);
}

template <typename Payload>
[[nodiscard]] Payload parse_typed_intent_payload(const std::string_view intent_name, const nlohmann::json& payload) {
    if constexpr (requires { parse_native_intent_payload(std::type_identity<Payload>{}, intent_name, payload); }) {
        return parse_native_intent_payload(std::type_identity<Payload>{}, intent_name, payload);
    } else {
        Payload routed{};
        api::from_json_reflected(payload, routed);
        return routed;
    }
}

struct NativeIntentRouteEntry {
    std::string_view id;
    std::array<Workflow, 6U> workflows{};
    std::size_t workflow_count = 0U;
    RoutedIntentPayload (*parse)(std::string_view, const nlohmann::json&) = nullptr;
};

[[nodiscard]] bool workflow_allowed(const NativeIntentRouteEntry& route, const Workflow workflow) noexcept {
    for (std::size_t i = 0U; i < route.workflow_count; ++i) {
        const Workflow allowed_workflow = route.workflows[i];
        if (allowed_workflow == workflow) {
            return true;
        }
    }
    return false;
}

template <typename Payload>
[[nodiscard]] RoutedIntentPayload parse_payload_variant(const std::string_view intent_name,
                                                        const nlohmann::json& payload) {
    return RoutedIntentPayload{parse_typed_intent_payload<Payload>(intent_name, payload)};
}

template <typename Payload, typename Descriptor>
[[nodiscard]] constexpr NativeIntentRouteEntry native_route_entry(const Descriptor& descriptor) {
    NativeIntentRouteEntry entry;
    entry.id = descriptor.id;
    entry.workflow_count = descriptor.workflows.workflows.size();
    for (std::size_t i = 0U; i < entry.workflow_count; ++i) {
        entry.workflows[i] = descriptor.workflows.workflows[i];
    }
    entry.parse = &parse_payload_variant<Payload>;
    return entry;
}

template <typename Payload, std::size_t N>
constexpr void append_native_route_entries(std::array<NativeIntentRouteEntry, N>& routes, std::size_t& index) {
    std::apply([&](const auto&... descriptors) { ((routes[index++] = native_route_entry<Payload>(descriptors)), ...); },
               api::payload_intent_descriptors<Payload>::values());
}

template <typename TypeList>
struct NativeIntentRouteBuilder;

template <typename... Payloads>
struct NativeIntentRouteBuilder<api::type_list<Payloads...>> {
    [[nodiscard]] static consteval auto routes() {
        std::array<NativeIntentRouteEntry, api::intent_metadata<BrowserIntentPayloadTypes>::intent_count()> out{};
        std::size_t index = 0U;
        (append_native_route_entries<Payloads>(out, index), ...);
        for (std::size_t i = 1U; i < out.size(); ++i) {
            const NativeIntentRouteEntry current = out[i];
            std::size_t j = i;
            while (j > 0U && current.id < out[j - 1U].id) {
                out[j] = out[j - 1U];
                --j;
            }
            out[j] = current;
        }
        return out;
    }
};

inline constexpr auto kNativeIntentRoutes = NativeIntentRouteBuilder<BrowserIntentPayloadTypes>::routes();

[[nodiscard]] bool route_native_intent(const std::string_view intent_name, const Workflow workflow,
                                       const nlohmann::json& payload, RoutedIntentPayload& routed_payload) {
    const auto found = std::lower_bound(
        kNativeIntentRoutes.begin(), kNativeIntentRoutes.end(), intent_name,
        [](const NativeIntentRouteEntry& route, const std::string_view needle) { return route.id < needle; });
    if (found == kNativeIntentRoutes.end() || found->id != intent_name) {
        return false;
    }
    if (!workflow_allowed(*found, workflow)) {
        throw_intent_payload_error(intent_name,
                                   "intent is not allowed for workflow `" + std::string(workflow_name(workflow)) + "`");
    }
    routed_payload = found->parse(intent_name, payload);
    return true;
}

[[nodiscard]] const BrowserNativeFileDialogContractSpec* file_dialog_contract_for_id(
    const std::string_view dialog_id) noexcept {
    const auto found =
        std::lower_bound(kBrowserNativeFileDialogsById.begin(), kBrowserNativeFileDialogsById.end(), dialog_id,
                         [](const BrowserNativeFileDialogContractSpec& spec, const std::string_view needle) {
                             return spec.id < needle;
                         });
    return found != kBrowserNativeFileDialogsById.end() && found->id == dialog_id ? &*found : nullptr;
}

[[nodiscard]] Workflow file_dialog_owner_workflow(const Workflow workflow) noexcept {
    return workflow == Workflow::Live ? Workflow::Predict : workflow;
}

void validate_file_dialog_request(const std::string_view intent_name, const Workflow workflow,
                                  const FileDialogRequestIntent& request) {
    const BrowserNativeFileDialogContractSpec* spec = file_dialog_contract_for_id(request.dialog_id);
    if (spec == nullptr) {
        throw_intent_payload_error(intent_name, "payload.dialog_id is not a generated file dialog id");
    }
    if (spec->workflow != file_dialog_owner_workflow(workflow)) {
        throw_intent_payload_error(
            intent_name, "payload.dialog_id is not owned by workflow `" + std::string(workflow_name(workflow)) + "`");
    }
    if (spec->mode != request.mode) {
        throw_intent_payload_error(
            intent_name, "payload.mode does not match generated file dialog mode for `" + request.dialog_id + "`");
    }
    if (spec->field != std::string_view{request.target_field}) {
        throw_intent_payload_error(
            intent_name,
            "payload.target_field does not match generated file dialog target for `" + request.dialog_id + "`");
    }
}

}  

std::string_view workflow_name(const Workflow workflow) noexcept {
    return api::enum_name(workflow);
}

Workflow workflow_from_name(const std::string_view name) {
    try {
        return api::enum_from_name<Workflow>(name);
    } catch (const std::runtime_error&) {
        throw_invalid_enum("workflow", name);
    }
}

std::string_view source_kind_name(const SourceKind kind) noexcept {
    return api::enum_name(kind);
}

SourceKind source_kind_from_name(const std::string_view name) {
    try {
        return api::enum_from_name<SourceKind>(name);
    } catch (const std::runtime_error&) {
        throw_invalid_enum("source kind", name);
    }
}

std::string_view browser_host_backend_name(const BrowserHostBackend backend) noexcept {
    return api::enum_name(backend);
}

BrowserHostBackend browser_host_backend_from_name(const std::string_view name) {
    try {
        return api::enum_from_name<BrowserHostBackend>(name);
    } catch (const std::runtime_error&) {
        throw_invalid_enum("browser host backend", name);
    }
}

std::string_view browser_runtime_capability_status_name(const BrowserRuntimeCapabilityStatus status) noexcept {
    return api::enum_name(status);
}

BrowserRuntimeCapabilityStatus browser_runtime_capability_status_from_name(const std::string_view name) {
    try {
        return api::enum_from_name<BrowserRuntimeCapabilityStatus>(name);
    } catch (const std::runtime_error&) {
        throw_invalid_enum("browser runtime capability status", name);
    }
}

std::string_view browser_bridge_phase_name(const BrowserBridgePhase phase) noexcept {
    return api::enum_name(phase);
}

BrowserBridgePhase browser_bridge_phase_from_name(const std::string_view name) {
    try {
        return api::enum_from_name<BrowserBridgePhase>(name);
    } catch (const std::runtime_error&) {
        throw_invalid_enum("browser bridge phase", name);
    }
}

std::string_view artifact_operation_phase_name(const ArtifactOperationPhase phase) noexcept {
    return api::enum_name(phase);
}

ArtifactOperationPhase artifact_operation_phase_from_name(const std::string_view name) {
    try {
        return api::enum_from_name<ArtifactOperationPhase>(name);
    } catch (const std::runtime_error&) {
        throw_invalid_enum("artifact operation phase", name);
    }
}

std::string_view file_dialog_mode_name(const FileDialogMode mode) noexcept {
    return api::enum_name(mode);
}

FileDialogMode file_dialog_mode_from_name(const std::string_view name) {
    try {
        return api::enum_from_name<FileDialogMode>(name);
    } catch (const std::runtime_error&) {
        throw_invalid_enum("file dialog mode", name);
    }
}

void to_json(nlohmann::json& j, const CaptureRegion& region) {
    api::to_json_reflected(j, region);
}

void from_json(const nlohmann::json& j, CaptureRegion& region) {
    api::from_json_reflected(j, region);
}

void to_json(nlohmann::json& j, const JobLogEntry& entry) {
    api::to_json_reflected(j, entry);
}

void from_json(const nlohmann::json& j, JobLogEntry& entry) {
    api::from_json_reflected(j, entry);
}

void to_json(nlohmann::json& j, const JobState& state) {
    api::to_json_reflected(j, state);
}

void from_json(const nlohmann::json& j, JobState& state) {
    api::from_json_reflected(j, state);
}

void to_json(nlohmann::json& j, const SourceMetadata& metadata) {
    api::to_json_reflected(j, metadata);
}

void from_json(const nlohmann::json& j, SourceMetadata& metadata) {
    api::from_json_reflected(j, metadata);
}

void to_json(nlohmann::json& j, const AnnotationDocumentState& state) {
    api::to_json_reflected(j, state);
}

void from_json(const nlohmann::json& j, AnnotationDocumentState& state) {
    api::from_json_reflected(j, state);
}

void to_json(nlohmann::json& j, const WorkspaceSurfaceInfo& surface_info) {
    api::to_json_reflected(j, surface_info);
}

void from_json(const nlohmann::json& j, WorkspaceSurfaceInfo& surface_info) {
    surface_info = WorkspaceSurfaceInfo{};
    get_optional_alias(j, "generation", "generation", surface_info.generation);
    get_optional_alias(j, "capacity_width", "capacityWidth", surface_info.capacity_width);
    get_optional_alias(j, "capacity_height", "capacityHeight", surface_info.capacity_height);
    get_optional_alias(j, "slot_count", "slotCount", surface_info.slot_count);
    get_optional_alias(j, "format", "format", surface_info.format);
    get_optional_alias(j, "orientation", "orientation", surface_info.orientation);
    get_optional_alias(j, "source_kind", "sourceKind", surface_info.source_kind);
    get_optional_alias(j, "ready", "ready", surface_info.ready);
}

void to_json(nlohmann::json& j, const BrowserRuntimeCapabilities& capabilities) {
    api::to_json_reflected(j, capabilities);
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

void to_json(nlohmann::json& j, const ArtifactSplitState& state) {
    j = nlohmann::json{{"path", state.path},
                       {"image_count", state.image_count},
                       {"width", state.width},
                       {"height", state.height},
                       {"channels", state.channels},
                       {"class_count", state.class_count},
                       {"class_names", state.class_names}};
}

void from_json(const nlohmann::json& j, ArtifactSplitState& state) {
    get_optional(j, "path", state.path);
    get_optional(j, "image_count", state.image_count);
    get_optional(j, "width", state.width);
    get_optional(j, "height", state.height);
    get_optional(j, "channels", state.channels);
    get_optional(j, "class_count", state.class_count);
    get_optional(j, "class_names", state.class_names);
}

void to_json(nlohmann::json& j, const DatasetArtifactState& state) {
    api::to_json_reflected(j, state);
}

void from_json(const nlohmann::json& j, DatasetArtifactState& state) {
    api::from_json_reflected(j, state);
}

void to_json(nlohmann::json& j, const WeightArtifactState& state) {
    j = nlohmann::json{{"phase", std::string(artifact_operation_phase_name(state.phase))},
                       {"status", std::string(api::enum_name(state.status))},
                       {"generation", state.generation},
                       {"preset_name", state.preset_name},
                       {"path", state.path},
                       {"error", state.error},
                       {"downloaded_bytes", state.downloaded_bytes},
                       {"total_bytes", state.total_bytes},
                       {"attempt", state.attempt},
                       {"retry_after_ms", state.retry_after_ms},
                       {"resumable", state.resumable}};
}

void from_json(const nlohmann::json& j, WeightArtifactState& state) {
    if (const auto it = j.find("phase"); it != j.end() && !it->is_null()) {
        state.phase = artifact_operation_phase_from_name(it->get<std::string>());
    }
    if (const auto it = j.find("status"); it != j.end() && !it->is_null()) {
        state.status = api::enum_from_name<WeightArtifactStatus>(it->get<std::string>());
    }
    get_optional(j, "generation", state.generation);
    get_optional(j, "preset_name", state.preset_name);
    get_optional(j, "path", state.path);
    get_optional(j, "error", state.error);
    get_optional(j, "downloaded_bytes", state.downloaded_bytes);
    get_optional(j, "total_bytes", state.total_bytes);
    get_optional(j, "attempt", state.attempt);
    get_optional(j, "retry_after_ms", state.retry_after_ms);
    get_optional(j, "resumable", state.resumable);
}

void to_json(nlohmann::json& j, const ArtifactState& state) {
    j = nlohmann::json{{"dataset", state.dataset}, {"weight", state.weight}};
}

void from_json(const nlohmann::json& j, ArtifactState& state) {
    get_optional(j, "dataset", state.dataset);
    get_optional(j, "weight", state.weight);
}

void to_json(nlohmann::json& j, const FileDialogState& state) {
    api::to_json_reflected(j, state);
}

void from_json(const nlohmann::json& j, FileDialogState& state) {
    api::from_json_reflected(j, state);
}

void to_json(nlohmann::json& j, const ModelPreflightState& state) {
    api::to_json_reflected(j, state);
}

void from_json(const nlohmann::json& j, ModelPreflightState& state) {
    api::from_json_reflected(j, state);
}

void to_json(nlohmann::json& j, const StateSnapshot& snapshot) {
    j = nlohmann::json{
        {"type", kStateSnapshotMessageType},
        {"protocol_version", snapshot.protocol_version},
        {"contract_hash", snapshot.contract_hash},
        {"state_revision", snapshot.state_revision},
        {"active_workflow", workflow_name(snapshot.active_workflow)},
        {"workflow_state", snapshot.workflow_state},
        {"settings_state", snapshot.settings_state},
        {"job", snapshot.job},
        {"source", snapshot.source},
        {"annotation", snapshot.annotation},
        {"runtime_capabilities", snapshot.runtime_capabilities},
        {"artifacts", snapshot.artifacts},
        {"file_dialog", snapshot.file_dialog},
        {"model_preflight", snapshot.model_preflight},
        {"workspace_surface", nullptr},
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
    snapshot.contract_hash = std::string(kBrowserUiContractHash);
    get_optional(j, "contract_hash", snapshot.contract_hash);
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
    if (j.contains("artifacts")) {
        j.at("artifacts").get_to(snapshot.artifacts);
    }
    if (j.contains("file_dialog")) {
        j.at("file_dialog").get_to(snapshot.file_dialog);
    }
    if (j.contains("model_preflight")) {
        j.at("model_preflight").get_to(snapshot.model_preflight);
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
    const std::size_t index = payload.index();
    return index < kDefaultRoutedIntentNamesByPayloadIndex.size() ? kDefaultRoutedIntentNamesByPayloadIndex[index]
                                                                  : std::string_view{};
}

std::string_view routed_intent_name(const RoutedIntent& routed) noexcept {
    return routed.intent.empty() ? routed_intent_name(routed.payload) : std::string_view{routed.intent};
}

RoutedIntent route_intent(const IntentMessage& intent) {
    validate_protocol_version(intent.protocol_version);
    const std::string_view intent_name = intent.intent;
    const nlohmann::json& payload = require_object_payload(intent);

    RoutedIntent routed;
    routed.protocol_version = intent.protocol_version;
    routed.request_id = intent.request_id;
    routed.workflow = intent.workflow;
    routed.intent = intent.intent;
    if (!route_native_intent(intent_name, intent.workflow, payload, routed.payload)) {
        throw_invalid_intent(intent_name);
    }
    if (const auto* file_dialog = std::get_if<FileDialogRequestIntent>(&routed.payload); file_dialog != nullptr) {
        validate_file_dialog_request(intent_name, intent.workflow, *file_dialog);
    }
    return routed;
}

}  
