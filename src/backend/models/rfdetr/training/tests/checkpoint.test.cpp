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
#include "model_state_access.h"
#include "model_state_technical.h"
#include "model_access.h"
#include "model_state_fixture.h"
#include "model_technical.h"
#include "parity_fixture_support.h"
#include "src/common/io/file_memory.h"
#include "torch_api.h"
#if defined(CHECK) && !defined(CATCH_TEST_MACROS_HPP_INCLUDED)
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>
import mmltk.backend.models.rfdetr.training.checkpoint;
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.core.model;
namespace fs = std::filesystem;
namespace tensor_api = mmltk::backend::ml::torch_api;
namespace {
using namespace mmltk::backend::models::rfdetr::testsupport;
auto& state_entries(mmltk::backend::models::rfdetr::DecodedNativeModelState& state) {
    return mmltk::backend::models::rfdetr::detail::model_state_owner(state).entries;
}
const auto& state_entries(const mmltk::backend::models::rfdetr::DecodedNativeModelState& state) {
    return mmltk::backend::models::rfdetr::detail::model_state_owner(state).entries;
}
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
fs::path native_golden_fixture_path(const ParityFixtureCase& fixture) {
    return fixture_root() / "golden" / (std::string(fixture.preset_name) + ".golden.native.pt");
}
const mmltk::backend::models::rfdetr::NormalizedModelStateEntry* find_entry(const mmltk::backend::models::rfdetr::DecodedNativeModelState& checkpoint,
                                                                            const char* name) {
    for (const auto& entry : state_entries(checkpoint)) {
        if (entry.name == name) { return &entry; }
    }
    return nullptr;
}
void write_archive_string(tensor_api::serialize::OutputArchive& archive, const char* key, std::string_view value) {
    archive.write(key, tensor_api::IValue(std::string(value)));
}
void write_archive_int(tensor_api::serialize::OutputArchive& archive, const char* key, int64_t value) { archive.write(key, tensor_api::IValue(value)); }
void write_legacy_native_checkpoint(const fs::path& output_path, int version = 1, std::string_view format = "fastloader.rfdetr.native_checkpoint") {
    fs::create_directories(output_path.parent_path());
    tensor_api::serialize::OutputArchive archive;
    write_archive_string(archive, "format", format);
    write_archive_int(archive, "format_version", version);
    write_archive_string(archive, "preset_name", "rf-detr-seg-medium");
    write_archive_string(archive, "source_kind", "legacy-native-test");
    write_archive_string(archive, "source_path", output_path.string());
    write_archive_int(archive, "num_classes", 7);
    tensor_api::serialize::OutputArchive state_archive;
    write_archive_int(state_archive, "entry_count", 1);
    tensor_api::serialize::OutputArchive entry_archive;
    write_archive_string(entry_archive, "name", "class_embed.bias");
    entry_archive.write("tensor", tensor_api::arange(7, tensor_api::TensorOptions().dtype(tensor_api::kFloat32)).contiguous());
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
            REQUIRE((tensor_api::equal(state_entries(native)[index].tensor, state_entries(upstream)[index].tensor)));
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
    const auto cpu_contiguous = tensor_api::arange(12, tensor_api::TensorOptions().dtype(tensor_api::kFloat32)).view({3, 4}).clone();
    const auto cpu_non_contiguous = cpu_contiguous.transpose(0, 1);
    state_entries(checkpoint).push_back({"cpu_contiguous", cpu_contiguous});
    state_entries(checkpoint).push_back({"cpu_non_contiguous", cpu_non_contiguous});
    const bool has_cuda = tensor_api::cuda::is_available();
    if (has_cuda) {
        state_entries(checkpoint)
            .push_back({"cuda_tensor", tensor_api::arange(6, tensor_api::TensorOptions().dtype(tensor_api::kFloat32).device(tensor_api::kCUDA)).view({2, 3})});
    }
    mmltk::backend::models::rfdetr::save_native_checkpoint(output_path, checkpoint);
    const auto loaded = mmltk::backend::models::rfdetr::decode_model_state(output_path);
    require_detection_metadata_equal(checkpoint.metadata, loaded.metadata);
    const auto* loaded_contiguous = find_entry(loaded, "cpu_contiguous");
    REQUIRE((loaded_contiguous != nullptr));
    REQUIRE((loaded_contiguous->tensor.device().is_cpu()));
    REQUIRE((loaded_contiguous->tensor.is_contiguous()));
    REQUIRE((tensor_api::equal(loaded_contiguous->tensor, cpu_contiguous)));
    const auto* loaded_non_contiguous = find_entry(loaded, "cpu_non_contiguous");
    REQUIRE((loaded_non_contiguous != nullptr));
    REQUIRE((loaded_non_contiguous->tensor.device().is_cpu()));
    REQUIRE((loaded_non_contiguous->tensor.is_contiguous()));
    REQUIRE((tensor_api::equal(loaded_non_contiguous->tensor, cpu_non_contiguous.contiguous())));
    const auto* loaded_cuda = find_entry(loaded, "cuda_tensor");
    REQUIRE(((loaded_cuda != nullptr) == has_cuda));
    if (loaded_cuda != nullptr) {
        const auto expected = state_entries(checkpoint).back().tensor.detach().to(tensor_api::Device(tensor_api::kCPU)).contiguous();
        REQUIRE((loaded_cuda->tensor.device().is_cpu()));
        REQUIRE((loaded_cuda->tensor.is_contiguous()));
        REQUIRE((tensor_api::equal(loaded_cuda->tensor, expected)));
    }
    checkpoint.metadata.for_each_detection_field([](const char*, auto& field) { field.reset(); });
    mmltk::backend::models::rfdetr::save_native_checkpoint(output_path, checkpoint);
    require_detection_metadata_equal(checkpoint.metadata, mmltk::backend::models::rfdetr::decode_model_state(output_path).metadata);
}
void test_upstream_checkpoint_scalar_type_bridge() {
    const fs::path upstream_path = fixture_root() / "upstream" / "rf-detr-nano-dtype-bridge.pth";
    mmltk::backend::models::rfdetr::DecodedNativeModelState state;
    populate_detection_metadata(state.metadata);
    auto& entries = state_entries(state);
    entries = {
        {"query_feat.weight", tensor_api::ones({4, kParityFixtureHiddenDim}, tensor_api::TensorOptions().dtype(tensor_api::kFloat16))},
        {"refpoint_embed.weight", tensor_api::zeros({4, 4}, tensor_api::TensorOptions().dtype(tensor_api::kFloat32))},
        {"class_embed.weight", tensor_api::ones({kParityFixtureNumClasses, kParityFixtureHiddenDim}, tensor_api::TensorOptions().dtype(tensor_api::kFloat32))},
        {"class_embed.bias", tensor_api::arange(kParityFixtureNumClasses, tensor_api::TensorOptions().dtype(tensor_api::kInt64))},
    };
    mmltk::backend::models::rfdetr::write_upstream_model_state(upstream_path, state);
    const auto checkpoint = mmltk::backend::models::rfdetr::decode_model_state(upstream_path);
    require_detection_metadata_equal(state.metadata, checkpoint.metadata);
    const auto* query_feat = find_entry(checkpoint, "query_feat.weight");
    const auto* class_bias = find_entry(checkpoint, "class_embed.bias");
    REQUIRE((query_feat != nullptr));
    REQUIRE((class_bias != nullptr));
    REQUIRE((query_feat->tensor.scalar_type() == tensor_api::kFloat16));
    REQUIRE((class_bias->tensor.scalar_type() == tensor_api::kInt64));
}
void test_cuda_upstream_raw_state_preserves_logical_values() {
    if (!tensor_api::cuda::is_available()) { SKIP("CUDA unavailable"); }
    mmltk::testsupport::ScopedTempDir temp{"mmltk-upstream-cuda-readback"};
    mmltk::backend::models::rfdetr::DecodedNativeModelState state;
    auto& entries = state_entries(state);
    entries = {
        {"query_feat.weight",
         tensor_api::ones({4, kParityFixtureHiddenDim}, tensor_api::TensorOptions().dtype(tensor_api::kFloat16).device(tensor_api::kCUDA))},
        {"refpoint_embed.weight", tensor_api::zeros({4, 4}, tensor_api::TensorOptions().device(tensor_api::kCUDA))},
        {"class_embed.weight", tensor_api::ones({kParityFixtureNumClasses, kParityFixtureHiddenDim}, tensor_api::TensorOptions().device(tensor_api::kCUDA))},
        {"class_embed.bias", tensor_api::arange(kParityFixtureNumClasses, tensor_api::TensorOptions().dtype(tensor_api::kInt64).device(tensor_api::kCUDA))},
        {"extra_view", tensor_api::arange(12, tensor_api::TensorOptions().dtype(tensor_api::kBFloat16).device(tensor_api::kCUDA)).view({3, 4}).transpose(0, 1)},
        {"extra_empty", tensor_api::empty({0}, tensor_api::TensorOptions().dtype(tensor_api::kBool))},
    };
    std::vector<const void*> pointers;
    for (const auto& entry : entries) pointers.push_back(entry.tensor.const_data_ptr());
    const auto path = temp.path() / "rf-detr-nano-readback.pth";
    mmltk::backend::models::rfdetr::write_upstream_model_state(path, state);
    const auto loaded = mmltk::backend::models::rfdetr::decode_model_state(path);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& expected = entries[index];
        const auto* actual = find_entry(loaded, expected.name.c_str());
        REQUIRE(actual != nullptr);
        REQUIRE(actual->tensor.scalar_type() == expected.tensor.scalar_type());
        REQUIRE(actual->tensor.sizes() == expected.tensor.sizes());
        REQUIRE(tensor_api::equal(actual->tensor, expected.tensor.cpu()));
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
    tensor_api::serialize::OutputArchive output;
    mmltk::backend::models::rfdetr::detail::write_training_supervision_config(output, config);
    output.save_to(path.string());
    tensor_api::serialize::InputArchive input;
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
    tensor_api::serialize::InputArchive input;
    input.load_from(path.string());
    tensor_api::Tensor encoded;
    input.read("training_supervision_config_cbor", encoded);
    tensor_api::serialize::OutputArchive obsolete_key_output;
    obsolete_key_output.write("training_supervision_config", encoded);
    obsolete_key_output.save_to(path.string());
    tensor_api::serialize::InputArchive obsolete_key_input;
    obsolete_key_input.load_from(path.string());
    REQUIRE(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(obsolete_key_input) ==
            mmltk::backend::models::rfdetr::TrainingSupervisionConfig{});
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
    tensor_api::serialize::OutputArchive trailing_output;
    trailing_output.write("training_supervision_config_cbor",
                          tensor_api::cat({encoded, tensor_api::zeros({1}, tensor_api::TensorOptions().dtype(tensor_api::kUInt8))}));
    trailing_output.save_to(path.string());
    tensor_api::serialize::InputArchive trailing_input;
    trailing_input.load_from(path.string());
    REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(trailing_input));
    tensor_api::serialize::OutputArchive malformed_output;
    malformed_output.write("training_supervision_config_cbor", tensor_api::zeros({4}));
    malformed_output.save_to(path.string());
    tensor_api::serialize::InputArchive malformed_input;
    malformed_input.load_from(path.string());
    REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(malformed_input));
    tensor_api::serialize::OutputArchive wrong_rank_output;
    wrong_rank_output.write("training_supervision_config_cbor", encoded.reshape({1, encoded.numel()}));
    wrong_rank_output.save_to(path.string());
    tensor_api::serialize::InputArchive wrong_rank_input;
    wrong_rank_input.load_from(path.string());
    REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(wrong_rank_input));
    tensor_api::serialize::OutputArchive truncated_output;
    truncated_output.write("training_supervision_config_cbor", encoded.narrow(0, 0, encoded.numel() - 1));
    truncated_output.save_to(path.string());
    tensor_api::serialize::InputArchive truncated_input;
    truncated_input.load_from(path.string());
    REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(truncated_input));
    if (tensor_api::cuda::is_available()) {
        tensor_api::serialize::OutputArchive wrong_device_output;
        wrong_device_output.write("training_supervision_config_cbor", encoded.to(tensor_api::Device(tensor_api::kCUDA)));
        wrong_device_output.save_to(path.string());
        tensor_api::serialize::InputArchive wrong_device_input;
        wrong_device_input.load_from(path.string());
        REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(wrong_device_input));
    }
    tensor_api::serialize::OutputArchive oversized_output;
    oversized_output.write("training_supervision_config_cbor", tensor_api::zeros({4096}, tensor_api::TensorOptions().dtype(tensor_api::kUInt8)));
    oversized_output.save_to(path.string());
    tensor_api::serialize::InputArchive oversized_input;
    oversized_input.load_from(path.string());
    REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(oversized_input));
    const auto require_raw_cbor_rejected = [&](const std::span<const std::uint8_t> raw) {
        auto blob = tensor_api::empty({static_cast<std::int64_t>(raw.size())}, tensor_api::TensorOptions().dtype(tensor_api::kUInt8));
        std::memcpy(blob.data_ptr<std::uint8_t>(), raw.data(), raw.size());
        tensor_api::serialize::OutputArchive raw_output;
        raw_output.write("training_supervision_config_cbor", blob);
        raw_output.save_to(path.string());
        tensor_api::serialize::InputArchive raw_input;
        raw_input.load_from(path.string());
        REQUIRE_THROWS(mmltk::backend::models::rfdetr::detail::read_training_supervision_config(raw_input));
    };
    constexpr std::array<std::uint8_t, 25> duplicate_assignment{
        0xa2U, 0x6aU, 'a', 's', 's', 'i', 'g', 'n', 'm', 'e', 'n', 't', 0U, 0x6aU, 'a', 's', 's', 'i', 'g', 'n', 'm', 'e', 'n', 't', 0U,
    };
    constexpr std::array<std::uint8_t, 10> unknown_field{
        0xa1U, 0x67U, 'u', 'n', 'k', 'n', 'o', 'w', 'n', 0U,
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
    auto& module = rfdetr::detail::native_model_owner(model).module();
    auto state = clone_normalized_model_state(module);
    REQUIRE_FALSE(state.empty());
    const auto first_before = module.named_parameters(true).begin()->value().detach().clone();
    const auto require_rejected_without_mutation = [&](const std::vector<rfdetr::NormalizedModelStateEntry>& candidate) {
        REQUIRE_THROWS(rfdetr::detail::native_model_owner(model).load_normalized_state(candidate, true));
        REQUIRE(tensor_api::equal(module.named_parameters(true).begin()->value(), first_before));
    };
    auto duplicate = state;
    duplicate.push_back(duplicate.front());
    require_rejected_without_mutation(duplicate);
    auto missing = state;
    missing.front().tensor = tensor_api::full_like(missing.front().tensor, 9.0);
    missing.pop_back();
    require_rejected_without_mutation(missing);
    auto valid_candidate_state = state;
    valid_candidate_state.front().tensor = tensor_api::full_like(valid_candidate_state.front().tensor, 9.0);
    auto candidate =
        rfdetr::detail::native_model_owner(model).stage_normalized_state(valid_candidate_state, rfdetr::detail::NormalizedModelStateAdmission::Exact);
    REQUIRE(tensor_api::equal(module.named_parameters(true).begin()->value(), first_before));
    rfdetr::detail::native_model_owner(model).commit_normalized_state(std::move(candidate));
    REQUIRE(tensor_api::equal(module.named_parameters(true).begin()->value(), tensor_api::full_like(first_before, 9.0)));
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
    REQUIRE_THROWS(
        rfdetr::detail::native_model_owner(model).stage_normalized_state(mismatched_class_head, rfdetr::detail::NormalizedModelStateAdmission::Exact));
    REQUIRE(tensor_api::equal(*class_head_destination, class_head_before));
    const auto source_layout = rfdetr::ResolvedClassLayout(model.class_layout()->record());
    REQUIRE_THROWS(rfdetr::detail::native_model_owner(model).stage_normalized_state(
        mismatched_class_head, rfdetr::detail::NormalizedModelStateAdmission::FreshTransfer, &source_layout));
    REQUIRE(tensor_api::equal(*class_head_destination, class_head_before));
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
    auto& owner = r::detail::native_model_owner(model);
    owner.initialize_training_supervision(29);
    const auto before = r::testsupport::clone_normalized_model_state(owner.module());
    auto source = r::testsupport::clone_normalized_model_state(owner.module());
    // Independent expected axes cover both classifier owners and distinct
    // supervision coordinates, rather than consulting the production inventory.
    const std::array axes{std::pair{"class_embed.weight", 0},
                          std::pair{"class_embed.bias", 0},
                          std::pair{"transformer.enc_out_class_embed.0.weight", 0},
                          std::pair{"transformer.enc_out_class_embed.0.bias", 0},
                          std::pair{"training_supervision.denoising_label_embedding.weight", 0},
                          std::pair{"training_supervision.ground_truth_mlp.linear1.weight", 1}};
    for (const auto& [name, dimension] : axes) {
        auto entry = std::ranges::find(source, name, &r::NormalizedModelStateEntry::name);
        REQUIRE(entry != source.end());
        for (std::int64_t row = 0; row < entry->tensor.size(dimension); ++row) entry->tensor.select(dimension, row).fill_(10. + static_cast<double>(row));
    }
    const auto transfer = [&](r::detail::NativeModelTechnicalOwner& destination) {
        const auto random_before = at::detail::getDefaultCPUGenerator().get_state();
        auto candidate = destination.stage_normalized_state(source, r::detail::NormalizedModelStateAdmission::FreshTransfer, &source_layout);
        destination.commit_normalized_state(std::move(candidate));
        CHECK(tensor_api::equal(random_before, at::detail::getDefaultCPUGenerator().get_state()));
        return destination.module().named_parameters(true);
    };
    const auto after = transfer(owner);
    for (const auto& [name, dimension] : axes) {
        const auto* actual = after.find(name);
        REQUIRE(actual);
        const auto seeded = std::ranges::find(before, name, &r::NormalizedModelStateEntry::name);
        REQUIRE(seeded != before.end());
        CHECK(tensor_api::equal(actual->select(dimension, 0), tensor_api::full_like(actual->select(dimension, 0), 12.)));
        CHECK(tensor_api::equal(actual->select(dimension, 1), tensor_api::full_like(actual->select(dimension, 1), 10.)));
        CHECK(tensor_api::equal(actual->select(dimension, 2), seeded->tensor.select(dimension, 2)));
        if (dimension == 1) {
            for (std::int64_t box = 3; box < 7; ++box)
                CHECK(tensor_api::equal(actual->select(1, box), tensor_api::full_like(actual->select(1, box), 10. + static_cast<double>(box))));
        } else if (actual->size(0) == 4) {
            CHECK(tensor_api::equal(actual->select(0, 3), seeded->tensor.select(0, 3)));
        }
    }
    const auto admitted = r::testsupport::clone_normalized_model_state(owner.module());
    const r::ResolvedClassLayout unknown(r::unresolved_class_layout(4));
    auto unbound = owner.stage_normalized_state(source, r::detail::NormalizedModelStateAdmission::FreshTransfer, &unknown);
    owner.commit_normalized_state(std::move(unbound));
    const auto retained = owner.module().named_parameters(true);
    for (const auto& [name, dimension] : axes) {
        const auto expected = std::ranges::find(admitted, name, &r::NormalizedModelStateEntry::name);
        CHECK(tensor_api::equal(*retained.find(name), expected->tensor));
    }
    config.num_classes = 3;
    for (const bool overlap : {true, false}) {
        r::NativeRfDetrModel smaller(config, r::native_training_class_layout(c::ClassCatalog({overlap ? "b" : "fresh", "new"})));
        auto& destination = r::detail::native_model_owner(smaller);
        destination.initialize_training_supervision(31);
        const auto seed = r::testsupport::clone_normalized_model_state(destination.module());
        const auto parameters = transfer(destination);
        for (const auto& [name, dimension] : axes) {
            const auto* actual = parameters.find(name);
            const auto original = std::ranges::find(seed, name, &r::NormalizedModelStateEntry::name);
            REQUIRE(actual);
            REQUIRE(original != seed.end());
            const auto first = actual->select(dimension, 0);
            CHECK(tensor_api::equal(first, overlap ? tensor_api::full_like(first, 11.) : original->tensor.select(dimension, 0)));
            CHECK(tensor_api::equal(actual->select(dimension, 1), original->tensor.select(dimension, 1)));
            if (dimension == 1) {
                for (std::int64_t box = 0; box < 4; ++box)
                    CHECK(tensor_api::equal(actual->select(1, 2 + box), tensor_api::full_like(actual->select(1, 2 + box), 13. + static_cast<double>(box))));
            } else if (actual->size(0) == 3) {
                CHECK(tensor_api::equal(actual->select(0, 2), original->tensor.select(0, 2)));
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
