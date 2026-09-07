#include "src/backend/models/rfdetr/core/model_state.h"

#include <cstdint>
#include <filesystem>
#include <meta>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "detail/archive_utils.h"
#include "detail/model_state_access.h"
#include "detail/model_state_technical.h"
#include "detail/model_technical.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"

namespace mmltk::backend::models::rfdetr {

DecodedNativeModelState decode_upstream_python_model_state(const std::filesystem::path& checkpoint_path);

namespace model_state_detail {
using torch::serialize::InputArchive;
}

struct DecodedNativeModelState::Impl final : detail::ModelStateTechnicalOwner {};

DecodedNativeModelState::DecodedNativeModelState() : impl_(std::make_unique<Impl>()) {}
DecodedNativeModelState::~DecodedNativeModelState() = default;
DecodedNativeModelState::DecodedNativeModelState(DecodedNativeModelState&&) noexcept = default;
DecodedNativeModelState& DecodedNativeModelState::operator=(DecodedNativeModelState&&) noexcept = default;

std::size_t DecodedNativeModelState::tensor_count() const noexcept { return impl_->entries.size(); }

void* DecodedNativeModelState::technical_handle() noexcept { return impl_.get(); }

const void* DecodedNativeModelState::technical_handle() const noexcept { return impl_.get(); }

namespace {

[[nodiscard]] std::filesystem::path canonical_path(const std::filesystem::path& path) {
    if (path.empty()) { throw std::runtime_error("RF-DETR checkpoint path must not be empty"); }
    const auto canonical = std::filesystem::absolute(path).lexically_normal();
    if (!std::filesystem::exists(canonical)) { throw std::runtime_error("missing RF-DETR checkpoint file: " + canonical.string()); }
    return canonical;
}

[[nodiscard]] bool supported_format(const std::string_view format) noexcept {
    return format == kNativeCheckpointFormat || format == "fastloader.rfdetr.native_checkpoint";
}

[[nodiscard]] const PresetCatalogEntry* checkpoint_preset(const NativeCheckpointMetadata& metadata, const std::filesystem::path& path) {
    if (!metadata.preset_name.empty()) {
        if (const auto* preset = find_model_preset(metadata.preset_name)) { return preset; }
    }
    return infer_model_preset_from_path(path);
}

void resolve_legacy_queries(NativeCheckpointMetadata& metadata, const std::filesystem::path& path) {
    if (metadata.num_queries > 0 || metadata.num_select > 0) { return; }
    if (const auto* preset = checkpoint_preset(metadata, path)) {
        metadata.num_queries = preset->query_count;
        metadata.num_select = preset->selected_query_count;
    }
}

void validate_queries(const NativeCheckpointMetadata& metadata, const std::string& path, bool required) {
    if (!required && metadata.num_queries == 0 && metadata.num_select == 0) { return; }
    if (metadata.num_queries <= 0 || metadata.num_select <= 0 || metadata.num_select > metadata.num_queries) {
        throw std::runtime_error("RF-DETR native checkpoint has invalid query metadata: " + path);
    }
}

template <class T>
inline constexpr bool is_optional = false;

template <class T>
inline constexpr bool is_optional<std::optional<T>> = true;

template <class Left, class Right, class Visitor>
void visit_matching_members(Left& left, const Right& right, Visitor&& visit) {
    template for (constexpr auto left_member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^Left, std::meta::access_context::current()))) {
        template for (constexpr auto right_member :
                      std::define_static_array(std::meta::nonstatic_data_members_of(^^Right, std::meta::access_context::current()))) {
            if constexpr (std::meta::identifier_of(left_member) == std::meta::identifier_of(right_member))
                visit(left.[:left_member:], right.[:right_member:]);
        }
    }
}

void apply_checkpoint_detection_overrides(NativeRfDetrConfig& config, const NativeCheckpointMetadata& metadata) {
    visit_matching_members(config, metadata, []<class Target, class Source>(Target& target, const Source& source) {
        if constexpr (is_optional<std::remove_cvref_t<Source>>) {
            if (source) target = *source;
        }
    });
}

void capture_checkpoint_detection_metadata(NativeCheckpointMetadata& metadata, const NativeRfDetrConfig& config) {
    visit_matching_members(metadata, config, []<class Target, class Source>(Target& target, const Source& source) {
        if constexpr (is_optional<std::remove_cvref_t<Target>>) target = source;
    });
}

}  // namespace

bool is_native_checkpoint_file(const std::filesystem::path& checkpoint_path) {
    try {
        model_state_detail::InputArchive archive;
        archive.load_from(canonical_path(checkpoint_path).string());
        return supported_format(require_string(archive, "format"));
    } catch (const std::exception&) { return false; }
}

DecodedNativeModelState decode_native_model_state(const std::filesystem::path& checkpoint_path) {
    const auto canonical = canonical_path(checkpoint_path);
    const auto path = canonical.string();
    model_state_detail::InputArchive archive;
    archive.load_from(path);
    if (!supported_format(require_string(archive, "format"))) {
        throw std::runtime_error("RF-DETR checkpoint is not a native checkpoint: " + path);
    }
    const auto version = require_int(archive, "format_version");
    if (version != kLegacyNativeCheckpointFormatVersion && version != kNativeCheckpointFormatVersion) {
        throw std::runtime_error("unsupported RF-DETR native checkpoint format version " + std::to_string(version) + ": " + path);
    }

    DecodedNativeModelState result;
    auto& metadata = result.metadata;
    metadata.preset_name = read_optional_value<std::string>(archive, "preset_name").value_or("");
    metadata.source_kind = read_optional_value<std::string>(archive, "source_kind").value_or("native");
    metadata.source_path = read_optional_value<std::string>(archive, "source_path").value_or(path);
    metadata.num_classes = read_optional_value<int64_t>(archive, "num_classes").value_or(0);
    if (version == kNativeCheckpointFormatVersion) {
        metadata.num_queries = require_int(archive, "num_queries");
        metadata.num_select = require_int(archive, "num_select");
    } else {
        metadata.num_queries = read_optional_value<int64_t>(archive, "num_queries").value_or(0);
        metadata.num_select = read_optional_value<int64_t>(archive, "num_select").value_or(0);
        resolve_legacy_queries(metadata, canonical);
    }
    validate_queries(metadata, path, version == kNativeCheckpointFormatVersion);
    metadata.for_each_detection_field([&archive]<class Name, class Optional>(const Name& name, Optional& field) {
        field = read_optional_value<typename Optional::value_type>(archive, name);
    });

    model_state_detail::InputArchive state_archive;
    archive.read("state", state_archive);
    const auto entry_count = require_int(state_archive, "entry_count");
    if (entry_count < 0) { throw std::runtime_error("RF-DETR checkpoint entry_count is negative: " + path); }
    auto& entries = detail::model_state_owner(result).entries;
    entries.reserve(static_cast<std::size_t>(entry_count));
    for (int64_t index = 0; index < entry_count; ++index) {
        model_state_detail::InputArchive entry_archive;
        state_archive.read(archive_entry_name(static_cast<std::size_t>(index)), entry_archive);
        NormalizedModelStateEntry entry;
        entry.name = require_string(entry_archive, "name");
        entry_archive.read("tensor", entry.tensor);
        entries.push_back(std::move(entry));
    }
    return result;
}

DecodedNativeModelState decode_model_state(const std::filesystem::path& checkpoint_path) {
    const auto canonical = canonical_path(checkpoint_path);
    if (is_native_checkpoint_file(canonical)) { return decode_native_model_state(canonical); }
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
    auto result = decode_upstream_python_model_state(canonical);
    result.metadata.source_kind = "upstream-python";
    result.metadata.source_path = canonical.string();
    const auto* preset = find_model_preset_by_weight_filename(canonical.filename().string());
    if (preset == nullptr) { preset = infer_model_preset_from_path(canonical); }
    if (preset != nullptr) {
        result.metadata.preset_name = std::string(preset->preset_name);
        result.metadata.num_classes = preset->class_count;
        if (result.metadata.num_queries <= 0) { result.metadata.num_queries = preset->query_count; }
        if (result.metadata.num_select <= 0) { result.metadata.num_select = preset->selected_query_count; }
    }
    for (const auto& entry : detail::model_state_owner(result).entries) {
        if ((entry.name == "class_embed.bias" || entry.name == "class_embed.weight") && entry.tensor.defined() && entry.tensor.dim() >= 1) {
            result.metadata.num_classes = entry.tensor.size(0);
            break;
        }
    }
    return result;
#else
    throw std::runtime_error("RF-DETR upstream Python checkpoint loading is disabled at build time: " + canonical.string());
#endif
}

ResolvedModelState resolve_model_state(const std::filesystem::path& weights_path, const std::string_view preset_name,
                                       const int resolution) {
    const auto canonical = canonical_path(weights_path);
    auto state = decode_model_state(canonical);
    const PresetCatalogEntry* preset = nullptr;
    if (!state.metadata.preset_name.empty()) { preset = find_model_preset(state.metadata.preset_name); }
    if (preset == nullptr) { preset = find_model_preset_by_weight_filename(canonical.filename().string()); }
    if (preset == nullptr) { preset = infer_model_preset_from_path(canonical); }
    if (preset == nullptr && !preset_name.empty()) { preset = find_model_preset(preset_name); }
    if (preset == nullptr) { throw std::runtime_error("unable to resolve RF-DETR weights preset"); }
    ResolvedModelArtifacts result;
    result.input_kind = state.metadata.source_kind == "upstream-python" ? "upstream-python" : "native-pt";
    result.input_path = canonical;
    result.weights_path = canonical;
    result.artifact_root = canonical.parent_path();
    result.preset_name = std::string(preset->preset_name);
    result.model_id = canonical.stem().string();
    result.config = native_config_from_preset(*preset);
    if (state.metadata.num_classes > 0) { result.config.num_classes = static_cast<int>(state.metadata.num_classes); }
    result.source_num_queries = state.metadata.num_queries > 0 ? static_cast<int>(state.metadata.num_queries) : result.config.num_queries;
    result.source_num_select = state.metadata.num_select > 0 ? static_cast<int>(state.metadata.num_select) : result.config.num_select;
    result.automatic_num_queries_cap = result.config.num_queries;
    if (!preset_name.empty() && result.config.preset_name != preset_name) {
        const auto* declared = find_model_preset(preset_name);
        if (declared == nullptr) { throw std::runtime_error("unknown RF-DETR preset override: " + std::string(preset_name)); }
        const auto classes = result.config.num_classes;
        result.config = native_config_from_preset(*declared);
        result.config.num_classes = classes;
        result.automatic_num_queries_cap = result.config.num_queries;
    }
    result.config.num_queries = result.source_num_queries;
    result.config.num_select = result.source_num_select;
    if (resolution > 0) { result.config.resolution = resolution; }
    apply_checkpoint_detection_overrides(result.config, state.metadata);
    return {
        .artifacts = std::move(result),
        .model_state = std::move(state),
    };
}

NativeCheckpointMetadata make_native_checkpoint_metadata(const ResolvedModelArtifacts& artifacts, const int64_t num_classes) {
    NativeCheckpointMetadata metadata;
    metadata.preset_name = artifacts.config.preset_name;
    metadata.source_kind = artifacts.input_kind;
    metadata.source_path = artifacts.input_path.string();
    metadata.num_classes = num_classes;
    metadata.num_queries = artifacts.config.num_queries;
    metadata.num_select = artifacts.config.num_select;
    capture_checkpoint_detection_metadata(metadata, artifacts.config);
    return metadata;
}

}  // namespace mmltk::backend::models::rfdetr
