#include "browser/browser_contract_metadata.h"
#include "browser/host_api_intents.h"
#include "gui/browser_file_dialog_contract.h"
#define MMLTK_BROWSER_SETTINGS_CONTRACT_METADATA_ONLY 1
#include "gui/browser_settings_contract.h"
#undef MMLTK_BROWSER_SETTINGS_CONTRACT_METADATA_ONLY

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
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
    std::filesystem::path rust_out;
    std::filesystem::path cpp_out;
};

struct CodegenIntentSpec {
    std::string id;
    std::string payload_schema;
    std::vector<std::string> workflows;
};

inline constexpr std::array<std::string_view, 21U> kBrowserAppFileDialogIds{{
    "annotate.model.weights",
    "annotate.output_dir",
    "annotate.source.image_folder",
    "annotate.source.single_image",
    "export.model.weights",
    "export.output_path",
    "predict.model.weights",
    "predict.output_path",
    "predict.source.image_folder",
    "predict.source.single_image",
    "train.dataset.compiled_directory",
    "train.dataset.source_dir",
    "train.dataset.test_compiled_path",
    "train.dataset.train_compiled_path",
    "train.dataset.val_compiled_path",
    "train.model.weights",
    "train.training.output_dir",
    "train.training.resume_path",
    "validate.dataset.compiled_path",
    "validate.dataset.source_dir",
    "validate.model.weights",
}};

consteval auto browser_app_file_dialogs() {
    std::array<mmltk::browser::BrowserNativeFileDialogContractSpec, kBrowserAppFileDialogIds.size()> result{};
    std::size_t result_size = 0U;
    for (const auto& dialog : mmltk::browser::kBrowserNativeFileDialogsById) {
        for (const std::string_view id : kBrowserAppFileDialogIds) {
            if (dialog.id == id) {
                result[result_size++] = dialog;
                break;
            }
        }
    }
    if (result_size != result.size()) {
        throw "browser app file-dialog ID does not exist in the native contract";
    }
    return result;
}

inline constexpr auto kBrowserAppFileDialogs = browser_app_file_dialogs();

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
        if (arg == "--rust-out" && i + 1 < argc) {
            options.rust_out = argv[++i];
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

inline constexpr std::array<std::string_view, 67U> kNativeSchemaNames{{
    "Workflow",
    "SourceKind",
    "TrainInputMode",
    "ModelInputMode",
    "ModelSelectionSource",
    "CompileMode",
    "FileDialogMode",
    "BrowserHostBackend",
    "BrowserRuntimeCapabilityStatus",
    "BrowserBridgePhase",
    "ArtifactOperationPhase",
    "DatasetCompilePhase",
    "WeightArtifactStatus",
    "FileDialogResultStatus",
    "CustomModelArtifactKind",
    "ModelPreflightStatus",
    "JobLogEntry",
    "JobState",
    "CaptureRegion",
    "SourceMetadata",
    "AnnotationDocumentState",
    "WorkspaceSurfaceInfo",
    "BrowserRuntimeCapabilities",
    "ArtifactSplitState",
    "DatasetArtifactState",
    "WeightArtifactState",
    "ArtifactState",
    "FileDialogState",
    "ModelPreflightState",
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
    "DatasetCompileStartIntent",
    "CustomModelSelectIntent",
    "SettingsResetIntent",
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
    "UiLogIntent",
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
    out["ModelSelectionSource"] = native_numeric_enum_schema("ModelSelectionSource");
    out["CompileMode"] = native_numeric_enum_schema("CompileMode");
    add_reflected_schema<FileDialogMode>(out, "FileDialogMode");
    add_reflected_schema<BrowserHostBackend>(out, "BrowserHostBackend");
    add_reflected_schema<BrowserRuntimeCapabilityStatus>(out, "BrowserRuntimeCapabilityStatus");
    add_reflected_schema<BrowserBridgePhase>(out, "BrowserBridgePhase");
    add_reflected_schema<ArtifactOperationPhase>(out, "ArtifactOperationPhase");
    add_reflected_schema<DatasetCompilePhase>(out, "DatasetCompilePhase");
    add_reflected_schema<WeightArtifactStatus>(out, "WeightArtifactStatus");
    add_reflected_schema<FileDialogResultStatus>(out, "FileDialogResultStatus");
    add_reflected_schema<CustomModelArtifactKind>(out, "CustomModelArtifactKind");
    add_reflected_schema<ModelPreflightStatus>(out, "ModelPreflightStatus");
    add_reflected_schema<JobLogEntry>(out, "JobLogEntry");
    add_reflected_schema<JobState>(out, "JobState");
    add_reflected_schema<CaptureRegion>(out, "CaptureRegion");
    add_reflected_schema<SourceMetadata>(out, "SourceMetadata");
    add_reflected_schema<AnnotationDocumentState>(out, "AnnotationDocumentState");
    add_reflected_schema<WorkspaceSurfaceInfo>(out, "WorkspaceSurfaceInfo");
    add_reflected_schema<BrowserRuntimeCapabilities>(out, "BrowserRuntimeCapabilities");
    add_reflected_schema<ArtifactSplitState>(out, "ArtifactSplitState");
    add_reflected_schema<DatasetArtifactState>(out, "DatasetArtifactState");
    add_reflected_schema<WeightArtifactState>(out, "WeightArtifactState");
    add_reflected_schema<ArtifactState>(out, "ArtifactState");
    add_reflected_schema<FileDialogState>(out, "FileDialogState");
    add_reflected_schema<ModelPreflightState>(out, "ModelPreflightState");
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
    add_reflected_schema<DatasetCompileStartIntent>(out, "DatasetCompileStartIntent");
    add_reflected_schema<CustomModelSelectIntent>(out, "CustomModelSelectIntent");
    add_reflected_schema<SettingsResetIntent>(out, "SettingsResetIntent");
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
    add_reflected_schema<UiLogIntent>(out, "UiLogIntent");
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
        out.push_back(Json{{"presetName", std::string(preset.preset_name)},
                           {"displayName", std::string(preset.display_name)},
                           {"sizeLabel", std::string(preset.size_label)},
                           {"task", std::string(mmltk::rfdetr::model_task_name(preset.task))},
                           {"resolution", preset.resolution},
                           {"canonicalWeightFilename", std::string(preset.canonical_weight_filename)}});
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

[[nodiscard]] std::string quote_string(const std::string_view value) {
    return Json(std::string(value)).dump();
}

[[nodiscard]] std::string rust_pascal_identifier(const std::string_view value) {
    std::string out;
    out.reserve(value.size());
    bool capitalize = true;
    for (const unsigned char ch : value) {
        if (!std::isalnum(ch)) {
            capitalize = true;
            continue;
        }
        if (capitalize && std::isalpha(ch)) {
            out.push_back(static_cast<char>(std::toupper(ch)));
        } else {
            out.push_back(static_cast<char>(ch));
        }
        capitalize = false;
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) {
        out.insert(out.begin(), 'V');
    }
    return out;
}

[[nodiscard]] std::string rust_settings_value_kind(const std::string_view value_type) {
    return rust_pascal_identifier(value_type);
}

[[nodiscard]] std::string generated_header(const std::string& hash) {
    return "// This file is generated by mmltk_browser_host_contract_codegen. Do not edit by hand.\n"
           "// Contract hash: " +
           hash + "\n";
}

[[nodiscard]] std::string render_rust_module(const std::string& hash) {
    std::ostringstream out;
    out << generated_header(hash) << '\n';
    out << "use serde::{Deserialize, Serialize};\n"
           "use serde_json::{Map, Value};\n\n";
    out << "pub const HOST_API_PROTOCOL_VERSION: u32 = " << mmltk::browser::contract::kProtocolVersion << ";\n";
    out << "pub const HOST_API_CONTRACT_HASH: &str = " << quote_string(hash) << ";\n";
    out << '\n';
    out << "pub const WORKFLOW_LABELS: &[(Workflow, &str)] = &[\n";
    for (const auto& workflow : mmltk::browser::contract::kWorkflows) {
        std::string variant(workflow.id);
        variant.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(variant.front())));
        out << "    (Workflow::" << variant << ", " << quote_string(workflow.label) << "),\n";
    }
    out << "];\n\n";
    out << R"rust(#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ModelTask {
    Detection,
    Segmentation,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct RfDetrPresetOption {
    pub preset_name: &'static str,
    pub display_name: &'static str,
    pub size_label: &'static str,
    pub task: ModelTask,
    pub resolution: u32,
    pub canonical_weight_filename: &'static str,
}

)rust";
    out << "pub const RF_DETR_PRESET_OPTIONS: &[RfDetrPresetOption] = &[\n";
    for (const auto& preset : mmltk::browser::contract::kPresets) {
        out << "    RfDetrPresetOption { preset_name: " << quote_string(preset.preset_name)
            << ", display_name: " << quote_string(preset.display_name)
            << ", size_label: " << quote_string(preset.size_label) << ", task: ModelTask::"
            << (preset.task == mmltk::rfdetr::ModelTask::Segmentation ? "Segmentation" : "Detection")
            << ", resolution: " << preset.resolution
            << ", canonical_weight_filename: " << quote_string(preset.canonical_weight_filename) << " },\n";
    }
    out << "];\n\n";
    out << "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]\n"
           "pub enum IntentId {\n";
    for (const CodegenIntentSpec& intent : intent_specs()) {
        out << "    #[serde(rename = " << quote_string(intent.id) << ")]\n    " << rust_pascal_identifier(intent.id)
            << ",\n";
    }
    out << "}\n\nimpl IntentId {\n    pub const fn as_str(self) -> &'static str {\n        match self {\n";
    for (const CodegenIntentSpec& intent : intent_specs()) {
        out << "            Self::" << rust_pascal_identifier(intent.id) << " => " << quote_string(intent.id) << ",\n";
    }
    out << "        }\n    }\n}\n\n";

    out << R"rust(#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileDialogMode {
    OpenFile,
    OpenFolder,
    SaveFile,
}

impl FileDialogMode {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::OpenFile => "open_file",
            Self::OpenFolder => "open_folder",
            Self::SaveFile => "save_file",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FileDialogSpec {
    pub id: &'static str,
    pub workflow: Workflow,
    pub mode: FileDialogMode,
    pub field: &'static str,
    pub title: &'static str,
    pub filter_name: &'static str,
    pub filter_patterns: &'static [&'static str],
}

)rust";
    out << "#[derive(Debug, Clone, Copy, PartialEq, Eq)]\n#[repr(usize)]\npub enum FileDialogId {\n";
    for (const auto& dialog : kBrowserAppFileDialogs) {
        out << "    " << rust_pascal_identifier(dialog.id) << ",\n";
    }
    out << "}\n\nimpl FileDialogId {\n    pub const fn spec(self) -> &'static FileDialogSpec {\n"
           "        &FILE_DIALOGS[self as usize]\n    }\n}\n\n";
    out << "pub const FILE_DIALOGS: &[FileDialogSpec] = &[\n";
    for (const auto& dialog : kBrowserAppFileDialogs) {
        out << "    FileDialogSpec { id: " << quote_string(dialog.id)
            << ", workflow: Workflow::" << rust_pascal_identifier(file_dialog_workflow_id(dialog))
            << ", mode: FileDialogMode::" << rust_pascal_identifier(file_dialog_mode_id(dialog))
            << ", field: " << quote_string(dialog.field) << ", title: " << quote_string(dialog.title)
            << ", filter_name: " << quote_string(dialog.filter_name) << ", filter_patterns: &[";
        for (std::size_t index = 0U; index < dialog.filter_pattern_count; ++index) {
            if (index != 0U) {
                out << ", ";
            }
            out << quote_string(dialog.filter_patterns[index]);
        }
        out << "] },\n";
    }
    out << "];\n\n";

    out << "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\npub enum SettingsValueKind {\n";
    std::vector<std::string> value_kinds;
    for (const auto& setting : mmltk::browser::kBrowserSettingsPatchValueTypes) {
        const std::string kind = rust_settings_value_kind(setting.value_type);
        if (std::ranges::find(value_kinds, kind) == value_kinds.end()) {
            value_kinds.push_back(kind);
            out << "    " << kind << ",\n";
        }
    }
    out << "}\n\n#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\npub enum SettingsPath {\n";
    for (const auto& setting : mmltk::browser::kBrowserSettingsPatchValueTypes) {
        out << "    " << rust_pascal_identifier(setting.path) << ",\n";
    }
    out << "}\n\nimpl SettingsPath {\n    pub const fn as_str(self) -> &'static str {\n        match self {\n";
    for (const auto& setting : mmltk::browser::kBrowserSettingsPatchValueTypes) {
        out << "            Self::" << rust_pascal_identifier(setting.path) << " => " << quote_string(setting.path)
            << ",\n";
    }
    out << "        }\n    }\n}\n\n#[derive(Debug, Clone, Copy, PartialEq, Eq)]\npub struct SettingsPathSpec {\n"
           "    pub path: SettingsPath,\n    pub value_kind: SettingsValueKind,\n}\n\n";
    out << "pub const SETTINGS_PATHS: &[SettingsPathSpec] = &[\n";
    for (const auto& setting : mmltk::browser::kBrowserSettingsPatchValueTypes) {
        out << "    SettingsPathSpec { path: SettingsPath::" << rust_pascal_identifier(setting.path)
            << ", value_kind: SettingsValueKind::" << rust_settings_value_kind(setting.value_type) << " },\n";
    }
    out << "];\n\n";
    out << R"rust(#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Workflow {
    #[default]
    Train,
    Validate,
    Predict,
    Annotate,
    Export,
    Live,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SourceKind {
    #[default]
    CompiledDataset,
    SingleImage,
    ImageFolder,
    VideoStream,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BrowserHostBackend {
    #[default]
    Unknown,
    Firefox,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BrowserRuntimeCapabilityStatus {
    #[default]
    Unknown,
    Available,
    Unavailable,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct CaptureRegion {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct JobLogEntry {
    pub sequence: u64,
    pub level: String,
    pub message: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct JobState {
    pub running: bool,
    pub label: String,
    pub summary: String,
    pub error: String,
    pub output_tail: String,
    pub recent_logs: Vec<JobLogEntry>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct SourceMetadata {
    pub kind: SourceKind,
    pub locator: String,
    pub recursive: bool,
    pub device_index: i32,
    pub capture_width: i32,
    pub capture_height: i32,
    pub capture_fps: i32,
    pub v4l2_buffer_count: i32,
    pub has_crop: bool,
    pub crop: CaptureRegion,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AnnotationPointState {
    pub x: i32,
    pub y: i32,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AnnotationEdgeState {
    pub source_index: u32,
    pub target_index: u32,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AnnotationObjectState {
    pub object_index: u32,
    pub x1: i32,
    pub y1: i32,
    pub x2: i32,
    pub y2: i32,
    pub points: Vec<AnnotationPointState>,
    pub edges: Vec<AnnotationEdgeState>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AnnotationHandleState {
    pub object_index: u32,
    pub element_index: u32,
    pub role: String,
    pub x: i32,
    pub y: i32,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AnnotationDocumentState {
    pub document_generation: u64,
    pub session_revision: u64,
    pub capture_width: u32,
    pub capture_height: u32,
    pub instance_count: u64,
    pub selected_instance: Option<u32>,
    pub objects: Vec<AnnotationObjectState>,
    pub handles: Vec<AnnotationHandleState>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkspaceSurfaceInfo {
    pub generation: String,
    #[serde(rename = "capacityWidth")]
    pub capacity_width: u32,
    #[serde(rename = "capacityHeight")]
    pub capacity_height: u32,
    #[serde(rename = "slotCount")]
    pub slot_count: u32,
    pub format: String,
    pub orientation: String,
    #[serde(rename = "sourceKind")]
    pub source_kind: String,
    pub ready: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkspaceSourceRegion {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkspacePresent {
    pub generation: String,
    pub slot: u32,
    pub revision: String,
    pub width: u32,
    pub height: u32,
    #[serde(rename = "sourceRegion")]
    pub source_region: WorkspaceSourceRegion,
    #[serde(rename = "captureNs")]
    pub capture_ns: String,
    #[serde(rename = "readyNs")]
    pub ready_ns: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct BrowserRuntimeCapabilities {
    pub host_backend: BrowserHostBackend,
    pub navigator_gpu: BrowserRuntimeCapabilityStatus,
    pub workspace_surface_bridge: BrowserRuntimeCapabilityStatus,
    pub workspace_surface_zero_copy: BrowserRuntimeCapabilityStatus,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ArtifactOperationPhase {
    #[default]
    Idle,
    Running,
    Complete,
    Failed,
    Cancelled,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DatasetCompilePhase {
    #[default]
    Idle,
    Planning,
    Labels,
    Pixels,
    Syncing,
    Publishing,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WeightArtifactStatus {
    #[default]
    Idle,
    Verifying,
    Downloading,
    RetryWaiting,
    Ready,
    NoConnection,
    CannotDownload,
    ChecksumError,
    FilesystemError,
    HttpError,
    Incompatible,
    Cancelled,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum FileDialogResultStatus {
    #[default]
    Idle,
    Pending,
    Selected,
    Cancelled,
    Failed,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CustomModelArtifactKind {
    #[default]
    Weights,
    Onnx,
    #[serde(rename = "tensorrt")]
    TensorRt,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ModelPreflightStatus {
    #[default]
    Idle,
    Verifying,
    Ready,
    Incompatible,
    Failed,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct ArtifactSplitState {
    pub path: String,
    pub image_count: u32,
    pub width: u32,
    pub height: u32,
    pub channels: u32,
    pub class_count: u32,
    pub class_names: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct DatasetArtifactState {
    pub phase: ArtifactOperationPhase,
    pub compile_phase: DatasetCompilePhase,
    pub generation: u64,
    pub source_dir: String,
    pub output_dir: String,
    pub preset_name: String,
    pub active_split: String,
    pub done: u64,
    pub total: u64,
    pub elapsed_ms: u64,
    pub remaining_ms: u64,
    pub dropped_instances: u64,
    pub eta_ready: bool,
    pub compatible: bool,
    pub compiling: bool,
    pub error: String,
    pub splits: Vec<ArtifactSplitState>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct WeightArtifactState {
    pub phase: ArtifactOperationPhase,
    pub status: WeightArtifactStatus,
    pub generation: u64,
    pub preset_name: String,
    pub path: String,
    pub error: String,
    pub downloaded_bytes: u64,
    pub total_bytes: u64,
    pub attempt: u32,
    pub retry_after_ms: u64,
    pub resumable: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct ArtifactState {
    pub dataset: DatasetArtifactState,
    pub weight: WeightArtifactState,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct FileDialogState {
    pub token: u64,
    pub status: FileDialogResultStatus,
    pub dialog_id: String,
    pub path: String,
    pub error: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct ModelPreflightState {
    pub generation: u64,
    pub workflow: Workflow,
    pub status: ModelPreflightStatus,
    pub artifact_kind: CustomModelArtifactKind,
    pub preset_name: String,
    pub resolution: u32,
    pub path: String,
    pub error: String,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct StateSnapshot {
    #[serde(rename = "type")]
    pub message_type: String,
    pub protocol_version: u32,
    pub contract_hash: String,
    pub state_revision: u64,
    pub active_workflow: Workflow,
    pub workflow_state: Map<String, Value>,
    pub settings_state: Map<String, Value>,
    pub job: JobState,
    pub source: SourceMetadata,
    pub annotation: AnnotationDocumentState,
    pub runtime_capabilities: BrowserRuntimeCapabilities,
    pub artifacts: ArtifactState,
    pub file_dialog: FileDialogState,
    pub model_preflight: ModelPreflightState,
    pub workspace_surface: Option<WorkspaceSurfaceInfo>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct IntentMessage {
    #[serde(rename = "type")]
    pub message_type: String,
    pub protocol_version: u32,
    pub request_id: u64,
    pub workflow: Workflow,
    pub intent: String,
    pub payload: Value,
}
)rust";
    return out.str();
}

[[nodiscard]] std::string render_cpp_header(const std::string& hash) {
    std::ostringstream out;
    out << generated_header(hash) << '\n';
    out << "#pragma once\n\n#include <string_view>\n\n";
    out << "namespace mmltk::browser {\n\n";
    out << "inline constexpr std::string_view kBrowserUiContractHash = " << quote_string(hash) << ";\n";
    out << "\n}  // namespace mmltk::browser\n";
    return out.str();
}

struct GeneratedOutputs {
    Json openapi;
    std::string openapi_text;
    std::string rust_module;
    std::string cpp_header;
};

[[nodiscard]] GeneratedOutputs generate_outputs() {
    GeneratedOutputs outputs;
    outputs.openapi = openapi_document();
    const std::string hash = stable_contract_hash(outputs.openapi.dump());
    outputs.openapi["x-mmltk-contract-hash"] = hash;
    outputs.openapi_text = outputs.openapi.dump(2) + "\n";
    outputs.rust_module = render_rust_module(hash);
    outputs.cpp_header = render_cpp_header(hash);
    return outputs;
}

}  

int main(const int argc, char** argv) {
    try {
        const CodegenOptions options = parse_args(argc, argv);
        const GeneratedOutputs outputs = generate_outputs();

        if (options.check) {
            bool ok = true;
            if (!options.openapi_out.empty()) {
                ok = check_file(options.openapi_out, outputs.openapi_text, "host_api.openapi.json") && ok;
            }
            if (!options.rust_out.empty()) {
                ok = check_file(options.rust_out, outputs.rust_module, "host_api_contract_generated.rs") && ok;
            }
            if (!options.cpp_out.empty()) {
                ok = check_file(options.cpp_out, outputs.cpp_header, "workflow_contract_generated.h") && ok;
            }
            return ok ? 0 : 1;
        }

        if (!options.openapi_out.empty()) {
            write_file_if_changed(options.openapi_out, outputs.openapi_text);
        }
        if (!options.rust_out.empty()) {
            write_file_if_changed(options.rust_out, outputs.rust_module);
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
