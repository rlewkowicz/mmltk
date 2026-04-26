#include "mmltk/rfdetr/checkpoint.h"
#include "checkpoint_fixture_support.h"
#include "parity_fixture_support.h"

#include "support/catch2_compat.hpp"
#include "support/filesystem_test_utils.hpp"
#include <cstdio>
#include <exception>
#include <filesystem>

#include <torch/serialize.h>

namespace fs = std::filesystem;

namespace {

using mmltk::rfdetr::testsupport::ParityFixtureCase;
using mmltk::rfdetr::testsupport::assert_matches_native_parity_fixture;
using mmltk::rfdetr::testsupport::kParityFixtureHiddenDim;
using mmltk::rfdetr::testsupport::kParityFixtureNumClasses;
using mmltk::rfdetr::testsupport::log_fixture_phase;
using mmltk::rfdetr::testsupport::make_native_parity_fixture;
using mmltk::rfdetr::testsupport::parity_fixture_cases;
using mmltk::rfdetr::testsupport::save_upstream_python_checkpoint;
using mmltk::rfdetr::testsupport::write_minimal_upstream_checkpoint;

fs::path fixture_root() {
    return fs::temp_directory_path() / "mmltk_rfdetr_checkpoint_fixture";
}

fs::path upstream_fixture_path(const ParityFixtureCase& fixture) {
    return fixture_root() / "upstream" / fixture.upstream_filename;
}

fs::path native_roundtrip_path(const ParityFixtureCase& fixture) {
    return fixture_root() / "native" / (std::string(fixture.preset_name) + ".native.pt");
}

fs::path native_golden_fixture_path(const ParityFixtureCase& fixture) {
    return fixture_root() / "golden" / (std::string(fixture.preset_name) + ".golden.native.pt");
}

const mmltk::rfdetr::StateDictEntry* find_entry(const mmltk::rfdetr::NativeCheckpoint& checkpoint, const char* name) {
    for (const auto& entry : checkpoint.state_dict) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

void write_archive_string(torch::serialize::OutputArchive& archive, const char* key, std::string_view value) {
    archive.write(key, c10::IValue(std::string(value)));
}

void write_archive_int(torch::serialize::OutputArchive& archive, const char* key, int64_t value) {
    archive.write(key, c10::IValue(value));
}

void write_legacy_native_checkpoint(const fs::path& output_path) {
    fs::create_directories(output_path.parent_path());

    torch::serialize::OutputArchive archive;
    write_archive_string(archive, "format", "fastloader.rfdetr.native_checkpoint");
    write_archive_int(archive, "format_version", mmltk::rfdetr::kNativeCheckpointFormatVersion);
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
    const auto checkpoint = mmltk::rfdetr::load_checkpoint(upstream_path);
    assert(checkpoint.metadata.source_kind == "upstream-python");
    assert(checkpoint.metadata.preset_name == fixture.preset_name);
    assert(checkpoint.metadata.num_classes == kParityFixtureNumClasses);
    assert(checkpoint.state_dict.size() == 4);

    bool found_query_feat = false;
    bool found_cls_bias = false;
    bool found_refpoint = false;
    for (const auto& entry : checkpoint.state_dict) {
        if (entry.name == "query_feat.weight") {
            found_query_feat = true;
            assert(entry.tensor.sizes() == torch::IntArrayRef({fixture.query_rows, kParityFixtureHiddenDim}));
        }
        if (entry.name == "refpoint_embed.weight") {
            found_refpoint = true;
            assert(entry.tensor.sizes() == torch::IntArrayRef({fixture.query_rows, 4}));
        }
        if (entry.name == "class_embed.bias") {
            found_cls_bias = true;
            assert(entry.tensor.sizes() == torch::IntArrayRef({kParityFixtureNumClasses}));
            assert(entry.tensor.index({0}).item<float>() == make_fixture_class_bias(fixture).index({0}).item<float>());
        }
    }
    assert(found_query_feat);
    assert(found_cls_bias);
    assert(found_refpoint);
}

void test_native_checkpoint_roundtrip(const ParityFixtureCase& fixture, const fs::path& upstream_path) {
    const auto upstream = mmltk::rfdetr::load_checkpoint(upstream_path);
    const fs::path output_path = native_roundtrip_path(fixture);
    fs::create_directories(output_path.parent_path());

    const auto normalized = mmltk::rfdetr::normalize_checkpoint_to_native(upstream_path, output_path);
    const bool output_exists = fs::exists(output_path);
    assert(output_exists);
    assert(normalized.metadata.preset_name == fixture.preset_name);
    assert(mmltk::rfdetr::is_native_checkpoint_file(output_path));

    const auto native = mmltk::rfdetr::load_checkpoint(output_path);
    assert(native.metadata.preset_name == upstream.metadata.preset_name);
    assert(native.metadata.source_kind == "upstream-python");
    assert(native.state_dict.size() == upstream.state_dict.size());

    bool compared_tensor = false;
    for (size_t index = 0; index < native.state_dict.size(); ++index) {
        assert(native.state_dict[index].name == upstream.state_dict[index].name);
        if (!compared_tensor && native.state_dict[index].name == "class_embed.weight") {
            assert(torch::equal(native.state_dict[index].tensor, upstream.state_dict[index].tensor));
            compared_tensor = true;
        }
    }
    assert(compared_tensor);
}

void test_native_golden_fixture_roundtrip(const ParityFixtureCase& fixture) {
    const fs::path output_path = native_golden_fixture_path(fixture);
    fs::create_directories(output_path.parent_path());

    const auto expected = make_native_parity_fixture(fixture);
    mmltk::rfdetr::save_native_checkpoint(output_path, expected);
    const bool output_exists = fs::exists(output_path);
    assert(output_exists);
    assert(mmltk::rfdetr::is_native_checkpoint_file(output_path));

    const auto loaded = mmltk::rfdetr::load_checkpoint(output_path);
    assert_matches_native_parity_fixture(loaded, fixture);
}

void test_native_checkpoint_tensor_preparation() {
    const fs::path output_path = fixture_root() / "native" / "tensor-prep.native.pt";
    fs::create_directories(output_path.parent_path());

    mmltk::rfdetr::NativeCheckpoint checkpoint;
    checkpoint.metadata.preset_name = "tensor-prep";
    checkpoint.metadata.source_kind = "unit-test";
    checkpoint.metadata.source_path = output_path.string();
    checkpoint.metadata.num_classes = 1;

    const auto cpu_contiguous = torch::arange(12, torch::TensorOptions().dtype(torch::kFloat32)).view({3, 4}).clone();
    const auto cpu_non_contiguous = cpu_contiguous.transpose(0, 1);
    checkpoint.state_dict.push_back({"cpu_contiguous", cpu_contiguous});
    checkpoint.state_dict.push_back({"cpu_non_contiguous", cpu_non_contiguous});
    const bool has_cuda = torch::cuda::is_available();
    if (has_cuda) {
        checkpoint.state_dict.push_back(
            {"cuda_tensor",
             torch::arange(6, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA)).view({2, 3})});
    }

    mmltk::rfdetr::save_native_checkpoint(output_path, checkpoint);
    const auto loaded = mmltk::rfdetr::load_checkpoint(output_path);

    const auto* loaded_contiguous = find_entry(loaded, "cpu_contiguous");
    assert(loaded_contiguous != nullptr);
    assert(loaded_contiguous->tensor.device().is_cpu());
    assert(loaded_contiguous->tensor.is_contiguous());
    assert(torch::equal(loaded_contiguous->tensor, cpu_contiguous));

    const auto* loaded_non_contiguous = find_entry(loaded, "cpu_non_contiguous");
    assert(loaded_non_contiguous != nullptr);
    assert(loaded_non_contiguous->tensor.device().is_cpu());
    assert(loaded_non_contiguous->tensor.is_contiguous());
    assert(torch::equal(loaded_non_contiguous->tensor, cpu_non_contiguous.contiguous()));

    const auto* loaded_cuda = find_entry(loaded, "cuda_tensor");
    assert((loaded_cuda != nullptr) == has_cuda);
    if (loaded_cuda != nullptr) {
        const auto expected = checkpoint.state_dict.back().tensor.detach().to(torch::Device(torch::kCPU)).contiguous();
        assert(loaded_cuda->tensor.device().is_cpu());
        assert(loaded_cuda->tensor.is_contiguous());
        assert(torch::equal(loaded_cuda->tensor, expected));
    }
}

void test_upstream_checkpoint_scalar_type_bridge() {
    const fs::path upstream_path = fixture_root() / "upstream" / "rf-detr-nano-dtype-bridge.pth";
    save_upstream_python_checkpoint(
        upstream_path,
        {
            {"query_feat.weight",
             torch::ones({4, kParityFixtureHiddenDim}, torch::TensorOptions().dtype(torch::kFloat16))},
            {"refpoint_embed.weight", torch::zeros({4, 4}, torch::TensorOptions().dtype(torch::kFloat32))},
            {"class_embed.weight", torch::ones({kParityFixtureNumClasses, kParityFixtureHiddenDim},
                                               torch::TensorOptions().dtype(torch::kFloat32))},
            {"class_embed.bias", torch::arange(kParityFixtureNumClasses, torch::TensorOptions().dtype(torch::kInt64))},
        });

    const auto checkpoint = mmltk::rfdetr::load_checkpoint(upstream_path);
    const auto* query_feat = find_entry(checkpoint, "query_feat.weight");
    const auto* class_bias = find_entry(checkpoint, "class_embed.bias");
    assert(query_feat != nullptr);
    assert(class_bias != nullptr);
    assert(query_feat->tensor.scalar_type() == torch::kFloat16);
    assert(class_bias->tensor.scalar_type() == torch::kInt64);
}

void test_legacy_native_checkpoint_format_support() {
    const fs::path legacy_path = fixture_root() / "legacy" / "legacy-format.pt";
    write_legacy_native_checkpoint(legacy_path);

    assert(mmltk::rfdetr::is_native_checkpoint_file(legacy_path));
    const auto checkpoint = mmltk::rfdetr::load_checkpoint(legacy_path);
    assert(checkpoint.metadata.preset_name == "rf-detr-seg-medium");
    assert(checkpoint.metadata.source_kind == "legacy-native-test");
    assert(checkpoint.metadata.num_classes == 7);
    assert(checkpoint.state_dict.size() == 1);

    const auto* class_bias = find_entry(checkpoint, "class_embed.bias");
    assert(class_bias != nullptr);
    assert(torch::equal(class_bias->tensor, torch::arange(7, torch::TensorOptions().dtype(torch::kFloat32))));
}

}  // namespace

void test_checkpoint_roundtrip_and_fixture_loading() {
    mmltk::testsupport::remove_path_recursively_best_effort(fixture_root());
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
    mmltk::testsupport::remove_path_recursively_best_effort(fixture_root());
}

void test_checkpoint_tensor_and_legacy_support() {
    test_native_checkpoint_tensor_preparation();
    test_upstream_checkpoint_scalar_type_bridge();
    test_legacy_native_checkpoint_format_support();
}

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][checkpoint]", test_checkpoint_roundtrip_and_fixture_loading);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][checkpoint]", test_checkpoint_tensor_and_legacy_support);
