#include "checkpoint_fixture_support/upstream_oracle_fixture.h"
#include "src/backend/ml/torch/scalar_type.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/tests/training_fixture.h"
#include "src/common/io/file_digest.h"
#include <caffe2/serialize/inline_container.h>
#include <c10/core/ScalarType.h>
#include <torch/csrc/jit/serialization/import_read.h>
#include <torch/csrc/autograd/autograd.h>
#include <torch/version.h>
#include <ATen/Context.h>
#include <ATen/autocast_mode.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>
namespace mmltk::backend::models::rfdetr::testsupport {
namespace {
constexpr std::uint64_t kManifestLimit = 32U * 1024U * 1024U;
constexpr std::uint64_t kArchiveLimit = 4ULL * 1024U * 1024U * 1024U;
using Json = nlohmann::json;
[[noreturn]] void invalid(const std::string& message) { throw std::runtime_error("RF-DETR upstream oracle: " + message); }
std::filesystem::path child(const std::filesystem::path& parent, const std::string& name) {
 if (name.empty() || name.size() > 256 || std::filesystem::path(name).filename() != name || name == "." || name == "..") invalid("invalid artifact filename");
 const auto value = parent / name;
 if (std::filesystem::is_symlink(value)) invalid("artifact symlink is not admitted: " + name);
 return value;
}
void verify_hash(const std::filesystem::path& file, const std::string& hash) {
 if (hash.size() != 64 || mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(file)) != hash) invalid("SHA256 mismatch: " + file.string());
}
at::ScalarType oracle_dtype(at::ScalarType dtype) {
 // The v1 oracle admits a strict subset of the shared scalar vocabulary.
 switch (dtype) {
  case at::kFloat: case at::kHalf: case at::kBFloat16: case at::kDouble:
  case at::kLong: case at::kInt: case at::kByte: case at::kBool: return dtype;
  default: invalid("unsupported tensor dtype");
 }
}
at::ScalarType oracle_dtype(std::string_view external_name) {
 if (!external_name.starts_with("torch.")) invalid("unsupported manifest tensor dtype");
 try {
  return oracle_dtype(mmltk::backend::ml::serialization::scalar_type_from_name(external_name.substr(6)));
 } catch (const std::runtime_error&) {
  invalid("unsupported manifest tensor dtype");
 }
}
std::string dtype_name(at::ScalarType dtype) {
 return "torch." + mmltk::backend::ml::serialization::scalar_type_name(oracle_dtype(dtype));
}
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
OracleArchiveRecord archive_record(const Json& value) {
 OracleArchiveRecord result;
 result.file = value.at("file").get<std::string>();
 result.sha256 = value.at("sha256").get<std::string>();
 result.bytes = value.at("bytes").get<std::uint64_t>();
 if (!result.bytes || result.bytes > kArchiveLimit || value.at("tensors").size() > 20000) invalid("unbounded tensor archive");
 std::uint64_t bytes = 0;
 for (const auto& [name, item] : value.at("tensors").items()) {
  if (name.empty() || name.size() > 512) invalid("invalid tensor name");
  OracleTensor tensor;
  tensor.defined = item.at("defined").get<bool>();
  if (tensor.defined) {
   tensor.shape = item.at("shape").get<std::vector<int64_t>>();
   tensor.stride = item.at("stride").get<std::vector<int64_t>>();
   tensor.dtype = oracle_dtype(item.at("dtype").get<std::string>());
   tensor.device = item.at("device").get<std::string>();
   tensor.finite_count = item.at("finite_count").get<int64_t>();
   tensor.elements = item.at("elements").get<int64_t>();
   if (tensor.shape.size() > 8 || tensor.stride.size() != tensor.shape.size() || tensor.elements < 0 || tensor.finite_count < 0 || tensor.finite_count > tensor.elements) invalid("invalid tensor metadata");
   if (std::ranges::any_of(tensor.stride, [](int64_t stride) { return stride < 0 || static_cast<std::uint64_t>(stride) > kArchiveLimit; })) invalid("invalid source tensor stride");
   const auto size = tensor_bytes(tensor);
   if (size > kArchiveLimit - bytes) invalid("unbounded aggregate tensor storage");
   bytes += size;
  }
  result.tensors.emplace(name, std::move(tensor));
 }
 return result;
}
std::set<std::string> keys(const TensorMap& values) {
 std::set<std::string> result;
 for (const auto& [name, value] : values) result.insert(name);
 return result;
}
}
at::ScalarType OracleCase::dtype() const {
 if (precision == "fp32") return torch::kFloat32;
 if (precision == "fp16") return torch::kFloat16;
 if (precision == "bf16") return torch::kBFloat16;
 invalid("unsupported precision " + precision);
}
OracleArchive::OracleArchive(const std::filesystem::path& directory, const OracleArchiveRecord& record) {
 const auto file = child(directory, record.file);
 if (std::filesystem::file_size(file) != record.bytes) invalid("archive size mismatch");
 verify_hash(file, record.sha256);
 caffe2::serialize::PyTorchStreamReader reader(file.string());
 std::uint64_t storage_budget = 0, storage_bytes = 0, metadata_bytes = 0;
 for (const auto& [name, spec] : record.tensors) if (spec.defined) {
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
  if (tensor.layout() != torch::kStrided || !tensor.is_cpu() || !tensor.is_contiguous() ||
      tensor.numel() < 0 || static_cast<std::uint64_t>(tensor.numel()) > kArchiveLimit / tensor.element_size()) invalid("unbounded or noncontiguous serialized tensor");
  const auto storage_elements = tensor.storage().nbytes() / tensor.element_size();
  if (tensor.storage_offset() < 0 || static_cast<std::uint64_t>(tensor.storage_offset()) > storage_elements ||
      static_cast<std::uint64_t>(tensor.numel()) > storage_elements - static_cast<std::uint64_t>(tensor.storage_offset())) invalid("serialized tensor exceeds its storage");
 }
 std::set<std::string> expected;
 for (const auto& [name, spec] : record.tensors) {
  if (!spec.defined) { undefined_.insert(name); continue; }
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
 for (const auto& item : dictionary) if (!item.key().isString() || !expected.erase(item.key().toStringRef())) invalid("unexpected archive entry");
 if (!expected.empty()) invalid("missing archive entries");
}
torch::Tensor OracleArchive::tensor(const std::string& name, const torch::Device device) const {
 if (undefined_.contains(name)) return {};
 const auto found = tensors_.find(name);
 if (found == tensors_.end()) invalid("required tensor missing: " + name);
 return found->second.to(device);
}
bool OracleArchive::contains(const std::string& name) const { return tensors_.contains(name) || undefined_.contains(name); }
std::set<std::string> OracleArchive::names(const std::string_view prefix) const {
 std::set<std::string> result;
 for (const auto& [name, value] : tensors_) if (name.starts_with(prefix)) result.insert(name.substr(prefix.size()));
 for (const auto& name : undefined_) if (name.starts_with(prefix)) result.insert(name.substr(prefix.size()));
 return result;
}
std::optional<std::filesystem::path> UpstreamOracle::requested() {
 const auto* value = std::getenv("MMLTK_RFDETR_UPSTREAM_ORACLE_DIR");
 if (!value) return std::nullopt;
 if (!*value) invalid("explicitly requested evidence path is empty");
 return std::filesystem::path(value);
}
UpstreamOracle::UpstreamOracle(const std::filesystem::path& root) {
 const auto path = child(root, "manifest.json");
 if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > kManifestLimit) invalid("requested manifest missing or oversized");
 std::ifstream stream(path);
 const auto manifest = Json::parse(stream);
 if (manifest.at("format") != "mmltk.rfdetr.upstream-oracle.v1" || manifest.at("revision") != "e9a138f70cc14e3fddca029a0683055e34ab8f35" ||
     manifest.at("status") != "references_complete_native_comparison_pending") invalid("incompatible oracle format/revision/status");
 if (manifest.at("policy").at("matmul_tf32").get<bool>() || manifest.at("policy").at("cudnn_tf32").get<bool>() || manifest.at("policy").at("gradient_scale") != 1 ||
     manifest.at("policy").at("accumulated_microbatches") != 1) invalid("incompatible controlled numerical policy");
 for (const auto* key : {"producer_sha256", "dependency_sha256", "payload_sha256", "fingerprint"})
  if (manifest.at(key).get<std::string>().size() != 64) invalid("missing provenance identity");
 if (manifest.at("source_hashes").empty() || manifest.at("source_hashes").size() > 5000) invalid("missing source provenance");
 if (!manifest.at("environment").at("torch").get<std::string>().starts_with(TORCH_VERSION) ||
     manifest.at("torch_build_config").get<std::string>().empty()) invalid("incompatible Torch payload provenance");
 directory_ = child(root, manifest.at("directory").get<std::string>());
 if (manifest.at("presets").size() != 2) invalid("missing required preset");
 for (const auto& preset : manifest.at("presets")) {
  const auto name = preset.at("name").get<std::string>();
  const auto checkpoint = child(directory_, preset.at("checkpoint").get<std::string>());
  verify_hash(checkpoint, preset.at("sha256").get<std::string>());
  verify_hash(child(directory_, preset.at("original").get<std::string>()), preset.at("original_sha256").get<std::string>());
  for (const auto& [kind, facts] : preset.at("constructor").items())
   for (const auto& [member, passed] : facts.items()) if (!passed.get<bool>()) invalid("upstream constructor invariant failed: " + member);
  if (!checkpoints_.emplace(name, checkpoint).second) invalid("duplicate preset");
 }
 if (manifest.at("cases").size() < 16 || manifest.at("cases").size() > 24) invalid("incomplete or unbounded model matrix");
 std::set<std::string> identities;
 for (const auto& item : manifest.at("cases")) {
  OracleCase entry;
  entry.id = item.at("id").get<std::string>();
  entry.preset = item.at("preset").get<std::string>();
  entry.precision = item.at("precision").get<std::string>();
  entry.training = item.at("training").get<bool>();
  entry.batch = item.at("batch").get<int>();
  entry.resolution = item.at("resolution").get<int>();
  entry.groups = item.at("groups").get<int>();
  entry.classes = item.at("classes").get<int>();
  entry.trainable = item.at("trainable").get<std::vector<std::string>>();
  entry.loss_weights = item.at("loss_weights").get<std::map<std::string, double>>();
  entry.clip_max_norm = item.at("clip_max_norm").get<double>();
  const auto& recipe = item.at("optimizer");
  entry.recipe.lr = recipe.at("lr");
  entry.recipe.lr_encoder = recipe.at("lr_encoder");
  entry.recipe.lr_component_decay = recipe.at("lr_component_decay");
  entry.recipe.encoder_layer_decay = recipe.at("lr_vit_layer_decay");
  entry.recipe.weight_decay = recipe.at("weight_decay");
  if (entry.id.size() > 128 || !identities.insert(entry.id).second || !checkpoints_.contains(entry.preset) || (entry.batch != 1 && entry.batch != 2) || entry.groups != 2 || entry.classes != 91 ||
      entry.resolution != (entry.preset == "rf-detr-nano" ? 384 : 312) || entry.trainable.empty() || entry.trainable.size() > 2000) invalid("invalid case configuration");
  (void)entry.dtype();
  if (item.at("comparison_policy") != (entry.dtype() == torch::kFloat32 || !entry.training ? "untouched_stock" : "upstream_with_fp32_matcher_boundary") ||
      item.at("execution_policy").at("autocast_enabled").get<bool>() != (entry.dtype() != torch::kFloat32) ||
      (entry.dtype() != torch::kFloat32 && item.at("execution_policy").at("autocast_dtype") != dtype_name(entry.dtype()))) invalid("case precision policy mismatch");
  for (const auto& group : item.at("parameter_groups")) {
   OracleParameterGroup value;
   value.names = group.at("names").get<std::vector<std::string>>();
   value.lr = group.at("lr"); value.weight_decay = group.at("weight_decay");
   value.beta1 = group.at("betas").at(0); value.beta2 = group.at("betas").at(1);
   value.eps = group.at("eps"); value.amsgrad = group.at("amsgrad");
   entry.parameter_groups.push_back(std::move(value));
  }
  const std::size_t expected_steps = entry.training && entry.batch == 2 && entry.preset == "rf-detr-nano" && entry.precision == "fp32" ? 8 : 1;
  if (item.at("steps").size() != expected_steps) invalid("trajectory length mismatch");
  for (const auto& step : item.at("steps")) {
   if (step.at("step").get<std::size_t>() != entry.steps.size()) invalid("trajectory step sequence mismatch");
   entry.steps.push_back(archive_record(step));
  }
  cases_.push_back(std::move(entry));
 }
 const bool bf16 = std::ranges::any_of(cases_, [](const auto& value) { return value.precision == "bf16"; });
 if (bf16 ? !manifest.at("unsupported").empty() : manifest.at("unsupported") != Json::array({"bf16: CUDA device does not support BF16"})) invalid("precision support inventory differs");
 for (const auto& preset : {"rf-detr-nano", "rf-detr-seg-nano"})
  for (const auto& precision : {"fp32", "fp16", "bf16"})
   for (const auto batch : {1, 2})
    for (const bool training : {false, true})
     if (std::ranges::count_if(cases_, [&](const auto& value) { return value.preset == preset && value.precision == precision && value.batch == batch && value.training == training; }) != (std::string_view(precision) == "bf16" && !bf16 ? 0 : 1))
      invalid("missing required model case");
 resize_ = archive_record(manifest.at("resize").at("archive"));
}
std::filesystem::path UpstreamOracle::checkpoint(const std::string_view preset) const { return checkpoints_.at(std::string(preset)); }
NativeRfDetrModel UpstreamOracle::model(const OracleCase& entry) const {
 auto admitted = resolve_model_state(checkpoint(entry.preset), entry.preset, entry.resolution);
 auto config = admitted.artifacts.config;
 config.group_detr = entry.groups;
 if (config.num_classes != entry.classes) invalid("checkpoint class inventory differs");
 NativeRfDetrModel model(config, admitted.artifacts.class_layout);
 auto candidate = model.stage_normalized_state(admitted.model_state.entries(), detail::NormalizedModelStateAdmission::RequiredCore);
 if (!candidate.summary.missing_names.empty() || !candidate.summary.incompatible_names.empty() || !candidate.summary.unexpected_names.empty()) invalid("upstream checkpoint admission inventory differs");
 model.commit_normalized_state(std::move(candidate));
 model.to(torch::Device(torch::kCUDA));
 model.train(entry.training);
 model.set_force_pytorch_deformable_attn(false);
 std::set<std::string> trainable;
 for (const auto& value : model.named_parameters(true)) if (value.value().requires_grad()) trainable.insert(value.key());
 if (trainable != std::set<std::string>(entry.trainable.begin(), entry.trainable.end())) invalid("trainable parameter inventory differs");
 return model;
}
OracleArchive UpstreamOracle::archive(const OracleCase& entry, std::size_t step) const { return {directory_, entry.steps.at(step)}; }
OracleArchive UpstreamOracle::resize_archive() const { return {directory_, resize_}; }
OracleComparison::OracleComparison(const std::filesystem::path& root, std::string name, at::ScalarType precision, std::string objective) : precision_(precision) {
 const auto reports = root / "comparisons";
 std::filesystem::create_directories(reports);
 report_.open(child(reports, name + ".jsonl"), std::ios::app);
 if (!report_) invalid("cannot write comparison evidence");
 static const auto executable_hash = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file("/proc/self/exe"));
 record({{"boundary", "policy"}, {"run_unix_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
         {"precision", dtype_name(precision)}, {"matmul_tf32", at::globalContext().allowTF32CuBLAS()}, {"cudnn_tf32", at::globalContext().allowTF32CuDNN()},
         {"forward_autocast", precision != torch::kFloat32}, {"cudnn_benchmark", at::globalContext().benchmarkCuDNN()},
         {"flash_sdp", at::globalContext().userEnabledFlashSDP()}, {"memory_efficient_sdp", at::globalContext().userEnabledMemEfficientSDP()}, {"math_sdp", at::globalContext().userEnabledMathSDP()},
         {"native_torch", TORCH_VERSION}, {"native_executable_sha256", executable_hash},
         {"attention", "native production CUDA"}, {"matcher", "native FP32"}, {"objective", std::move(objective)},
         {"hyperparameter_tolerance_reason", "eight float64 epsilons allow upstream decay**2 versus native repeated multiplication; zero settings and group membership remain exact"},
         {"tolerance_reason", "FP32 operation/reduction order; AMP output rounding and CUDA atomic VJP accumulation; discrete identities and finite masks always exact"}});
}
void OracleComparison::record(Json value) { report_ << value.dump() << '\n'; report_.flush(); if (!report_) invalid("comparison evidence write failed"); }
void OracleComparison::fact(const std::string& boundary, const Json& value) { record({{"boundary", boundary}, {"observation", value}}); }
void OracleComparison::inventory(const std::string& boundary, const std::set<std::string>& actual, const std::set<std::string>& expected) {
 const bool match = actual == expected;
 record({{"boundary", boundary}, {"actual", actual}, {"expected", expected}, {"passed", match}, {"first_difference", !match && !failed_}});
 if (!match) { failed_ = true; invalid("first differing inventory: " + boundary); }
}
void OracleComparison::compare(const std::string& boundary, const torch::Tensor& actual, const torch::Tensor& expected, OracleComparisonPolicy policy, bool policy_difference) {
 Json result{{"boundary", boundary}, {"policy_difference", policy_difference}, {"actual_defined", actual.defined()}, {"expected_defined", expected.defined()}};
 bool passed = actual.defined() == expected.defined();
 if (actual.defined() && expected.defined()) {
  result["actual_dtype"] = dtype_name(actual.scalar_type()); result["expected_dtype"] = dtype_name(expected.scalar_type());
   result["actual_stride"] = actual.strides().vec();
   result["actual_device"] = actual.device().str(); result["archive_device"] = expected.device().str();
  result["actual_shape"] = actual.sizes().vec(); result["expected_shape"] = expected.sizes().vec();
  passed = actual.sizes() == expected.sizes() && actual.scalar_type() == expected.scalar_type();
  if (actual.sizes() == expected.sizes()) {
   const auto a = actual.detach().cpu(), b = expected.detach().cpu();
   const auto finite = torch::isfinite(a);
   bool finite_equal = torch::equal(finite, torch::isfinite(b));
   if (a.is_floating_point() && b.is_floating_point()) finite_equal &= torch::equal(torch::isnan(a), torch::isnan(b)) && torch::equal(torch::isposinf(a), torch::isposinf(b)) && torch::equal(torch::isneginf(a), torch::isneginf(b));
   const bool all_finite = finite.all().item<bool>();
   result["finite_equal"] = finite_equal; result["all_finite"] = all_finite; passed &= finite_equal && all_finite;
   if (!a.is_floating_point() || policy == OracleComparisonPolicy::Exact) {
    passed &= torch::equal(a, b); result["discrete_equal"] = torch::equal(a, b);
   } else if (a.numel() && finite_equal) {
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
     const double normalized = derivative ? std::max(maximum / (absolute + relative * bv.abs().max().item<double>()), norm / (absolute + relative * bv.norm().item<double>()))
                                          : (difference.abs() / (absolute + relative * bv.abs()).clamp_min(std::numeric_limits<double>::min())).max().item<double>();
     result["max_absolute"] = maximum; result["l2_error"] = norm; result["normalized_error"] = normalized;
     result["absolute_tolerance"] = absolute; result["relative_tolerance"] = relative; result["derivative_norm_bound"] = derivative;
     passed &= std::isfinite(normalized) && normalized <= 1.;
    }
   }
  }
 }
 result["passed"] = passed; result["first_difference"] = !passed && !failed_ && !policy_difference;
 record(std::move(result));
 if (!passed && !policy_difference) { failed_ = true; invalid("first differing tensor boundary: " + boundary); }
}
TensorMap oracle_output_tensors(const ModelOutputs& outputs) {
 TensorMap result;
 const auto layer = [&](const std::string& prefix, const OutputLayer& value) {
  result.emplace(prefix + "/pred_logits", value.pred_logits);
  result.emplace(prefix + "/pred_boxes", value.pred_boxes);
  if (value.pred_masks) result.emplace(prefix + "/pred_masks", *value.pred_masks);
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
 std::vector<torch::Tensor> boxes, labels, masks;
 int64_t offset = 0;
 for (int image = 0; image < entry.batch; ++image) {
  const auto prefix = "target/" + std::to_string(image) + "/";
  auto b = archive.tensor(prefix + "boxes", device), l = archive.tensor(prefix + "labels", device);
  result.targets.push_back({.boxes = b, .labels = l});
  result.offsets.push_back(offset); result.counts.push_back(l.numel()); offset += l.numel();
  boxes.push_back(b); labels.push_back(l);
  if (archive.contains(prefix + "masks")) masks.push_back(archive.tensor(prefix + "masks"));
 }
 result.all_boxes = torch::cat(boxes); result.all_labels = torch::cat(labels);
 const auto integers = torch::TensorOptions().device(device).dtype(torch::kInt64);
 result.target_counts = torch::tensor(result.counts, integers); result.target_offsets = torch::tensor(result.offsets, integers);
 result.target_indices = torch::arange(offset, integers); result.resolved_query_count = entry.preset == "rf-detr-nano" ? 300 : 100;
 if (!masks.empty()) {
  const auto dense = torch::cat(masks).to(torch::kBool).contiguous();
  const auto pixels = dense.size(1) * dense.size(2), words = (pixels + 63) / 64;
  auto bits = torch::zeros({offset, words}, torch::kInt64);
  auto* destination = reinterpret_cast<std::uint64_t*>(bits.data_ptr<int64_t>());
  const auto* source = dense.data_ptr<bool>();
  for (int64_t row = 0; row < offset; ++row)
   for (int64_t pixel = 0; pixel < pixels; ++pixel) if (source[row * pixels + pixel]) destination[row * words + pixel / 64] |= std::uint64_t{1} << (pixel % 64);
  result.packed_masks = PackedTargetMasks{bits.to(device), dense.size(1), dense.size(2)};
 }
 return result;
}
std::vector<LayerMaskSamples> oracle_samples(const OracleArchive& archive, std::size_t layers, torch::Device device, const std::string& prefix) {
 std::vector<LayerMaskSamples> samples(layers);
 if (!archive.contains(prefix + "samples/0/matcher")) return samples;
 for (std::size_t i = 0; i < layers; ++i) {
  const auto key = prefix + "samples/" + std::to_string(i) + "/";
  samples[i] = {archive.tensor(key + "matcher", device), archive.tensor(key + "candidates", device), archive.tensor(key + "remainder", device)};
 }
 return samples;
}
DetectionConfig oracle_detection(const NativeRfDetrConfig& config) {
 auto result = detection_fixture_base(config);
 project_loss_coefficients(result, config);
 result.include_masks = config.segmentation; result.mask_point_sample_ratio = config.mask_point_sample_ratio;
 populate_default_detection_weight_dict(result);
 return result;
}
void compare_oracle_outputs(OracleComparison& comparison, const OracleArchive& archive, const ModelOutputs& outputs, const std::vector<torch::Tensor>& encoder_indices, const std::string& prefix) {
 const auto tensors = oracle_output_tensors(outputs);
 comparison.inventory(prefix + "output inventory", keys(tensors), archive.names(prefix + "output/"));
 std::set<std::string> indices;
 for (std::size_t group = 0; group < encoder_indices.size(); ++group) indices.insert(std::to_string(group));
 comparison.inventory(prefix + "encoder identity inventory", indices, archive.names(prefix + "encoder/"));
 for (std::size_t group = 0; group < encoder_indices.size(); ++group) {
  const auto name = prefix + "encoder/" + std::to_string(group);
  comparison.compare(name, encoder_indices[group], archive.tensor(name), OracleComparisonPolicy::Exact);
 }
 for (const auto& [name, value] : tensors) comparison.compare(prefix + "output/" + name, value, archive.tensor(prefix + "output/" + name), OracleComparisonPolicy::Value);
}
void compare_oracle_vjp(OracleComparison& comparison, const OracleArchive& archive, NativeRfDetrModel& model, const ModelOutputs& outputs, const torch::Tensor& input, bool retain_graph) {
 const auto tensors = oracle_output_tensors(outputs);
 comparison.inventory("cotangent inventory", keys(tensors), archive.names("cotangent/"));
 torch::Tensor scalar;
 for (const auto& [name, value] : tensors) {
  auto term = (value * archive.tensor("cotangent/" + name, value.device())).sum();
  scalar = scalar.defined() ? scalar + term : term;
 }
 std::vector<torch::Tensor> operands{input};
 std::vector<std::string> names{"input"};
 for (const auto& entry : model.named_parameters(true)) if (entry.value().requires_grad()) { names.push_back("parameter/" + entry.key()); operands.push_back(entry.value()); }
 comparison.inventory("VJP inventory", {names.begin(), names.end()}, archive.names("vjp/"));
 const auto gradients = torch::autograd::grad({scalar}, operands, {}, retain_graph, false, true);
 for (std::size_t i = 0; i < names.size(); ++i) comparison.compare("vjp/" + names[i], gradients[i], archive.tensor("vjp/" + names[i]), OracleComparisonPolicy::Derivative);
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
