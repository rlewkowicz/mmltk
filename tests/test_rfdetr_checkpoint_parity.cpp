#include "fastloader/rfdetr/model.h"
#include "fastloader/rfdetr/model_config.h"
#include "test_rfdetr_checkpoint_fixture_support.h"
#include "test_rfdetr_parity_fixture_support.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using fastloader::rfdetr::ModelOutputs;
using fastloader::rfdetr::NativeRfDetrConfig;
using fastloader::rfdetr::NativeRfDetrModel;
using fastloader::rfdetr::OutputLayer;
using fastloader::rfdetr::StateDictLoadSummary;
using fastloader::rfdetr::find_model_preset;
using fastloader::rfdetr::nested_tensor_from_tensor_list;
using fastloader::rfdetr::resolve_upstream_weight_artifacts;
using fastloader::rfdetr::testsupport::ParityFixtureCase;
using fastloader::rfdetr::testsupport::log_fixture_phase;
using fastloader::rfdetr::testsupport::make_fixture_image;
using fastloader::rfdetr::testsupport::parity_fixture_cases;
using fastloader::rfdetr::testsupport::write_module_upstream_checkpoint;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name_prefix) {
        std::string pattern = (fs::temp_directory_path() / (std::string(name_prefix) + ".XXXXXX")).string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        }
        path_ = fs::path(created);
    }

    ~ScopedTempDir() {
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }

    const fs::path& path() const {
        return path_;
    }

private:
    fs::path path_;
};

NativeRfDetrConfig config_for_fixture(const ParityFixtureCase& fixture) {
    const auto* preset = find_model_preset(fixture.preset_name);
    if (preset == nullptr) {
        throw std::runtime_error(std::string("missing model preset for parity fixture: ") + fixture.preset_name);
    }

    NativeRfDetrConfig config;
    config.preset_name = std::string(preset->preset_name);
    config.resolution = preset->resolution;
    config.segmentation = preset->segmentation_head;
    config.num_classes = preset->num_classes;
    config.num_queries = preset->num_queries;
    config.num_select = preset->num_select;
    config.dec_layers = preset->dec_layers;
    config.group_detr = preset->group_detr;
    config.two_stage = preset->two_stage;
    config.hidden_dim = preset->hidden_dim;
    config.patch_size = preset->patch_size;
    config.num_windows = preset->num_windows;
    config.positional_encoding_size = preset->positional_encoding_size;
    return config;
}

std::string shape_string(const torch::Tensor& tensor) {
    std::ostringstream stream;
    stream << "[";
    for (int64_t index = 0; index < tensor.dim(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << tensor.size(index);
    }
    stream << "]";
    return stream.str();
}

void assert_clean_summary(const StateDictLoadSummary& summary, const std::string& label) {
    if (summary.missing_names.empty() && summary.unexpected_names.empty() && summary.incompatible_names.empty()) {
        return;
    }

    std::ostringstream message;
    message << label << " state_dict mismatch";
    if (!summary.missing_names.empty()) {
        message << " missing=" << summary.missing_names.front();
        if (summary.missing_names.size() > 1) {
            message << " +" << (summary.missing_names.size() - 1);
        }
    }
    if (!summary.unexpected_names.empty()) {
        message << " unexpected=" << summary.unexpected_names.front();
        if (summary.unexpected_names.size() > 1) {
            message << " +" << (summary.unexpected_names.size() - 1);
        }
    }
    if (!summary.incompatible_names.empty()) {
        message << " incompatible=" << summary.incompatible_names.front();
        if (summary.incompatible_names.size() > 1) {
            message << " +" << (summary.incompatible_names.size() - 1);
        }
    }
    throw std::runtime_error(message.str());
}

void assert_tensor_bitwise_equal(const torch::Tensor& actual,
                                 const torch::Tensor& expected,
                                 const std::string& label) {
    if (actual.sizes() != expected.sizes()) {
        throw std::runtime_error(
            label + " shape mismatch: actual=" + shape_string(actual) + " expected=" + shape_string(expected));
    }

    const auto actual_cpu = actual.detach().cpu().contiguous();
    const auto expected_cpu = expected.detach().cpu().to(actual_cpu.scalar_type()).contiguous();
    if (torch::equal(actual_cpu, expected_cpu)) {
        return;
    }

    const auto abs_diff = (actual_cpu - expected_cpu).abs();
    const double max_abs = abs_diff.max().item<double>();
    throw std::runtime_error(label + " differs at the bitwise level: max_abs=" + std::to_string(max_abs));
}

void assert_output_layer_bitwise_equal(const OutputLayer& actual,
                                       const OutputLayer& expected,
                                       const std::string& label) {
    assert_tensor_bitwise_equal(actual.pred_logits, expected.pred_logits, label + ".pred_logits");
    assert_tensor_bitwise_equal(actual.pred_boxes, expected.pred_boxes, label + ".pred_boxes");
    if (actual.pred_masks.has_value() != expected.pred_masks.has_value()) {
        throw std::runtime_error(label + ".pred_masks presence mismatch");
    }
    if (actual.pred_masks.has_value()) {
        assert_tensor_bitwise_equal(*actual.pred_masks, *expected.pred_masks, label + ".pred_masks");
    }
}

void assert_outputs_bitwise_equal(const ModelOutputs& actual,
                                  const ModelOutputs& expected,
                                  const char* preset_name) {
    assert_output_layer_bitwise_equal(actual.main, expected.main, std::string(preset_name) + ".main");
    if (actual.aux_outputs.size() != expected.aux_outputs.size()) {
        throw std::runtime_error(std::string(preset_name) + ".aux_outputs size mismatch");
    }
    for (size_t index = 0; index < actual.aux_outputs.size(); ++index) {
        assert_output_layer_bitwise_equal(actual.aux_outputs[index],
                                          expected.aux_outputs[index],
                                          std::string(preset_name) + ".aux[" + std::to_string(index) + "]");
    }
    if (actual.enc_outputs.has_value() != expected.enc_outputs.has_value()) {
        throw std::runtime_error(std::string(preset_name) + ".enc_outputs presence mismatch");
    }
    if (actual.enc_outputs.has_value()) {
        assert_output_layer_bitwise_equal(*actual.enc_outputs,
                                          *expected.enc_outputs,
                                          std::string(preset_name) + ".enc");
    }
}

void run_checkpoint_parity_case(const ParityFixtureCase& fixture, size_t index, size_t total) {
    const ScopedTempDir temp_dir("fastloader_rfdetr_checkpoint_parity");
    const fs::path upstream_path = temp_dir.path() / "weights" / fixture.upstream_filename;
    const fs::path native_path = temp_dir.path() / "weights" / (std::string(fixture.preset_name) + ".native.pt");

    log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "seed", fixture.preset_name);
    torch::manual_seed(fixture.query_rows + fixture.input_size);
    NativeRfDetrModel seeded_model(config_for_fixture(fixture));
    seeded_model.eval();
    write_module_upstream_checkpoint(upstream_path, seeded_model);

    log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "normalize", fixture.preset_name);
    const auto normalized = fastloader::rfdetr::normalize_checkpoint_to_native(upstream_path, native_path);
    if (!fs::exists(native_path)) {
        throw std::runtime_error("failed to write native checkpoint parity fixture");
    }
    if (normalized.metadata.preset_name != fixture.preset_name) {
        throw std::runtime_error("normalized checkpoint preset mismatch for parity fixture");
    }

    const auto artifacts = resolve_upstream_weight_artifacts(upstream_path);
    NativeRfDetrModel upstream_model(artifacts.config);
    NativeRfDetrModel native_model(artifacts.config);

    log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "load", fixture.preset_name);
    assert_clean_summary(upstream_model.load_weights(upstream_path, true), "upstream");
    assert_clean_summary(native_model.load_weights(native_path, true), "native");

    upstream_model.eval();
    native_model.eval();
    const auto image = make_fixture_image(fixture);

    log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "forward", fixture.preset_name);
    const auto upstream_outputs = upstream_model.forward(nested_tensor_from_tensor_list({image.clone()}));
    const auto native_outputs = native_model.forward(nested_tensor_from_tensor_list({image.clone()}));
    assert_outputs_bitwise_equal(upstream_outputs, native_outputs, fixture.preset_name);
}

const ParityFixtureCase* find_fixture_by_name(const std::string& preset_name) {
    const auto& fixtures = parity_fixture_cases();
    const auto it = std::find_if(fixtures.begin(), fixtures.end(), [&](const ParityFixtureCase& fixture) {
        return preset_name == fixture.preset_name;
    });
    return it == fixtures.end() ? nullptr : &*it;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::fprintf(stderr, "usage: %s [preset-name]\n", argv[0]);
        return 1;
    }

    try {
        if (argc == 2) {
            const auto* fixture = find_fixture_by_name(argv[1]);
            if (fixture == nullptr) {
                throw std::runtime_error(std::string("unknown parity fixture preset: ") + argv[1]);
            }
            run_checkpoint_parity_case(*fixture, 1, 1);
            return 0;
        }

        const auto& fixtures = parity_fixture_cases();
        for (size_t index = 0; index < fixtures.size(); ++index) {
            run_checkpoint_parity_case(fixtures[index], index + 1, fixtures.size());
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_rfdetr_checkpoint_parity error: %s\n", error.what());
        return 1;
    }
}
