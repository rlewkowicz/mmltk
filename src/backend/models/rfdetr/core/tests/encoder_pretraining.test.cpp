#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/core/encoder_pretraining.h"
#include "src/backend/models/rfdetr/core/detail/backbone.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "tools/oracle/native_catalog.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <torch/nn/functional.h>
#include <torch/version.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
namespace {
namespace rf = mmltk::backend::models::rfdetr;
namespace io = mmltk::common::io;
namespace fixtures = mmltk::testsupport;
using Json = nlohmann::json;
std::vector<std::uint8_t> safe_bytes(std::string header, std::vector<float> data = {1.F, 2.F}) {
 while (header.size() % 8U) header.push_back(' ');
 std::vector<std::uint8_t> bytes(8U + header.size() + data.size() * sizeof(float));
 for (std::size_t index = 0; index < 8U; ++index) bytes[index] = static_cast<std::uint8_t>((header.size() >> (8U * index)) & 255U);
 std::memcpy(bytes.data() + 8, header.data(), header.size());
 std::memcpy(bytes.data() + 8 + header.size(), data.data(), data.size() * sizeof(float));
 return bytes;
}
std::vector<rf::NormalizedModelStateEntry> decode(const std::vector<std::uint8_t>& bytes) { return rf::detail::decode_encoder_safetensors(bytes, bytes.size()); }
rf::NativeRfDetrConfig synthetic_config() {
 auto config = rf::native_config_from_preset(*rf::find_preset_catalog_entry("rf-detr-nano"));
 config.resolution = 64;
 config.group_detr = 1;
 config.num_queries = config.num_select = 2;
 return config;
}
std::vector<rf::NormalizedModelStateEntry> synthetic_source(const rf::NativeRfDetrConfig& destination) {
 auto config = destination;
 config.patch_size = 14;
 config.positional_encoding_size = 37;
 const auto encoder = rf::detail::make_backbone_encoder(config);
 std::vector<rf::NormalizedModelStateEntry> result;
 for (const auto& parameter : encoder->named_parameters(true)) result.push_back({parameter.key().substr(std::string_view("encoder.").size()), parameter.value().detach().clone()});
 for (const auto& buffer : encoder->named_buffers(true)) result.push_back({buffer.key().substr(std::string_view("encoder.").size()), buffer.value().detach().clone()});
 return result;
}
std::string tensor_digest(const torch::Tensor& tensor) {
 const auto contiguous = tensor.detach().cpu().contiguous();
 return io::sha256_hex(io::sha256_bytes({static_cast<const std::uint8_t*>(contiguous.const_data_ptr()), contiguous.nbytes()}));
}
std::map<std::string, std::string> state_digests(const rf::NativeRfDetrModel& model) {
 std::map<std::string, std::string> result;
 for (const auto& parameter : model.named_parameters(true)) result.emplace(parameter.key(), tensor_digest(parameter.value()));
 for (const auto& buffer : model.named_buffers(true)) result.emplace(buffer.key(), tensor_digest(buffer.value()));
 return result;
}
void admit(bool condition, const std::string& message) {
 if (!condition) throw std::runtime_error("RF-DETR encoder evidence: " + message);
}
Json bounded_json(const std::filesystem::path& path) {
 admit(std::filesystem::is_regular_file(path), "missing manifest: " + path.string());
 const auto mapped = io::MappedFile::open_readonly(path.string());
 admit(mapped.size() <= 32U * 1024U * 1024U, "manifest exceeds byte bound");
 std::vector<std::set<std::string>> keys;
 return Json::parse(mapped.data(), mapped.data() + mapped.size(), [&](int depth, Json::parse_event_t event, Json& value) {
  admit(depth <= 16, "manifest exceeds depth bound");
  if (event == Json::parse_event_t::object_start)
   keys.emplace_back();
  else if (event == Json::parse_event_t::key)
   admit(keys.back().insert(value.get<std::string>()).second, "duplicate manifest key");
  else if (event == Json::parse_event_t::object_end)
   keys.pop_back();
  return true;
 });
}
std::optional<std::filesystem::path> encoder_evidence_directory(const char* supplied) {
 if (!supplied) return std::nullopt;
 admit(*supplied != '\0', "explicit evidence directory is empty");
 const auto root = std::filesystem::canonical(supplied);
 admit(std::filesystem::is_directory(root), "explicit evidence path is not a directory");
 admit(std::filesystem::is_regular_file(root / "manifest.json"), "explicit evidence manifest is missing");
 return root;
}
std::filesystem::path evidence_repository() {
 const char* repository = std::getenv("MMLTK_REPO_ROOT");
 admit(repository && *repository, "wrapper repository provenance is missing");
 return repository;
}
std::string file_digest(const std::filesystem::path& path) { return io::sha256_hex(io::sha256_file(path)); }
void admit_digest(const Json& digest) {
 admit(digest.is_string(), "SHA-256 identity is not a string");
 const auto& value = digest.get_ref<const std::string&>();
 admit(value.size() == 64 && std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "malformed SHA-256 identity");
}
void admit_version_map(const Json& versions) {
 admit(versions.is_object() && !versions.empty() && versions.size() <= 512, "missing or unbounded dependency versions");
 for (const auto& [name, version] : versions.items()) {
  admit(!name.empty() && name.size() <= 128 && name.find('\0') == std::string::npos && version.is_string(), "malformed dependency version record");
  const auto& value = version.get_ref<const std::string&>();
  admit(!value.empty() && value.size() <= 128 && value.find('\0') == std::string::npos, "malformed dependency version");
 }
}
Json pinned_dependencies(const std::filesystem::path& repository) {
 std::istringstream input(fixtures::read_text_file(repository / "tools/oracle/rfdetr_oracle_requirements.txt"));
 Json result = Json::object();
 for (std::string line; std::getline(input, line);) {
  if (line.empty() || line.starts_with('#')) continue;
  const auto separator = line.find("==");
  admit(separator != std::string::npos && line.find("==", separator + 2) == std::string::npos, "dependency is not exactly pinned");
  std::string name;
  for (auto c : line.substr(0, separator)) {
   if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
   if (c == '_' || c == '.') c = '-';
   if (c != '-' || name.empty() || name.back() != '-') name.push_back(c);
  }
  admit(!result.contains(name), "duplicate dependency pin");
  result[name] = line.substr(separator + 2);
 }
 admit_version_map(result);
 return result;
}
const Json& encoder_reference_policy() {
 static const Json policy{
  {"transformations", {{"patch", "timm.layers.patch_embed.resample_patch_embed"}, {"positions", "bicubic antialias align_corners=false; prefix preserved"}, {"intermediate_dtype", "torch.float32"},
                       {"pinv_atol", nullptr}, {"pinv_rtol", nullptr}, {"runtime_grid", "4*num_windows; stored grid follows native preset"}}},
  {"execution_policy", {{"sdpa_backend", "math"}, {"matmul_tf32", false}, {"cudnn_tf32", false}, {"matmul_precision", "highest"}, {"flash_sdp", false}, {"math_sdp", true},
                        {"memory_efficient_sdp", false}, {"cudnn_sdp", false}, {"grad_enabled", false}}},
  {"numerical_policy",
   {{"patch_atol", 3.e-5}, {"patch_rtol", 3.e-4}, {"feature_atol", 5.e-4}, {"feature_rtol", 5.e-4}, {"controls", "CPU FP32 vs FP64 pinv; independent upstream CPU vs CUDA FP32 math SDPA"}}}
 };
 return policy;
}
void admit_encoder_manifest(const Json& manifest, const std::filesystem::path& repository) {
 admit(
  manifest.at("format") == "mmltk.rfdetr.encoder-pretraining.v1" && manifest.at("mode") == "encoder-pretraining" && manifest.at("status") == "encoder_references_complete_native_comparison_pending",
  "incompatible format, mode or completion status");
 const auto sources = repository / "tools/oracle/rfdetr_sources.json", requirements = repository / "tools/oracle/rfdetr_oracle_requirements.txt";
 const auto pins = bounded_json(sources), payload = bounded_json(repository / "docker/nvidia-payload.json");
 admit(manifest.at("sources") == pins, "source pins differ");
 admit(manifest.at("native_catalog") == Json::parse(rfdetr_native_catalog_json()), "native catalog differs");
 for (const auto* key : {"fingerprint", "producer_sha256", "dependency_sha256", "source_manifest_sha256"}) admit_digest(manifest.at(key));
 // Producer/image identities are opaque in the native container. Available
 // repository inputs are bound by bytes; no Python installation is required.
 admit(manifest.at("dependency_sha256") == file_digest(requirements), "dependency file digest differs");
 admit(manifest.at("source_manifest_sha256") == file_digest(sources), "source manifest digest differs");
 const auto& hashes = manifest.at("source_hashes");
 admit(hashes.is_object() && hashes.size() == 2, "source package hash maps differ");
 for (const auto* kind : {"core", "plus"}) {
  const auto& package = hashes.at(kind);
  admit(package.is_object() && !package.empty() && package.size() <= 5000, "missing or unbounded source hash map");
  for (const auto& [name, hash] : package.items()) {
   const std::filesystem::path path(name);
   admit(!name.empty() && name.size() <= 1024 && name.find('\0') == std::string::npos && !path.is_absolute() && std::ranges::none_of(path, [](const auto& part) { return part == ".."; }),
    "invalid source hash path");
   admit_digest(hash);
  }
 }
 const auto& operators = manifest.at("operator_sources");
 admit(operators.is_object() && operators.size() == 2, "operator source identities differ");
 for (const auto* name : {"patch_embed", "pos_embed"}) admit_digest(operators.at(name));
 const auto& environment = manifest.at("environment");
 const auto& distributions = environment.at("distributions");
 const auto& dependencies = environment.at("dependency_closure");
 admit_version_map(distributions);
 admit_version_map(dependencies);
 const auto torch_version = environment.at("torch").get<std::string>();
 admit(torch_version == payload.at("donor_metadata").at("torch_runtime_version").get_ref<const std::string&>() && torch_version.starts_with(TORCH_VERSION), "incompatible Torch runtime");
 admit(distributions.at("torch") == payload.at("donor_metadata").at("torch_distribution_version"), "incompatible Torch distribution");
 const auto cuda_version = payload.at("versions").at("cuda").get<std::string>();
 admit(environment.at("cuda") == cuda_version.substr(0, cuda_version.rfind('.')), "incompatible CUDA runtime");
 admit(environment.at("torchvision") == distributions.at("torchvision") && distributions.contains("triton"), "incomplete donor runtime versions");
 const auto required_dependencies = pinned_dependencies(repository);
 for (const auto& [name, version] : required_dependencies.items()) admit(dependencies.at(name) == version, "incompatible dependency: " + name);
 admit(dependencies.at("timm") == pins.at("timm"), "incompatible pinned operators");
 for (const auto& [name, policy] : encoder_reference_policy().items()) admit(manifest.at(name) == policy, "incompatible " + name);
 admit(manifest.at("precision_capability").at("fp32") == true, "missing FP32 capability");
 admit(manifest.at("cases").is_array() && manifest.at("cases").size() == rf::kPresetCatalog.size(), "incomplete preset matrix");
 admit(manifest.at("assets").is_array() && manifest.at("assets").size() == rf::kEncoderPretrainingAssets.size(), "incomplete source assets");
}
std::filesystem::path evidence_file(const std::filesystem::path& root, const std::string& relative) {
 REQUIRE(!relative.empty());
 REQUIRE(!std::filesystem::path(relative).is_absolute());
 const auto path = std::filesystem::canonical(root / relative);
 REQUIRE(fixtures::canonical_fixture_path(root, path).has_value());
 return path;
}
Json compare(const torch::Tensor& actual, const torch::Tensor& expected, const double atol, const double rtol) {
 REQUIRE(actual.sizes() == expected.sizes());
 REQUIRE(actual.scalar_type() == torch::kFloat32);
 REQUIRE(torch::isfinite(actual).all().item<bool>());
 REQUIRE(torch::allclose(actual, expected, rtol, atol));
 const auto difference = (actual - expected).abs();
 return {{"max_abs", difference.max().item<double>()}, {"rms", difference.square().mean().sqrt().item<double>()}};
}
}  // namespace
TEST_CASE("Encoder safetensors decoder bounds metadata and covers every payload byte", "[rfdetr][encoder_pretraining]") {
 const std::string valid = R"({"a":{"dtype":"F32","shape":[2],"data_offsets":[0,8]},"__metadata__":{"format":"pt"}})";
 const auto decoded = decode(safe_bytes(valid));
 REQUIRE(decoded.size() == 1);
 CHECK(torch::equal(decoded[0].tensor, torch::tensor({1.F, 2.F})));
 for (const auto* invalid :
  {R"({"a":{"dtype":"F32","shape":[2],"data_offsets":[0,8]},"a":{"dtype":"F32","shape":[2],"data_offsets":[0,8]}})", R"({"a":{"dtype":"F32","dtype":"F32","shape":[2],"data_offsets":[0,8]}})",
   R"({"a":{"dtype":"F16","shape":[2],"data_offsets":[0,8]}})", R"({"a":{"dtype":"F32","shape":[1,1,1,1,2],"data_offsets":[0,8]}})",
   R"({"a":{"dtype":"F32","shape":[18446744073709551615,2],"data_offsets":[0,8]}})", R"({"a":{"dtype":"F32","shape":[-2],"data_offsets":[0,8]}})",
   R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[4,8]}})", R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})",
   R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]},"b":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", R"({"a":{"dtype":"F32","shape":[2],"data_offsets":[0,12]}})",
   R"({"a":{"dtype":"F32","shape":[2],"data_offsets":[0,8],"extra":0}})", R"({"a":{"dtype":"F32","shape":[2],"data_offsets":[0,8]},"__metadata__":{"bad":2}})", R"({"model":{"a":2}})"}) {
  INFO(invalid);
  CHECK_THROWS(decode(safe_bytes(invalid)));
 }
 auto bytes = safe_bytes(valid);
 CHECK_THROWS(rf::detail::decode_encoder_safetensors(bytes, bytes.size() - 1));
 bytes.pop_back();
 CHECK_THROWS(decode(bytes));
 bytes = safe_bytes(valid);
 std::fill_n(bytes.begin(), 8, 255U);
 CHECK_THROWS(decode(bytes));
 CHECK_THROWS(decode(safe_bytes(std::string(1024U * 1024U + 1U, ' '))));
 Json many = Json::object();
 for (int index = 0; index < 4097; ++index) many[std::to_string(index)] = {{"dtype", "F32"}, {"shape", {1}}, {"data_offsets", {0, 4}}};
 CHECK_THROWS(decode(safe_bytes(many.dump())));
 CHECK_THROWS(decode(safe_bytes(valid, {std::numeric_limits<float>::infinity(), 2.F})));
 CHECK_THROWS(decode(safe_bytes(valid, {std::numeric_limits<float>::quiet_NaN(), 2.F})));
}
TEST_CASE("Encoder staging is exact and leaves fresh detector state intact", "[rfdetr][encoder_pretraining]") {
 torch::NoGradGuard guard;
 const auto config = synthetic_config();
 auto source = synthetic_source(config);
 torch::manual_seed(2381);
 rf::NativeRfDetrModel model(config);
 const auto before = state_digests(model);
 SECTION("missing state") {
  source.pop_back();
  CHECK_THROWS(model.stage_encoder_pretraining(source));
 }
 SECTION("duplicate state") {
  source.push_back(source.front());
  CHECK_THROWS(model.stage_encoder_pretraining(source));
 }
 SECTION("detector state") {
  source.push_back({"class_embed.weight", torch::zeros({1})});
  CHECK_THROWS(model.stage_encoder_pretraining(source));
 }
 SECTION("register tokens") {
  source.push_back({"embeddings.register_tokens", torch::zeros({1, 4, 384})});
  CHECK_THROWS(model.stage_encoder_pretraining(source));
 }
 SECTION("wrong dtype") {
  source.front().tensor = source.front().tensor.to(torch::kFloat64);
  CHECK_THROWS(model.stage_encoder_pretraining(source));
 }
 SECTION("wrong shape") {
  source.front().tensor = torch::zeros({1});
  CHECK_THROWS(model.stage_encoder_pretraining(source));
 }
 SECTION("nonfinite state") {
  source.front().tensor.fill_(std::numeric_limits<float>::quiet_NaN());
  CHECK_THROWS(model.stage_encoder_pretraining(source));
 }
 SECTION("family topology mismatch") {
  auto wrong_config = config;
  wrong_config.backbone.kind = rf::BackboneKind::DinoV2Base;
  auto encoder = rf::detail::make_backbone_encoder(config);
  CHECK_THROWS(rf::detail::stage_encoder_pretraining(*encoder, wrong_config, source));
 }
 SECTION("source custody failure rolls back copied state") {
  auto candidate = model.stage_encoder_pretraining(source);
  int checks = 0;
  candidate.validate_source = [&] {
   if (++checks == 2) throw std::runtime_error("source changed");
  };
  CHECK_THROWS(model.commit_normalized_state(std::move(candidate)));
  CHECK(checks == 2);
 }
 SECTION("complete encoder commit") {
  auto candidate = model.stage_encoder_pretraining(source);
  REQUIRE(candidate.summary.loaded_names.size() == 223);
  REQUIRE(candidate.summary.missing_names.empty());
  model.commit_normalized_state(std::move(candidate));
  const auto after = state_digests(model);
  std::size_t changed = 0;
  for (const auto& [name, digest] : before) {
   if (!name.starts_with("backbone.0.encoder."))
    CHECK(after.at(name) == digest);
   else
    changed += after.at(name) != digest;
  }
  CHECK(changed > 0);
  return;
 }
 CHECK(state_digests(model) == before);
}
TEST_CASE("Encoder file admission rejects wrong integrity and cancellation before mutation", "[rfdetr][encoder_pretraining]") {
 fixtures::ScopedTempDir directory("encoder-admission");
 const auto path = directory.path() / "source.safetensors";
 fixtures::write_text_file(path, "synthetic source has no official provenance");
 rf::NativeRfDetrModel model(synthetic_config());
 const auto before = state_digests(model);
 CHECK_THROWS(rf::initialize_encoder_pretraining(model, path));
 std::filesystem::resize_file(path, rf::find_encoder_pretraining_asset(model.config().backbone.kind)->file_bytes);
 CHECK_THROWS(rf::initialize_encoder_pretraining(model, path));
 std::stop_source stop;
 stop.request_stop();
 CHECK_THROWS(rf::initialize_encoder_pretraining(model, path, stop.get_token()));
 CHECK(state_digests(model) == before);
}
TEST_CASE("PE encoder staging admits exactly the standalone classifier exclusions", "[rfdetr][encoder_pretraining]") {
 torch::NoGradGuard guard;
 auto config = rf::native_config_from_preset(*rf::find_preset_catalog_entry("rf-detr-atto"));
 auto original = config;
 original.patch_size = 16;
 original.positional_encoding_size = 24;
 auto source_encoder = rf::detail::make_backbone_encoder(original);
 std::vector<rf::NormalizedModelStateEntry> source;
 for (const auto& parameter : source_encoder->named_parameters(true)) source.push_back({parameter.key().substr(std::string_view("model.").size()), parameter.value().detach()});
 // Independently reconstruct the standard attention-pool/classifier linear
 // shapes from the original tiny model's 192-wide/512-output topology.
 const auto add_linear = [&](const std::string& name, int input, int output) {
  source.push_back({name + ".weight", torch::zeros({output, input})});
  source.push_back({name + ".bias", torch::zeros({output})});
 };
 add_linear("attn_pool.kv", 192, 384);
 add_linear("attn_pool.q", 192, 192);
 add_linear("attn_pool.proj", 192, 192);
 add_linear("attn_pool.mlp.fc1", 192, 768);
 add_linear("attn_pool.mlp.fc2", 768, 192);
 add_linear("head", 192, 512);
 source.push_back({"attn_pool.norm.weight", torch::ones({192})});
 source.push_back({"attn_pool.norm.bias", torch::zeros({192})});
 source.push_back({"attn_pool.latent", torch::zeros({1, 1, 192})});
 REQUIRE(source.size() == 166);
 rf::NativeRfDetrModel model(config);
 const auto before = state_digests(model);
 SECTION("complete source") {
  auto candidate = model.stage_encoder_pretraining(source);
  REQUIRE(candidate.summary.loaded_names.size() == 151);
  model.commit_normalized_state(std::move(candidate));
  for (const auto& [name, digest] : state_digests(model))
   if (!name.starts_with("backbone.0.encoder.")) CHECK(before.at(name) == digest);
  return;
 }
 SECTION("missing excluded tensor") { source.pop_back(); }
 SECTION("wrong excluded shape") { source.back().tensor = torch::zeros({1, 2, 192}); }
 SECTION("wrong excluded dtype") { source.back().tensor = source.back().tensor.to(torch::kFloat16); }
 SECTION("nonfinite excluded value") { source.back().tensor.fill_(std::numeric_limits<float>::infinity()); }
 SECTION("unexpected pool state") { source.push_back({"attn_pool.extra", torch::zeros({1})}); }
 CHECK_THROWS(model.stage_encoder_pretraining(source));
 CHECK(state_digests(model) == before);
}
TEST_CASE("Encoder initialization excludes both projector owners", "[rfdetr][encoder_pretraining]") {
 torch::NoGradGuard guard;
 auto config = synthetic_config();
 config.projector = rf::ProjectorKind::DualKeypointOnly;
 const auto source = synthetic_source(config);
 rf::detail::NativeBackboneImpl backbone(config);
 std::map<std::string, std::string> before;
 for (const auto& parameter : backbone.named_parameters(true))
  if (!parameter.key().starts_with("encoder.")) before.emplace(parameter.key(), tensor_digest(parameter.value()));
 REQUIRE(std::ranges::any_of(before, [](const auto& entry) { return entry.first.starts_with("cross_attn_projector."); }));
 auto candidate = backbone.stage_encoder_pretraining(config, source);
 for (std::size_t index = 0; index < candidate.tensors.size(); ++index) candidate.destinations[index].copy_(candidate.tensors[index]);
 for (const auto& parameter : backbone.named_parameters(true))
  if (!parameter.key().starts_with("encoder.")) CHECK(before.at(parameter.key()) == tensor_digest(parameter.value()));
}
TEST_CASE("Encoder resampling preserves identity and prefix with a pseudoinverse patch policy", "[rfdetr][encoder_pretraining]") {
 const auto kernel = torch::arange(3 * 14 * 14, torch::kFloat32).reshape({1, 3, 14, 14}) / 100.F;
 CHECK(torch::equal(rf::detail::resample_encoder_patch(kernel, 14), kernel));
 const auto positions = torch::arange(1 * (37 * 37 + 1) * 4, torch::kFloat32).reshape({1, 37 * 37 + 1, 4}) / 100.F;
 CHECK(torch::equal(rf::detail::resample_encoder_positions(positions, 37), positions));
 for (const int size : {12, 16, 20}) {
  const auto result = rf::detail::resample_encoder_patch(kernel, size);
  REQUIRE(result.sizes().vec() == std::vector<std::int64_t>{1, 3, size, size});
  CHECK(torch::isfinite(result).all().item<bool>());
  const auto interpolated =
   torch::nn::functional::interpolate(kernel, torch::nn::functional::InterpolateFuncOptions().size(std::vector<std::int64_t>{size, size}).mode(torch::kBicubic).align_corners(false).antialias(true));
  CHECK_FALSE(torch::allclose(result, interpolated));
  if (size > 14) {
   // The defining resampling invariant: a resized image patch keeps its
   // original convolution response under the inverse-resized kernel.
   const auto pixels = torch::sin(torch::arange(3 * 14 * 14, torch::kFloat32)).reshape({1, 3, 14, 14});
   const auto resized =
    torch::nn::functional::interpolate(pixels, torch::nn::functional::InterpolateFuncOptions().size(std::vector<std::int64_t>{size, size}).mode(torch::kBicubic).align_corners(false).antialias(true));
   CHECK(torch::allclose((result * resized).sum(), (kernel * pixels).sum(), 1.e-4, 1.e-3));
  }
  CHECK(torch::equal(rf::detail::resample_encoder_positions(positions, size).narrow(1, 0, 1), positions.narrow(1, 0, 1)));
 }
 CHECK(rf::detail::resample_encoder_patch(torch::ones({1, 3, 16, 16}), 20).size(2) == 20);
}
TEST_CASE("Encoder evidence selection distinguishes absent and invalid explicit paths", "[rfdetr][encoder_pretraining]") {
 fixtures::ScopedTempDir directory("encoder-metadata");
 CHECK_FALSE(encoder_evidence_directory(nullptr).has_value());
 CHECK_THROWS(encoder_evidence_directory(""));
 CHECK_THROWS(encoder_evidence_directory((directory.path() / "missing").c_str()));
 CHECK_THROWS(encoder_evidence_directory(directory.path().c_str()));
 const auto path = directory.path() / "manifest.json";
 fixtures::write_text_file(path, "{}");
 CHECK_THROWS(encoder_evidence_directory(path.c_str()));
 CHECK(encoder_evidence_directory(directory.path().c_str()) == std::filesystem::canonical(directory.path()));
 CHECK_THROWS(admit_encoder_manifest(bounded_json(path), evidence_repository()));
 fixtures::write_text_file(path, R"({"fingerprint":"a","fingerprint":"b"})");
 CHECK_THROWS(bounded_json(path));
 fixtures::write_text_file(path, std::string(17, '[') + "0" + std::string(17, ']'));
 CHECK_THROWS(bounded_json(path));
 std::filesystem::resize_file(path, 32U * 1024U * 1024U + 1U);
 CHECK_THROWS(bounded_json(path));
}
TEST_CASE("Encoder manifest admission binds provenance and controlled policy before tensors", "[rfdetr][encoder_pretraining]") {
 const auto repository = evidence_repository();
 const auto payload = bounded_json(repository / "docker/nvidia-payload.json");
 const auto cuda = payload.at("versions").at("cuda").get<std::string>();
 const std::string identity(64, 'a');
 // This descriptor has no tensor payloads and cannot initialize a model or
 // certify numerical agreement. It exercises only the real metadata boundary.
 Json complete{
  {"format", "mmltk.rfdetr.encoder-pretraining.v1"}, {"mode", "encoder-pretraining"}, {"status", "encoder_references_complete_native_comparison_pending"},
  {"sources", bounded_json(repository / "tools/oracle/rfdetr_sources.json")}, {"native_catalog", Json::parse(rfdetr_native_catalog_json())}, {"fingerprint", identity}, {"producer_sha256", identity},
  {"dependency_sha256", file_digest(repository / "tools/oracle/rfdetr_oracle_requirements.txt")}, {"source_manifest_sha256", file_digest(repository / "tools/oracle/rfdetr_sources.json")},
  {"source_hashes", {{"core", {{"README.md", identity}}}, {"plus", {{"README.md", identity}}}}}, {"operator_sources", {{"patch_embed", identity}, {"pos_embed", identity}}},
  {"environment", {{"torch", payload.at("donor_metadata").at("torch_runtime_version")}, {"cuda", cuda.substr(0, cuda.rfind('.'))}, {"torchvision", "metadata-fixture"},
                   {"distributions", {{"torch", payload.at("donor_metadata").at("torch_distribution_version")}, {"torchvision", "metadata-fixture"}, {"triton", "metadata-fixture"}}},
                   {"dependency_closure", pinned_dependencies(repository)}}},
  {"precision_capability", {{"fp32", true}}}, {"cases", std::vector<Json>(rf::kPresetCatalog.size())}, {"assets", std::vector<Json>(rf::kEncoderPretrainingAssets.size())}
 };
 complete.update(encoder_reference_policy());
 CHECK_NOTHROW(admit_encoder_manifest(complete, repository));
 for (const auto* key : {"fingerprint", "producer_sha256", "dependency_sha256", "source_manifest_sha256", "source_hashes", "operator_sources", "environment", "transformations", "execution_policy"}) {
  INFO(key);
  auto stripped = complete;
  stripped.erase(key);
  CHECK_THROWS(admit_encoder_manifest(stripped, repository));
 }
 const std::vector<std::pair<std::string, Json>> incompatible{
  {"/fingerprint", std::string(63, 'a')}, {"/producer_sha256", std::string(64, 'A')}, {"/producer_sha256", std::string(64, 'g')}, {"/fingerprint", 1}, {"/dependency_sha256", identity},
  {"/source_manifest_sha256", identity}, {"/source_hashes/core", Json::object()}, {"/source_hashes/plus", Json::array()}, {"/source_hashes/core/README.md", "invalid"},
  {"/source_hashes/plus/..~1escape", identity}, {"/operator_sources/patch_embed", "invalid"}, {"/operator_sources/pos_embed", nullptr}, {"/environment/torch", "0.0-incompatible"},
  {"/environment/cuda", "0.0"}, {"/environment/distributions/torch", "0.0"}, {"/environment/torchvision", "contradiction"}, {"/environment/dependency_closure/transformers", "0.0"},
  {"/environment/dependency_closure/timm", "0.0"}, {"/transformations/patch", "bilinear"}, {"/transformations/positions", "prefix interpolated"},
  {"/transformations/intermediate_dtype", "torch.float64"}, {"/transformations/pinv_atol", 0.0}, {"/transformations/pinv_rtol", 1.e-5}, {"/transformations/runtime_grid", "different"},
  {"/execution_policy/sdpa_backend", "default"}, {"/execution_policy/matmul_tf32", true}, {"/execution_policy/cudnn_tf32", true}, {"/execution_policy/matmul_precision", "high"},
  {"/execution_policy/flash_sdp", true}, {"/execution_policy/math_sdp", false}, {"/execution_policy/memory_efficient_sdp", true}, {"/execution_policy/cudnn_sdp", true},
  {"/execution_policy/grad_enabled", true}, {"/numerical_policy/patch_atol", 1.e-3}, {"/numerical_policy/patch_rtol", 1.e-3}, {"/numerical_policy/feature_atol", 1.e-2},
  {"/numerical_policy/feature_rtol", 1.e-2}
 };
 for (const auto& [pointer, value] : incompatible) {
  INFO(pointer);
  auto changed = complete;
  changed[Json::json_pointer(pointer)] = value;
  CHECK_THROWS(admit_encoder_manifest(changed, repository));
 }
 for (const auto* kind : {"core", "plus"}) {
  auto oversized = complete;
  auto& hashes = oversized["source_hashes"][kind];
  for (int index = 0; index < 5000; ++index) hashes[std::to_string(index)] = identity;
  CHECK_THROWS(admit_encoder_manifest(oversized, repository));
 }
}
TEST_CASE("Official encoder initialization agrees with independent pinned sources and FP32 controls", "[rfdetr][encoder_pretraining_oracle]") {
 const auto requested = encoder_evidence_directory(std::getenv("MMLTK_RFDETR_ENCODER_PRETRAINING_DIR"));
 if (!requested) SKIP("Explicit encoder evidence is absent; prepare --encoder-pretraining-only or supply MMLTK_RFDETR_ENCODER_PRETRAINING_DIR");
 const auto& root = *requested;
 const auto manifest = bounded_json(root / "manifest.json");
 admit_encoder_manifest(manifest, evidence_repository());
 const auto native_catalog = Json::parse(rfdetr_native_catalog_json());
 Json receipt{
  {"format", "mmltk.rfdetr.encoder-pretraining-comparison.v1"}, {"status", "incomplete"}, {"scope", "encoder initialization only"},
  {"manifest_sha256", io::sha256_hex(io::sha256_file(root / "manifest.json"))}, {"cases", Json::array()}, {"assets", Json::array()}
 };
 const auto write_receipt = [&] {
  fixtures::write_text_file(root / "encoder-comparison.json.partial", receipt.dump(2));
  std::filesystem::rename(root / "encoder-comparison.json.partial", root / "encoder-comparison.json");
 };
 write_receipt();
 torch::NoGradGuard guard;
 std::set<std::string> covered;
 for (const auto& asset : rf::kEncoderPretrainingAssets) {
  const auto kind = std::string(mmltk::frameworks::reflection::enum_name(asset.backbone));
  const auto found = std::ranges::find_if(manifest.at("assets"), [&](const auto& value) { return value.at("backbone") == kind; });
  REQUIRE(found != manifest.at("assets").end());
  auto source_facts = *found;
  source_facts.erase("path");
  source_facts.erase("tensors");
  const auto native_asset = std::ranges::find_if(native_catalog.at("encoder_pretraining"), [&](const auto& value) { return value.at("backbone") == kind; });
  REQUIRE(native_asset != native_catalog.at("encoder_pretraining").end());
  REQUIRE(source_facts == *native_asset);
  const auto path = evidence_file(root, found->at("path").template get<std::string>());
  REQUIRE(std::filesystem::file_size(path) == asset.file_bytes);
  REQUIRE(io::sha256_hex(io::sha256_file(path)) == asset.sha256);
  const auto mapped = io::MappedFile::open_readonly(path.string());
  const auto source = rf::detail::decode_encoder_safetensors({mapped.data(), mapped.size()}, asset.file_bytes);
  REQUIRE(found->at("tensors").size() == source.size());
  for (const auto& tensor : source) {
   const auto& record = found->at("tensors").at(tensor.name);
   REQUIRE(record.at("shape") == tensor.tensor.sizes().vec());
   REQUIRE(record.at("dtype") == "torch.float32");
   REQUIRE(record.at("sha256") == tensor_digest(tensor.tensor));
  }
  bool admitted_model = false;
  for (const auto& preset : rf::kPresetCatalog) {
   if (preset.backbone.kind != asset.backbone) continue;
   INFO(preset.preset_name);
   const auto case_record = std::ranges::find_if(manifest.at("cases"), [&](const auto& value) { return value.at("preset") == preset.preset_name; });
   REQUIRE(case_record != manifest.at("cases").end());
   REQUIRE(covered.insert(std::string(preset.preset_name)).second);
   auto config = rf::native_config_from_preset(preset);
   config.group_detr = 1;
   REQUIRE(case_record->at("configuration") == Json::parse(rfdetr_native_configuration_json(preset.preset_name, 1)));
   REQUIRE(case_record->at("source_sha256") == asset.sha256);
   REQUIRE(case_record->at("adaptation_revision") == asset.adaptation_revision);
   const auto reference_path = evidence_file(root, "references/" + case_record->at("reference").at("file").template get<std::string>());
   REQUIRE(std::filesystem::file_size(reference_path) == case_record->at("reference").at("bytes"));
   REQUIRE(io::sha256_hex(io::sha256_file(reference_path)) == case_record->at("reference").at("sha256").get_ref<const std::string&>());
   const auto reference_map = io::MappedFile::open_readonly(reference_path.string());
   const auto tensors = rf::detail::decode_encoder_safetensors({reference_map.data(), reference_map.size()}, 64U * 1024U * 1024U);
   std::map<std::string, torch::Tensor> reference;
   for (const auto& entry : tensors) reference.emplace(entry.name, entry.tensor);
   const std::string prefix = asset.backbone == rf::BackboneKind::PeCoreTiny ? "model." : "encoder.";
   const std::string patch = prefix + (asset.backbone == rf::BackboneKind::PeCoreTiny ? "patch_embed.proj.weight" : "embeddings.patch_embeddings.projection.weight");
   const std::string positions = prefix + (asset.backbone == rf::BackboneKind::PeCoreTiny ? "pos_embed" : "embeddings.position_embeddings");
   const auto transformed = case_record->at("transformed_state").template get<std::vector<std::string>>();
   std::set<std::string> expected_transforms;
   if (config.patch_size != asset.geometry.patch_size) expected_transforms.insert(patch);
   if (config.positional_encoding_size != asset.geometry.position_grid) expected_transforms.insert(positions);
   REQUIRE(transformed.size() == expected_transforms.size());
   REQUIRE(std::set<std::string>(transformed.begin(), transformed.end()) == expected_transforms);
   std::set<std::string> expected_references{"input", "control/patch_fp64", "state/" + patch, "state/" + positions};
   for (std::size_t index = 0; index < config.backbone.features.size(); ++index) {
    expected_references.insert("feature/" + std::to_string(index));
    expected_references.insert("control/cuda_feature/" + std::to_string(index));
   }
   REQUIRE(reference.size() == expected_references.size());
   for (const auto& name : expected_references) REQUIRE(reference.contains(name));
   const std::int64_t side = config.patch_size * config.num_windows * 4;
   REQUIRE(case_record->at("runtime_side") == side);
   REQUIRE(reference.at("input").sizes().vec() == std::vector<std::int64_t>{1, 3, side, side});
   auto encoder = rf::detail::make_backbone_encoder(config);
   encoder->eval();
   encoder->set_force_math_attention(true);
   auto staged = rf::detail::stage_encoder_pretraining(*encoder, config, source);
   REQUIRE(staged.summary.loaded_names.size() == (asset.backbone == rf::BackboneKind::PeCoreTiny ? 151U : 223U));
   Json errors = Json::object();
   REQUIRE(case_record->at("exact_state").size() + case_record->at("transformed_state").size() == staged.summary.loaded_names.size());
   for (std::size_t index = 0; index < staged.tensors.size(); ++index) {
    const auto& name = staged.summary.loaded_names[index];
    const auto& value = staged.tensors[index];
    if (name == positions) {
     const auto original = std::ranges::find(source, positions.substr(prefix.size()), &rf::NormalizedModelStateEntry::name);
     REQUIRE(original != source.end());
     REQUIRE(torch::equal(value.narrow(1, 0, 1), original->tensor.narrow(1, 0, 1)));
    }
    if (case_record->at("exact_state").contains(name)) {
     REQUIRE(case_record->at("exact_state").at(name).at("sha256") == tensor_digest(value));
     REQUIRE(case_record->at("exact_state").at(name).at("shape") == value.sizes().vec());
    } else
     errors[name] = compare(value, reference.at("state/" + name), 3.e-5, 3.e-4);
    staged.destinations[index].copy_(value);
   }
   const auto features = encoder->forward(reference.at("input"));
   REQUIRE(features.size() == config.backbone.features.size());
   for (std::size_t index = 0; index < features.size(); ++index) {
    const auto name = std::to_string(index);
    errors["feature/" + name] = compare(features[index], reference.at("feature/" + name), 5.e-4, 5.e-4);
    errors["cuda_control/" + name] = compare(reference.at("feature/" + name), reference.at("control/cuda_feature/" + name), 5.e-4, 5.e-4);
   }
   errors["patch_fp64_control"] = compare(reference.at("state/" + patch), reference.at("control/patch_fp64"), 3.e-5, 3.e-4);
   if (!admitted_model) {
    rf::NativeRfDetrModel model(config);
    const auto before = state_digests(model);
    const auto result = rf::initialize_encoder_pretraining(model, path);
    REQUIRE(result.provenance == rf::encoder_pretraining_provenance(asset));
    REQUIRE(result.summary.loaded_names.size() == staged.summary.loaded_names.size());
    const auto parameters = encoder->named_parameters(true), buffers = encoder->named_buffers(true);
    for (const auto& [name, digest] : state_digests(model)) {
     if (!name.starts_with("backbone.0.encoder."))
      REQUIRE(before.at(name) == digest);
     else {
      const auto relative = name.substr(std::string_view("backbone.0.encoder.").size());
      const auto* value = parameters.find(relative);
      if (!value) value = buffers.find(relative);
      REQUIRE(value != nullptr);
      REQUIRE(digest == tensor_digest(*value));
     }
    }
    admitted_model = true;
   }
   receipt["cases"].push_back({{"preset", preset.preset_name}, {"source_sha256", asset.sha256}, {"loaded", staged.summary.loaded_names.size()}, {"errors", errors}});
  }
  REQUIRE(admitted_model);
  receipt["assets"].push_back({{"asset_id", asset.source_repository}, {"sha256", asset.sha256}, {"source_tensors", source.size()}});
 }
 REQUIRE(covered.size() == 16U);
 receipt["status"] = "passed_encoder_initialization";
 write_receipt();
}
