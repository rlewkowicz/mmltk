#include "src/backend/ml/torch/tests/catch_support.h"
#include "checkpoint_fixture_support/upstream_oracle_fixture.h"
#include "src/backend/ml/torch/scalar_type.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/core/tests/training_fixture.h"
#include "src/common/io/file_digest.h"
#include <caffe2/serialize/inline_container.h>
#include <c10/core/ScalarType.h>
#include <torch/csrc/jit/serialization/import_read.h>
#include <torch/csrc/autograd/autograd.h>
#include <torch/version.h>
#include <ATen/Context.h>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/autocast_mode.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <nlohmann/json.hpp>
#include "src/backend/models/rfdetr/contract/output_roles.h"
namespace mmltk::backend::models::rfdetr::testsupport {
namespace {
constexpr std::uint64_t kManifestLimit = 32U * 1024U * 1024U;
constexpr std::uint64_t kArchiveLimit = 4ULL * 1024U * 1024U * 1024U;
constexpr std::uint64_t kStepStorageLimit = 64ULL * 1024U * 1024U * 1024U;
constexpr std::size_t kShardLimit = 256, kTensorLimit = 20000;
using Json = nlohmann::json;
[[noreturn]] void invalid(const std::string& message) { throw std::runtime_error("RF-DETR upstream oracle: " + message); }
std::filesystem::path child(const std::filesystem::path& parent, const std::string& name) {
 if (std::filesystem::is_symlink(parent) || name.empty() || name.size() > 256 || std::filesystem::path(name).filename() != name || name == "." || name == "..") invalid("invalid artifact filename");
 const auto value = parent / name;
 if (std::filesystem::is_symlink(value)) invalid("artifact symlink is not admitted: " + name);
 return value;
}
void verify_hash(const std::filesystem::path& file, const std::string& hash) {
 if (hash.size() != 64 || mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(file)) != hash) invalid("SHA256 mismatch: " + file.string());
}
Json read_index(const std::filesystem::path& path) {
 if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path) || std::filesystem::file_size(path) > kManifestLimit) invalid("invalid evidence JSON");
 std::ifstream input(path);
 std::vector<std::set<std::string>> keys;
 return Json::parse(input, [&](int, Json::parse_event_t event, Json& value) {
  if (event == Json::parse_event_t::object_start)
   keys.emplace_back();
  else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
   invalid("duplicate evidence JSON key");
  else if (event == Json::parse_event_t::object_end)
   keys.pop_back();
  return true;
 });
}
at::ScalarType oracle_dtype(at::ScalarType dtype) {
 // The oracle admits a strict subset of the shared scalar vocabulary.
 switch (dtype) {
  case at::kFloat:
  case at::kHalf:
  case at::kBFloat16:
  case at::kDouble:
  case at::kLong:
  case at::kInt:
  case at::kByte:
  case at::kBool: return dtype;
  default: invalid("unsupported tensor dtype");
 }
}
at::ScalarType oracle_dtype(std::string_view external_name) {
 if (!external_name.starts_with("torch.")) invalid("unsupported manifest tensor dtype");
 try {
  return oracle_dtype(mmltk::backend::ml::serialization::scalar_type_from_name(external_name.substr(6)));
 } catch (const std::runtime_error&) { invalid("unsupported manifest tensor dtype"); }
}
std::string dtype_name(at::ScalarType dtype) { return "torch." + mmltk::backend::ml::serialization::scalar_type_name(oracle_dtype(dtype)); }
std::uint64_t tensor_bytes(const OracleTensor& tensor) {
 std::uint64_t width = c10::elementSize(oracle_dtype(tensor.dtype));
 std::uint64_t product = 1;
 bool empty = false;
 for (const auto dimension : tensor.shape) {
  if (dimension < 0 || static_cast<std::uint64_t>(dimension) > kArchiveLimit) invalid("unbounded tensor dimension");
  empty |= dimension == 0;
  const auto extent = static_cast<std::uint64_t>(std::max<int64_t>(dimension, 1));
  if (product > kArchiveLimit / extent) invalid("unbounded tensor shape product");
  product *= extent;
 }
 if (empty) product = 0;
 if (product != static_cast<std::uint64_t>(tensor.elements)) invalid("tensor element count mismatch");
 // Floating archives include one boolean finite mask per value.
 if (c10::isFloatingType(tensor.dtype)) ++width;
 if (product > kArchiveLimit / width) invalid("unbounded tensor storage");
 return product * width;
}
OracleShardRecord shard_record(const Json& value) {
 OracleShardRecord result;
 result.file = value.at("file").get<std::string>();
 result.sha256 = value.at("sha256").get<std::string>();
 result.bytes = value.at("bytes").get<std::uint64_t>();
 if (!result.bytes || result.bytes > kArchiveLimit || value.at("tensors").size() > 20000) invalid("unbounded tensor archive");
 std::uint64_t bytes = 0;
 for (const auto& [name, item] : value.at("tensors").items()) {
  if (name.empty() || name.size() > 512 || name.starts_with("finite/")) invalid("invalid tensor name");
  OracleTensor tensor;
  tensor.defined = item.at("defined").get<bool>();
  if (tensor.defined) {
   tensor.shape = item.at("shape").get<std::vector<int64_t>>();
   tensor.stride = item.at("stride").get<std::vector<int64_t>>();
   tensor.dtype = oracle_dtype(item.at("dtype").get<std::string>());
   tensor.device = item.at("device").get<std::string>();
   tensor.finite_count = item.at("finite_count").get<int64_t>();
   tensor.elements = item.at("elements").get<int64_t>();
   if (tensor.shape.size() > 8 || tensor.stride.size() != tensor.shape.size() || tensor.elements < 0 || tensor.finite_count < 0 || tensor.finite_count > tensor.elements)
    invalid("invalid tensor metadata");
   if (std::ranges::any_of(tensor.stride, [](int64_t stride) { return stride < 0 || static_cast<std::uint64_t>(stride) > kArchiveLimit; })) invalid("invalid source tensor stride");
   const auto size = tensor_bytes(tensor);
   if (size > kArchiveLimit - bytes) invalid("unbounded aggregate tensor storage");
   bytes += size;
  }
  result.tensors.emplace(name, std::move(tensor));
 }
 return result;
}
Json authenticated_index(const std::filesystem::path& directory, const Json& reference) {
 const auto path = child(directory, reference.at("file").get<std::string>());
 const auto bytes = reference.at("bytes").get<std::uint64_t>();
 if (!bytes || bytes > kManifestLimit || std::filesystem::file_size(path) != bytes) invalid("index size mismatch");
 verify_hash(path, reference.at("sha256").get<std::string>());
 return read_index(path);
}
OracleArchiveRecord archive_record(const std::filesystem::path& directory, const Json& reference) {
 const auto index = authenticated_index(directory, reference);
 if (index.at("format") != "mmltk.rfdetr.step.v2" || index.at("shards").empty() || index.at("shards").size() > kShardLimit) invalid("unbounded step shard inventory");
 OracleArchiveRecord result{directory, {}};
 std::set<std::string> files, names;
 std::uint64_t bytes = 0, metadata = 0;
 for (const auto& ref : index.at("shards")) {
  if (!files.insert(ref.at("file").get<std::string>()).second) invalid("duplicate shard index");
  const auto index_bytes = ref.at("bytes").get<std::uint64_t>();
  if (index_bytes > kManifestLimit - metadata) invalid("unbounded step metadata");
  metadata += index_bytes;
  auto shard = shard_record(authenticated_index(directory, ref));
  if (!files.insert(shard.file).second || shard.bytes > kStepStorageLimit - bytes) invalid("duplicate or unbounded physical storage");
  bytes += shard.bytes;
  for (const auto& [name, spec] : shard.tensors)
   if (!names.insert(name).second || names.size() > kTensorLimit) invalid("duplicate or unbounded logical tensor inventory");
  result.shards.push_back(std::move(shard));
 }
 if (bytes != index.at("bytes") || names.size() != index.at("tensor_count")) invalid("step accounting mismatch");
 return result;
}
std::set<std::string> keys(const TensorMap& values) {
 std::set<std::string> result;
 for (const auto& [name, value] : values) result.insert(name);
 return result;
}
}  // namespace
at::ScalarType OracleCase::dtype() const {
 if (precision == "fp32") return torch::kFloat32;
 if (precision == "fp16") return torch::kFloat16;
 if (precision == "bf16") return torch::kBFloat16;
 invalid("unsupported precision " + precision);
}
OracleArchive::OracleArchive(const std::filesystem::path& directory, const Json& reference, std::function<void(std::size_t)> physical_load)
    : OracleArchive(archive_record(directory, reference), std::move(physical_load)) {}
OracleArchive OracleArchive::select(const std::filesystem::path& directory, const Json& reference, std::span<const std::string> names) {
 if (names.empty() || names.size() > kTensorLimit) invalid("empty or unbounded tensor selection");
 auto record = archive_record(directory, reference);
 const std::set<std::string> selected(names.begin(), names.end());
 auto missing = selected;
 std::erase_if(record.shards, [&](const auto& shard) {
  bool needed = false;
  for (const auto& [name, spec] : shard.tensors)
   if (selected.contains(name)) {
    missing.erase(name);
    needed = true;
   }
  return !needed;
 });
 if (!missing.empty()) invalid("required tensor missing: " + *missing.begin());
 OracleArchive result(std::move(record), {});
 std::erase_if(result.locations_, [&](const auto& item) { return !selected.contains(item.first); });
 std::erase_if(result.serialized_order_, [&](const auto& name) { return !selected.contains(name); });
 return result;
}
OracleArchive::OracleArchive(OracleArchiveRecord record, std::function<void(std::size_t)> physical_load) : record_(std::move(record)), physical_load_(std::move(physical_load)) {
 for (std::size_t index = 0; index < record_.shards.size(); ++index) {
  for (const auto& [name, spec] : record_.shards[index].tensors) {
   if (!locations_.emplace(name, index).second) invalid("duplicate logical tensor");
  }
  // Validate every physical shard before exposing the logical view, retaining
  // only one bounded shard. Later reads reuse that shard or replace its cache.
  load_shard(index, true);
 }
}
void OracleArchive::load_shard(std::size_t index, bool remember_order) const {
 tensors_.clear();
 loaded_.reset();
 const auto& record = record_.shards.at(index);
 const auto& directory = record_.directory;
 const auto file = child(directory, record.file);
 if (std::filesystem::file_size(file) != record.bytes) invalid("archive size mismatch");
 verify_hash(file, record.sha256);
 caffe2::serialize::PyTorchStreamReader reader(file.string());
 std::uint64_t storage_budget = 0, storage_bytes = 0, metadata_bytes = 0;
 for (const auto& [name, spec] : record.tensors)
  if (spec.defined) {
   const auto size = tensor_bytes(spec);
   if (size > kArchiveLimit - storage_budget) invalid("unbounded aggregate tensor storage");
   storage_budget += size;
  }
 const auto records = reader.getAllRecords();
 if (records.size() > record.tensors.size() * 2 + 16) invalid("unbounded serialized record inventory");
 for (const auto& name : records) {
  const auto size = reader.getRecordSize(name);
  auto& total = name.starts_with("data/") ? storage_bytes : metadata_bytes;
  const auto limit = name.starts_with("data/") ? storage_budget : kManifestLimit;
  if (size > limit - total) invalid("unbounded serialized archive record");
  total += size;
 }
 const auto value = torch::jit::readArchiveAndTensors("data", "", "", std::nullopt, std::nullopt, torch::Device(torch::kCPU), reader);
 if (!value.isGenericDict()) invalid("ordinary Torch tensor dictionary required");
 const auto dictionary = value.toGenericDict();
 if (dictionary.size() > record.tensors.size() * 2) invalid("unexpected tensor archive inventory");
 // Admit every serialized value, including finite masks, before any tensor
 // operation can materialize an attacker-controlled strided logical view.
 for (const auto& item : dictionary) {
  if (!item.key().isString() || !item.value().isTensor()) invalid("ordinary tensor entries required");
  const auto tensor = item.value().toTensor();
  if (tensor.layout() != torch::kStrided || !tensor.is_cpu() || !tensor.is_contiguous() || tensor.numel() < 0 || static_cast<std::uint64_t>(tensor.numel()) > kArchiveLimit / tensor.element_size())
   invalid("unbounded or noncontiguous serialized tensor");
  const auto storage_elements = tensor.storage().nbytes() / tensor.element_size();
  if (tensor.storage_offset() < 0 || static_cast<std::uint64_t>(tensor.storage_offset()) > storage_elements ||
      static_cast<std::uint64_t>(tensor.numel()) > storage_elements - static_cast<std::uint64_t>(tensor.storage_offset()))
   invalid("serialized tensor exceeds its storage");
 }
 std::set<std::string> expected;
 for (const auto& [name, spec] : record.tensors) {
  if (!spec.defined) { continue; }
  expected.insert(name);
  if (!dictionary.contains(name) || !dictionary.at(name).isTensor()) invalid("missing tensor: " + name);
  const auto tensor = dictionary.at(name).toTensor();
  if (tensor.sizes().vec() != spec.shape || tensor.numel() != spec.elements || tensor.scalar_type() != spec.dtype || !tensor.is_cpu()) invalid("tensor metadata mismatch: " + name);
  const auto finite = torch::isfinite(tensor);
  if (finite.sum().item<int64_t>() != spec.finite_count) invalid("finite inventory mismatch: " + name);
  if (tensor.is_floating_point()) {
   expected.insert("finite/" + name);
   if (!dictionary.contains("finite/" + name)) invalid("missing finite mask: " + name);
   const auto mask = dictionary.at("finite/" + name).toTensor();
   if (mask.scalar_type() != torch::kBool || mask.sizes() != tensor.sizes() || !torch::equal(finite, mask)) invalid("finite mask mismatch: " + name);
  }
  tensors_.emplace(name, tensor);
 }
 for (const auto& item : dictionary) {
  if (!item.key().isString() || !expected.erase(item.key().toStringRef())) invalid("unexpected archive entry");
  if (remember_order && tensors_.contains(item.key().toStringRef())) serialized_order_.push_back(item.key().toStringRef());
 }
 if (!expected.empty()) invalid("missing archive entries");
 loaded_ = index;
 if (physical_load_) physical_load_(index);
}
TensorMap OracleArchive::read_tensors(std::span<const std::string> names, const torch::Device device) const {
 TensorMap result;
 std::map<std::size_t, std::vector<TensorMap::iterator>> requests;
 std::uint64_t selected_bytes = 0;
 // Resolve the entire set and its owning-storage budget before physical reads.
 // Repeated names select one result, and undefined entries need no shard load.
 for (const auto& name : names) {
  const auto location = locations_.find(name);
  if (location == locations_.end()) invalid("required tensor missing: " + name);
  const auto [selected, inserted] = result.try_emplace(name);
  if (!inserted) continue;
  const auto& spec = record_.shards[location->second].tensors.at(name);
  if (!spec.defined) continue;
  const auto bytes = static_cast<std::uint64_t>(spec.elements) * c10::elementSize(spec.dtype);
  if (bytes > kStepStorageLimit - selected_bytes) invalid("unbounded selected tensor storage");
  selected_bytes += bytes;
  requests[location->second].push_back(selected);
 }
 const auto gather = [&](const auto& request) {
  if (loaded_ != request.first) load_shard(request.first);
  for (const auto selected : request.second) {
   const auto found = tensors_.find(selected->first);
   if (found == tensors_.end()) invalid("required tensor missing: " + selected->first);
   // A contiguous serialized view can still share a larger storage. Force an
   // owning copy so a selected value never pins unrelated shard storage.
   selected->second = found->second.to(device, found->second.scalar_type(), false, true);
  }
 };
 // Consume the current working shard before replacing it, when requested.
 if (loaded_) {
  const auto cached = requests.find(*loaded_);
  if (cached != requests.end()) {
   gather(*cached);
   requests.erase(cached);
  }
 }
 for (const auto& request : requests) gather(request);
 return result;
}
torch::Tensor OracleArchive::tensor(const std::string& name, const torch::Device device) const { return read_tensors(std::span<const std::string>(&name, 1), device).at(name); }
bool OracleArchive::contains(const std::string& name) const { return locations_.contains(name); }
std::vector<std::string> OracleArchive::ordered_defined_names(const std::string_view prefix) const {
 std::vector<std::string> result;
 for (const auto& name : serialized_order_)
  if (name.starts_with(prefix)) result.push_back(name.substr(prefix.size()));
 return result;
}
std::set<std::string> OracleArchive::names(const std::string_view prefix) const {
 std::set<std::string> result;
 for (const auto& [name, value] : locations_)
  if (name.starts_with(prefix)) result.insert(name.substr(prefix.size()));
 return result;
}
std::filesystem::path UpstreamOracle::requested() {
 const auto* value = std::getenv("MMLTK_RFDETR_UPSTREAM_ORACLE_DIR");
 if (!value) SKIP("upstream validation is opt-in: use --with-upstream or --env MMLTK_RFDETR_UPSTREAM_ORACLE_DIR=PATH");
 if (!*value) invalid("explicitly requested evidence path is empty");
 return std::filesystem::path(value);
}
UpstreamOracle::UpstreamOracle(const std::filesystem::path& root) {
 const auto path = child(root, "manifest.json");
 if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > kManifestLimit) invalid("requested manifest missing or oversized");
 const auto manifest = read_index(path);
 const auto* repository = std::getenv("MMLTK_REPO_ROOT");
 if (!repository) invalid("wrapper repository provenance missing");
 const auto pins_path = std::filesystem::path(repository) / "tools/oracle/rfdetr_sources.json";
 std::ifstream pins_input(pins_path);
 const auto pins = Json::parse(pins_input);
 if (manifest.at("sources") != pins || manifest.at("revision") != pins.at("core").at("revision")) invalid("source pins differ");
 const auto selected_case = manifest.value("selected_case", std::string{});
 const bool selected = manifest.contains("selected_case");
 if ((selected && (selected_case.empty() || selected_case.size() > 128)) || manifest.at("format") != "mmltk.rfdetr.upstream-oracle.v2" ||
     manifest.at("status") != (selected ? "selected_reference_native_comparison_pending" : "references_complete_native_comparison_pending"))
  invalid("incompatible oracle format/revision/status");
 if (manifest.at("policy").at("matmul_tf32").get<bool>() || manifest.at("policy").at("cudnn_tf32").get<bool>() || manifest.at("policy").at("gradient_scale") != 1 ||
     manifest.at("policy").at("accumulated_microbatches") != 1)
  invalid("incompatible controlled numerical policy");
 const auto sdpa = manifest.at("policy").value("sdpa_backend", std::string("default"));
 if (sdpa != "default" && sdpa != "math") invalid("unknown SDPA comparison policy");
 if (sdpa == "math" && (manifest.at("policy").at("flash_sdp").get<bool>() || manifest.at("policy").at("memory_efficient_sdp").get<bool>() || manifest.at("policy").at("cudnn_sdp").get<bool>() ||
                        !manifest.at("policy").at("math_sdp").get<bool>()))
  invalid("math SDPA policy disagrees with producer backends");
 for (const auto* key : {"producer_sha256", "dependency_sha256", "payload_sha256", "fingerprint"})
  if (manifest.at(key).get<std::string>().size() != 64) invalid("missing provenance identity");
 for (const auto* key : {"source_hashes", "plus_source_hashes"})
  if (manifest.at(key).empty() || manifest.at(key).size() > 5000) invalid("missing source provenance");
 verify_hash(child(child(root, "inventory"), "manifest.json"), manifest.at("inventory_sha256").get<std::string>());
 if (!manifest.at("environment").at("torch").get<std::string>().starts_with(TORCH_VERSION) || manifest.at("torch_build_config").get<std::string>().empty())
  invalid("incompatible Torch payload provenance");
 directory_ = child(root, manifest.at("directory").get<std::string>());
 if (manifest.at("presets").size() != (selected ? 1 : kPresetCatalog.size())) invalid("missing required preset");
 std::map<std::string, std::pair<std::string, std::string>> checkpoint_hashes;
 for (const auto& preset : manifest.at("presets")) {
  const auto name = preset.at("name").get<std::string>();
  const auto checkpoint = child(directory_, preset.at("checkpoint").get<std::string>());
  verify_hash(checkpoint, preset.at("sha256").get<std::string>());
  verify_hash(child(directory_, preset.at("original").get<std::string>()), preset.at("original_sha256").get<std::string>());
  for (const auto& [kind, facts] : preset.at("constructor").items())
   for (const auto& [member, passed] : facts.items())
    if (!passed.get<bool>()) invalid("upstream constructor invariant failed: " + member);
  const auto* row = find_preset_catalog_entry(name);
  if (!row) invalid("unknown catalog preset");
  const auto& asset = preset.at("verified_asset");
  if (asset.at("preset") != name || asset.at("filename") != row->canonical_weight_filename || asset.at("md5") != row->canonical_weight_md5 || asset.at("url") != row->canonical_weight_url ||
      asset.at("classifier_width") != row->class_count || asset.at("sha256") != preset.at("original_sha256"))
   invalid("official asset binding differs");
  checkpoint_hashes.emplace(name, std::pair{preset.at("sha256").get<std::string>(), preset.at("original_sha256").get<std::string>()});
  originals_.emplace(name, child(directory_, preset.at("original").get<std::string>()));
  if (!checkpoints_.emplace(name, checkpoint).second) invalid("duplicate preset");
 }
 if (selected ? manifest.at("cases").size() != 1 : manifest.at("cases").size() < kPresetCatalog.size() * 9 || manifest.at("cases").size() > kPresetCatalog.size() * 13)
  invalid("incomplete or unbounded model matrix");
 std::set<std::string> identities;
 std::set<std::string> case_directories;
 for (const auto& reference : manifest.at("cases")) {
  const auto case_name = reference.at("directory").get<std::string>();
  if (!case_directories.insert(case_name).second) invalid("duplicate case directory");
  const auto case_directory = child(directory_, case_name);
  const auto item = authenticated_index(case_directory, reference);
  OracleCase entry;
  if (item.contains("upstream_export") && !item.at("upstream_export").is_null()) {
   const auto& artifact = item.at("upstream_export");
   OracleExportArtifact value{child(case_directory, artifact.at("file").get<std::string>()), artifact.at("sha256").get<std::string>(), artifact.at("bytes").get<std::uint64_t>(),
    artifact.at("outputs").get<std::map<std::string, std::string>>()};
   if (!value.bytes || value.bytes > kArchiveLimit || value.sha256.size() != 64 || value.outputs.size() < 2 || value.outputs.size() > 3 ||
       artifact.at("exporter") != "rfdetr.export._onnx.exporter.OnnxExporter" || artifact.at("onnx_version").get<std::string>().empty())
    invalid("invalid upstream export declaration");
   std::set<std::string> external_names;
   for (const auto& [role, name] : value.outputs)
    if (role.size() > 128 || name.empty() || name.size() > 128 || name.find('\0') != std::string::npos || !external_names.insert(name).second) invalid("invalid upstream export output mapping");
   entry.upstream_export = std::move(value);
  }
  entry.id = item.at("id").get<std::string>();
  entry.protocol = item.at("protocol").get<std::string>();
  const bool stock = entry.protocol == "stock";
  if ((!stock && entry.protocol != "controlled-group2") || entry.id != case_name) invalid("case protocol identity differs");
  entry.preset = item.at("preset").get<std::string>();
  entry.precision = item.at("precision").get<std::string>();
  entry.training = item.at("training").get<bool>();
  entry.math_sdpa = sdpa == "math";
  entry.batch = item.at("batch").get<int>();
  if (entry.id != entry.preset + "-" + entry.protocol + "-" + entry.precision + "-b" + std::to_string(entry.batch) + (entry.training ? "-train" : "-eval") ||
      (selected && entry.id != selected_case))
   invalid("selected case identity differs");
  entry.resolution = item.at("resolution").get<int>();
  entry.groups = item.at("groups").get<int>();
  entry.classes = item.at("classes").get<int>();
  entry.queries = item.at("queries").get<int>();
  entry.keypoints_per_class = item.at("keypoints_per_class").get<std::vector<std::uint16_t>>();
  entry.trainable = item.at("trainable").get<std::vector<std::string>>();
  entry.loss_weights = item.at("loss_weights").get<std::map<std::string, double>>();
  entry.clip_max_norm = item.at("clip_max_norm").get<double>();
  const auto& recipe = item.at("optimizer");
  entry.recipe.lr = recipe.at("lr");
  entry.recipe.lr_encoder = recipe.at("lr_encoder");
  entry.recipe.lr_component_decay = recipe.at("lr_component_decay");
  entry.recipe.encoder_layer_decay = recipe.at("lr_vit_layer_decay");
  entry.recipe.weight_decay = recipe.at("weight_decay");
  const auto* row = find_preset_catalog_entry(entry.preset);
  if (!row) invalid("unknown case preset");
  if (entry.upstream_export) {
   std::set<std::string> expected_roles, actual_roles;
   RfdetrOutputSchema::visit([&]<RfdetrOutputRole>(RfdetrOutputPolicy facts) {
    if (facts.supports(row->task)) expected_roles.emplace(facts.name);
   });
   for (const auto& [role, name] : entry.upstream_export->outputs) actual_roles.insert(role);
   if (stock || entry.precision != "fp32" || entry.batch != 1 || actual_roles != expected_roles) invalid("upstream export task or output roles differ");
  }
  if (entry.id.size() > 128 || !identities.insert(entry.id).second || !checkpoints_.contains(entry.preset) || (entry.batch != 1 && entry.batch != 2) ||
      entry.groups != (stock ? row->group_count : 2) || entry.classes != row->class_count || entry.queries != row->query_count || entry.resolution != static_cast<int>(row->resolution) ||
      entry.trainable.empty() || entry.trainable.size() > 2000)
   invalid("invalid case configuration");
  if (stock && (entry.precision != "fp32" || !entry.training || entry.batch != 1)) invalid("invalid stock case");
  const auto& hashes = checkpoint_hashes.at(entry.preset);
  if (item.at("checkpoint_sha256") != (stock ? hashes.second : hashes.first)) invalid("case checkpoint identity differs");
  if (item.at("producer_sha256") != manifest.at("producer_sha256") || item.at("fingerprint") != manifest.at("fingerprint")) invalid("case producer binding differs");
  (void)entry.dtype();
  if (item.at("comparison_policy") != (entry.dtype() == torch::kFloat32 || !entry.training ? "untouched_stock" : "upstream_with_fp32_matcher_boundary") ||
      item.at("execution_policy").at("autocast_enabled").get<bool>() != (entry.dtype() != torch::kFloat32) ||
      (entry.dtype() != torch::kFloat32 && item.at("execution_policy").at("autocast_dtype") != dtype_name(entry.dtype())))
   invalid("case precision policy mismatch");
  for (const auto& group : item.at("parameter_groups")) {
   OracleParameterGroup value;
   value.names = group.at("names").get<std::vector<std::string>>();
   value.lr = group.at("lr");
   value.weight_decay = group.at("weight_decay");
   value.beta1 = group.at("betas").at(0);
   value.beta2 = group.at("betas").at(1);
   value.eps = group.at("eps");
   value.amsgrad = group.at("amsgrad");
   entry.parameter_groups.push_back(std::move(value));
  }
  const std::size_t expected_steps = !stock && entry.training && entry.batch == 2 ? 8 : 1;
  if (item.at("steps").size() != expected_steps) invalid("trajectory length mismatch");
  std::set<std::string> step_files;
  for (const auto& step : item.at("steps")) {
   if (step.at("step").get<std::size_t>() != entry.steps.size()) invalid("trajectory step sequence mismatch");
   if (!step_files.insert(step.at("file").get<std::string>()).second) invalid("duplicate step index");
   const auto admitted = archive_record(case_directory, step);
   for (const auto& shard : admitted.shards)
    if (!step_files.insert(shard.file).second) invalid("physical shard reused across trajectory steps");
   entry.steps.push_back({case_directory, step});
  }
  cases_.push_back(std::move(entry));
 }
 if (at::cuda::getNumGPUs() == 0) invalid("required CUDA device unavailable");
 const bool bf16 = at::cuda::getDeviceProperties(0)->major >= 8;
 const auto& capability = manifest.at("precision_capability");
 if (capability.at("fp32") != true || capability.at("fp16") != true || capability.at("bf16") != bf16) invalid("current supported precision coverage differs");
 if (bf16 ? !manifest.at("unsupported").empty() : manifest.at("unsupported") != Json::array({"bf16: CUDA device does not support BF16"})) invalid("precision support inventory differs");
 for (const auto& row : kPresetCatalog)
  for (const auto& precision : {"fp32", "fp16", "bf16"})
   for (const auto batch : {1, 2})
    for (const bool training : {false, true})
     if (!selected && std::ranges::count_if(cases_, [&](const auto& value) {
      return value.protocol == "controlled-group2" && value.preset == row.preset_name && value.precision == precision && value.batch == batch && value.training == training;
     }) != (std::string_view(precision) == "bf16" && !bf16 ? 0 : 1))
      invalid("missing required model case");
 for (const auto& row : kPresetCatalog)
  if (!selected && std::ranges::count_if(cases_, [&](const auto& value) { return value.protocol == "stock" && value.preset == row.preset_name; }) != 1) invalid("missing full stock training case");
 for (const auto* family : {"resize", "keypoints"})
  if (!case_directories.insert(manifest.at(family).at("directory").get<std::string>()).second) invalid("duplicate supplemental evidence directory");
 const auto resize_directory = child(directory_, manifest.at("resize").at("directory").get<std::string>());
 const auto keypoint_directory = child(directory_, manifest.at("keypoints").at("directory").get<std::string>());
 const auto resize = authenticated_index(resize_directory, manifest.at("resize"));
 const auto keypoints = authenticated_index(keypoint_directory, manifest.at("keypoints"));
 (void)archive_record(resize_directory, resize.at("archive"));
 (void)archive_record(keypoint_directory, keypoints.at("archive"));
 resize_ = {resize_directory, resize.at("archive")};
 keypoints_ = {keypoint_directory, keypoints.at("archive")};
 keypoint_cases_ = keypoints.at("cases");
 if (keypoint_cases_.size() != 5) invalid("incomplete keypoint boundary cases");
}
std::filesystem::path UpstreamOracle::checkpoint(const std::string_view preset) const { return checkpoints_.at(std::string(preset)); }
std::filesystem::path UpstreamOracle::official_checkpoint(const std::string_view preset) const { return originals_.at(std::string(preset)); }
NativeRfDetrModel UpstreamOracle::model(const OracleCase& entry) const {
 auto admitted = resolve_model_state(entry.protocol == "stock" ? official_checkpoint(entry.preset) : checkpoint(entry.preset), entry.preset, entry.resolution);
 auto config = admitted.artifacts.config;
 if (entry.protocol == "stock") apply_stock_training_coefficients(config);
 if (config.group_detr != entry.groups) invalid("checkpoint group topology differs");
 if (config.num_classes != entry.classes || config.num_queries != entry.queries || config.keypoints_per_class != entry.keypoints_per_class) invalid("checkpoint class/query/joint inventory differs");
 auto layout = official_layouts_.find(entry.preset);
 if (layout == official_layouts_.end()) {
  // Reduced-group numerical checkpoints are derived upstream state, so their
  // new bytes cannot inherit a verified official digest. Admit class meaning
  // independently from the retained original asset through production APIs.
  auto official = resolve_model_artifacts(official_checkpoint(entry.preset), entry.preset, entry.resolution);
  if (official.config.num_classes != config.num_classes || official.config.keypoints_per_class != config.keypoints_per_class) invalid("official and derived class schema differs");
  layout = official_layouts_.emplace(entry.preset, std::move(official.class_layout)).first;
 }
 NativeRfDetrModel model(config, layout->second);
 auto candidate = model.stage_normalized_state(admitted.model_state.entries(), detail::NormalizedModelStateAdmission::RequiredCore);
 if (!candidate.summary.missing_names.empty() || !candidate.summary.incompatible_names.empty() || !candidate.summary.unexpected_names.empty())
  invalid("upstream checkpoint admission inventory differs");
 model.commit_normalized_state(std::move(candidate));
 model.to(torch::Device(torch::kCUDA));
 model.train(entry.training);
 model.set_force_pytorch_deformable_attn(false);
 model.set_force_math_attention(entry.math_sdpa);
 std::set<std::string> trainable;
 for (const auto& value : model.named_parameters(true))
  if (value.value().requires_grad()) trainable.insert(value.key());
 if (trainable != std::set<std::string>(entry.trainable.begin(), entry.trainable.end())) invalid("trainable parameter inventory differs");
 return model;
}
OracleArchive UpstreamOracle::archive(const OracleCase& entry, std::size_t step, std::span<const std::string> names) const {
 const auto& reference = entry.steps.at(step);
 return names.empty() ? OracleArchive(reference.directory, reference.index) : OracleArchive::select(reference.directory, reference.index, names);
}
std::filesystem::path UpstreamOracle::export_onnx(const OracleCase& entry) const {
 if (!entry.upstream_export) invalid("actual upstream ONNX export was not requested");
 const auto& artifact = *entry.upstream_export;
 if (!std::filesystem::is_regular_file(artifact.path) || std::filesystem::is_symlink(artifact.path) || std::filesystem::file_size(artifact.path) != artifact.bytes)
  invalid("upstream ONNX artifact changed");
 verify_hash(artifact.path, artifact.sha256);
 return artifact.path;
}
OracleArchive UpstreamOracle::keypoint_archive() const { return OracleArchive(keypoints_.directory, keypoints_.index); }
std::vector<CompilationMode> oracle_execution_modes(const OracleCase& entry) {
 const auto* preset = find_preset_catalog_entry(entry.preset);
 if (!preset) invalid("unknown execution preset");
 if (entry.protocol == "controlled-group2" && entry.batch == 1 && (preset->backbone.kind != BackboneKind::DinoV2Small || preset->task == ModelTask::Keypoints))
  return {CompilationMode::kNone, CompilationMode::kSelective, CompilationMode::kFullTrace};
 return {CompilationMode::kNone};
}
OracleArchive UpstreamOracle::resize_archive() const { return OracleArchive(resize_.directory, resize_.index); }
OracleComparison::OracleComparison(const std::filesystem::path& root, std::string name, at::ScalarType precision, std::string objective)
    : precision_(precision), run_unix_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) {
 auto reports = root / "comparisons";
 const auto* attempt = std::getenv("MMLTK_RFDETR_ORACLE_ATTEMPT");
 const auto* evidence = std::getenv("MMLTK_RFDETR_UPSTREAM_ORACLE_DIR");
 if (attempt && evidence && std::filesystem::equivalent(root, evidence)) {
  reports = child(reports, attempt);
  if (!std::filesystem::is_directory(reports)) invalid("comparison attempt was not admitted by the wrapper");
 }
 std::filesystem::create_directories(reports);
 const auto report_path = child(reports, name + ".jsonl");
 report_.open(report_path, std::ios::app);
 if (!report_) {
  const std::error_code error(errno, std::generic_category());
  invalid("cannot write comparison evidence " + report_path.string() + (error ? ": " + error.message() : ""));
 }
 static const auto executable_hash = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file("/proc/self/exe"));
 record(
  {{"boundary", "policy"}, {"precision", dtype_name(precision)}, {"matmul_tf32", at::globalContext().allowTF32CuBLAS()}, {"cudnn_tf32", at::globalContext().allowTF32CuDNN()},
   {"forward_autocast", precision != torch::kFloat32}, {"cudnn_benchmark", at::globalContext().benchmarkCuDNN()}, {"flash_sdp", at::globalContext().userEnabledFlashSDP()},
   {"memory_efficient_sdp", at::globalContext().userEnabledMemEfficientSDP()}, {"math_sdp", at::globalContext().userEnabledMathSDP()}, {"native_torch", TORCH_VERSION},
   {"native_executable_sha256", executable_hash}, {"attention", "native production CUDA"}, {"matcher", "native FP32"}, {"objective", std::move(objective)},
   {"hyperparameter_tolerance_reason", "eight float64 epsilons allow upstream decay**2 versus native repeated multiplication; zero settings and group membership remain exact"},
   {"tolerance_reason",
    "FP32 operation/reduction order; AMP rounding and CUDA atomic accumulation. Derivatives gate the complete vector at the declared norm bound; per-tensor bounds remain reported. Matched-state "
    "identities are exact; independent trajectories measure values and identities. Shape/dtype/definedness/finite values remain required."}});
}
void OracleComparison::record(Json value) {
 value["run_unix_ns"] = run_unix_ns_;
 report_ << value.dump() << '\n';
 report_.flush();
 if (!report_) invalid("comparison evidence write failed");
}
void OracleComparison::fact(const std::string& boundary, const Json& value) { record({{"boundary", boundary}, {"observation", value}}); }
void OracleComparison::observe_values(const std::function<void()>& comparisons) {
 const bool previous = observing_;
 observing_ = true;
 try {
  comparisons();
 } catch (...) {
  observing_ = previous;
  throw;
 }
 observing_ = previous;
}
void OracleComparison::derivatives(const std::string& boundary, const TensorMap& actual, const TensorMap& expected) {
 inventory(boundary + "inventory", keys(actual), keys(expected));
 std::vector<torch::Tensor> a, b;
 observe_values([&] {
  for (const auto& [name, value] : actual) {
   const auto& reference = expected.at(name);
   compare(boundary + name, value, reference, OracleComparisonPolicy::Derivative);
   if (!value.defined()) continue;
   a.push_back(value.detach().cpu().flatten());
   b.push_back(reference.detach().cpu().flatten());
  }
 });
 if (!a.empty()) compare(boundary + "complete-vector", torch::cat(a), torch::cat(b), OracleComparisonPolicy::Derivative);
}
void OracleComparison::compare_all(const std::function<void()>& comparisons) {
 if (collecting_) invalid("nested comparison collection");
 collecting_ = true;
 deferred_difference_.clear();
 try {
  comparisons();
 } catch (...) {
  collecting_ = false;
  throw;
 }
 collecting_ = false;
 if (!deferred_difference_.empty()) invalid("first differing tensor boundary: " + deferred_difference_);
}
void OracleComparison::inventory(const std::string& boundary, const std::set<std::string>& actual, const std::set<std::string>& expected) {
 const bool match = actual == expected;
 record({{"boundary", boundary}, {"actual", actual}, {"expected", expected}, {"passed", match}, {"first_difference", !match && !failed_}});
 if (!match) {
  failed_ = true;
  invalid("first differing inventory: " + boundary);
 }
}
void OracleComparison::compare(const std::string& boundary, const torch::Tensor& actual, const torch::Tensor& expected, OracleComparisonPolicy policy, bool policy_difference) {
 compare_impl(boundary, actual, expected, policy, policy_difference, {});
}
void OracleComparison::parameter_update(
 const std::string& boundary, const torch::Tensor& actual_before, const torch::Tensor& actual_after, const torch::Tensor& expected_before, const torch::Tensor& expected_after) {
 const auto before = expected_before.detach().cpu(), after = expected_after.detach().cpu();
 compare_impl(boundary, actual_after.detach().cpu() - actual_before.detach().cpu(), after - before, OracleComparisonPolicy::Derivative, false, torch::maximum(before.abs(), after.abs()));
}
void OracleComparison::compare_impl(
 const std::string& boundary, const torch::Tensor& actual, const torch::Tensor& expected, OracleComparisonPolicy policy, bool policy_difference, const torch::Tensor& rounding_scale) {
 Json result{{"boundary", boundary}, {"policy_difference", policy_difference}, {"measurement_only", observing_}, {"actual_defined", actual.defined()}, {"expected_defined", expected.defined()}};
 bool passed = actual.defined() == expected.defined();
 bool structurally_valid = passed;
 bool finite_numerical_comparison = false;
 if (actual.defined() && expected.defined()) {
  result["actual_dtype"] = dtype_name(actual.scalar_type());
  result["expected_dtype"] = dtype_name(expected.scalar_type());
  result["actual_stride"] = actual.strides().vec();
  result["actual_device"] = actual.device().str();
  result["archive_device"] = expected.device().str();
  result["actual_shape"] = actual.sizes().vec();
  result["expected_shape"] = expected.sizes().vec();
  passed = actual.sizes() == expected.sizes() && actual.scalar_type() == expected.scalar_type();
  if (actual.sizes() == expected.sizes()) {
   const auto a = actual.detach().cpu(), b = expected.detach().cpu();
   const auto finite = torch::isfinite(a);
   bool finite_equal = torch::equal(finite, torch::isfinite(b));
   if (a.is_floating_point() && b.is_floating_point())
    finite_equal &= torch::equal(torch::isnan(a), torch::isnan(b)) && torch::equal(torch::isposinf(a), torch::isposinf(b)) && torch::equal(torch::isneginf(a), torch::isneginf(b));
   const bool all_finite = finite.all().item<bool>();
   result["finite_equal"] = finite_equal;
   result["all_finite"] = all_finite;
   passed &= finite_equal && all_finite;
   structurally_valid = passed;
   if (!a.is_floating_point() || policy == OracleComparisonPolicy::Exact) {
    passed &= torch::equal(a, b);
    result["discrete_equal"] = torch::equal(a, b);
   } else if (a.numel() && finite_equal) {
    finite_numerical_comparison = passed && policy != OracleComparisonPolicy::Hyperparameter;
    const auto av = a.to(torch::kFloat64).masked_select(finite), bv = b.to(torch::kFloat64).masked_select(finite);
    if (av.numel()) {
     const auto difference = av - bv;
     const bool hyperparameter = policy == OracleComparisonPolicy::Hyperparameter;
     if (hyperparameter) passed &= torch::equal(av.masked_select(bv.eq(0)), bv.masked_select(bv.eq(0)));
     const bool second_moment = policy == OracleComparisonPolicy::SecondMoment;
     const bool derivative = policy == OracleComparisonPolicy::Derivative || second_moment;
     const double relative = hyperparameter ? 8 * std::numeric_limits<double>::epsilon() : precision_ == torch::kFloat32 ? (derivative ? .002 : .0003) : precision_ == torch::kFloat16 ? .015 : .03;
     const double absolute = hyperparameter ? 0. : second_moment ? 1e-12 : derivative ? 1e-7 : precision_ == torch::kFloat32 ? 2e-5 : 2e-3;
     const double maximum = difference.abs().max().item<double>(), norm = difference.norm().item<double>();
     auto bounded_error = difference.abs();
     if (rounding_scale.defined()) {
      const auto quantum = torch::nextafter(rounding_scale, torch::full_like(rounding_scale, std::numeric_limits<double>::infinity())) - rounding_scale;
      result["parameter_quantization_allowance"] = "one representable interval at max(abs(before),abs(after)); raw errors are unchanged";
      result["parameter_quantization_max"] = quantum.max().item<double>();
      result["unadjusted_normalized_error"] = std::max(maximum / (absolute + relative * bv.abs().max().item<double>()), norm / (absolute + relative * bv.norm().item<double>()));
      bounded_error = (bounded_error - quantum.to(torch::kFloat64).masked_select(finite)).clamp_min(0);
     }
     const double normalized = derivative ? std::max(bounded_error.max().item<double>() / (absolute + relative * bv.abs().max().item<double>()),
                                             bounded_error.norm().item<double>() / (absolute + relative * bv.norm().item<double>()))
                                          : (difference.abs() / (absolute + relative * bv.abs()).clamp_min(std::numeric_limits<double>::min())).max().item<double>();
     result["max_absolute"] = maximum;
     result["l2_error"] = norm;
     result["actual_l2"] = av.norm().item<double>();
     result["expected_l2"] = bv.norm().item<double>();
     result["actual_max_absolute"] = av.abs().max().item<double>();
     result["expected_max_absolute"] = bv.abs().max().item<double>();
     result["normalized_error"] = normalized;
     result["absolute_tolerance"] = absolute;
     result["relative_tolerance"] = relative;
     result["derivative_norm_bound"] = derivative;
     passed &= std::isfinite(normalized) && normalized <= 1.;
     if (!passed && !observing_ && !policy_difference) {
      const auto error = derivative ? bounded_error : difference.abs() / (absolute + relative * bv.abs()).clamp_min(std::numeric_limits<double>::min());
      const auto index = error.argmax().item<int64_t>();
      result["worst_finite_index"] = index;
      result["worst_actual"] = av[index].item<double>();
      result["worst_expected"] = bv[index].item<double>();
      result["worst_absolute_error"] = std::abs(av[index].item<double>() - bv[index].item<double>());
     }
    }
   }
  }
 }
 if (actual.defined() && expected.defined() && (actual.sizes() != expected.sizes() || actual.scalar_type() != expected.scalar_type())) structurally_valid = false;
 result["passed"] = passed;
 result["first_difference"] = !passed && !failed_ && !policy_difference && (!observing_ || !structurally_valid);
 record(std::move(result));
 if (observing_ && structurally_valid && policy != OracleComparisonPolicy::Hyperparameter) return;
 if (!passed && !policy_difference) {
  failed_ = true;
  if (collecting_ && finite_numerical_comparison) {
   if (deferred_difference_.empty()) deferred_difference_ = boundary;
   return;
  }
  invalid("first differing tensor boundary: " + boundary);
 }
}
TensorMap oracle_output_tensors(const ModelOutputs& outputs) {
 TensorMap result;
 const auto layer = [&](const std::string& prefix, const OutputLayer& value) {
  result.emplace(prefix + "/pred_logits", value.pred_logits);
  result.emplace(prefix + "/pred_boxes", value.pred_boxes);
  if (value.pred_masks) result.emplace(prefix + "/pred_masks", *value.pred_masks);
  if (value.pred_keypoints) result.emplace(prefix + "/pred_keypoints", *value.pred_keypoints);
  if (value.sparse_pred_masks) {
   result.emplace(prefix + "/pred_masks/spatial_features", value.sparse_pred_masks->spatial_features);
   result.emplace(prefix + "/pred_masks/query_features", value.sparse_pred_masks->query_features);
   result.emplace(prefix + "/pred_masks/bias", value.sparse_pred_masks->bias);
  }
 };
 layer("main", outputs.main);
 for (std::size_t i = 0; i < outputs.aux_outputs.size(); ++i) layer("aux." + std::to_string(i), outputs.aux_outputs[i]);
 if (outputs.enc_outputs) layer("enc", *outputs.enc_outputs);
 if (outputs.denoising) invalid("stock oracle must not acquire denoising outputs");
 return result;
}
PreparedTargets oracle_targets(const OracleArchive& archive, const OracleCase& entry, torch::Device device) {
 PreparedTargets result;
 std::vector<torch::Tensor> boxes, labels, masks, joints, counts;
 int64_t offset = 0;
 for (int image = 0; image < entry.batch; ++image) {
  const auto prefix = "target/" + std::to_string(image) + "/";
  auto b = archive.tensor(prefix + "boxes", device), l = archive.tensor(prefix + "labels", device);
  auto& target = result.targets.emplace_back();
  target.boxes = b;
  target.labels = l;
  result.offsets.push_back(offset);
  result.counts.push_back(l.numel());
  offset += l.numel();
  boxes.push_back(b);
  labels.push_back(l);
  if (archive.contains(prefix + "masks")) masks.push_back(archive.tensor(prefix + "masks"));
  if (archive.contains(prefix + "keypoints")) {
   target.keypoints = archive.tensor(prefix + "keypoints", device);
   target.keypoint_counts =
    torch::tensor(std::vector<int64_t>(entry.keypoints_per_class.begin(), entry.keypoints_per_class.end()), torch::TensorOptions().dtype(torch::kInt64).device(device)).index_select(0, l);
   joints.push_back(target.keypoints);
   counts.push_back(target.keypoint_counts);
  }
 }
 result.all_boxes = torch::cat(boxes);
 result.all_labels = torch::cat(labels);
 const auto integers = torch::TensorOptions().device(device).dtype(torch::kInt64);
 result.target_counts = torch::tensor(result.counts, integers);
 result.target_offsets = torch::tensor(result.offsets, integers);
 result.target_indices = torch::arange(offset, integers);
 result.resolved_query_count = entry.queries;
 if (!joints.empty()) {
  result.all_keypoints = torch::cat(joints);
  result.all_keypoint_counts = torch::cat(counts);
 }
 if (!masks.empty()) {
  const auto dense = torch::cat(masks).to(torch::kBool).contiguous();
  const auto pixels = dense.size(1) * dense.size(2), words = (pixels + 63) / 64;
  auto bits = torch::zeros({offset, words}, torch::kInt64);
  auto* destination = reinterpret_cast<std::uint64_t*>(bits.data_ptr<int64_t>());
  const auto* source = dense.data_ptr<bool>();
  for (int64_t row = 0; row < offset; ++row)
   for (int64_t pixel = 0; pixel < pixels; ++pixel)
    if (source[row * pixels + pixel]) destination[row * words + pixel / 64] |= std::uint64_t{1} << (pixel % 64);
  result.packed_masks = PackedTargetMasks{bits.to(device), dense.size(1), dense.size(2)};
 }
 return result;
}
std::vector<LayerMaskSamples> oracle_samples(const OracleArchive& archive, std::size_t layers, torch::Device device, const std::string& prefix) {
 std::vector<LayerMaskSamples> samples(layers);
 if (!archive.contains(prefix + "samples/0/matcher")) return samples;
 std::vector<std::string> read_names;
 for (std::size_t i = 0; i < layers; ++i) {
  const auto key = prefix + "samples/" + std::to_string(i) + "/";
  for (const auto* field : {"matcher", "candidates", "remainder"}) read_names.push_back(key + field);
 }
 // All sampled inputs already live together in the returned layer vector.
 const auto expected = archive.read_tensors(read_names, device);
 for (std::size_t i = 0; i < layers; ++i) {
  const auto key = prefix + "samples/" + std::to_string(i) + "/";
  samples[i] = {expected.at(key + "matcher"), expected.at(key + "candidates"), expected.at(key + "remainder")};
 }
 return samples;
}
DetectionConfig oracle_detection(const NativeRfDetrConfig& config) {
 auto result = detection_fixture_base(config);
 result.include_masks = config.has_masks();
 result.mask_point_sample_ratio = config.mask_point_sample_ratio;
 populate_default_detection_weights(result);
 return result;
}
void compare_oracle_outputs(
 OracleComparison& comparison, const OracleArchive& archive, const ModelOutputs& outputs, const std::vector<torch::Tensor>& encoder_indices, const std::string& prefix, bool observed_encoder) {
 const auto tensors = oracle_output_tensors(outputs);
 comparison.inventory(prefix + "output inventory", keys(tensors), archive.names(prefix + "output/"));
 std::set<std::string> indices;
 for (std::size_t group = 0; group < encoder_indices.size(); ++group) indices.insert(std::to_string(group));
 std::vector<std::string> read_names;
 const auto captures = observed_encoder ? archive.names(prefix + "encoder/").size() : 0;
 for (std::size_t capture = 0; capture < captures; ++capture) read_names.push_back(prefix + "encoder/" + std::to_string(capture));
 for (const auto& [name, value] : tensors) read_names.push_back(prefix + "output/" + name);
 const auto expected = archive.read_tensors(read_names);
 TensorMap expected_indices;
 for (std::size_t capture = 0; capture < captures; ++capture) {
  const auto& value = expected.at(prefix + "encoder/" + std::to_string(capture));
  if (!value.defined() || value.scalar_type() != torch::kInt64 || (value.dim() != 2 && value.dim() != 3)) invalid("invalid encoder proposal capture");
  // Upstream topk captures either one [batch, queries] group or a batched
  // [groups, batch, queries] call. Project both into logical group order.
  for (const auto& group : (value.dim() == 2 ? value.unsqueeze(0) : value).unbind(0)) expected_indices.emplace(std::to_string(expected_indices.size()), group);
 }
 if (observed_encoder) comparison.inventory(prefix + "encoder identity inventory", indices, keys(expected_indices));
 for (std::size_t group = 0; group < encoder_indices.size(); ++group) {
  comparison.compare(prefix + "encoder/" + std::to_string(group), encoder_indices[group], expected_indices.at(std::to_string(group)), OracleComparisonPolicy::Exact);
 }
 std::exception_ptr first_difference;
 for (const auto& [name, value] : tensors) {
  try {
   comparison.compare(prefix + "output/" + name, value, expected.at(prefix + "output/" + name), OracleComparisonPolicy::Value);
  } catch (const std::runtime_error&) {
   if (!first_difference) first_difference = std::current_exception();
  }
 }
 if (first_difference) std::rethrow_exception(first_difference);
}
void compare_oracle_vjp(OracleComparison& comparison, const OracleArchive& archive, NativeRfDetrModel& model, const ModelOutputs& outputs, const torch::Tensor& input, bool retain_graph) {
 const auto tensors = oracle_output_tensors(outputs);
 comparison.inventory("cotangent inventory", keys(tensors), archive.names("cotangent/"));
 torch::Tensor scalar;
 // Retain the producer's scalar reduction order from its hash-admitted tensor
 // dictionary. Lexical map order changes low-precision backward accumulation.
 const auto cotangents = archive.ordered_defined_names("cotangent/");
 comparison.inventory("defined cotangent inventory", {cotangents.begin(), cotangents.end()}, keys(tensors));
 {
  std::vector<std::string> read_names;
  for (const auto& name : cotangents) read_names.push_back("cotangent/" + name);
  const auto expected = archive.read_tensors(read_names);
  for (const auto& name : cotangents) {
   const auto& value = tensors.at(name);
   auto term = (value * expected.at("cotangent/" + name).to(value.device())).sum();
   scalar = scalar.defined() ? scalar + term : term;
  }
 }
 std::vector<torch::Tensor> operands{input};
 std::vector<std::string> names{"input"};
 for (const auto& entry : model.named_parameters(true))
  if (entry.value().requires_grad()) {
   names.push_back("parameter/" + entry.key());
   operands.push_back(entry.value());
  }
 comparison.inventory("VJP inventory", {names.begin(), names.end()}, archive.names("vjp/"));
 const auto gradients = torch::autograd::grad({scalar}, operands, {}, retain_graph, false, true);
 std::vector<std::string> read_names;
 for (const auto& name : names) read_names.push_back("vjp/" + name);
 const auto references = archive.read_tensors(read_names);
 TensorMap actual, expected;
 for (std::size_t i = 0; i < names.size(); ++i) {
  actual.emplace(names[i], gradients[i]);
  expected.emplace(names[i], references.at("vjp/" + names[i]));
 }
 comparison.derivatives("vjp/", actual, expected);
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
