#include <stdexcept>
// RF-DETR command spelling coverage.
#include <string>
#include <vector>

#include "catch2_compat.hpp"
#include "subprocess_test_utils.hpp"

namespace {

using namespace mmltk::testsupport;

SubprocessResult run_train_options(std::initializer_list<const char*> options);

void test_evaluate_aliases_require_compiled() {
    const std::string cli_path = mmltk_cli_path();
    for (const char* alias : {"evaluate", "eval", "val"}) {
        const SubprocessResult result = run_subprocess_capture_output({
            cli_path,
            "rfdetr",
            alias,
        });
        MMLTK_ASSERT(result.exit_code == 1);
        MMLTK_ASSERT(result.output_text.find("rfdetr evaluate requires --compiled") != std::string::npos);
    }
}

void test_validate_still_routes_to_validate() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "validate",
    });
    MMLTK_ASSERT(result.exit_code == 1);
    MMLTK_ASSERT(result.output_text.find("rfdetr validate requires --compiled") != std::string::npos);
}

void test_validate_help_lists_recompile_compile_options() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "validate",
        "--help",
    });
    MMLTK_ASSERT(result.exit_code == 0);
    MMLTK_ASSERT(result.output_text.find("--recompile") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("--compile-workers") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("--compile-cuda-mask-batch-size") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("--compile-cuda-device-id") != std::string::npos);
}

void test_top_level_help_lists_primary_commands() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--help",
    });
    MMLTK_ASSERT(result.exit_code == 0);
    MMLTK_ASSERT(result.output_text.find("compile") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("bench") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("info") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("rfdetr") != std::string::npos);
}

void test_predict_help_lists_model_inputs() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "predict",
        "--help",
    });
    MMLTK_ASSERT(result.exit_code == 0);
    MMLTK_ASSERT(result.output_text.find("--compiled") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("--output") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("--weights") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("--onnx") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("--tensorrt") != std::string::npos);
}

// CLEANUP-IGNORE -- each CLI scenario keeps its command and expected diagnostic adjacent.
void test_info_requires_exactly_one_model_input() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "info",
    });
    MMLTK_ASSERT(result.exit_code == 1);
    MMLTK_ASSERT(result.output_text.find("rfdetr info requires exactly one of --onnx or --tensorrt") != std::string::npos);
}

// CLEANUP-IGNORE -- each CLI scenario keeps its command and expected diagnostic adjacent.
void test_normalize_weights_requires_paths() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "normalize-weights",
    });
    MMLTK_ASSERT(result.exit_code == 1);
    MMLTK_ASSERT(result.output_text.find("rfdetr normalize-weights requires --input and --output") != std::string::npos);
}

void test_train_help_lists_optimizer_controls() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "train",
        "--help",
    });
    MMLTK_ASSERT(result.exit_code == 0);
    MMLTK_ASSERT(result.output_text.find("--optimizer") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("--momentum") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("--warmup-momentum") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("adamw or muon") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("fused AdamW backend") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("AdamW only") != std::string::npos);
    const auto single = result.output_text.find("--device-id");
    const auto multiple = result.output_text.find("--device-ids");
    MMLTK_ASSERT(single != std::string::npos);
    MMLTK_ASSERT(multiple != std::string::npos);
    MMLTK_ASSERT(single < multiple);
}

void test_train_help_lists_canonical_supervision_controls() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "train",
        "--help",
    });
    MMLTK_ASSERT(result.exit_code == 0);
    for (const char* option : {
             "--assignment",
             "--match-free-rho",
             "--match-free-correspondence-weight",
             "--match-free-query-weight",
             "--dn",
             "--no-dn",
             "--dn-groups",
             "--dn-label-noise-ratio",
             "--dn-center-noise-scale",
             "--dn-size-noise-scale",
         }) {
        MMLTK_ASSERT(result.output_text.find(option) != std::string::npos);
    }
    MMLTK_ASSERT(result.output_text.find("hungarian or match-free") != std::string::npos);
}

void test_train_assignment_spellings_parse_through_the_canonical_descriptor() {
    for (const char* assignment : {"hungarian", "match-free"}) {
        const auto result = run_train_options({"--assignment", assignment});
        MMLTK_ASSERT(result.exit_code == 1);
        MMLTK_ASSERT(result.output_text.find("requires --train-compiled") != std::string::npos);
    }
    const auto invalid = run_train_options({"--assignment", "MatchFree"});
    MMLTK_ASSERT(invalid.exit_code == 1);
    MMLTK_ASSERT(invalid.output_text.find("--assignment") != std::string::npos);
}

void test_train_supervision_values_parse_as_one_nested_configuration() {
    const auto enabled = run_train_options({
        "--assignment",
        "match-free",
        "--match-free-rho",
        "0.99999994",
        "--match-free-correspondence-weight",
        "0.75",
        "--match-free-query-weight",
        "1.25",
        "--dn",
        "--dn-groups",
        "10",
        "--dn-label-noise-ratio",
        "0.3",
        "--dn-center-noise-scale",
        "1e-45",
        "--dn-size-noise-scale",
        "0.99999994",
    });
    MMLTK_ASSERT(enabled.exit_code == 1);
    MMLTK_ASSERT(enabled.output_text.find("requires --train-compiled") != std::string::npos);

    const auto disabled = run_train_options({"--assignment", "hungarian", "--no-dn"});
    MMLTK_ASSERT(disabled.exit_code == 1);
    MMLTK_ASSERT(disabled.output_text.find("requires --train-compiled") != std::string::npos);
}

SubprocessResult run_train_options(std::initializer_list<const char*> options) {
    std::vector<std::string> arguments{mmltk_cli_path(), "rfdetr", "train"};
    arguments.insert(arguments.end(), options.begin(), options.end());
    return run_subprocess_capture_output(arguments);
}

void test_train_device_grammar_and_conflicts_are_preserved() {
    for (const auto options : {
             std::initializer_list<const char*>{"--device-id", "0"},
             std::initializer_list<const char*>{"--device-ids", "0,2"},
         }) {
        const auto result = run_train_options(options);
        MMLTK_ASSERT(result.exit_code == 1);
        MMLTK_ASSERT(result.output_text.find("requires --train-compiled") != std::string::npos);
    }
    for (const auto options : {
             std::initializer_list<const char*>{"--device-id", "0", "--device-ids", "0,2"},
             std::initializer_list<const char*>{"--device-ids", "0,2", "--device-id", "0"},
         }) {
        const auto result = run_train_options(options);
        MMLTK_ASSERT(result.exit_code == 1);
        MMLTK_ASSERT(result.output_text.find("accepts only one of --device-id or --device-ids") != std::string::npos);
    }
    for (const auto value : {"0,,2", "0,0", "-1", "zero", "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16"}) {
        const auto result = run_train_options({"--device-ids", value});
        MMLTK_ASSERT(result.exit_code == 1);
        MMLTK_ASSERT(result.output_text.find("--device-ids") != std::string::npos);
    }
    const auto duplicate = run_train_options({"--device-id", "0", "--device-id", "0"});
    MMLTK_ASSERT(duplicate.output_text.find("duplicate option") != std::string::npos);
    const auto unknown = run_train_options({"--unknown-train-option"});
    MMLTK_ASSERT(unknown.output_text.find("unknown option") != std::string::npos);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli]", test_evaluate_aliases_require_compiled);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli]", test_validate_still_routes_to_validate);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli]", test_validate_help_lists_recompile_compile_options);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli]", test_top_level_help_lists_primary_commands);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli]", test_predict_help_lists_model_inputs);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli]", test_info_requires_exactly_one_model_input);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli]", test_normalize_weights_requires_paths);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli]", test_train_help_lists_optimizer_controls);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli][training_supervision]", test_train_help_lists_canonical_supervision_controls);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli][training_supervision]",
                         test_train_assignment_spellings_parse_through_the_canonical_descriptor);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli][training_supervision]",
                         test_train_supervision_values_parse_as_one_nested_configuration);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][cli_aliases][cli]", test_train_device_grammar_and_conflicts_are_preserved);

TEST_CASE("compiled image commands expose opt-in GDRCopy and reject the removed H2D flag", "[cli][transport]") {
    for (const std::string command : {"bench", "train", "evaluate", "validate", "predict"}) {
        std::vector<std::string> arguments{mmltk_cli_path()};
        if (command != "bench") arguments.emplace_back("rfdetr");
        arguments.push_back(command);
        arguments.emplace_back("--help");
        const auto help = run_subprocess_capture_output(arguments);
        REQUIRE(help.exit_code == 0);
        CHECK(help.output_text.find("--gdrcopy") != std::string::npos);
        CHECK(help.output_text.find("--h2d-dataloader") == std::string::npos);
        arguments.back() = "--h2d-dataloader";
        const auto removed = run_subprocess_capture_output(arguments);
        CHECK(removed.exit_code != 0);
        CHECK(removed.output_text.find("unknown option") != std::string::npos);
    }
}
