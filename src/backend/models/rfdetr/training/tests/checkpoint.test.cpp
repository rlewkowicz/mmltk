#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <span>

#include "catch2_compat.hpp"
#include "filesystem_test_utils.hpp"

// Import-bearing support follows every textual standard-library test helper.
#include "checkpoint_fixture_support.h"
#include "detail/checkpoint_private.h"
#include "model_state_access.h"
#include "model_state_technical.h"
#include "model_access.h"
#include "model_state_fixture.h"
#include "model_technical.h"
#include "parity_fixture_support.h"
#include "src/common/io/filesystem_utils.h"
#include "torch_api.h"

import mmltk.backend.models.rfdetr.training.checkpoint;
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

fs::path fixture_root() { return fs::temp_directory_path() / "mmltk_rfdetr_checkpoint_fixture"; }

fs::path upstream_fixture_path(const ParityFixtureCase& fixture) { return fixture_root() / "upstream" / fixture.upstream_filename; }

fs::path native_roundtrip_path(const ParityFixtureCase& fixture) {
    return fixture_root() / "native" / (std::string(fixture.preset_name) + ".native.pt");
}

fs::path native_golden_fixture_path(const ParityFixtureCase& fixture) {
    return fixture_root() / "golden" / (std::string(fixture.preset_name) + ".golden.native.pt");
}

const mmltk::backend::models::rfdetr::NormalizedModelStateEntry* find_entry(
    const mmltk::backend::models::rfdetr::DecodedNativeModelState& checkpoint, const char* name) {
    for (const auto& entry : state_entries(checkpoint)) {
        if (entry.name == name) { return &entry; }
    }
    return nullptr;
}

void write_archive_string(tensor_api::serialize::OutputArchive& archive, const char* key, std::string_view value) {
    archive.write(key, tensor_api::IValue(std::string(value)));
}

void write_archive_int(tensor_api::serialize::OutputArchive& archive, const char* key, int64_t value) {
    archive.write(key, tensor_api::IValue(value));
}

void write_legacy_native_checkpoint(const fs::path& output_path) {
    fs::create_directories(output_path.parent_path());

    tensor_api::serialize::OutputArchive archive;
    write_archive_string(archive, "format", "fastloader.rfdetr.native_checkpoint");
    write_archive_int(archive, "format_version", mmltk::backend::models::rfdetr::kLegacyNativeCheckpointFormatVersion);
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
    MMLTK_ASSERT(checkpoint.metadata.source_kind == "upstream-python");
    MMLTK_ASSERT(checkpoint.metadata.preset_name == fixture.preset_name);
    MMLTK_ASSERT(checkpoint.metadata.num_classes == kParityFixtureNumClasses);
    MMLTK_ASSERT(state_entries(checkpoint).size() == 4);

    bool found_query_feat = false;
    bool found_cls_bias = false;
    bool found_refpoint = false;
    for (const auto& entry : state_entries(checkpoint)) {
        if (entry.name == "query_feat.weight") {
            found_query_feat = true;
            MMLTK_ASSERT(entry.tensor.dim() == 2);
            MMLTK_ASSERT(entry.tensor.size(0) == fixture.query_rows);
            MMLTK_ASSERT(entry.tensor.size(1) == kParityFixtureHiddenDim);
        }
        if (entry.name == "refpoint_embed.weight") {
            found_refpoint = true;
            MMLTK_ASSERT(entry.tensor.dim() == 2);
            MMLTK_ASSERT(entry.tensor.size(0) == fixture.query_rows);
            MMLTK_ASSERT(entry.tensor.size(1) == 4);
        }
        if (entry.name == "class_embed.bias") {
            found_cls_bias = true;
            MMLTK_ASSERT(entry.tensor.dim() == 1);
            MMLTK_ASSERT(entry.tensor.size(0) == kParityFixtureNumClasses);
            MMLTK_ASSERT(entry.tensor.index({0}).item<float>() == make_fixture_class_bias(fixture).index({0}).item<float>());
        }
    }
    MMLTK_ASSERT(found_query_feat);
    MMLTK_ASSERT(found_cls_bias);
    MMLTK_ASSERT(found_refpoint);
}

void test_native_checkpoint_roundtrip(const ParityFixtureCase& fixture, const fs::path& upstream_path) {
    const auto upstream = mmltk::backend::models::rfdetr::decode_model_state(upstream_path);
    const fs::path output_path = native_roundtrip_path(fixture);
    fs::create_directories(output_path.parent_path());

    const auto normalized = mmltk::backend::models::rfdetr::normalize_checkpoint_to_native(upstream_path, output_path);
    const bool output_exists = fs::exists(output_path);
    MMLTK_ASSERT(output_exists);
    MMLTK_ASSERT(normalized.metadata.preset_name == fixture.preset_name);
    MMLTK_ASSERT(mmltk::backend::models::rfdetr::is_native_checkpoint_file(output_path));

    const auto native = mmltk::backend::models::rfdetr::decode_model_state(output_path);
    MMLTK_ASSERT(native.metadata.preset_name == upstream.metadata.preset_name);
    MMLTK_ASSERT(native.metadata.source_kind == "upstream-python");
    MMLTK_ASSERT(state_entries(native).size() == state_entries(upstream).size());

    bool compared_tensor = false;
    for (size_t index = 0; index < state_entries(native).size(); ++index) {
        MMLTK_ASSERT(state_entries(native)[index].name == state_entries(upstream)[index].name);
        if (!compared_tensor && state_entries(native)[index].name == "class_embed.weight") {
            MMLTK_ASSERT(tensor_api::equal(state_entries(native)[index].tensor, state_entries(upstream)[index].tensor));
            compared_tensor = true;
        }
    }
    MMLTK_ASSERT(compared_tensor);
}

void test_native_golden_fixture_roundtrip(const ParityFixtureCase& fixture) {
    const fs::path output_path = native_golden_fixture_path(fixture);
    fs::create_directories(output_path.parent_path());

    const auto expected = make_native_parity_fixture(fixture);
    mmltk::backend::models::rfdetr::save_native_checkpoint(output_path, expected);
    const bool output_exists = fs::exists(output_path);
    MMLTK_ASSERT(output_exists);
    MMLTK_ASSERT(mmltk::backend::models::rfdetr::is_native_checkpoint_file(output_path));

    const auto loaded = mmltk::backend::models::rfdetr::decode_model_state(output_path);
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
    checkpoint.metadata.num_queries = 1;
    checkpoint.metadata.num_select = 1;

    const auto cpu_contiguous = tensor_api::arange(12, tensor_api::TensorOptions().dtype(tensor_api::kFloat32)).view({3, 4}).clone();
    const auto cpu_non_contiguous = cpu_contiguous.transpose(0, 1);
    state_entries(checkpoint).push_back({"cpu_contiguous", cpu_contiguous});
    state_entries(checkpoint).push_back({"cpu_non_contiguous", cpu_non_contiguous});
    const bool has_cuda = tensor_api::cuda::is_available();
    if (has_cuda) {
        state_entries(checkpoint)
            .push_back(
                {"cuda_tensor",
                 tensor_api::arange(6, tensor_api::TensorOptions().dtype(tensor_api::kFloat32).device(tensor_api::kCUDA)).view({2, 3})});
    }

    mmltk::backend::models::rfdetr::save_native_checkpoint(output_path, checkpoint);
    const auto loaded = mmltk::backend::models::rfdetr::decode_model_state(output_path);

    const auto* loaded_contiguous = find_entry(loaded, "cpu_contiguous");
    MMLTK_ASSERT(loaded_contiguous != nullptr);
    MMLTK_ASSERT(loaded_contiguous->tensor.device().is_cpu());
    MMLTK_ASSERT(loaded_contiguous->tensor.is_contiguous());
    MMLTK_ASSERT(tensor_api::equal(loaded_contiguous->tensor, cpu_contiguous));

    const auto* loaded_non_contiguous = find_entry(loaded, "cpu_non_contiguous");
    MMLTK_ASSERT(loaded_non_contiguous != nullptr);
    MMLTK_ASSERT(loaded_non_contiguous->tensor.device().is_cpu());
    MMLTK_ASSERT(loaded_non_contiguous->tensor.is_contiguous());
    MMLTK_ASSERT(tensor_api::equal(loaded_non_contiguous->tensor, cpu_non_contiguous.contiguous()));

    const auto* loaded_cuda = find_entry(loaded, "cuda_tensor");
    MMLTK_ASSERT((loaded_cuda != nullptr) == has_cuda);
    if (loaded_cuda != nullptr) {
        const auto expected = state_entries(checkpoint).back().tensor.detach().to(tensor_api::Device(tensor_api::kCPU)).contiguous();
        MMLTK_ASSERT(loaded_cuda->tensor.device().is_cpu());
        MMLTK_ASSERT(loaded_cuda->tensor.is_contiguous());
        MMLTK_ASSERT(tensor_api::equal(loaded_cuda->tensor, expected));
    }
}

void test_upstream_checkpoint_scalar_type_bridge() {
    const fs::path upstream_path = fixture_root() / "upstream" / "rf-detr-nano-dtype-bridge.pth";
    mmltk::backend::models::rfdetr::DecodedNativeModelState state;
    auto& entries = state_entries(state);
    entries = {
        {"query_feat.weight", tensor_api::ones({4, kParityFixtureHiddenDim}, tensor_api::TensorOptions().dtype(tensor_api::kFloat16))},
        {"refpoint_embed.weight", tensor_api::zeros({4, 4}, tensor_api::TensorOptions().dtype(tensor_api::kFloat32))},
        {"class_embed.weight",
         tensor_api::ones({kParityFixtureNumClasses, kParityFixtureHiddenDim}, tensor_api::TensorOptions().dtype(tensor_api::kFloat32))},
        {"class_embed.bias", tensor_api::arange(kParityFixtureNumClasses, tensor_api::TensorOptions().dtype(tensor_api::kInt64))},
    };
    mmltk::backend::models::rfdetr::write_upstream_model_state(upstream_path, state);

    const auto checkpoint = mmltk::backend::models::rfdetr::decode_model_state(upstream_path);
    const auto* query_feat = find_entry(checkpoint, "query_feat.weight");
    const auto* class_bias = find_entry(checkpoint, "class_embed.bias");
    MMLTK_ASSERT(query_feat != nullptr);
    MMLTK_ASSERT(class_bias != nullptr);
    MMLTK_ASSERT(query_feat->tensor.scalar_type() == tensor_api::kFloat16);
    MMLTK_ASSERT(class_bias->tensor.scalar_type() == tensor_api::kInt64);
}

void test_legacy_native_checkpoint_format_support() {
    const fs::path legacy_path = fixture_root() / "legacy" / "legacy-format.pt";
    write_legacy_native_checkpoint(legacy_path);

    MMLTK_ASSERT(mmltk::backend::models::rfdetr::is_native_checkpoint_file(legacy_path));
    const auto checkpoint = mmltk::backend::models::rfdetr::decode_model_state(legacy_path);
    MMLTK_ASSERT(checkpoint.metadata.preset_name == "rf-detr-seg-medium");
    MMLTK_ASSERT(checkpoint.metadata.source_kind == "legacy-native-test");
    MMLTK_ASSERT(checkpoint.metadata.num_classes == 7);
    MMLTK_ASSERT(checkpoint.metadata.num_queries == 200);
    MMLTK_ASSERT(checkpoint.metadata.num_select == 200);
    MMLTK_ASSERT(state_entries(checkpoint).size() == 1);

    const auto* class_bias = find_entry(checkpoint, "class_embed.bias");
    MMLTK_ASSERT(class_bias != nullptr);
    MMLTK_ASSERT(tensor_api::equal(class_bias->tensor, tensor_api::arange(7, tensor_api::TensorOptions().dtype(tensor_api::kFloat32))));
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

    MMLTK_ASSERT(round_trip_training_supervision_config(path, config) == config);
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
    for (const auto& variant : variants) {
        REQUIRE(round_trip_training_supervision_config(path, variant) == variant);
    }

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
    oversized_output.write("training_supervision_config_cbor",
                           tensor_api::zeros({4096}, tensor_api::TensorOptions().dtype(tensor_api::kUInt8)));
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
    rfdetr::NativeRfDetrModel model(config);
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
    auto candidate = rfdetr::detail::native_model_owner(model).stage_normalized_state(valid_candidate_state,
                                                                                      rfdetr::detail::NormalizedModelStateAdmission::Exact);
    REQUIRE(tensor_api::equal(module.named_parameters(true).begin()->value(), first_before));
    rfdetr::detail::native_model_owner(model).commit_normalized_state(std::move(candidate));
    REQUIRE(tensor_api::equal(module.named_parameters(true).begin()->value(), tensor_api::full_like(first_before, 9.0)));

    const auto class_head =
        std::ranges::find_if(state, [](const rfdetr::NormalizedModelStateEntry& entry) { return entry.name == "class_embed.weight"; });
    REQUIRE(class_head != state.end());
    REQUIRE(class_head->tensor.size(0) > 1);
    auto mismatched_class_head = state;
    auto& mismatched_entry = mismatched_class_head.at(static_cast<std::size_t>(std::distance(state.begin(), class_head)));
    mismatched_entry.tensor = mismatched_entry.tensor.narrow(0, 0, 1).clone();
    auto current_parameters = module.named_parameters(true);
    auto* class_head_destination = current_parameters.find(class_head->name);
    REQUIRE(class_head_destination != nullptr);
    const auto class_head_before = class_head_destination->detach().clone();
    REQUIRE_THROWS(rfdetr::detail::native_model_owner(model).stage_normalized_state(mismatched_class_head,
                                                                                    rfdetr::detail::NormalizedModelStateAdmission::Exact));
    REQUIRE(tensor_api::equal(*class_head_destination, class_head_before));

    auto adapted = rfdetr::detail::native_model_owner(model).stage_normalized_state(
        mismatched_class_head, rfdetr::detail::NormalizedModelStateAdmission::AdaptDetectionClassHead);
    rfdetr::detail::native_model_owner(model).commit_normalized_state(std::move(adapted));
    REQUIRE(tensor_api::equal(*class_head_destination, mismatched_entry.tensor.repeat({class_head_before.size(0), 1})));
}

}  // namespace

void test_checkpoint_roundtrip_and_fixture_loading() {
    mmltk::common::io::filesystem_utils::remove_path_recursively_best_effort(fixture_root());
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
    mmltk::common::io::filesystem_utils::remove_path_recursively_best_effort(fixture_root());
}

void test_checkpoint_tensor_and_legacy_support() {
    test_native_checkpoint_tensor_preparation();
    test_upstream_checkpoint_scalar_type_bridge();
    test_legacy_native_checkpoint_format_support();
    test_training_supervision_checkpoint_blob_admission();
    test_strict_model_state_admission_is_duplicate_free_and_atomic();
}

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][checkpoint]", test_checkpoint_roundtrip_and_fixture_loading);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][checkpoint][training_supervision]", test_checkpoint_tensor_and_legacy_support);
