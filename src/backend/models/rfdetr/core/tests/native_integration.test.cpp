
#include "src/backend/models/rfdetr/core/model_state.h"
// RF-DETR core integration coverage.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "catch2_compat.hpp"
#include "subprocess_test_utils.hpp"

// Import-bearing support follows every textual standard-library and POSIX test helper.
#include "asset_cache_support.h"
#include "checkpoint_fixture_support.h"
#include "filesystem_test_utils.hpp"
#include "model_state_access.h"
#include "model_state_technical.h"
#include "parity_fixture_support.h"

namespace fs = std::filesystem;

namespace {

using mmltk::backend::models::rfdetr::testsupport::kParityFixtureHiddenDim;
using mmltk::backend::models::rfdetr::testsupport::kParityFixtureNumClasses;
using mmltk::backend::models::rfdetr::testsupport::log_fixture_phase;
using mmltk::backend::models::rfdetr::testsupport::parity_fixture_cases;
using mmltk::backend::models::rfdetr::testsupport::ParityFixtureCase;
using mmltk::backend::models::rfdetr::testsupport::write_minimal_upstream_checkpoint;

fs::path mmltk_cli_path() {
    const fs::path cli_path = mmltk::testsupport::mmltk_cli_path();
    if (!fs::exists(cli_path)) { throw std::runtime_error("configured mmltk path does not exist: " + cli_path.string()); }
    return cli_path;
}

std::string command_string(const std::vector<std::string>& args) {
    std::string command;
    for (size_t index = 0; index < args.size(); ++index) {
        if (index > 0) { command.push_back(' '); }
        command += args[index];
    }
    return command;
}

void run_subprocess(const std::vector<std::string>& args) {
    const auto result = mmltk::testsupport::run_subprocess_capture_output(args);
    INFO("command: " << command_string(args) << "\nexit status: " << result.exit_code << "\n" << result.output_text);
    REQUIRE(result.exit_code == 0);
}

void assert_native_checkpoint(const fs::path& path, const char* expected_preset) {
    const bool path_exists = fs::exists(path);
    REQUIRE(path_exists);
    REQUIRE(mmltk::backend::models::rfdetr::is_native_checkpoint_file(path));

    const auto checkpoint = mmltk::backend::models::rfdetr::decode_model_state(path);
    REQUIRE(checkpoint.metadata.preset_name == expected_preset);
    REQUIRE(checkpoint.metadata.source_kind == "upstream-python");
    REQUIRE(checkpoint.metadata.num_classes == kParityFixtureNumClasses);
    REQUIRE(checkpoint.metadata.num_queries > 0);
    REQUIRE(checkpoint.metadata.num_select > 0);
    const auto& entries = mmltk::backend::models::rfdetr::detail::model_state_owner(checkpoint).entries;
    REQUIRE(!entries.empty());

    bool found_query_feat = false;
    bool found_class_embed = false;
    for (const auto& entry : entries) {
        if (entry.name == "query_feat.weight") { found_query_feat = true; }
        if (entry.name == "class_embed.weight") {
            found_class_embed = true;
            REQUIRE(entry.tensor.size(0) == kParityFixtureNumClasses);
            REQUIRE(entry.tensor.size(1) == kParityFixtureHiddenDim);
        }
    }
    REQUIRE(found_query_feat);
    REQUIRE(found_class_embed);
}

void test_native_rfdetr_cli_checkpoint_smoke() {
    const mmltk::testsupport::ScopedTempDir temp_dir("mmltk_rfdetr_native_integration");
    const fs::path& root = temp_dir.path();
    const fs::path cli_path = mmltk_cli_path();

    const auto& fixtures = parity_fixture_cases();
    for (size_t index = 0; index < fixtures.size(); ++index) {
        const auto& fixture = fixtures[index];
        log_fixture_phase("test_rfdetr_native_integration", index + 1, fixtures.size(), "normalize", fixture.preset_name);
        const fs::path upstream_path = root / "weights" / fixture.upstream_filename;
        const fs::path native_path = root / "weights" / (std::string(fixture.preset_name) + ".native.pt");

        write_minimal_upstream_checkpoint(upstream_path, fixture);
        run_subprocess({
            cli_path.string(),
            "rfdetr",
            "normalize-weights",
            "--input",
            upstream_path.string(),
            "--output",
            native_path.string(),
        });
        assert_native_checkpoint(native_path, fixture.preset_name);
    }
}

void test_native_rfdetr_cached_nano_export_pipeline() {
    const fs::path cli_path = mmltk_cli_path();
    const auto assets = mmltk::backend::models::rfdetr::testsupport::ensure_cached_model_assets("rf-detr-nano");

    REQUIRE(fs::exists(assets.upstream_weights_path));
    REQUIRE(fs::exists(assets.native_checkpoint_path));
    REQUIRE(fs::exists(assets.onnx_path));
    REQUIRE(fs::exists(assets.tensorrt_path));
    assert_native_checkpoint(assets.native_checkpoint_path, "rf-detr-nano");

    REQUIRE(assets.onnx_metadata_validated);
    run_subprocess({
        cli_path.string(),
        "rfdetr",
        "info",
        "--tensorrt",
        assets.tensorrt_path.string(),
    });
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_integration][cli][integration]", test_native_rfdetr_cli_checkpoint_smoke);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][native_integration][cli][integration]", test_native_rfdetr_cached_nano_export_pipeline);
