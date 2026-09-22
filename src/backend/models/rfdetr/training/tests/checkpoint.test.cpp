#include "src/backend/ml/torch/tests/catch_support.h"
#include <torch/cuda.h>
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
#include "src/backend/models/rfdetr/core/model_info.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include <ATen/CPUGeneratorImpl.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <meta>
#include <type_traits>
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/checkpoint_fixture_support.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "detail/checkpoint_private.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "model_state_fixture.h"
#include "parity_fixture_support.h"
#include "src/common/io/file_memory.h"
#include <torch/types.h>
#include <torch/serialize.h>
import mmltk.backend.models.rfdetr.model_export;
namespace fs = std::filesystem;
namespace {
using namespace mmltk::backend::models::rfdetr::testsupport;
auto& state_entries(mmltk::backend::models::rfdetr::DecodedNativeModelState& state) { return state.entries(); }
const auto& state_entries(const mmltk::backend::models::rfdetr::DecodedNativeModelState& state) { return state.entries(); }
void populate_detection_metadata(mmltk::backend::models::rfdetr::NativeCheckpointMetadata& metadata) {
 std::size_t index = 0;
 metadata.for_each_detection_field([&]<class Optional>(const char*, Optional& field) {
  using Value = typename Optional::value_type;
  if constexpr (std::is_same_v<Value, bool>)
   field = (index % 2) != 0;
  else if constexpr (std::is_integral_v<Value>)
   field = 4;
  else
   field = 0.25;
  ++index;
 });
 REQUIRE(index == 15);
}
template <class Metadata>
void require_detection_metadata_equal(const Metadata& expected, const Metadata& actual) {
 template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^Metadata, std::meta::access_context::current()))) {
  using Field = std::remove_cvref_t<decltype(expected.[:member:])>;
  if constexpr (mmltk::backend::models::rfdetr::model_state_detail::is_optional<Field>) {
   INFO(std::define_static_string(std::meta::identifier_of(member)));
   REQUIRE(expected.[:member:] == actual.[:member:]);
  }
 }
}
fs::path fixture_root() { return fs::temp_directory_path() / "mmltk_rfdetr_checkpoint_fixture"; }
fs::path upstream_fixture_path(const ParityFixtureCase& fixture) { return fixture_root() / "upstream" / fixture.upstream_filename; }
fs::path native_roundtrip_path(const ParityFixtureCase& fixture) { return fixture_root() / "native" / (std::string(fixture.preset_name) + ".native.pt"); }
fs::path native_golden_fixture_path(const ParityFixtureCase& fixture) { return fixture_root() / "golden" / (std::string(fixture.preset_name) + ".golden.native.pt"); }
const mmltk::backend::models::rfdetr::NormalizedModelStateEntry* find_entry(const mmltk::backend::models::rfdetr::DecodedNativeModelState& checkpoint, const char* name) {
 for (const auto& entry : state_entries(checkpoint)) {
  if (entry.name == name) { return &entry; }
 }
 return nullptr;
}
void write_archive_string(torch::serialize::OutputArchive& archive, const char* key, std::string_view value) { archive.write(key, c10::IValue(std::string(value))); }
void write_archive_int(torch::serialize::OutputArchive& archive, const char* key, int64_t value) { archive.write(key, c10::IValue(value)); }
void write_legacy_native_checkpoint(const fs::path& output_path, int version = 1, std::string_view format = "fastloader.rfdetr.native_checkpoint") {
 fs::create_directories(output_path.parent_path());
 torch::serialize::OutputArchive archive;
 write_archive_string(archive, "format", format);
 write_archive_int(archive, "format_version", version);
 write_archive_string(archive, "preset_name", "rf-detr-seg-medium");
 write_archive_string(archive, "source_kind", "legacy-native-test");
 write_archive_string(archive, "source_path", output_path.string());
 write_archive_int(archive, "num_classes", 7);
 torch::serialize::OutputArchive state_archive;
 write_archive_int(state_archive, "entry_count", 1);
 torch::serialize::OutputArchive entry_archive;
 write_archive_string(entry_archive, "name", "class_embed.bias");
 entry_archive.write("tensor", torch::arange(7, torch::TensorOptions().dtype(torch::kFloat32)).contiguous());
 state_archive.write("entry_000000", entry_archive);
 archive.write("state", state_archive);
 archive.save_to(output_path.string());
}
void test_upstream_checkpoint_parse(const ParityFixtureCase& fixture, const fs::path& upstream_path) {
 const auto checkpoint = mmltk::backend::models::rfdetr::decode_model_state(upstream_path);
 REQUIRE((checkpoint.metadata.source_kind == "upstream-python"));
 REQUIRE((checkpoint.metadata.preset_name == fixture.preset_name));
 REQUIRE((checkpoint.metadata.num_classes == kParityFixtureNumClasses));
 REQUIRE((state_entries(checkpoint).size() == 4));
 bool found_query_feat = false;
 bool found_cls_bias = false;
 bool found_refpoint = false;
 for (const auto& entry : state_entries(checkpoint)) {
  if (entry.name == "query_feat.weight") {
   found_query_feat = true;
   REQUIRE((entry.tensor.dim() == 2));
   REQUIRE((entry.tensor.size(0) == fixture.query_rows));
   REQUIRE((entry.tensor.size(1) == kParityFixtureHiddenDim));
  }
  if (entry.name == "refpoint_embed.weight") {
   found_refpoint = true;
   REQUIRE((entry.tensor.dim() == 2));
   REQUIRE((entry.tensor.size(0) == fixture.query_rows));
   REQUIRE((entry.tensor.size(1) == 4));
  }
  if (entry.name == "class_embed.bias") {
   found_cls_bias = true;
   REQUIRE((entry.tensor.dim() == 1));
   REQUIRE((entry.tensor.size(0) == kParityFixtureNumClasses));
   REQUIRE((entry.tensor.index({0}).item<float>() == make_fixture_class_bias(fixture).index({0}).item<float>()));
  }
 }
 REQUIRE((found_query_feat));
 REQUIRE((found_cls_bias));
 REQUIRE((found_refpoint));
}
void test_native_checkpoint_roundtrip(const ParityFixtureCase& fixture, const fs::path& upstream_path) {
 const auto upstream = mmltk::backend::models::rfdetr::decode_model_state(upstream_path);
 const fs::path output_path = native_roundtrip_path(fixture);
 fs::create_directories(output_path.parent_path());
 const auto normalized = mmltk::backend::models::rfdetr::normalize_checkpoint_to_native(upstream_path, output_path);
 const bool output_exists = fs::exists(output_path);
 REQUIRE((output_exists));
 REQUIRE((normalized.metadata.preset_name == fixture.preset_name));
 REQUIRE((mmltk::backend::models::rfdetr::is_native_checkpoint_file(output_path)));
 const auto native = mmltk::backend::models::rfdetr::decode_model_state(output_path);
 REQUIRE(native.class_artifact);
 CHECK(native.class_artifact->Matches(output_path));
 CHECK(native.class_artifact->Resolve(native.metadata.num_classes, native.metadata.class_layout) == native.metadata.class_layout);
 REQUIRE((native.metadata.preset_name == upstream.metadata.preset_name));
 REQUIRE((native.metadata.source_kind == "upstream-python"));
 REQUIRE((state_entries(native).size() == state_entries(upstream).size()));
 bool compared_tensor = false;
 for (size_t index = 0; index < state_entries(native).size(); ++index) {
  REQUIRE((state_entries(native)[index].name == state_entries(upstream)[index].name));
  if (!compared_tensor && state_entries(native)[index].name == "class_embed.weight") {
   REQUIRE((torch::equal(state_entries(native)[index].tensor, state_entries(upstream)[index].tensor)));
   compared_tensor = true;
  }
 }
 REQUIRE((compared_tensor));
}
void test_native_golden_fixture_roundtrip(const ParityFixtureCase& fixture) {
 const fs::path output_path = native_golden_fixture_path(fixture);
 fs::create_directories(output_path.parent_path());
 const auto expected = make_native_parity_fixture(fixture);
 mmltk::backend::models::rfdetr::save_native_checkpoint(output_path, expected);
 const bool output_exists = fs::exists(output_path);
 REQUIRE((output_exists));
 REQUIRE((mmltk::backend::models::rfdetr::is_native_checkpoint_file(output_path)));
 const auto loaded = mmltk::backend::models::rfdetr::decode_model_state(output_path);
 require_detection_metadata_equal(expected.metadata, loaded.metadata);
 assert_matches_native_parity_fixture(loaded, fixture);
}
void test_native_checkpoint_tensor_preparation() {
 const fs::path output_path = fixture_root() / "native" / "tensor-prep.native.pt";
 fs::create_directories(output_path.parent_path());
 mmltk::backend::models::rfdetr::DecodedNativeModelState checkpoint;
 checkpoint.metadata.preset_name = "tensor-prep";
 checkpoint.metadata.source_kind = "unit-test";
 checkpoint.metadata.source_path = output_path.string();
 checkpoint.metadata.num_classes = 1;
 checkpoint.metadata.class_layout = mmltk::backend::models::rfdetr::unresolved_class_layout(1);
 checkpoint.metadata.num_queries = 1;
 checkpoint.metadata.num_select = 1;
 populate_detection_metadata(checkpoint.metadata);
 const auto cpu_contiguous = torch::arange(12, torch::TensorOptions().dtype(torch::kFloat32)).view({3, 4}).clone();
 const auto cpu_non_contiguous = cpu_contiguous.transpose(0, 1);
 std::vector<mmltk::backend::models::rfdetr::NormalizedModelStateEntry> synthetic_entries;
 synthetic_entries.push_back({"cpu_contiguous", cpu_contiguous});
 synthetic_entries.push_back({"cpu_non_contiguous", cpu_non_contiguous});
 const bool has_cuda = torch::cuda::is_available();
 if (has_cuda) { synthetic_entries.push_back({"cuda_tensor", torch::arange(6, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA)).view({2, 3})}); }
 set_synthetic_model_state(checkpoint, std::move(synthetic_entries));
 mmltk::backend::models::rfdetr::save_native_checkpoint(output_path, checkpoint);
 const auto loaded = mmltk::backend::models::rfdetr::decode_model_state(output_path);
 require_detection_metadata_equal(checkpoint.metadata, loaded.metadata);
 const auto* loaded_contiguous = find_entry(loaded, "cpu_contiguous");
 REQUIRE((loaded_contiguous != nullptr));
 REQUIRE((loaded_contiguous->tensor.device().is_cpu()));
 REQUIRE((loaded_contiguous->tensor.is_contiguous()));
 REQUIRE((torch::equal(loaded_contiguous->tensor, cpu_contiguous)));
 const auto* loaded_non_contiguous = find_entry(loaded, "cpu_non_contiguous");
 REQUIRE((loaded_non_contiguous != nullptr));
 REQUIRE((loaded_non_contiguous->tensor.device().is_cpu()));
 REQUIRE((loaded_non_contiguous->tensor.is_contiguous()));
 REQUIRE((torch::equal(loaded_non_contiguous->tensor, cpu_non_contiguous.contiguous())));
 const auto* loaded_cuda = find_entry(loaded, "cuda_tensor");
 REQUIRE(((loaded_cuda != nullptr) == has_cuda));
 if (loaded_cuda != nullptr) {
  const auto expected = state_entries(checkpoint).back().tensor.detach().to(torch::Device(torch::kCPU)).contiguous();
  REQUIRE((loaded_cuda->tensor.device().is_cpu()));
  REQUIRE((loaded_cuda->tensor.is_contiguous()));
  REQUIRE((torch::equal(loaded_cuda->tensor, expected)));
 }
 checkpoint.metadata.for_each_detection_field([](const char*, auto& field) { field.reset(); });
 mmltk::backend::models::rfdetr::save_native_checkpoint(output_path, checkpoint);
 require_detection_metadata_equal(checkpoint.metadata, mmltk::backend::models::rfdetr::decode_model_state(output_path).metadata);
}
void test_upstream_checkpoint_scalar_type_bridge() {
 const fs::path upstream_path = fixture_root() / "upstream" / "rf-detr-nano-dtype-bridge.pth";
 mmltk::backend::models::rfdetr::DecodedNativeModelState state;
 populate_detection_metadata(state.metadata);
 std::vector<mmltk::backend::models::rfdetr::NormalizedModelStateEntry> entries;
 entries = {
  {"query_feat.weight", torch::ones({4, kParityFixtureHiddenDim}, torch::TensorOptions().dtype(torch::kFloat16))},
  {"refpoint_embed.weight", torch::zeros({4, 4}, torch::TensorOptions().dtype(torch::kFloat32))},
  {"class_embed.weight", torch::ones({kParityFixtureNumClasses, kParityFixtureHiddenDim}, torch::TensorOptions().dtype(torch::kFloat32))},
  {"class_embed.bias", torch::arange(kParityFixtureNumClasses, torch::TensorOptions().dtype(torch::kInt64))},
 };
 set_synthetic_model_state(state, entries);
 mmltk::backend::models::rfdetr::write_upstream_model_state(upstream_path, state);
 const auto checkpoint = mmltk::backend::models::rfdetr::decode_model_state(upstream_path);
 require_detection_metadata_equal(state.metadata, checkpoint.metadata);
 const auto* query_feat = find_entry(checkpoint, "query_feat.weight");
 const auto* class_bias = find_entry(checkpoint, "class_embed.bias");
 REQUIRE((query_feat != nullptr));
 REQUIRE((class_bias != nullptr));
 REQUIRE((query_feat->tensor.scalar_type() == torch::kFloat16));
 REQUIRE((class_bias->tensor.scalar_type() == torch::kInt64));
}
void test_cuda_upstream_raw_state_preserves_logical_values() {
 if (!torch::cuda::is_available()) { SKIP("CUDA unavailable"); }
 mmltk::testsupport::ScopedTempDir temp{"mmltk-upstream-cuda-readback"};
 mmltk::backend::models::rfdetr::DecodedNativeModelState state;
 std::vector<mmltk::backend::models::rfdetr::NormalizedModelStateEntry> entries;
 entries = {
  {"query_feat.weight", torch::ones({4, kParityFixtureHiddenDim}, torch::TensorOptions().dtype(torch::kFloat16).device(torch::kCUDA))},
  {"refpoint_embed.weight", torch::zeros({4, 4}, torch::TensorOptions().device(torch::kCUDA))},
  {"class_embed.weight", torch::ones({kParityFixtureNumClasses, kParityFixtureHiddenDim}, torch::TensorOptions().device(torch::kCUDA))},
  {"class_embed.bias", torch::arange(kParityFixtureNumClasses, torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA))},
  {"extra_view", torch::arange(12, torch::TensorOptions().dtype(torch::kBFloat16).device(torch::kCUDA)).view({3, 4}).transpose(0, 1)},
  {"extra_empty", torch::empty({0}, torch::TensorOptions().dtype(torch::kBool))},
 };
 std::vector<const void*> pointers;
 for (const auto& entry : entries) pointers.push_back(entry.tensor.const_data_ptr());
 const auto path = temp.path() / "rf-detr-nano-readback.pth";
 set_synthetic_model_state(state, entries);
 mmltk::backend::models::rfdetr::write_upstream_model_state(path, state);
 const auto loaded = mmltk::backend::models::rfdetr::decode_model_state(path);
 for (std::size_t index = 0; index < entries.size(); ++index) {
  const auto& expected = entries[index];
  const auto* actual = find_entry(loaded, expected.name.c_str());
  REQUIRE(actual != nullptr);
  REQUIRE(actual->tensor.scalar_type() == expected.tensor.scalar_type());
  REQUIRE(actual->tensor.sizes() == expected.tensor.sizes());
  REQUIRE(torch::equal(actual->tensor, expected.tensor.cpu()));
  REQUIRE(expected.tensor.const_data_ptr() == pointers[index]);
 }
}
void test_legacy_native_checkpoint_format_support() {
 const fs::path legacy_path = fixture_root() / "legacy" / "legacy-format.pt";
 for (const int version : {1, 2}) {
  for (const auto format : {"fastloader.rfdetr.native_checkpoint", mmltk::backend::models::rfdetr::kNativeCheckpointFormat}) {
   write_legacy_native_checkpoint(legacy_path, version, format);
   REQUIRE(mmltk::backend::models::rfdetr::is_native_checkpoint_file(legacy_path));
   REQUIRE_THROWS(mmltk::backend::models::rfdetr::decode_model_state(legacy_path));
   REQUIRE_THROWS(mmltk::backend::models::rfdetr::normalize_checkpoint_to_native(legacy_path, legacy_path.string() + ".normalized"));
  }
 }
}
auto round_trip_training_supervision_config(const fs::path& path, const mmltk::backend::models::rfdetr::TrainingSupervisionConfig& config) {
 torch::serialize::OutputArchive output;
 mmltk::backend::models::rfdetr::detail::write_training_supervision_config(output, config);
 output.save_to(path.string());
 torch::serialize::InputArchive input;
 input.load_from(path.string());
 return mmltk::backend::models::rfdetr::detail::read_training_supervision_config(input);
}
void test_training_supervision_checkpoint_blob_admission() {
 const fs::path path = fixture_root() / "native" / "supervision-config.pt";
 fs::create_directories(path.parent_path());
 mmltk::backend::models::rfdetr::TrainingSupervisionConfig config;
 config.assignment = mmltk::backend::models::rfdetr::TrainAssignmentKind::MatchFree;
 config.match_free.rho = 0.625F;
 config.denoising.enabled = true;
 REQUIRE((round_trip_training_supervision_config(path, config) == config));
 torch::serialize::InputArchive input;
 input.load_from(path.string());
 torch::Tensor encoded;
 input.read("training_supervision_config_cbor", encoded);
 torch::serialize::OutputArchive obsolete_key_output;
 obsolete_key_output.write("training_supervision_config", encoded);
 obsolete_key_output.save_to(path.string());
 torch::serialize::InputArchive obsolete_key_input;
 obsolete_key_input.load_from(path.string());
 REQUIRE(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(obsolete_key_input) == mmltk::backend::models::rfdetr::TrainingSupervisionConfig{});
 const std::array variants{
  [] {
   mmltk::backend::models::rfdetr::TrainingSupervisionConfig value;
   value.assignment = mmltk::backend::models::rfdetr::TrainAssignmentKind::MatchFree;
   return value;
  }(),
  [] {
   mmltk::backend::models::rfdetr::TrainingSupervisionConfig value;
   value.denoising.enabled = true;
   return value;
  }(),
  config,
 };
 for (const auto& variant : variants) { REQUIRE(round_trip_training_supervision_config(path, variant) == variant); }
 torch::serialize::OutputArchive trailing_output;
 trailing_output.write("training_supervision_config_cbor", torch::cat({encoded, torch::zeros({1}, torch::TensorOptions().dtype(torch::kUInt8))}));
 trailing_output.save_to(path.string());
 torch::serialize::InputArchive trailing_input;
 trailing_input.load_from(path.string());
 REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(trailing_input));
 torch::serialize::OutputArchive malformed_output;
 malformed_output.write("training_supervision_config_cbor", torch::zeros({4}));
 malformed_output.save_to(path.string());
 torch::serialize::InputArchive malformed_input;
 malformed_input.load_from(path.string());
 REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(malformed_input));
 torch::serialize::OutputArchive wrong_rank_output;
 wrong_rank_output.write("training_supervision_config_cbor", encoded.reshape({1, encoded.numel()}));
 wrong_rank_output.save_to(path.string());
 torch::serialize::InputArchive wrong_rank_input;
 wrong_rank_input.load_from(path.string());
 REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(wrong_rank_input));
 torch::serialize::OutputArchive truncated_output;
 truncated_output.write("training_supervision_config_cbor", encoded.narrow(0, 0, encoded.numel() - 1));
 truncated_output.save_to(path.string());
 torch::serialize::InputArchive truncated_input;
 truncated_input.load_from(path.string());
 REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(truncated_input));
 if (torch::cuda::is_available()) {
  torch::serialize::OutputArchive wrong_device_output;
  wrong_device_output.write("training_supervision_config_cbor", encoded.to(torch::Device(torch::kCUDA)));
  wrong_device_output.save_to(path.string());
  torch::serialize::InputArchive wrong_device_input;
  wrong_device_input.load_from(path.string());
  REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(wrong_device_input));
 }
 torch::serialize::OutputArchive oversized_output;
 oversized_output.write("training_supervision_config_cbor", torch::zeros({4096}, torch::TensorOptions().dtype(torch::kUInt8)));
 oversized_output.save_to(path.string());
 torch::serialize::InputArchive oversized_input;
 oversized_input.load_from(path.string());
 REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(oversized_input));
 const auto require_raw_cbor_rejected = [&](const std::span<const std::uint8_t> raw) {
  auto blob = torch::empty({static_cast<std::int64_t>(raw.size())}, torch::TensorOptions().dtype(torch::kUInt8));
  std::memcpy(blob.data_ptr<std::uint8_t>(), raw.data(), raw.size());
  torch::serialize::OutputArchive raw_output;
  raw_output.write("training_supervision_config_cbor", blob);
  raw_output.save_to(path.string());
  torch::serialize::InputArchive raw_input;
  raw_input.load_from(path.string());
  REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(raw_input));
 };
 constexpr std::array<std::uint8_t, 25> duplicate_assignment{
  0xa2U,
  0x6aU,
  'a',
  's',
  's',
  'i',
  'g',
  'n',
  'm',
  'e',
  'n',
  't',
  0U,
  // CLEANUP-IGNORE: The repeated CBOR key is the malformed input this fixture must reject.
  0x6aU,
  'a',
  's',
  's',
  'i',
  'g',
  'n',
  'm',
  'e',
  'n',
  't',
  0U,
 };
 constexpr std::array<std::uint8_t, 10> unknown_field{
  0xa1U,
  0x67U,
  'u',
  'n',
  'k',
  'n',
  'o',
  'w',
  'n',
  0U,
 };
 constexpr std::array<std::uint8_t, 1> missing_required_field{0xa0U};
 require_raw_cbor_rejected(duplicate_assignment);
 require_raw_cbor_rejected(unknown_field);
 require_raw_cbor_rejected(missing_required_field);
}
void test_strict_model_state_admission_is_duplicate_free_and_atomic() {
 namespace rfdetr = mmltk::backend::models::rfdetr;
 auto config = rfdetr::native_config_from_preset(rfdetr::model_presets().front());
 config.resolution = 64;
 config.segmentation = false;
 config.aux_loss = false;
 config.two_stage = false;
 config.dec_layers = 1;
 config.num_queries = 3;
 config.num_select = 3;
 config.group_detr = 1;
 config.training_supervision.assignment = rfdetr::TrainAssignmentKind::MatchFree;
 rfdetr::NativeRfDetrModel model(config, rfdetr::testsupport::synthetic_training_layout(config.num_classes - 1));
 auto& module = (model);
 auto state = clone_normalized_model_state(module);
 REQUIRE_FALSE(state.empty());
 const auto first_before = module.named_parameters(true).begin()->value().detach().clone();
 const auto require_rejected_without_mutation = [&](const std::vector<rfdetr::NormalizedModelStateEntry>& candidate) {
  REQUIRE_THROWS(model.load_normalized_state(candidate, true));
  REQUIRE(torch::equal(module.named_parameters(true).begin()->value(), first_before));
 };
 auto duplicate = state;
 duplicate.push_back(duplicate.front());
 require_rejected_without_mutation(duplicate);
 auto missing = state;
 missing.front().tensor = torch::full_like(missing.front().tensor, 9.0);
 missing.pop_back();
 require_rejected_without_mutation(missing);
 auto valid_candidate_state = state;
 valid_candidate_state.front().tensor = torch::full_like(valid_candidate_state.front().tensor, 9.0);
 auto candidate = model.stage_normalized_state(valid_candidate_state, rfdetr::detail::NormalizedModelStateAdmission::Exact);
 REQUIRE(torch::equal(module.named_parameters(true).begin()->value(), first_before));
 model.commit_normalized_state(std::move(candidate));
 REQUIRE(torch::equal(module.named_parameters(true).begin()->value(), torch::full_like(first_before, 9.0)));
 const auto class_head = std::ranges::find_if(state, [](const rfdetr::NormalizedModelStateEntry& entry) { return entry.name == "class_embed.weight"; });
 REQUIRE(class_head != state.end());
 REQUIRE(class_head->tensor.size(0) > 1);
 auto mismatched_class_head = state;
 auto& mismatched_entry = mismatched_class_head.at(static_cast<std::size_t>(std::distance(state.begin(), class_head)));
 mismatched_entry.tensor = mismatched_entry.tensor.narrow(0, 0, 1).clone();
 auto current_parameters = module.named_parameters(true);
 auto* class_head_destination = current_parameters.find(class_head->name);
 REQUIRE(class_head_destination != nullptr);
 const auto class_head_before = class_head_destination->detach().clone();
 REQUIRE_THROWS(model.stage_normalized_state(mismatched_class_head, rfdetr::detail::NormalizedModelStateAdmission::Exact));
 REQUIRE(torch::equal(*class_head_destination, class_head_before));
 const auto source_layout = rfdetr::ResolvedClassLayout(model.class_layout()->record());
 REQUIRE_THROWS(model.stage_normalized_state(mismatched_class_head, rfdetr::detail::NormalizedModelStateAdmission::FreshTransfer, &source_layout));
 REQUIRE(torch::equal(*class_head_destination, class_head_before));
}
}  // namespace
void test_checkpoint_roundtrip_and_fixture_loading() {
 mmltk::common::io::remove_path_recursively_best_effort(fixture_root());
 const auto& fixtures = parity_fixture_cases();
 for (size_t index = 0; index < fixtures.size(); ++index) {
  const auto& fixture = fixtures[index];
  log_fixture_phase("test_rfdetr_checkpoint", index + 1, fixtures.size(), "checkpoint", fixture.preset_name);
  const fs::path upstream_path = upstream_fixture_path(fixture);
  write_minimal_upstream_checkpoint(upstream_path, fixture);
  test_upstream_checkpoint_parse(fixture, upstream_path);
  test_native_checkpoint_roundtrip(fixture, upstream_path);
  test_native_golden_fixture_roundtrip(fixture);
 }
 mmltk::common::io::remove_path_recursively_best_effort(fixture_root());
}
void test_checkpoint_tensor_and_legacy_support() {
 test_native_checkpoint_tensor_preparation();
 test_upstream_checkpoint_scalar_type_bridge();
 test_legacy_native_checkpoint_format_support();
 test_training_supervision_checkpoint_blob_admission();
 test_strict_model_state_admission_is_duplicate_free_and_atomic();
}
TEST_CASE("test_checkpoint_roundtrip_and_fixture_loading", "[model][rfdetr][checkpoint]") { test_checkpoint_roundtrip_and_fixture_loading(); }
TEST_CASE("test_cuda_upstream_raw_state_preserves_logical_values", "[model][rfdetr][checkpoint][cuda]") { test_cuda_upstream_raw_state_preserves_logical_values(); }
TEST_CASE("test_checkpoint_tensor_and_legacy_support", "[model][rfdetr][checkpoint][training_supervision]") { test_checkpoint_tensor_and_legacy_support(); }
TEST_CASE("Fresh transfer maps actual classifier and supervision axes by class identity", "[model][rfdetr][checkpoint][layout]") {
 namespace r = mmltk::backend::models::rfdetr;
 namespace c = mmltk::backend::data::catalog;
 auto config = r::native_config_from_preset(r::model_presets().front());
 config.resolution = 64;
 config.num_classes = 4;
 config.num_queries = config.num_select = 3;
 config.group_detr = 1;
 config.dec_layers = 1;
 config.segmentation = false;
 config.training_supervision.assignment = r::TrainAssignmentKind::MatchFree;
 config.training_supervision.denoising.enabled = true;
 const r::ResolvedClassLayout source_layout(r::native_training_class_layout(c::ClassCatalog({"a", "b", "c"})));
 r::NativeRfDetrModel model(config, r::native_training_class_layout(c::ClassCatalog({"c", "a", "new"})));
 auto& owner = (model);
 owner.initialize_training_supervision(29);
 const auto before = r::testsupport::clone_normalized_model_state(owner);
 auto source = r::testsupport::clone_normalized_model_state(owner);
 // Independent expected axes cover both classifier owners and distinct
 // supervision coordinates, rather than consulting the production inventory.
 const std::array axes{std::pair{"class_embed.weight", 0}, std::pair{"class_embed.bias", 0}, std::pair{"transformer.enc_out_class_embed.0.weight", 0},
  std::pair{"transformer.enc_out_class_embed.0.bias", 0}, std::pair{"training_supervision.denoising_label_embedding.weight", 0}, std::pair{"training_supervision.ground_truth_mlp.linear1.weight", 1}};
 for (const auto& [name, dimension] : axes) {
  auto entry = std::ranges::find(source, name, &r::NormalizedModelStateEntry::name);
  REQUIRE(entry != source.end());
  for (std::int64_t row = 0; row < entry->tensor.size(dimension); ++row) entry->tensor.select(dimension, row).fill_(10. + static_cast<double>(row));
 }
 const auto transfer = [&](r::NativeRfDetrModel& destination) {
  const auto random_before = at::detail::getDefaultCPUGenerator().get_state();
  auto candidate = destination.stage_normalized_state(source, r::detail::NormalizedModelStateAdmission::FreshTransfer, &source_layout);
  destination.commit_normalized_state(std::move(candidate));
  CHECK(torch::equal(random_before, at::detail::getDefaultCPUGenerator().get_state()));
  return destination.named_parameters(true);
 };
 const auto after = transfer(owner);
 for (const auto& [name, dimension] : axes) {
  const auto* actual = after.find(name);
  REQUIRE(actual);
  const auto seeded = std::ranges::find(before, name, &r::NormalizedModelStateEntry::name);
  REQUIRE(seeded != before.end());
  CHECK(torch::equal(actual->select(dimension, 0), torch::full_like(actual->select(dimension, 0), 12.)));
  CHECK(torch::equal(actual->select(dimension, 1), torch::full_like(actual->select(dimension, 1), 10.)));
  CHECK(torch::equal(actual->select(dimension, 2), seeded->tensor.select(dimension, 2)));
  if (dimension == 1) {
   for (std::int64_t box = 3; box < 7; ++box) CHECK(torch::equal(actual->select(1, box), torch::full_like(actual->select(1, box), 10. + static_cast<double>(box))));
  } else if (actual->size(0) == 4) {
   CHECK(torch::equal(actual->select(0, 3), seeded->tensor.select(0, 3)));
  }
 }
 const auto admitted = r::testsupport::clone_normalized_model_state(owner);
 const r::ResolvedClassLayout unknown(r::unresolved_class_layout(4));
 auto unbound = owner.stage_normalized_state(source, r::detail::NormalizedModelStateAdmission::FreshTransfer, &unknown);
 owner.commit_normalized_state(std::move(unbound));
 const auto retained = owner.named_parameters(true);
 for (const auto& [name, dimension] : axes) {
  const auto expected = std::ranges::find(admitted, name, &r::NormalizedModelStateEntry::name);
  CHECK(torch::equal(*retained.find(name), expected->tensor));
 }
 config.num_classes = 3;
 for (const bool overlap : {true, false}) {
  r::NativeRfDetrModel smaller(config, r::native_training_class_layout(c::ClassCatalog({overlap ? "b" : "fresh", "new"})));
  auto& destination = (smaller);
  destination.initialize_training_supervision(31);
  const auto seed = r::testsupport::clone_normalized_model_state(destination);
  const auto parameters = transfer(destination);
  for (const auto& [name, dimension] : axes) {
   const auto* actual = parameters.find(name);
   const auto original = std::ranges::find(seed, name, &r::NormalizedModelStateEntry::name);
   REQUIRE(actual);
   REQUIRE(original != seed.end());
   const auto first = actual->select(dimension, 0);
   CHECK(torch::equal(first, overlap ? torch::full_like(first, 11.) : original->tensor.select(dimension, 0)));
   CHECK(torch::equal(actual->select(dimension, 1), original->tensor.select(dimension, 1)));
   if (dimension == 1) {
    for (std::int64_t box = 0; box < 4; ++box) CHECK(torch::equal(actual->select(1, 2 + box), torch::full_like(actual->select(1, 2 + box), 13. + static_cast<double>(box))));
   } else if (actual->size(0) == 3) {
    CHECK(torch::equal(actual->select(0, 2), original->tensor.select(0, 2)));
   }
  }
 }
}
TEST_CASE("Native replacement and normalization retire old automatic companions", "[model][rfdetr][checkpoint][publication]") {
 namespace r = mmltk::backend::models::rfdetr;
 namespace io = mmltk::common::io;
 const mmltk::testsupport::ScopedTempDir root("native-class-bundle");
 const auto input = root.path() / "input.pt", output = root.path() / "output.pt";
 const auto companion = fs::path(output.string() + ".classes.json");
 auto source = make_native_parity_fixture(parity_fixture_cases().front());
 source.metadata.class_layout = synthetic_training_layout(kParityFixtureNumClasses - 1);
 r::save_native_checkpoint(input, source);
 const auto restore_output = [&] {
  fs::copy_file(input, output, fs::copy_options::overwrite_existing);
  std::ofstream descriptor(companion);
  descriptor << r::encode_class_descriptor({1, io::sha256_hex(io::sha256_file(output)), source.metadata.class_layout});
 };
 restore_output();
 const auto previous = io::sha256_file(output);
 CHECK_THROWS(r::normalize_checkpoint_to_native(input, output, companion));
 CHECK(io::sha256_file(output) == previous);
 CHECK(fs::exists(companion));
 r::save_native_checkpoint(output, source);
 CHECK_FALSE(fs::exists(companion));
 CHECK(r::decode_model_state(output).metadata.class_layout == source.metadata.class_layout);
 restore_output();
 r::normalize_checkpoint_to_native(input, output);
 CHECK_FALSE(fs::exists(companion));
 CHECK(r::decode_model_state(output).metadata.class_layout == source.metadata.class_layout);
}
TEST_CASE("Direct ONNX export replaces an old automatic companion with embedded layout", "[model][rfdetr][layout][gpu]") {
 namespace r = mmltk::backend::models::rfdetr;
 namespace io = mmltk::common::io;
 const mmltk::testsupport::ScopedTempDir root("onnx-export-bundle");
 auto source = make_native_parity_fixture(parity_fixture_cases().front());
 source.metadata.class_layout = synthetic_training_layout(kParityFixtureNumClasses - 1);
 const auto weights = root.path() / "model.native.pt";
 const auto output = root.path() / "model.onnx";
 const auto companion = std::filesystem::path(output.string() + ".classes.json");
 r::save_native_checkpoint(weights, source);
 {
  std::ofstream old(output);
  old << "old producer bytes";
 }
 {
  std::ofstream descriptor(companion);
  descriptor << r::encode_class_descriptor({1, io::sha256_hex(io::sha256_file(output)), source.metadata.class_layout});
 }
 r::ExportOnnxRequest request;
 request.weights_path = weights;
 request.output_path = output;
 request.resolution = 64;
 request.simplify = true;
 r::export_onnx(request);
 CHECK_FALSE(std::filesystem::exists(companion));
 const auto info = r::load_onnx_model_info(output);
 CHECK(info.class_layout == source.metadata.class_layout);
 CHECK(r::ClassArtifactAdmission(output).Resolve(info.num_classes, info.class_layout) == source.metadata.class_layout);
 CHECK(r::rfdetr_output_roles(info).size() == info.outputs.size());
}
