#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/common/io/file_digest.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
#include "src/backend/models/rfdetr/contract/weight_catalog.h"
#include <fstream>
#include <caffe2/serialize/inline_container.h>
#include <filesystem>
#include <meta>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include "src/backend/ml/torch/archive.h"
#include "detail/class_tensor_axes.h"
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
namespace mmltk::backend::models::rfdetr {
DecodedNativeModelState decode_upstream_python_model_state(const std::filesystem::path& checkpoint_path);
namespace model_state_detail {
using torch::serialize::InputArchive;
}
struct DecodedNativeModelState::Impl final {
    std::vector<NormalizedModelStateEntry> entries;
    std::unique_ptr<torch::serialize::InputArchive> native_archive;
};
DecodedNativeModelState::DecodedNativeModelState() : impl_(std::make_unique<Impl>()) {}
DecodedNativeModelState::~DecodedNativeModelState() = default;
DecodedNativeModelState::DecodedNativeModelState(DecodedNativeModelState&&) noexcept = default;
DecodedNativeModelState& DecodedNativeModelState::operator=(DecodedNativeModelState&&) noexcept = default;
std::size_t DecodedNativeModelState::tensor_count() const noexcept { return impl_->entries.size(); }
DecodedNativeModelState::DecodedNativeModelState(std::vector<NormalizedModelStateEntry> entries) : DecodedNativeModelState() {
    impl_->entries = std::move(entries);
}
const std::vector<NormalizedModelStateEntry>& DecodedNativeModelState::entries() const noexcept { return impl_->entries; }
torch::serialize::InputArchive* DecodedNativeModelState::admitted_archive() const noexcept { return impl_->native_archive.get(); }
void DecodedNativeModelState::retain_admitted_archive(std::unique_ptr<torch::serialize::InputArchive> archive) { impl_->native_archive = std::move(archive); }
std::vector<NormalizedModelStateEntry> DecodedNativeModelState::consume_entries() { return std::move(impl_->entries); }
void DecodedNativeModelState::release_admission() noexcept {
    impl_->entries.clear();
    impl_->native_archive.reset();
}
void DecodedNativeModelState::replace_entries(std::vector<NormalizedModelStateEntry> entries) { impl_->entries = std::move(entries); }
namespace {
[[nodiscard]] std::filesystem::path canonical_path(const std::filesystem::path& path) {
    if (path.empty()) { throw std::runtime_error("RF-DETR checkpoint path must not be empty"); }
    const auto canonical = std::filesystem::absolute(path).lexically_normal();
    if (!std::filesystem::exists(canonical)) { throw std::runtime_error("missing RF-DETR checkpoint file: " + canonical.string()); }
    return canonical;
}
[[nodiscard]] bool supported_format(const std::string_view format) noexcept { return format == kNativeCheckpointFormat; }
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
    template for (constexpr auto left_member : std::define_static_array(std::meta::nonstatic_data_members_of(^^Left, std::meta::access_context::current()))) {
        template for (constexpr auto right_member :
                      std::define_static_array(std::meta::nonstatic_data_members_of(^^Right, std::meta::access_context::current()))) {
            if constexpr (std::meta::identifier_of(left_member) == std::meta::identifier_of(right_member)) visit(left.[:left_member:], right.[:right_member:]);
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
void validate_decoded_model_state(const DecodedNativeModelState& state) {
    const ResolvedClassLayout layout(state.metadata.class_layout);
    if (state.metadata.num_classes <= 0 || layout.output_width() != static_cast<std::size_t>(state.metadata.num_classes))
        throw std::invalid_argument("checkpoint output width disagrees with its class layout");
    std::unordered_set<std::string_view> names;
    names.reserve(state.tensor_count());
    for (const auto& entry : state.entries()) {
        if (entry.name.empty() || entry.name.size() > 4096 || !names.insert(entry.name).second)
            throw std::invalid_argument("checkpoint tensor names must be unique and nonempty");
        const auto axis = detail::class_tensor_shape(entry.name);
        if (!axis) continue;
        if (!entry.tensor.defined() || entry.tensor.dim() != axis->rank || entry.tensor.size(axis->dimension) <= 0)
            throw std::invalid_argument("invalid class-dependent checkpoint tensor: " + entry.name);
        const bool classifier = axis->coordinates == detail::ClassTensorCoordinates::OutputSlots;
        const auto box_columns = axis->coordinates == detail::ClassTensorCoordinates::ForegroundWithBoxes ? 4 : 0;
        if (classifier || layout.record().supervision_in_foreground_order) {
            const auto expected = classifier ? layout.output_width() : layout.catalog()->size() + box_columns;
            if (entry.tensor.size(axis->dimension) != static_cast<std::int64_t>(expected))
                throw std::invalid_argument("class-dependent checkpoint tensor disagrees with layout: " + entry.name);
        } else if (entry.tensor.size(axis->dimension) <= box_columns) {
            throw std::invalid_argument("external supervision tensor has no class-input block: " + entry.name);
        }
    }
}
bool is_native_checkpoint_file(const std::filesystem::path& checkpoint_path) {
    const auto path = canonical_path(checkpoint_path);
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 4> signature{};
    stream.read(signature.data(), signature.size());
    if (signature != std::array<char, 4>{'P', 'K', 3, 4}) return false;
    try {
        caffe2::serialize::PyTorchStreamReader reader(path.string());
        // InputArchive is a TorchScript archive. Python pickle archives do not
        // contain its constants/code members. Identification never loads tensors.
        const auto records = reader.getAllRecords();
        const bool native = std::ranges::any_of(records, [](const auto& name) { return name == "constants.pkl" || name.starts_with("code/"); });
        if (native && reader.getRecordSize("data.pkl") > 16U * kClassLayoutByteBudget)
            throw std::invalid_argument("native checkpoint metadata exceeds admission budget");
        return native;
    } catch (const std::exception& error) { throw std::runtime_error("corrupt RF-DETR checkpoint archive: " + path.string() + ": " + error.what()); }
}
static DecodedNativeModelState load_native_model_state(const std::filesystem::path& checkpoint_path) {
    const auto canonical = canonical_path(checkpoint_path);
    const auto path = canonical.string();
    const auto snapshot = mmltk::common::io::FileSnapshot::Read(canonical);
    if (!is_native_checkpoint_file(canonical)) throw std::invalid_argument("not an RF-DETR native archive");
    auto admitted_archive = std::make_unique<model_state_detail::InputArchive>();
    auto& archive = *admitted_archive;
    archive.load_from(path, torch::Device(torch::kCPU));
    if (!supported_format(mmltk::backend::ml::serialization::require_string(archive, "format"))) {
        throw std::runtime_error("RF-DETR checkpoint is not a native checkpoint: " + path);
    }
    const auto version = mmltk::backend::ml::serialization::require_int(archive, "format_version");
    if (version != kNativeCheckpointFormatVersion) {
        throw std::runtime_error("unsupported RF-DETR native checkpoint format version " + std::to_string(version) + ": " + path);
    }
    DecodedNativeModelState result;
    auto& metadata = result.metadata;
    metadata.preset_name = mmltk::backend::ml::serialization::read_optional_value<std::string>(archive, "preset_name").value_or("");
    metadata.source_kind = mmltk::backend::ml::serialization::read_optional_value<std::string>(archive, "source_kind").value_or("native");
    metadata.source_path = mmltk::backend::ml::serialization::read_optional_value<std::string>(archive, "source_path").value_or(path);
    metadata.num_classes = mmltk::backend::ml::serialization::read_optional_value<int64_t>(archive, "num_classes").value_or(0);
    metadata.num_queries = mmltk::backend::ml::serialization::require_int(archive, "num_queries");
    metadata.num_select = mmltk::backend::ml::serialization::require_int(archive, "num_select");
    validate_queries(metadata, path, true);
    metadata.class_layout = decode_class_layout(mmltk::backend::ml::serialization::require_string(archive, "class_layout"));
    if (metadata.num_classes < 0 || static_cast<std::size_t>(metadata.num_classes) != metadata.class_layout.slots.size())
        throw std::runtime_error("native checkpoint output width disagrees with class layout");
    metadata.for_each_detection_field([&archive]<class Name, class Optional>(const Name& name, Optional& field) {
        field = mmltk::backend::ml::serialization::read_optional_value<typename Optional::value_type>(archive, name);
    });
    model_state_detail::InputArchive state_archive;
    archive.read("state", state_archive);
    const auto entry_count = mmltk::backend::ml::serialization::require_int(state_archive, "entry_count");
    if (entry_count < 0 || entry_count > 100000) { throw std::runtime_error("RF-DETR checkpoint entry_count is negative: " + path); }
    std::vector<NormalizedModelStateEntry> entries;
    entries.reserve(static_cast<std::size_t>(entry_count));
    for (int64_t index = 0; index < entry_count; ++index) {
        model_state_detail::InputArchive entry_archive;
        state_archive.read(mmltk::backend::ml::serialization::archive_entry_name(static_cast<std::size_t>(index)), entry_archive);
        NormalizedModelStateEntry entry;
        entry.name = mmltk::backend::ml::serialization::require_string(entry_archive, "name");
        entry_archive.read("tensor", entry.tensor);
        entries.push_back(std::move(entry));
    }
    result.replace_entries(std::move(entries));
    validate_decoded_model_state(result);
    result.retain_admitted_archive(std::move(admitted_archive));
    snapshot.RequireUnchanged(canonical);
    return result;
}
DecodedNativeModelState decode_model_state(const std::filesystem::path& checkpoint_path, std::shared_ptr<const ClassArtifactAdmission> admission,
                                           const std::filesystem::path& class_layout_path, std::stop_token stop) {
    if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
    const auto canonical = canonical_path(checkpoint_path);
    const auto snapshot = mmltk::common::io::FileSnapshot::Read(canonical);
    const bool native = is_native_checkpoint_file(canonical);
    if (!admission) admission = std::make_shared<const ClassArtifactAdmission>(canonical, class_layout_path, nullptr, stop, !native);
    if (!admission->Matches(canonical, class_layout_path)) throw std::runtime_error("checkpoint admission does not match selected artifact");
    admission->RequireUnchanged(stop);
    const auto& digests = admission->file();
    if (digests->snapshot != snapshot) throw std::runtime_error("checkpoint changed during archive identification");
    digests->snapshot.RequireUnchanged(canonical);
    DecodedNativeModelState result;
    if (native) {
        result = load_native_model_state(canonical);
        if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
    } else {
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
        result = decode_upstream_python_model_state(canonical);
        if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
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
        for (const auto& entry : result.entries()) {
            if (const auto axis = detail::kDecoderClassAxis.Match(entry.name); axis && entry.tensor.defined() && entry.tensor.dim() > axis->dimension) {
                result.metadata.num_classes = entry.tensor.size(axis->dimension);
                break;
            }
        }
        if (result.metadata.class_layout.slots.empty()) {
            auto evidence = std::move(result.metadata.class_layout.class_name_evidence);
            result.metadata.class_layout = unresolved_class_layout(static_cast<std::size_t>(result.metadata.num_classes));
            result.metadata.class_layout.class_name_evidence = std::move(evidence);
            for (const auto& asset : weight_catalog()) {
                if (asset.coco_sparse_slots && asset.md5_hash == digests->md5) {
                    auto source_evidence = std::move(result.metadata.class_layout.class_name_evidence);
                    result.metadata.class_layout =
                        coco_class_layout({ClassLayoutOrigin::VerifiedAsset, std::string(asset.filename), mmltk::common::io::sha256_hex(digests->sha256)});
                    result.metadata.class_layout.class_name_evidence = std::move(source_evidence);
                    break;
                }
            }
        }
        const ResolvedClassLayout layout(result.metadata.class_layout);
        if (layout.output_width() != static_cast<std::size_t>(result.metadata.num_classes))
            throw std::runtime_error("external checkpoint output width disagrees with class layout");
#else
        throw std::runtime_error("RF-DETR upstream Python checkpoint loading is disabled at build time: " + canonical.string());
#endif
    }
    result.metadata.class_layout = admission->Resolve(result.metadata.num_classes, std::optional{result.metadata.class_layout}, stop);
    validate_decoded_model_state(result);
    admission->RequireUnchanged(stop);
    result.class_artifact = std::move(admission);
    return result;
}
DecodedNativeModelState decode_native_model_state(const std::filesystem::path& checkpoint_path) {
    auto admission = std::make_shared<const ClassArtifactAdmission>(checkpoint_path);
    auto result = load_native_model_state(admission->artifact_path());
    result.metadata.class_layout = admission->Resolve(result.metadata.num_classes, result.metadata.class_layout);
    result.class_artifact = std::move(admission);
    return result;
}
ResolvedModelState resolve_model_state(const std::filesystem::path& weights_path, const std::string_view preset_name, const int resolution,
                                       const std::filesystem::path& class_layout_path, std::shared_ptr<const ClassArtifactAdmission> admission,
                                       std::stop_token stop) {
    if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
    const auto canonical = canonical_path(weights_path);
    auto state = decode_model_state(canonical, std::move(admission), class_layout_path, stop);
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
    result.class_layout = state.metadata.class_layout;
    result.artifact_sha256 = mmltk::common::io::sha256_hex(state.class_artifact->file()->sha256);
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
    metadata.class_layout = artifacts.class_layout;
    metadata.preset_name = artifacts.config.preset_name;
    metadata.source_kind = artifacts.input_kind;
    metadata.source_path = artifacts.input_path.string();
    metadata.num_classes = num_classes;
    metadata.num_queries = artifacts.config.num_queries;
    metadata.num_select = artifacts.config.num_select;
    capture_checkpoint_detection_metadata(metadata, artifacts.config);
    return metadata;
}
ResolvedModelArtifacts resolve_model_artifacts(const std::filesystem::path& weights_path, std::string_view preset_name, int resolution) {
    return resolve_model_state(weights_path, preset_name, resolution).artifacts;
}
}  // namespace mmltk::backend::models::rfdetr
