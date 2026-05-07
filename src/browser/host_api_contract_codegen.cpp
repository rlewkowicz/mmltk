#include "browser/browser_contract_metadata.h"
#include "browser/host_api_intents.h"
#include "gui/browser_file_dialog_contract.h"
#define MMLTK_BROWSER_SETTINGS_CONTRACT_METADATA_ONLY 1
#include "gui/browser_settings_contract.h"
#undef MMLTK_BROWSER_SETTINGS_CONTRACT_METADATA_ONLY

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;

struct CodegenOptions {
    bool check = false;
    bool write = false;
    std::filesystem::path openapi_out;
    std::filesystem::path ts_out_dir;
    std::filesystem::path cpp_out;
};

struct CodegenIntentSpec {
    std::string id;
    std::string payload_schema;
    std::vector<std::string> workflows;
};

struct CodegenSettingsPatchValueType {
    std::string path;
    std::string value_type;
};

[[nodiscard]] CodegenOptions parse_args(const int argc, char** argv) {
    CodegenOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--check") {
            options.check = true;
            continue;
        }
        if (arg == "--write") {
            options.write = true;
            continue;
        }
        if (arg == "--openapi-out" && i + 1 < argc) {
            options.openapi_out = argv[++i];
            continue;
        }
        if (arg == "--ts-out-dir" && i + 1 < argc) {
            options.ts_out_dir = argv[++i];
            continue;
        }
        if (arg == "--cpp-out" && i + 1 < argc) {
            options.cpp_out = argv[++i];
            continue;
        }
        throw std::runtime_error("unsupported contract codegen argument: " + std::string(arg));
    }
    if (!options.check && !options.write) {
        options.write = true;
    }
    if (options.check && options.write) {
        throw std::runtime_error("--check and --write are mutually exclusive");
    }
    return options;
}

[[nodiscard]] bool read_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    const std::uintmax_t byte_count = std::filesystem::file_size(path);
    if (byte_count > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("contract artifact is too large to read: " + path.string());
    }

    out.clear();
    out.resize(static_cast<std::size_t>(byte_count));
    std::size_t offset = 0U;
    constexpr auto max_chunk = static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max());
    while (offset < out.size()) {
        const std::size_t remaining = out.size() - offset;
        const auto chunk = static_cast<std::streamsize>(std::min(remaining, max_chunk));
        stream.read(out.data() + offset, chunk);
        const std::streamsize bytes_read = stream.gcount();
        if (bytes_read <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(bytes_read);
    }
    return true;
}

void write_file_if_changed(const std::filesystem::path& path, const std::string& content) {
    std::string existing;
    if (read_file(path, existing) && existing == content) {
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("failed to open generated contract artifact for writing: " + path.string());
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

[[nodiscard]] bool check_file(const std::filesystem::path& path, const std::string& content,
                              const std::string_view label) {
    std::string existing;
    if (!read_file(path, existing) || existing != content) {
        std::cerr << "Generated contract artifact is stale: " << label << " -> " << path
                  << " (native sources: src/browser/browser_contract_metadata.h, "
                     "src/browser/host_api_intents.h, src/gui/browser_file_dialog_contract.h, "
                     "src/gui/browser_settings_contract.h)\n";
        return false;
    }
    return true;
}

[[nodiscard]] std::string stable_contract_hash(const std::string_view input) {
    std::array<std::uint64_t, 4U> state{
        0xcbf29ce484222325ULL,
        0x84222325cbf29ce4ULL,
        0x9e3779b97f4a7c15ULL,
        0x3c79ac492ba7b653ULL,
    };
    for (const unsigned char byte : input) {
        state[0] ^= byte;
        state[0] *= 0x100000001b3ULL;
        state[1] ^= state[0] + byte;
        state[1] = (state[1] << 7U) | (state[1] >> 57U);
        state[1] *= 0x9e3779b185ebca87ULL;
        state[2] ^= state[1] + (static_cast<std::uint64_t>(byte) << 17U);
        state[2] = (state[2] << 13U) | (state[2] >> 51U);
        state[2] *= 0xc2b2ae3d27d4eb4fULL;
        state[3] ^= state[2] + state[0];
        state[3] = (state[3] << 31U) | (state[3] >> 33U);
        state[3] *= 0x165667b19e3779f9ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const std::uint64_t word : state) {
        out << std::setw(16) << word;
    }
    return out.str();
}

[[nodiscard]] Json string_array(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const std::string& value : values) {
        out.push_back(value);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> workflow_ids() {
    std::vector<std::string> out;
    out.reserve(mmltk::browser::contract::kWorkflows.size());
    for (const auto& workflow : mmltk::browser::contract::kWorkflows) {
        out.emplace_back(workflow.id);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> preset_names() {
    std::vector<std::string> out;
    out.reserve(mmltk::browser::contract::kPresets.size());
    for (const auto& preset : mmltk::browser::contract::kPresets) {
        out.emplace_back(preset.preset_name);
    }
    return out;
}

[[nodiscard]] std::string file_dialog_workflow_id(const mmltk::browser::BrowserNativeFileDialogContractSpec& dialog) {
    return std::string(mmltk::browser::api::enum_name(dialog.workflow));
}

[[nodiscard]] std::string file_dialog_mode_id(const mmltk::browser::BrowserNativeFileDialogContractSpec& dialog) {
    return std::string(mmltk::browser::api::enum_name(dialog.mode));
}

template <typename Payload>
void append_payload_intent_specs(std::vector<CodegenIntentSpec>& specs) {
    std::apply(
        [&](const auto&... descriptors) {
            (([&] {
                 CodegenIntentSpec spec;
                 spec.id = descriptors.id;
                 spec.payload_schema = descriptors.payload_schema;
                 spec.workflows.reserve(descriptors.workflows.workflows.size());
                 for (const auto workflow : descriptors.workflows.workflows) {
                     spec.workflows.emplace_back(mmltk::browser::api::enum_name(workflow));
                 }
                 specs.push_back(std::move(spec));
             }()),
             ...);
        },
        mmltk::browser::api::payload_intent_descriptors<Payload>::values());
}

template <typename TypeList>
struct CodegenIntentList;

template <typename... Payloads>
struct CodegenIntentList<mmltk::browser::api::type_list<Payloads...>> {
    [[nodiscard]] static std::vector<CodegenIntentSpec> specs() {
        std::vector<CodegenIntentSpec> out;
        out.reserve(mmltk::browser::api::intent_metadata<mmltk::browser::BrowserIntentPayloadTypes>::intent_count());
        (append_payload_intent_specs<Payloads>(out), ...);
        return out;
    }
};

[[nodiscard]] const std::vector<CodegenIntentSpec>& intent_specs() {
    static const std::vector<CodegenIntentSpec> specs =
        CodegenIntentList<mmltk::browser::BrowserIntentPayloadTypes>::specs();
    return specs;
}

[[nodiscard]] std::vector<std::string> intent_ids() {
    const auto& specs = intent_specs();
    std::vector<std::string> out;
    out.reserve(specs.size());
    for (const CodegenIntentSpec& intent : specs) {
        out.push_back(intent.id);
    }
    return out;
}

[[nodiscard]] Json ref_schema(const std::string_view schema_name) {
    return Json{{"$ref", "#/components/schemas/" + std::string(schema_name)}};
}

[[nodiscard]] Json typed_schema(const std::string_view type) {
    return Json{{"type", std::string(type)}};
}

[[nodiscard]] Json enum_schema(const std::string_view type, const std::vector<std::string>& values) {
    Json schema = typed_schema(type);
    schema["enum"] = string_array(values);
    return schema;
}

[[nodiscard]] Json native_numeric_enum_schema(const std::string_view schema_name) {
    for (const auto& schema : mmltk::browser::kBrowserSettingsNumericEnumSchemas) {
        if (schema.schema_name != schema_name) {
            continue;
        }
        Json values = Json::array();
        for (std::size_t i = 0U; i < schema.value_count; ++i) {
            values.push_back(schema.values[i]);
        }
        return Json{{"type", "number"}, {"enum", std::move(values)}};
    }
    throw std::runtime_error("unknown native numeric enum schema: " + std::string(schema_name));
}

[[nodiscard]] Json native_numeric_enum_value_schema(const std::string_view value_type) {
    for (const auto& schema : mmltk::browser::kBrowserSettingsNumericEnumSchemas) {
        if (schema.value_type != value_type) {
            continue;
        }
        return native_numeric_enum_schema(schema.schema_name);
    }
    throw std::runtime_error("unknown native numeric enum settings value type: " + std::string(value_type));
}

[[nodiscard]] bool native_numeric_enum_value_type_is_known(const std::string_view value_type) {
    for (const auto& schema : mmltk::browser::kBrowserSettingsNumericEnumSchemas) {
        if (schema.value_type == value_type) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Json object_schema(Json properties, const bool additional_properties = false) {
    Json schema = {
        {"type", "object"},
        {"additionalProperties", additional_properties},
        {"properties", std::move(properties)},
    };
    return schema;
}

[[nodiscard]] Json array_schema(Json item_schema) {
    return Json{{"type", "array"}, {"items", std::move(item_schema)}};
}

[[nodiscard]] Json settings_leaf_schema(const std::string_view value_type) {
    Json schema;
    if (value_type == "workflow") {
        schema = enum_schema("string", workflow_ids());
    } else if (value_type == "preset") {
        schema = enum_schema("string", preset_names());
    } else if (value_type == "string") {
        schema = typed_schema("string");
    } else if (value_type == "number") {
        schema = typed_schema("number");
    } else if (value_type == "boolean") {
        schema = typed_schema("boolean");
    } else if (value_type == "number_array") {
        schema = array_schema(typed_schema("number"));
    } else if (value_type == "boolean_array") {
        schema = array_schema(typed_schema("boolean"));
    } else if (native_numeric_enum_value_type_is_known(value_type)) {
        schema = native_numeric_enum_value_schema(value_type);
    } else {
        throw std::runtime_error("unsupported settings patch value type in native metadata: " +
                                 std::string(value_type));
    }
    schema["x-mmltk-value-type"] = std::string(value_type);
    return schema;
}

void insert_settings_leaf(Json& root, const std::string_view path, const std::string_view value_type) {
    Json* node = &root;
    std::size_t part_begin = 0U;
    while (part_begin < path.size()) {
        const std::size_t dot = path.find('.', part_begin);
        const bool leaf = dot == std::string_view::npos;
        const std::string part(path.substr(part_begin, leaf ? path.size() - part_begin : dot - part_begin));
        Json& properties = (*node)["properties"];
        if (leaf) {
            properties[part] = settings_leaf_schema(value_type);
            return;
        }
        Json& child = properties[part];
        if (!child.is_object()) {
            child = object_schema(Json::object());
        }
        node = &child;
        part_begin = dot + 1U;
    }
}

[[nodiscard]] Json settings_patch_schema_for_prefix(const std::string_view prefix) {
    Json schema = object_schema(Json::object());
    for (const auto& value_type : mmltk::browser::kBrowserSettingsPatchValueTypes) {
        if (prefix.empty()) {
            insert_settings_leaf(schema, value_type.path, value_type.value_type);
            continue;
        }
        if (value_type.path.starts_with(prefix) && value_type.path.size() > prefix.size() &&
            value_type.path[prefix.size()] == '.') {
            insert_settings_leaf(schema, value_type.path.substr(prefix.size() + 1U), value_type.value_type);
        }
    }
    return schema;
}

[[nodiscard]] Json file_dialog_ids_schema() {
    std::vector<std::string> ids;
    ids.reserve(mmltk::browser::kBrowserNativeFileDialogsById.size());
    for (const auto& dialog : mmltk::browser::kBrowserNativeFileDialogsById) {
        ids.emplace_back(dialog.id);
    }
    return enum_schema("string", ids);
}

[[nodiscard]] Json open_payload_schema() {
    return object_schema(Json::object(), true);
}

inline constexpr std::array<std::string_view, 51U> kNativeSchemaNames{{
    "Workflow",
    "SourceKind",
    "TrainInputMode",
    "ModelInputMode",
    "CompileMode",
    "FileDialogMode",
    "BrowserHostBackend",
    "BrowserRuntimeCapabilityStatus",
    "BrowserBridgePhase",
    "JobLogEntry",
    "JobState",
    "CaptureRegion",
    "SourceMetadata",
    "AnnotationDocumentState",
    "WorkspaceSurfaceInfo",
    "BrowserRuntimeCapabilities",
    "StateSnapshot",
    "SettingsPatch",
    "UiSettingsPatch",
    "TrainWorkflowSettingsPatch",
    "ValidateWorkflowSettingsPatch",
    "PredictWorkflowSettingsPatch",
    "AnnotateWorkflowSettingsPatch",
    "ExportWorkflowSettingsPatch",
    "IntentMessage",
    "SettingsUpdateIntent",
    "FileDialogFilter",
    "FileDialogRequestIntent",
    "PredictStartIntent",
    "EmptyIntentPayload",
    "AnnotateLiveStartIntent",
    "AnnotateLiveStopIntent",
    "TrainStopIntent",
    "TrainRemoteOfferArmIntent",
    "AnnotateHoldSaveIntent",
    "AnnotateBrushRadiusIntent",
    "AnnotateSidebarIntent",
    "AnnotateWorkspaceClickIntent",
    "AnnotateWorkspaceBoxDragIntent",
    "AnnotateWorkspaceHandleRef",
    "AnnotateWorkspaceHandleDragIntent",
    "AnnotateWorkspaceBrushIntent",
    "AnnotateWorkspacePointerIntent",
    "AnnotateWorkspacePointIntent",
    "ToolSelectIntent",
    "LivePreviewControlIntent",
    "WorkspaceRendererEventIntent",
    "WorkspaceSurfaceRect",
    "WorkspaceBoundsIntent",
    "CropCommitIntent",
    "ViewportCommitIntent",
}};

template <std::size_t N>
[[nodiscard]] consteval bool string_views_are_unique(const std::array<std::string_view, N>& values) {
    for (std::size_t i = 0U; i < values.size(); ++i) {
        for (std::size_t j = i + 1U; j < values.size(); ++j) {
            if (values[i] == values[j]) {
                return false;
            }
        }
    }
    return true;
}

static_assert(string_views_are_unique(kNativeSchemaNames), "native browser contract schema names contain duplicates");

[[nodiscard]] bool native_schema_name_is_known(const std::string_view schema_name) {
    return std::find(kNativeSchemaNames.begin(), kNativeSchemaNames.end(), schema_name) != kNativeSchemaNames.end();
}

template <typename T>
void add_reflected_schema(Json& out, const std::string_view schema_name) {
    out[std::string(schema_name)] = mmltk::browser::api::schema_for<T>();
}

void validate_emitted_schema_names(const Json& schemas) {
    if (schemas.size() != kNativeSchemaNames.size()) {
        throw std::runtime_error("native browser contract schema count does not match schema metadata");
    }
    for (const std::string_view schema_name : kNativeSchemaNames) {
        if (!schemas.contains(std::string(schema_name))) {
            throw std::runtime_error("native browser contract schema metadata is missing emitted schema `" +
                                     std::string(schema_name) + "`");
        }
    }
}

[[nodiscard]] Json schemas() {
    using namespace mmltk::browser;

    Json out = Json::object();
    add_reflected_schema<Workflow>(out, "Workflow");
    add_reflected_schema<SourceKind>(out, "SourceKind");
    out["TrainInputMode"] = native_numeric_enum_schema("TrainInputMode");
    out["ModelInputMode"] = native_numeric_enum_schema("ModelInputMode");
    out["CompileMode"] = native_numeric_enum_schema("CompileMode");
    add_reflected_schema<FileDialogMode>(out, "FileDialogMode");
    add_reflected_schema<BrowserHostBackend>(out, "BrowserHostBackend");
    add_reflected_schema<BrowserRuntimeCapabilityStatus>(out, "BrowserRuntimeCapabilityStatus");
    add_reflected_schema<BrowserBridgePhase>(out, "BrowserBridgePhase");
    add_reflected_schema<JobLogEntry>(out, "JobLogEntry");
    add_reflected_schema<JobState>(out, "JobState");
    add_reflected_schema<CaptureRegion>(out, "CaptureRegion");
    add_reflected_schema<SourceMetadata>(out, "SourceMetadata");
    add_reflected_schema<AnnotationDocumentState>(out, "AnnotationDocumentState");
    add_reflected_schema<WorkspaceSurfaceInfo>(out, "WorkspaceSurfaceInfo");
    add_reflected_schema<BrowserRuntimeCapabilities>(out, "BrowserRuntimeCapabilities");
    add_reflected_schema<StateSnapshot>(out, "StateSnapshot");
    out["StateSnapshot"]["properties"]["type"] = enum_schema("string", {"state.snapshot"});
    out["SettingsPatch"] = settings_patch_schema_for_prefix("");
    out["UiSettingsPatch"] = settings_patch_schema_for_prefix("ui");
    out["TrainWorkflowSettingsPatch"] = settings_patch_schema_for_prefix("workflows.train");
    out["ValidateWorkflowSettingsPatch"] = settings_patch_schema_for_prefix("workflows.validate");
    out["PredictWorkflowSettingsPatch"] = settings_patch_schema_for_prefix("workflows.predict");
    out["AnnotateWorkflowSettingsPatch"] = settings_patch_schema_for_prefix("workflows.annotate");
    out["ExportWorkflowSettingsPatch"] = settings_patch_schema_for_prefix("workflows.export");
    add_reflected_schema<IntentMessage>(out, "IntentMessage");
    out["IntentMessage"]["properties"]["type"] = enum_schema("string", {"intent"});
    out["IntentMessage"]["properties"]["intent"] = enum_schema("string", intent_ids());
    add_reflected_schema<SettingsUpdateIntent>(out, "SettingsUpdateIntent");
    out["SettingsUpdateIntent"]["properties"]["patch"] = ref_schema("SettingsPatch");
    add_reflected_schema<FileDialogFilter>(out, "FileDialogFilter");
    add_reflected_schema<FileDialogRequestIntent>(out, "FileDialogRequestIntent");
    out["FileDialogRequestIntent"]["properties"]["dialog_id"] = file_dialog_ids_schema();
    add_reflected_schema<PredictStartIntentPayload>(out, "PredictStartIntent");
    add_reflected_schema<EmptyIntentPayload>(out, "EmptyIntentPayload");
    add_reflected_schema<AnnotateLiveStartIntentPayload>(out, "AnnotateLiveStartIntent");
    add_reflected_schema<AnnotateLiveStopIntentPayload>(out, "AnnotateLiveStopIntent");
    add_reflected_schema<TrainStopIntent>(out, "TrainStopIntent");
    add_reflected_schema<TrainRemoteOfferArmIntent>(out, "TrainRemoteOfferArmIntent");
    add_reflected_schema<AnnotateHoldSaveIntent>(out, "AnnotateHoldSaveIntent");
    add_reflected_schema<AnnotateBrushRadiusIntent>(out, "AnnotateBrushRadiusIntent");
    add_reflected_schema<AnnotateSidebarIntent>(out, "AnnotateSidebarIntent");
    add_reflected_schema<AnnotateWorkspaceClickIntent>(out, "AnnotateWorkspaceClickIntent");
    add_reflected_schema<AnnotateWorkspaceBoxDragIntent>(out, "AnnotateWorkspaceBoxDragIntent");
    add_reflected_schema<AnnotateWorkspaceHandleRef>(out, "AnnotateWorkspaceHandleRef");
    add_reflected_schema<AnnotateWorkspaceHandleDragIntent>(out, "AnnotateWorkspaceHandleDragIntent");
    add_reflected_schema<AnnotateWorkspaceBrushIntent>(out, "AnnotateWorkspaceBrushIntent");
    add_reflected_schema<AnnotateWorkspacePointerIntent>(out, "AnnotateWorkspacePointerIntent");
    add_reflected_schema<AnnotateWorkspaceFillIntent>(out, "AnnotateWorkspacePointIntent");
    add_reflected_schema<ToolSelectIntentPayload>(out, "ToolSelectIntent");
    add_reflected_schema<LivePreviewControlIntentPayload>(out, "LivePreviewControlIntent");
    add_reflected_schema<WorkspaceRendererEventIntent>(out, "WorkspaceRendererEventIntent");
    add_reflected_schema<WorkspaceSurfaceRect>(out, "WorkspaceSurfaceRect");
    add_reflected_schema<WorkspaceBoundsIntent>(out, "WorkspaceBoundsIntent");
    add_reflected_schema<CropCommitIntent>(out, "CropCommitIntent");
    add_reflected_schema<ViewportCommitIntent>(out, "ViewportCommitIntent");
    validate_emitted_schema_names(out);
    return out;
}

[[nodiscard]] Json workflows_json() {
    Json out = Json::array();
    for (const auto& workflow : mmltk::browser::contract::kWorkflows) {
        out.push_back(Json{{"id", std::string(workflow.id)}, {"label", std::string(workflow.label)}});
    }
    return out;
}

[[nodiscard]] Json presets_json() {
    Json out = Json::array();
    for (const auto& preset : mmltk::browser::contract::kPresets) {
        out.push_back(
            Json{{"presetName", std::string(preset.preset_name)}, {"displayName", std::string(preset.display_name)}});
    }
    return out;
}

[[nodiscard]] Json file_dialogs_json() {
    Json out = Json::array();
    for (const auto& dialog : mmltk::browser::kBrowserNativeFileDialogsById) {
        Json filters = Json::array();
        if (dialog.filter_pattern_count > 0U) {
            Json patterns = Json::array();
            for (std::size_t i = 0U; i < dialog.filter_pattern_count; ++i) {
                patterns.push_back(std::string(dialog.filter_patterns[i]));
            }
            filters.push_back(Json{{"name", std::string(dialog.filter_name)}, {"patterns", std::move(patterns)}});
        }
        out.push_back(Json{{"id", std::string(dialog.id)},
                           {"workflow", file_dialog_workflow_id(dialog)},
                           {"mode", file_dialog_mode_id(dialog)},
                           {"field", std::string(dialog.field)},
                           {"title", std::string(dialog.title)},
                           {"filters", std::move(filters)}});
    }
    return out;
}

[[nodiscard]] Json workflows_for_intent(const CodegenIntentSpec& intent) {
    Json out = Json::array();
    for (const std::string& workflow : intent.workflows) {
        out.push_back(workflow);
    }
    return out;
}

[[nodiscard]] Json intent_payload_schema(const CodegenIntentSpec& intent) {
    if (intent.payload_schema.empty()) {
        return open_payload_schema();
    }
    if (!native_schema_name_is_known(intent.payload_schema)) {
        throw std::runtime_error("intent `" + intent.id + "` references unknown native payload schema `" +
                                 intent.payload_schema + "`");
    }
    return ref_schema(intent.payload_schema);
}

[[nodiscard]] Json intents_json() {
    Json out = Json::array();
    for (const CodegenIntentSpec& intent : intent_specs()) {
        Json spec{{"id", std::string(intent.id)}, {"workflows", workflows_for_intent(intent)}};
        spec["payloadSchema"] = intent.payload_schema.empty() ? Json(nullptr) : ref_schema(intent.payload_schema);
        out.push_back(std::move(spec));
    }
    return out;
}

[[nodiscard]] std::string operation_id(const std::string_view intent_id) {
    std::string out = "post";
    bool capitalize = true;
    for (const char ch : intent_id) {
        if (ch == '.' || ch == '_' || ch == '-') {
            capitalize = true;
            continue;
        }
        if (capitalize && ch >= 'a' && ch <= 'z') {
            out.push_back(static_cast<char>(ch - ('a' - 'A')));
        } else {
            out.push_back(ch);
        }
        capitalize = false;
    }
    return out;
}

[[nodiscard]] Json paths_json() {
    Json paths = Json::object();
    paths["/snapshot"] = Json{
        {"get",
         Json{{"operationId", "getSnapshot"},
              {"responses", Json{{"200", Json{{"description", "Current state snapshot."},
                                              {"content", Json{{"application/json",
                                                                Json{{"schema", ref_schema("StateSnapshot")}}}}}}}}}}}};
    for (const CodegenIntentSpec& intent : intent_specs()) {
        paths["/intents/" + std::string(intent.id)] = Json{
            {"post",
             Json{{"operationId", operation_id(intent.id)},
                  {"x-mmltk-browser-message", true},
                  {"x-mmltk-intent-name", std::string(intent.id)},
                  {"x-mmltk-workflows", workflows_for_intent(intent)},
                  {"requestBody",
                   Json{{"required", true},
                        {"content", Json{{"application/json", Json{{"schema", intent_payload_schema(intent)}}}}}}},
                  {"responses", Json{{"202", Json{{"description", "Intent accepted by the native host bridge."}}}}}}}};
    }
    return paths;
}

[[nodiscard]] Json openapi_document() {
    Json document = {
        {"openapi", "3.1.0"},
        {"info", Json{{"title", "MMLTK Browser Host Bridge API"},
                      {"version", std::to_string(mmltk::browser::contract::kProtocolVersion)}}},
        {"x-mmltk-contract-kind", "browser-host"},
        {"x-mmltk-protocol-version", mmltk::browser::contract::kProtocolVersion},
        {"x-mmltk-workflows", workflows_json()},
        {"x-mmltk-presets", presets_json()},
        {"x-mmltk-file-dialogs", file_dialogs_json()},
        {"x-mmltk-intents", intents_json()},
        {"paths", paths_json()},
        {"components", Json{{"schemas", schemas()}}},
    };
    return document;
}

[[nodiscard]] std::string quote_ts(const std::string_view value) {
    return Json(std::string(value)).dump();
}

[[nodiscard]] std::string generated_header(const std::string& hash) {
    return "// This file is generated by mmltk_browser_host_contract_codegen. Do not edit by hand.\n"
           "// Contract hash: " +
           hash + "\n";
}

[[nodiscard]] std::string render_ts_array(const Json& array) {
    return array.dump(2);
}

[[nodiscard]] std::string render_host_api_ts(const std::string& hash) {
    Json workflows = Json::array();
    for (const auto& workflow : mmltk::browser::contract::kWorkflows) {
        workflows.push_back(std::string(workflow.id));
    }
    Json modes = Json::array({"open_file", "open_folder", "save_file"});
    Json intents = Json::array();
    for (const CodegenIntentSpec& intent : intent_specs()) {
        intents.push_back(std::string(intent.id));
    }

    std::ostringstream out;
    out << generated_header(hash) << '\n';
    out << "export const HOST_API_PROTOCOL_VERSION = " << mmltk::browser::contract::kProtocolVersion << " as const;\n";
    out << "export const HOST_API_CONTRACT_HASH = " << quote_ts(hash) << " as const;\n";
    out << "export const HOST_API_WORKFLOWS = " << render_ts_array(workflows) << " as const;\n";
    out << "export const HOST_API_FILE_DIALOG_MODES = " << render_ts_array(modes) << " as const;\n";
    out << "export const HOST_API_INTENTS = " << render_ts_array(intents) << " as const;\n\n";
    out << "export type HostApiGeneratedWorkflow = (typeof HOST_API_WORKFLOWS)[number];\n";
    out << "export type HostApiGeneratedIntent = (typeof HOST_API_INTENTS)[number];\n";
    out << "export type Workflow = HostApiGeneratedWorkflow;\n";
    out << "export type SourceKind = \"compiled_dataset\" | \"single_image\" | \"image_folder\" | \"video_stream\";\n";
    out << "export type BrowserHostBackend = \"unknown\" | \"cef\";\n";
    out << "export type BrowserRuntimeCapabilityStatus = \"unknown\" | \"available\" | \"unavailable\";\n";
    out << "export type FileDialogMode = (typeof HOST_API_FILE_DIALOG_MODES)[number];\n\n";
    out << "export interface CaptureRegion {\n"
           "  x: number;\n"
           "  y: number;\n"
           "  width: number;\n"
           "  height: number;\n"
           "}\n\n";
    out << "export interface JobLogEntry {\n"
           "  sequence: number;\n"
           "  level: string;\n"
           "  message: string;\n"
           "}\n\n";
    out << "export interface JobState {\n"
           "  running: boolean;\n"
           "  label: string;\n"
           "  summary: string;\n"
           "  error: string;\n"
           "  output_tail: string;\n"
           "  recent_logs: JobLogEntry[];\n"
           "}\n\n";
    out << "export interface SourceMetadata {\n"
           "  kind: SourceKind;\n"
           "  locator: string;\n"
           "  recursive: boolean;\n"
           "  device_index: number;\n"
           "  capture_width: number;\n"
           "  capture_height: number;\n"
           "  capture_fps: number;\n"
           "  v4l2_buffer_count: number;\n"
           "  has_crop: boolean;\n"
           "  crop: CaptureRegion;\n"
           "}\n\n";
    out << "export interface AnnotationDocumentState {\n"
           "  document_generation: number;\n"
           "  session_revision: number;\n"
           "  capture_width: number;\n"
           "  capture_height: number;\n"
           "  instance_count: number;\n"
           "  selected_instance: number | null;\n"
           "}\n\n";
    out << "export interface WorkspaceSurfaceInfo {\n"
           "  surfaceId: \"workspace\";\n"
           "  revision: string;\n"
           "  width: number;\n"
           "  height: number;\n"
           "  textureFormat: \"rgba8unorm\";\n"
           "  opaque: true;\n"
           "  upright: true;\n"
           "}\n\n";
    out << "export interface BrowserRuntimeCapabilities {\n"
           "  host_backend: BrowserHostBackend;\n"
           "  navigator_gpu: BrowserRuntimeCapabilityStatus;\n"
           "  workspace_surface_bridge: BrowserRuntimeCapabilityStatus;\n"
           "  workspace_surface_zero_copy: BrowserRuntimeCapabilityStatus;\n"
           "}\n\n";
    out << "export interface StateSnapshot {\n"
           "  type: \"state.snapshot\";\n"
           "  protocol_version: number;\n"
           "  contract_hash: string;\n"
           "  state_revision: number;\n"
           "  active_workflow: Workflow;\n"
           "  workflow_state: Record<string, unknown>;\n"
           "  settings_state: Record<string, unknown>;\n"
           "  job: JobState;\n"
           "  source: SourceMetadata;\n"
           "  annotation: AnnotationDocumentState;\n"
           "  runtime_capabilities: BrowserRuntimeCapabilities;\n"
           "  workspace_surface?: WorkspaceSurfaceInfo | null;\n"
           "}\n\n";
    out << "export interface IntentMessage {\n"
           "  type: \"intent\";\n"
           "  protocol_version: number;\n"
           "  request_id: number;\n"
           "  workflow: Workflow;\n"
           "  intent: string;\n"
           "  payload: Record<string, unknown>;\n"
           "}\n";
    return out.str();
}

[[nodiscard]] const std::vector<CodegenSettingsPatchValueType>& sorted_settings_patch_value_types() {
    static const std::vector<CodegenSettingsPatchValueType> values = [] {
        std::vector<CodegenSettingsPatchValueType> out;
        out.reserve(mmltk::browser::kBrowserSettingsPatchValueTypes.size());
        for (const auto& value_type : mmltk::browser::kBrowserSettingsPatchValueTypes) {
            out.push_back(
                CodegenSettingsPatchValueType{std::string(value_type.path), std::string(value_type.value_type)});
        }
        std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) { return left.path < right.path; });
        return out;
    }();
    return values;
}

[[nodiscard]] Json settings_patch_value_types_json() {
    Json out = Json::array();
    for (const CodegenSettingsPatchValueType& value_type : sorted_settings_patch_value_types()) {
        out.push_back(Json{{"path", value_type.path}, {"valueType", value_type.value_type}});
    }
    return out;
}

[[nodiscard]] Json settings_patch_paths_json() {
    Json out = Json::array();
    for (const CodegenSettingsPatchValueType& value_type : sorted_settings_patch_value_types()) {
        out.push_back(value_type.path);
    }
    return out;
}

[[nodiscard]] Json intent_payload_schemas_json() {
    Json out = Json::object();
    for (const CodegenIntentSpec& intent : intent_specs()) {
        out[std::string(intent.id)] = intent_payload_schema(intent);
    }
    return out;
}

[[nodiscard]] Json intent_workflows_json() {
    Json out = Json::object();
    for (const CodegenIntentSpec& intent : intent_specs()) {
        out[std::string(intent.id)] = workflows_for_intent(intent);
    }
    return out;
}

[[nodiscard]] std::string render_workflow_contract_ts(const Json& schema_bundle, const std::string& hash) {
    Json workflow_specs = Json::array();
    for (const auto& workflow : mmltk::browser::contract::kWorkflows) {
        workflow_specs.push_back(Json{{"id", std::string(workflow.id)}, {"label", std::string(workflow.label)}});
    }

    std::ostringstream out;
    out << generated_header(hash) << '\n';
    out << "import type { FileDialogMode, Workflow } from \"./host_api\";\n\n";
    out << "export const WORKFLOW_CONTRACT_HASH = " << quote_ts(hash) << " as const;\n\n";
    out << "export type SettingsPatchValueType = \"workflow\" | \"preset\" | \"string\" | \"number\" | \"boolean\" | "
           "\"number_array\" | \"boolean_array\" | \"train_input_mode\" | \"model_input\" | \"compile_mode\";\n";
    out << "export type HostApiJsonSchema = { readonly [key: string]: unknown };\n\n";
    out << "export interface RfDetrPresetContract {\n  readonly presetName: string;\n  readonly displayName: "
           "string;\n}\n\n";
    out << "export interface WorkflowContractSpec {\n  readonly id: Workflow;\n  readonly label: string;\n}\n\n";
    out << "export interface FileDialogFilterContractSpec {\n  readonly name: string;\n  readonly patterns: readonly "
           "string[];\n}\n\n";
    out << "export interface FileDialogContractSpec {\n  readonly id: string;\n  readonly workflow: Workflow;\n  "
           "readonly mode: FileDialogMode;\n  readonly field: string;\n  readonly title: string;\n  readonly filters: "
           "readonly FileDialogFilterContractSpec[];\n}\n\n";
    out << "export interface SettingsPatchValueTypeSpec {\n  readonly path: string;\n  readonly valueType: "
           "SettingsPatchValueType;\n}\n\n";
    out << "export const WORKFLOW_CONTRACT = " << workflow_specs.dump(2)
        << " as const satisfies readonly WorkflowContractSpec[];\n";
    out << "export const RF_DETR_PRESET_OPTIONS = " << presets_json().dump(2)
        << " as const satisfies readonly RfDetrPresetContract[];\n";
    out << "export const FILE_DIALOG_CONTRACT = " << file_dialogs_json().dump(2)
        << " as const satisfies readonly FileDialogContractSpec[];\n";
    out << "export const FILE_DIALOG_IDS = FILE_DIALOG_CONTRACT.map((dialog) => dialog.id);\n";
    out << "export const SETTINGS_PATCH_PATHS = " << settings_patch_paths_json().dump(2) << " as const;\n";
    out << "export const SETTINGS_PATCH_VALUE_TYPES = " << settings_patch_value_types_json().dump(2)
        << " as const satisfies readonly SettingsPatchValueTypeSpec[];\n";
    out << "export const HOST_API_SCHEMA_BUNDLE = " << schema_bundle.dump(2) << " as const;\n";
    out << "export const HOST_API_INTENT_PAYLOAD_SCHEMAS = " << intent_payload_schemas_json().dump(2) << " as const;\n";
    out << "export const HOST_API_INTENT_WORKFLOWS = " << intent_workflows_json().dump(2) << " as const;\n\n";
    out << "export function hostApiSchema(schemaName: keyof typeof HOST_API_SCHEMA_BUNDLE.schemas): HostApiJsonSchema "
           "{\n";
    out << "  return HOST_API_SCHEMA_BUNDLE.schemas[schemaName] as HostApiJsonSchema;\n";
    out << "}\n\n";
    out << "export function hostApiIntentPayloadSchema(intent: string): HostApiJsonSchema | undefined {\n";
    out << "  return (HOST_API_INTENT_PAYLOAD_SCHEMAS as Readonly<Record<string, HostApiJsonSchema>>)[intent];\n";
    out << "}\n";
    return out.str();
}

[[nodiscard]] std::string render_cpp_header(const std::string& hash) {
    std::ostringstream out;
    out << generated_header(hash) << '\n';
    out << "#pragma once\n\n#include <string_view>\n\n";
    out << "namespace mmltk::browser {\n\n";
    out << "inline constexpr std::string_view kBrowserUiContractHash = " << quote_ts(hash) << ";\n";
    out << "\n}  // namespace mmltk::browser\n";
    return out.str();
}

struct GeneratedOutputs {
    Json openapi;
    std::string openapi_text;
    std::string host_api_ts;
    std::string workflow_contract_ts;
    std::string cpp_header;
};

[[nodiscard]] GeneratedOutputs generate_outputs() {
    GeneratedOutputs outputs;
    outputs.openapi = openapi_document();
    const std::string hash = stable_contract_hash(outputs.openapi.dump());
    outputs.openapi["x-mmltk-contract-hash"] = hash;
    outputs.openapi_text = outputs.openapi.dump(2) + "\n";
    const Json schema_bundle = Json{{"schemas", outputs.openapi["components"]["schemas"]},
                                    {"settingsPatchSchema", ref_schema("SettingsPatch")}};
    outputs.host_api_ts = render_host_api_ts(hash);
    outputs.workflow_contract_ts = render_workflow_contract_ts(schema_bundle, hash);
    outputs.cpp_header = render_cpp_header(hash);
    return outputs;
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        const CodegenOptions options = parse_args(argc, argv);
        const GeneratedOutputs outputs = generate_outputs();

        if (options.check) {
            bool ok = true;
            if (!options.openapi_out.empty()) {
                ok = check_file(options.openapi_out, outputs.openapi_text, "host_api.openapi.json") && ok;
            }
            if (!options.ts_out_dir.empty()) {
                ok = check_file(options.ts_out_dir / "host_api.generated.ts", outputs.host_api_ts,
                                "host_api.generated.ts") &&
                     ok;
                ok = check_file(options.ts_out_dir / "workflow_contract.generated.ts", outputs.workflow_contract_ts,
                                "workflow_contract.generated.ts") &&
                     ok;
            }
            if (!options.cpp_out.empty()) {
                ok = check_file(options.cpp_out, outputs.cpp_header, "workflow_contract_generated.h") && ok;
            }
            return ok ? 0 : 1;
        }

        if (!options.openapi_out.empty()) {
            write_file_if_changed(options.openapi_out, outputs.openapi_text);
        }
        if (!options.ts_out_dir.empty()) {
            write_file_if_changed(options.ts_out_dir / "host_api.generated.ts", outputs.host_api_ts);
            write_file_if_changed(options.ts_out_dir / "workflow_contract.generated.ts", outputs.workflow_contract_ts);
        }
        if (!options.cpp_out.empty()) {
            write_file_if_changed(options.cpp_out, outputs.cpp_header);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mmltk_browser_host_contract_codegen: " << error.what() << '\n';
        return 1;
    }
}
