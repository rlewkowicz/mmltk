#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/common/io/file_digest.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
#include "src/backend/models/rfdetr/contract/weight_catalog.h"
#include "src/pch_std.h"
#include "src/common/io/file_memory.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include <caffe2/serialize/inline_container.h>
#include <charconv>
#include "src/backend/ml/torch/archive.h"
#include "src/backend/ml/torch/archive_cbor.h"
#include "detail/class_tensor_axes.h"
#include "keypoint_layout.h"
#include "src/frameworks/serialization/json_wire.h"
#include "src/frameworks/serialization/serialization.h"
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
DecodedNativeModelState::DecodedNativeModelState(std::vector<NormalizedModelStateEntry> entries) : DecodedNativeModelState() { impl_->entries = std::move(entries); }
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
// Raw state dictionaries carry no filename or Python arguments. Infer only an
// unambiguous catalog topology from actual backbone, head and decoder state.
const PresetCatalogEntry* infer_state_preset(const std::vector<NormalizedModelStateEntry>& entries) {
 const auto tensor = [&](std::string_view name) -> const torch::Tensor* {
  const auto found = std::ranges::find(entries, name, &NormalizedModelStateEntry::name);
  return found == entries.end() ? nullptr : &found->tensor;
 };
 const auto* patch = tensor("backbone.0.encoder.model.patch_embed.proj.weight");
 const bool pe = patch != nullptr;
 if (!patch) patch = tensor("backbone.0.encoder.encoder.embeddings.patch_embeddings.projection.weight");
 const auto* positions = tensor(pe ? "backbone.0.encoder.model.pos_embed" : "backbone.0.encoder.encoder.embeddings.position_embeddings");
 if (!patch || !positions || patch->dim() != 4 || positions->dim() != 3) return nullptr;
 const bool keypoints = tensor("keypoint_embed.layers.2.weight") != nullptr;
 const bool segmentation = std::ranges::any_of(entries, [](const auto& entry) { return entry.name.starts_with("segmentation_head."); });
 int decoder_layers = 0;
 for (const auto& entry : entries) {
  constexpr std::string_view prefix = "transformer.decoder.layers.";
  if (!entry.name.starts_with(prefix)) continue;
  const auto suffix = std::string_view(entry.name).substr(prefix.size());
  const auto end = suffix.find('.');
  if (end == std::string_view::npos) continue;
  int layer = -1;
  const auto parsed = std::from_chars(suffix.data(), suffix.data() + end, layer);
  if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + end || layer < 0 || layer >= 64) throw std::invalid_argument("invalid decoder state layer identity");
  decoder_layers = std::max(decoder_layers, layer + 1);
 }
 const PresetCatalogEntry* match = nullptr;
 for (const auto& preset : kPresetCatalog) {
  const auto config = native_config_from_preset(preset);
  if (pe != (config.backbone.kind == BackboneKind::PeCoreTiny) || config.has_keypoints() != keypoints || config.has_masks() != segmentation || config.dec_layers != decoder_layers ||
      patch->size(0) != config.backbone.dimension || patch->size(1) != 3 || patch->size(2) != config.patch_size || patch->size(3) != config.patch_size || positions->size(0) != 1 ||
      positions->size(1) != config.positional_encoding_size * config.positional_encoding_size + 1 || positions->size(2) != config.backbone.dimension)
   continue;
  if (match) throw std::invalid_argument("raw RF-DETR state has ambiguous catalog topology");
  match = &preset;
 }
 return match;
}
void validate_queries(const NativeCheckpointMetadata& metadata, const std::string& path, bool required) {
 if (!required && metadata.num_queries == 0 && metadata.num_select == 0) { return; }
 if (metadata.num_queries <= 0 || metadata.num_select <= 0 || metadata.num_select > metadata.num_queries) { throw std::runtime_error("RF-DETR native checkpoint has invalid query metadata: " + path); }
}
void apply_checkpoint_detection_overrides(NativeRfDetrConfig& config, const NativeCheckpointMetadata& metadata) {
 model_state_detail::visit_matching_members(config, metadata, []<class Target, class Source>(Target& target, const Source& source) {
  if constexpr (model_state_detail::is_scalar_optional<std::remove_cvref_t<Source>>) {
   if (source) target = *source;
  }
 });
}
void capture_checkpoint_detection_metadata(NativeCheckpointMetadata& metadata, const NativeRfDetrConfig& config) {
 model_state_detail::visit_matching_members(metadata, config, []<class Target, class Source>(Target& target, const Source& source) {
  if constexpr (model_state_detail::is_scalar_optional<std::remove_cvref_t<Target>>) target = source;
 });
}
template <class Config, class Metadata>
void validate_checkpoint_topology(const Config& config, const Metadata& metadata) {
 template for (constexpr auto evidence : std::define_static_array(std::meta::nonstatic_data_members_of(^^Metadata, std::meta::access_context::current()))) {
  if constexpr (std::meta::identifier_of(evidence).starts_with("topology_")) {
   template for (constexpr auto field : std::define_static_array(std::meta::nonstatic_data_members_of(^^Config, std::meta::access_context::current()))) {
    if constexpr (std::meta::identifier_of(evidence).substr(std::string_view("topology_").size()) == std::meta::identifier_of(field)) {
     if (metadata.[:evidence:] && *metadata.[:evidence:] != config.[:field:])
      throw std::invalid_argument("RF-DETR checkpoint topology disagrees with selected preset: " + std::string(std::meta::identifier_of(field)));
    }
   }
  }
 }
}
consteval bool configuration_policy_field(std::meta::info member) {
 const auto name = std::meta::identifier_of(member);
 if (name == "preset_name" || name == "resolution" || name == "num_select" || name == "training_supervision") return true;
 return []<class Metadata>(std::string_view field_name) consteval {
  template for (constexpr auto evidence : std::define_static_array(std::meta::nonstatic_data_members_of(^^Metadata, std::meta::access_context::current()))) {
   using Value = std::remove_cvref_t<decltype(std::declval<Metadata>().[:evidence:])>;
   if constexpr (model_state_detail::is_scalar_optional<Value>)
    if (std::meta::identifier_of(evidence) == field_name) return true;
  }
  return false;
 }.template operator()<NativeCheckpointMetadata>(name);
}
}  // namespace
std::string encode_checkpoint_configuration(const NativeRfDetrConfig& config) {
 if (!native_model_config_valid(config)) throw std::invalid_argument("invalid native RF-DETR checkpoint configuration");
 std::vector<std::byte> bytes;
 if (!mmltk::frameworks::serialization::encode(config, bytes, {.max_bytes = 65536U, .max_items = 2U * kMaximumClassOutputSlots}))
  throw std::invalid_argument("RF-DETR configuration exceeds artifact budget");
 auto text = mmltk::frameworks::serialization::json_from_cbor({bytes, {}}, {.max_bytes = 65536U, .max_items = 2U * kMaximumClassOutputSlots}).dump();
 if (text.size() > 65536U) throw std::invalid_argument("RF-DETR configuration escaped JSON exceeds artifact budget");
 return text;
}
NativeRfDetrConfig decode_checkpoint_configuration(std::string_view text) {
 if (text.empty() || text.size() > 65536U) throw std::invalid_argument("missing or oversized native RF-DETR configuration");
 const auto bytes = mmltk::frameworks::serialization::json_to_cbor(text, {.max_bytes = 65536U, .max_items = 2U * kMaximumClassOutputSlots});
 auto decoded = mmltk::frameworks::serialization::decode<NativeRfDetrConfig>({bytes, {}}, {.max_bytes = 65536U, .max_items = 2U * kMaximumClassOutputSlots});
 if (!decoded || !native_model_config_valid(*decoded)) throw std::invalid_argument("malformed native RF-DETR configuration");
 return std::move(*decoded);
}
NativeRfDetrConfig checkpoint_configuration(const NativeCheckpointMetadata& metadata) {
 NativeRfDetrConfig result;
 if (!metadata.native_configuration.empty())
  result = decode_checkpoint_configuration(metadata.native_configuration);
 else {
  const auto* preset = find_preset_catalog_entry(metadata.preset_name);
  if (!preset) throw std::invalid_argument("legacy/external RF-DETR state requires an admitted preset");
  result = native_config_from_preset(*preset);
  result.num_classes = static_cast<int>(metadata.num_classes);
  if (metadata.num_queries > 0) result.num_queries = static_cast<int>(metadata.num_queries);
  if (metadata.num_select > 0) result.num_select = static_cast<int>(metadata.num_select);
 }
 apply_checkpoint_detection_overrides(result, metadata);
 if (result.num_classes != metadata.num_classes || result.num_queries != metadata.num_queries || result.num_select != metadata.num_select)
  throw std::invalid_argument("native RF-DETR configuration conflicts with checkpoint class/query metadata");
 if (!metadata.external_topology.empty()) {
  const auto topology = nlohmann::json::parse(metadata.external_topology);
  const auto require = [&]<class Value>(std::string_view name, const Value& expected) {
   const auto found = topology.find(name);
   if (found != topology.end() && *found != nlohmann::json(expected)) throw std::invalid_argument("unsupported or conflicting external RF-DETR topology: " + std::string(name));
  };
  // One external-to-native conversion; ordinary native configuration remains canonical.
  require("encoder", backbone_encoder_name(result.backbone.kind));
  require("out_feature_indexes", result.backbone.features);
  require("vit_encoder_num_layers", result.backbone.layers);
  require("projector_scale", std::array{std::string_view("P4")});
  require("position_embedding", "sine");
  require("decoder_norm", "LN");
  require("decoder_norm_type", "LN");
  require("layer_norm", true);
  require("rms_norm", false);
  require("encoder_only", false);
  require("backbone_only", false);
  require("num_channels", 3);
  require("num_feature_levels", 1);
  require("dropout", 0.0);
  require("drop_path", 0.0);
  require("drop_path_rate", 0.0);
  require("num_decoder_registers", 0);
  require("num_registers", 0);
  require("num_register_tokens", 0);
  require("mask_downsample_ratio", 4);
  require("use_grouppose_keypoints", result.has_keypoints());
  require("dual_projector", result.projector == ProjectorKind::DualKeypointOnly);
  require("dual_projector_kp_only", result.projector == ProjectorKind::DualKeypointOnly);
  require("grouppose_keypoint_dim_downscale", result.keypoint_dimension_downscale);
  require("keypoint_cross_attn", result.keypoint_cross_attention);
  require("inter_instance_kp_attn", result.inter_instance_keypoint_attention);
  if (const auto counts = topology.find("num_keypoints_per_class"); counts != topology.end()) {
   if (!counts->is_array() || counts->size() > kMaximumClassOutputSlots) throw std::invalid_argument("invalid external keypoint counts");
   std::vector<std::uint16_t> admitted;
   for (const auto& count : *counts) {
    if (!count.is_number_integer() || count.get<int64_t>() < 0 || count.get<int64_t>() > static_cast<int64_t>(mmltk::backend::data::catalog::kKeypointsPerClassCapacity))
     throw std::invalid_argument("invalid external joint count");
    admitted.push_back(count.get<std::uint16_t>());
   }
   if (metadata.native_configuration.empty())
    result.keypoints_per_class = std::move(admitted);
   else
    require("num_keypoints_per_class", result.keypoints_per_class);
  }
  if (const auto schedule = topology.find("window_block_indexes"); schedule != topology.end() && !schedule->is_null()) {
   std::vector<int> windowed;
   if (result.backbone.kind == BackboneKind::PeCoreTiny) throw std::invalid_argument("PE-Core-T does not admit a separate window block schedule");
   for (int index = 0; index < result.backbone.layers; ++index)
    if (std::ranges::find(result.backbone.features, index) == result.backbone.features.end()) windowed.push_back(index);
   require("window_block_indexes", windowed);
  }
  [&]<class Config>(const Config& configuration) {
   template for (constexpr auto field : std::define_static_array(std::meta::nonstatic_data_members_of(^^Config, std::meta::access_context::current()))) {
    using Value = std::remove_cvref_t<decltype(configuration.[:field:])>;
    if constexpr (std::is_arithmetic_v<Value>) require(std::meta::identifier_of(field), configuration.[:field:]);
   }
  }(result);
 }
 validate_checkpoint_topology(result, metadata);
 if (!native_model_config_valid(result)) throw std::invalid_argument("RF-DETR checkpoint has unsupported topology");
 return result;
}
void require_checkpoint_model_topology(const NativeRfDetrConfig& destination, const NativeCheckpointMetadata& metadata) {
 const auto source = checkpoint_configuration(metadata);
 // Execution resolution, selection and training policy can be requested anew.
 // Every remaining native fact participates automatically in exact topology.
 []<class Config>(const Config& admitted, const Config& selected) {
  template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^Config, std::meta::access_context::current()))) {
   constexpr bool policy = configuration_policy_field(member);
   if constexpr (!policy)
    if (admitted.[:member:] != selected.[:member:]) throw std::invalid_argument("exact checkpoint loading conflicts with model topology: " + std::string(std::meta::identifier_of(member)));
  }
 }(source, destination);
}
bool same_native_model_semantics(const NativeCheckpointMetadata& left, const NativeCheckpointMetadata& right) {
 if (left.class_layout != right.class_layout) return false;
 const auto a = checkpoint_configuration(left);
 auto b = checkpoint_configuration(right);
 // Optional metadata records supplied evidence; the admitted configuration
 // owns its effective meaning. Resolution belongs to the execution context
 // and was not persisted by format 4. Session/data identities bind that context.
 b.resolution = a.resolution;
 return a == b;
}
void validate_decoded_model_state(const DecodedNativeModelState& state) {
 if (state.metadata.encoder_pretraining && !encoder_pretraining_provenance_valid(*state.metadata.encoder_pretraining)) throw std::invalid_argument("invalid encoder pretraining provenance");
 const ResolvedClassLayout layout(state.metadata.class_layout);
 if (state.metadata.num_classes <= 0 || layout.output_width() != static_cast<std::size_t>(state.metadata.num_classes))
  throw std::invalid_argument("checkpoint output width disagrees with its class layout");
 std::optional<NativeRfDetrConfig> config;
 if (!state.metadata.native_configuration.empty() || !state.metadata.preset_name.empty()) config = checkpoint_configuration(state.metadata);
 std::optional<KeypointTensorLayout> keypoints;
 if (config) keypoints.emplace(config->keypoints_per_class);
 std::unordered_set<std::string_view> names;
 names.reserve(state.tensor_count());
 for (const auto& entry : state.entries()) {
  if (entry.name.empty() || entry.name.size() > 4096 || !names.insert(entry.name).second) throw std::invalid_argument("checkpoint tensor names must be unique and nonempty");
  if (!entry.tensor.defined()) throw std::invalid_argument("undefined checkpoint tensor: " + entry.name);
  if (config) {
   const auto& value = *config;
   if (entry.name == "_kp_active_mask") {
    const auto supplied = KeypointTensorLayout::from_active_mask(entry.tensor);
    if (!std::ranges::equal(supplied.counts(), keypoints->counts())) throw std::invalid_argument("checkpoint active mask conflicts with model configuration");
    supplied.require_class_layout(layout);
   }
   if (entry.name == "transformer.decoder.keypoint_class_mask" &&
       (!value.has_keypoints() || entry.tensor.scalar_type() != torch::kBool || !torch::equal(entry.tensor.to(torch::kCPU), keypoints->class_mask())))
    throw std::invalid_argument("checkpoint keypoint attention mask disagrees with schema");
   const bool dino_patch = entry.name == "backbone.0.encoder.encoder.embeddings.patch_embeddings.projection.weight";
   const bool pe_patch = entry.name == "backbone.0.encoder.model.patch_embed.proj.weight";
   if (dino_patch || pe_patch) {
    const std::array<int64_t, 4> expected{value.backbone.dimension, 3, value.patch_size, value.patch_size};
    if (pe_patch != (value.backbone.kind == BackboneKind::PeCoreTiny) || entry.tensor.sizes() != c10::IntArrayRef(expected) || !entry.tensor.is_floating_point())
     throw std::invalid_argument("checkpoint patch embedding disagrees with admitted backbone");
   }
  }
  const auto axis = detail::class_tensor_shape(entry.name);
  if (!axis) continue;
  if (!entry.tensor.defined() || entry.tensor.dim() != axis->rank || entry.tensor.size(axis->dimension) <= 0) throw std::invalid_argument("invalid class-dependent checkpoint tensor: " + entry.name);
  if (axis->coordinates == detail::ClassTensorCoordinates::NamedJoints) {
   if (!config || !config->has_keypoints() || entry.tensor.size(axis->dimension) != keypoints->compact_count())
    throw std::invalid_argument("joint-dependent checkpoint tensor disagrees with admitted schema: " + entry.name);
   continue;
  }
  const bool classifier = axis->coordinates == detail::ClassTensorCoordinates::OutputSlots;
  const auto box_columns = axis->coordinates == detail::ClassTensorCoordinates::ForegroundWithBoxes ? 4 : 0;
  if (classifier || layout.record().supervision_in_foreground_order) {
   const auto expected = classifier ? layout.output_width() : layout.catalog()->size() + box_columns;
   if (entry.tensor.size(axis->dimension) != static_cast<std::int64_t>(expected)) throw std::invalid_argument("class-dependent checkpoint tensor disagrees with layout: " + entry.name);
  } else if (entry.tensor.size(axis->dimension) <= box_columns) {
   throw std::invalid_argument("external supervision tensor has no class-input block: " + entry.name);
  }
 }
 if (config && config->has_keypoints()) {
  if (!names.contains("_kp_active_mask") || !names.contains("transformer.decoder.keypoint_class_mask")) throw std::invalid_argument("keypoint checkpoint is missing its schema masks");
  keypoints->require_class_layout(layout);
 }
}
ModelStateContainer identify_model_state_container(const std::filesystem::path& checkpoint_path) {
 const auto path = canonical_path(checkpoint_path);
 std::ifstream stream(path, std::ios::binary);
 std::array<char, 4> signature{};
 stream.read(signature.data(), signature.size());
 if (!stream) throw std::invalid_argument("truncated checkpoint container");
 if (signature != std::array<char, 4>{'P', 'K', 3, 4})
  return static_cast<unsigned char>(signature[0]) == 0x80 && signature[1] >= 2 && signature[1] <= 5 ? ModelStateContainer::Python : ModelStateContainer::Unknown;
 try {
  caffe2::serialize::PyTorchStreamReader reader(path.string());
  // InputArchive is a TorchScript archive. Python pickle archives do not
  // contain its constants/code members. Identification never loads tensors.
  const auto records = reader.getAllRecords();
  const bool native = std::ranges::any_of(records, [](const auto& name) { return name == "constants.pkl" || name.starts_with("code/"); });
  if (native && reader.getRecordSize("data.pkl") > 16U * kClassLayoutByteBudget) throw std::invalid_argument("native checkpoint metadata exceeds admission budget");
  return native ? ModelStateContainer::Native : reader.hasRecord("data.pkl") ? ModelStateContainer::Python : ModelStateContainer::Unknown;
 } catch (const std::exception& error) { throw std::runtime_error("corrupt RF-DETR checkpoint archive: " + path.string() + ": " + error.what()); }
}
bool is_native_checkpoint_file(const std::filesystem::path& checkpoint_path) { return identify_model_state_container(checkpoint_path) == ModelStateContainer::Native; }
static DecodedNativeModelState load_native_model_state(const std::filesystem::path& checkpoint_path, std::stop_token stop) {
 const auto canonical = canonical_path(checkpoint_path);
 const auto path = canonical.string();
 const auto snapshot = mmltk::common::io::FileSnapshot::Read(canonical);
 if (!is_native_checkpoint_file(canonical)) throw std::invalid_argument("not an RF-DETR native archive");
 auto admitted_archive = std::make_unique<model_state_detail::InputArchive>();
 auto& archive = *admitted_archive;
 const auto file = mmltk::common::io::FileHandle::open_readonly(path);
 const auto bytes = file.size();
 archive.load_from([&](std::uint64_t offset, void* target, std::size_t count) {
  if (offset > bytes || count > bytes - offset) throw std::runtime_error("checkpoint archive read outside file");
  std::size_t copied = 0;
  while (copied != count) {
   if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
   const auto chunk = std::min<std::size_t>(count - copied, 1024U * 1024U);
   file.pread_all(static_cast<std::byte*>(target) + copied, chunk, offset + copied);
   copied += chunk;
  }
  return copied;
 }, [&] { return bytes; }, torch::Device(torch::kCPU));
 if (!supported_format(mmltk::backend::ml::serialization::require_string(archive, "format"))) { throw std::runtime_error("RF-DETR checkpoint is not a native checkpoint: " + path); }
 const auto version = mmltk::backend::ml::serialization::require_int(archive, "format_version");
 if (version != 4 && version != kNativeCheckpointFormatVersion) { throw std::runtime_error("unsupported RF-DETR native checkpoint format version " + std::to_string(version) + ": " + path); }
 DecodedNativeModelState result;
 auto& metadata = result.metadata;
 if (version == kNativeCheckpointFormatVersion) {
  metadata.native_configuration = mmltk::backend::ml::serialization::require_string(archive, "native_configuration");
  static_cast<void>(decode_checkpoint_configuration(metadata.native_configuration));
 }
 metadata.preset_name = mmltk::backend::ml::serialization::read_optional_value<std::string>(archive, "preset_name").value_or("");
 metadata.source_kind = mmltk::backend::ml::serialization::read_optional_value<std::string>(archive, "source_kind").value_or("native");
 metadata.source_path = mmltk::backend::ml::serialization::read_optional_value<std::string>(archive, "source_path").value_or(path);
 c10::IValue provenance;
 if (archive.try_read("encoder_pretraining_cbor", provenance)) {
  if (!provenance.isTensor()) throw std::runtime_error("invalid encoder pretraining CBOR metadata");
  metadata.encoder_pretraining = mmltk::backend::ml::serialization::read_cbor_record<EncoderPretrainingProvenance>(
   provenance.toTensor(), {.max_items = 32, .max_depth = 4}, "invalid encoder pretraining CBOR extent", "invalid encoder pretraining provenance");
  if (!encoder_pretraining_provenance_valid(*metadata.encoder_pretraining)) throw std::runtime_error("invalid encoder pretraining provenance");
 }
 metadata.num_classes = mmltk::backend::ml::serialization::read_optional_value<int64_t>(archive, "num_classes").value_or(0);
 metadata.num_queries = mmltk::backend::ml::serialization::require_int(archive, "num_queries");
 metadata.num_select = mmltk::backend::ml::serialization::require_int(archive, "num_select");
 validate_queries(metadata, path, true);
 metadata.class_layout = decode_class_layout(mmltk::backend::ml::serialization::require_string(archive, "class_layout"));
 if (metadata.num_classes < 0 || static_cast<std::size_t>(metadata.num_classes) != metadata.class_layout.slots.size())
  throw std::runtime_error("native checkpoint output width disagrees with class layout");
 metadata.for_each_optional_field(
  [&archive]<class Name, class Optional>(const Name& name, Optional& field) { field = mmltk::backend::ml::serialization::read_optional_value<typename Optional::value_type>(archive, name); });
 model_state_detail::InputArchive state_archive;
 archive.read("state", state_archive);
 const auto entry_count = mmltk::backend::ml::serialization::require_int(state_archive, "entry_count");
 if (entry_count < 0 || entry_count > 100000) { throw std::runtime_error("RF-DETR checkpoint entry_count is negative: " + path); }
 std::vector<NormalizedModelStateEntry> entries;
 entries.reserve(static_cast<std::size_t>(entry_count));
 for (int64_t index = 0; index < entry_count; ++index) {
  if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
  model_state_detail::InputArchive entry_archive;
  state_archive.read(mmltk::backend::ml::serialization::archive_entry_name(static_cast<std::size_t>(index)), entry_archive);
  NormalizedModelStateEntry entry;
  entry.name = mmltk::backend::ml::serialization::require_string(entry_archive, "name");
  entry_archive.read("tensor", entry.tensor);
  entries.push_back(std::move(entry));
 }
 result.replace_entries(std::move(entries));
 if (version == 4) {
  const auto converted = checkpoint_configuration(metadata);
  if (converted.backbone.kind != BackboneKind::DinoV2Small || converted.has_keypoints() || converted.dec_layers == 0 || metadata.class_layout.foreground.keypoints)
   throw std::invalid_argument("native format 4 contains topology outside its historical schema");
  metadata.native_configuration = encode_checkpoint_configuration(converted);
 }
 validate_decoded_model_state(result);
 result.retain_admitted_archive(std::move(admitted_archive));
 snapshot.RequireUnchanged(canonical);
 return result;
}
namespace {
void normalize_legacy_official_queries(DecodedNativeModelState& state, const NativeRfDetrConfig& config) {
 const int64_t rows = static_cast<int64_t>(config.num_queries) * config.group_detr;
 const std::array specifications{std::pair{detail::kQueryFeatureWeight, static_cast<int64_t>(config.hidden_dim)}, std::pair{detail::kReferencePointWeight, int64_t{4}}};
 auto entries = state.consume_entries();
 std::array<NormalizedModelStateEntry*, specifications.size()> queries{};
 int64_t source_rows = 0;
 for (std::size_t index = 0; index < specifications.size(); ++index) {
  const auto& [name, width] = specifications[index];
  const auto found = std::ranges::find(entries, name, &NormalizedModelStateEntry::name);
  if (found == entries.end() || !found->tensor.defined() || found->tensor.dim() != 2 || found->tensor.size(0) < rows || found->tensor.size(1) != width ||
      (source_rows && found->tensor.size(0) != source_rows))
   throw std::invalid_argument("verified legacy asset has incompatible query tensor: " + std::string(name));
  queries[index] = &*found;
  source_rows = found->tensor.size(0);
 }
 // Pinned upstream weights.load_pretrain_weights retains this flat prefix
 // when published legacy assets omit both query-count and group metadata.
 if (source_rows > rows)
  for (auto* query : queries) query->tensor = query->tensor.narrow(0, 0, rows).clone();
 state.replace_entries(std::move(entries));
}
void normalize_legacy_keypoint_projection(DecodedNativeModelState& state, const NativeRfDetrConfig& config) {
 constexpr std::string_view prefix = "keypoint_head.keypoint_proj.";
 if (std::ranges::none_of(state.entries(), [&](const auto& entry) { return entry.name.starts_with(prefix); })) return;
 // The pinned Preview checkpoint retains this unused pre-GroupPose head
 // (upstream models/weights.py). Admit its complete verified inventory only.
 const KeypointTensorLayout keypoints(config.keypoints_per_class);
 if (!config.has_keypoints() || config.hidden_dim != 256 || config.num_classes != 2 || config.keypoints_per_class.size() != 2 || keypoints.compact_count() != 17 || keypoints.maximum_joints() != 17)
  throw std::invalid_argument("legacy GroupPose projection disagrees with admitted topology");
 constexpr std::array<int64_t, 2> hidden_weight{256, 256}, projection_weight{238, 256};
 constexpr std::array<int64_t, 1> hidden_bias{256}, projection_bias{238};
 const std::array<std::pair<std::string_view, c10::IntArrayRef>, 4> expected{{
  {"keypoint_head.keypoint_proj.0.weight", hidden_weight},
  {"keypoint_head.keypoint_proj.0.bias", hidden_bias},
  {"keypoint_head.keypoint_proj.2.weight", projection_weight},
  {"keypoint_head.keypoint_proj.2.bias", projection_bias},
 }};
 std::array<bool, 4> seen{};
 for (const auto& entry : state.entries()) {
  if (!entry.name.starts_with(prefix)) continue;
  const auto found = std::ranges::find_if(expected, [&](const auto& specification) { return specification.first == entry.name; });
  if (found == expected.end()) throw std::invalid_argument("unknown legacy GroupPose projection tensor: " + entry.name);
  const auto index = static_cast<std::size_t>(found - expected.begin());
  if (seen[index] || !entry.tensor.defined() || entry.tensor.scalar_type() != torch::kFloat32 || entry.tensor.sizes() != found->second)
   throw std::invalid_argument("invalid legacy GroupPose projection tensor: " + entry.name);
  seen[index] = true;
 }
 if (!std::ranges::all_of(seen, [](bool supplied) { return supplied; })) throw std::invalid_argument("incomplete legacy GroupPose projection");
 auto entries = state.consume_entries();
 std::erase_if(entries, [&](const auto& entry) { return entry.name.starts_with(prefix); });
 state.replace_entries(std::move(entries));
}
}  // namespace
DecodedNativeModelState decode_model_state(
 const std::filesystem::path& checkpoint_path, std::shared_ptr<const ClassArtifactAdmission> admission, const std::filesystem::path& class_layout_path, std::stop_token stop) {
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
 bool normalize_legacy_queries = false;
 if (native) {
  result = load_native_model_state(canonical, stop);
  if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
 } else {
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
  result = decode_upstream_python_model_state(canonical);
  if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
  const bool lacks_query_layout = result.metadata.num_queries <= 0 && result.metadata.native_configuration.empty() &&
                                  (result.metadata.external_topology.empty() || !nlohmann::json::parse(result.metadata.external_topology).contains("group_detr"));
  result.metadata.source_kind = "upstream-python";
  result.metadata.source_path = canonical.string();
  const PresetCatalogEntry* preset = nullptr;
  for (const auto& asset : weight_catalog())
   if (asset.md5_hash == digests->md5) {
    preset = find_model_preset_by_weight_filename(asset.filename);
    break;
   }
  if (!preset && !result.metadata.native_configuration.empty()) preset = find_preset_catalog_entry(decode_checkpoint_configuration(result.metadata.native_configuration).preset_name);
  if (!preset) preset = find_model_preset_by_weight_filename(canonical.filename().string());
  if (preset == nullptr) { preset = infer_model_preset_from_path(canonical); }
  if (!preset) preset = infer_state_preset(result.entries());
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
  }
  for (const auto& asset : weight_catalog()) {
   if (asset.md5_hash != digests->md5) continue;
   normalize_legacy_queries = lacks_query_layout;
   if (asset.classifier_width != result.metadata.num_classes) throw std::invalid_argument("verified asset classifier width disagrees with its hash-bound evidence");
   const auto embedded = result.metadata.class_layout;
   auto source_evidence = std::move(result.metadata.class_layout.class_name_evidence);
   const ClassLayoutProvenance provenance{ClassLayoutOrigin::VerifiedAsset, std::string(asset.filename), mmltk::common::io::sha256_hex(digests->sha256)};
   switch (asset.class_layout) {
    case OfficialClassLayout::CocoSparse: result.metadata.class_layout = coco_class_layout(provenance); break;
    case OfficialClassLayout::PersonActiveFirst:
    case OfficialClassLayout::PersonBackgroundFirst: {
     auto& layout = result.metadata.class_layout;
     layout.foreground.names = {{"person"}};
     layout.foreground.keypoints.emplace(1U, mmltk::backend::data::catalog::coco_person_keypoint_schema());
     layout.slots.assign(asset.classifier_width, {ClassSlotRole::Unused, std::nullopt});
     const auto foreground_slot = asset.class_layout == OfficialClassLayout::PersonActiveFirst ? 0U : 1U;
     if (foreground_slot >= layout.slots.size()) throw std::invalid_argument("verified person asset has an invalid foreground slot");
     layout.slots[foreground_slot] = {ClassSlotRole::Foreground, 0U};
     const auto background_slot = asset.class_layout == OfficialClassLayout::PersonActiveFirst ? 1U : 0U;
     if (background_slot >= layout.slots.size()) throw std::invalid_argument("verified person asset has an invalid background slot");
     layout.slots[background_slot] = {ClassSlotRole::Background, std::nullopt};
     layout.no_object = NoObjectEncoding::ExplicitBackground;
     layout.provenance = provenance;
     break;
    }
    case OfficialClassLayout::Unresolved: throw std::invalid_argument("registered official asset has ambiguous class evidence");
   }
   result.metadata.class_layout.class_name_evidence = std::move(source_evidence);
   if (ResolvedClassLayout(embedded).semantic()) {
    auto expected = result.metadata.class_layout;
    auto supplied = embedded;
    expected.provenance = supplied.provenance;
    if (expected != supplied) throw std::invalid_argument("official checkpoint class layout conflicts with hash-bound evidence");
   }
   break;
  }
  const ResolvedClassLayout layout(result.metadata.class_layout);
  if (layout.output_width() != static_cast<std::size_t>(result.metadata.num_classes)) throw std::runtime_error("external checkpoint output width disagrees with class layout");
#else
  throw std::runtime_error("RF-DETR upstream Python checkpoint loading is disabled at build time: " + canonical.string());
#endif
 }
 result.metadata.class_layout = admission->Resolve(result.metadata.num_classes, std::optional{result.metadata.class_layout}, stop);
 if (!native && !result.metadata.preset_name.empty()) {
  auto config = checkpoint_configuration(result.metadata);
  const auto active = std::ranges::find(result.entries(), std::string_view("_kp_active_mask"), &NormalizedModelStateEntry::name);
  if (active != result.entries().end()) {
   const auto keypoints = KeypointTensorLayout::from_active_mask(active->tensor);
   const auto counts = keypoints.counts();
   if (!config.has_keypoints() && !counts.empty()) throw std::invalid_argument("active joint mask on a non-keypoint model");
   if (!result.metadata.native_configuration.empty() && !std::ranges::equal(counts, config.keypoints_per_class)) throw std::invalid_argument("native configuration conflicts with active joint mask");
   config.keypoints_per_class.assign(counts.begin(), counts.end());
   if (counts.empty()) {
    auto entries = result.consume_entries();
    std::erase_if(entries, [](const auto& entry) { return entry.name == "_kp_active_mask"; });
    result.replace_entries(std::move(entries));
   }
  }
  KeypointTensorLayout(config.keypoints_per_class).require_class_layout(ResolvedClassLayout(result.metadata.class_layout));
  if (normalize_legacy_queries) normalize_legacy_official_queries(result, config);
  normalize_legacy_keypoint_projection(result, config);
  result.metadata.native_configuration = encode_checkpoint_configuration(config);
  // External keypoint counts may be stale after upstream head resizing. The
  // tensor mask is authoritative; discard only that superseded optional field.
  if (!result.metadata.external_topology.empty()) {
   auto topology = nlohmann::json::parse(result.metadata.external_topology);
   topology.erase("num_keypoints_per_class");
   result.metadata.external_topology = topology.dump();
  }
 }
 validate_decoded_model_state(result);
 admission->RequireUnchanged(stop);
 result.class_artifact = std::move(admission);
 return result;
}
DecodedNativeModelState decode_native_model_state(const std::filesystem::path& checkpoint_path, std::stop_token stop) {
 auto admission = std::make_shared<const ClassArtifactAdmission>(checkpoint_path, std::filesystem::path{}, nullptr, stop);
 auto result = load_native_model_state(admission->artifact_path(), stop);
 result.metadata.class_layout = admission->Resolve(result.metadata.num_classes, result.metadata.class_layout, stop);
 result.class_artifact = std::move(admission);
 return result;
}
ResolvedModelState resolve_model_state(const std::filesystem::path& weights_path, const std::string_view preset_name, const int resolution, const std::filesystem::path& class_layout_path,
 std::shared_ptr<const ClassArtifactAdmission> admission, std::stop_token stop) {
 if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
 const auto canonical = canonical_path(weights_path);
 auto state = decode_model_state(canonical, std::move(admission), class_layout_path, stop);
 auto artifacts = resolve_admitted_model_artifacts(state, canonical, preset_name, resolution, class_layout_path, stop);
 return {.artifacts = std::move(artifacts), .model_state = std::move(state)};
}
ResolvedModelArtifacts resolve_admitted_model_artifacts(
 const DecodedNativeModelState& state, const std::filesystem::path& weights_path, std::string_view preset_name, int resolution, const std::filesystem::path& class_layout_path, std::stop_token stop) {
 if (!state.class_artifact) throw std::invalid_argument("model resolution requires admitted file evidence");
 state.class_artifact->RequireUnchanged(stop);
 const auto canonical = canonical_path(weights_path);
 if (canonical != state.class_artifact->artifact_path()) throw std::invalid_argument("model resolution path differs from admitted state");
 auto evidence = state.class_artifact;
 if (!class_layout_path.empty() && std::filesystem::absolute(class_layout_path).lexically_normal() != evidence->descriptor_path())
  evidence = std::make_shared<const ClassArtifactAdmission>(canonical, class_layout_path, evidence->file(), stop);
 const PresetCatalogEntry* preset = nullptr;
 if (!state.metadata.preset_name.empty()) { preset = find_preset_catalog_entry(state.metadata.preset_name); }
 // External pretrained inference already ran in decode_model_state. Native
 // artifacts use embedded metadata or the caller's explicit preset.
 if (preset == nullptr && !preset_name.empty()) { preset = find_preset_catalog_entry(preset_name); }
 if (preset == nullptr && state.metadata.native_configuration.empty()) { throw std::runtime_error("unable to resolve RF-DETR weights preset"); }
 ResolvedModelArtifacts result;
 result.input_kind = state.metadata.source_kind == "upstream-python" ? "upstream-python" : "native-pt";
 result.encoder_pretraining = state.metadata.encoder_pretraining;
 result.input_path = canonical;
 result.weights_path = canonical;
 result.artifact_root = canonical.parent_path();
 result.preset_name = preset ? std::string(preset->preset_name) : decode_checkpoint_configuration(state.metadata.native_configuration).preset_name;
 result.model_id = canonical.stem().string();
 result.class_layout = evidence == state.class_artifact ? state.metadata.class_layout : evidence->Resolve(state.metadata.num_classes, state.metadata.class_layout, stop);
 result.artifact_sha256 = mmltk::common::io::sha256_hex(state.class_artifact->file()->sha256);
 auto configured_metadata = state.metadata;
 if (configured_metadata.native_configuration.empty() && configured_metadata.preset_name.empty() && preset) {
  configured_metadata.preset_name = std::string(preset->preset_name);
  if (configured_metadata.num_queries <= 0) configured_metadata.num_queries = preset->query_count;
  if (configured_metadata.num_select <= 0) configured_metadata.num_select = preset->selected_query_count;
 }
 result.config = checkpoint_configuration(configured_metadata);
 if (state.metadata.num_classes > 0) { result.config.num_classes = static_cast<int>(state.metadata.num_classes); }
 result.source_num_queries = state.metadata.num_queries > 0 ? static_cast<int>(state.metadata.num_queries) : result.config.num_queries;
 result.source_num_select = state.metadata.num_select > 0 ? static_cast<int>(state.metadata.num_select) : result.config.num_select;
 result.automatic_num_queries_cap = result.config.num_queries;
 if (!preset_name.empty() && result.config.preset_name != preset_name) {
  const auto* declared = find_preset_catalog_entry(preset_name);
  if (declared == nullptr) { throw std::runtime_error("unknown RF-DETR preset override: " + std::string(preset_name)); }
  const auto classes = result.config.num_classes;
  result.config = native_config_from_preset(*declared);
  result.config.num_classes = classes;
  result.automatic_num_queries_cap = result.config.num_queries;
 }
 result.config.num_queries = result.source_num_queries;
 result.config.num_select = result.source_num_select;
 if (resolution > 0) { result.config.resolution = resolution; }
 validate_checkpoint_topology(result.config, state.metadata);
 apply_checkpoint_detection_overrides(result.config, state.metadata);
 if (!native_model_config_valid(result.config)) throw std::invalid_argument("RF-DETR artifact has invalid task or head configuration");
 return result;
}
NativeCheckpointMetadata make_native_checkpoint_metadata(const ResolvedModelArtifacts& artifacts, const int64_t num_classes) {
 NativeCheckpointMetadata metadata;
 metadata.class_layout = artifacts.class_layout;
 metadata.encoder_pretraining = artifacts.encoder_pretraining;
 metadata.preset_name = artifacts.config.preset_name;
 metadata.source_kind = artifacts.input_kind;
 metadata.source_path = artifacts.input_path.string();
 metadata.num_classes = num_classes;
 metadata.num_queries = artifacts.config.num_queries;
 metadata.num_select = artifacts.config.num_select;
 capture_checkpoint_detection_metadata(metadata, artifacts.config);
 metadata.native_configuration = encode_checkpoint_configuration(artifacts.config);
 return metadata;
}
ResolvedModelArtifacts resolve_model_artifacts(const std::filesystem::path& weights_path, std::string_view preset_name, int resolution) {
 return resolve_model_state(weights_path, preset_name, resolution).artifacts;
}
}  // namespace mmltk::backend::models::rfdetr
