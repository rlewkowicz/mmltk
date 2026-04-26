#include "support/catch2_compat.hpp"
#include "support/subprocess_test_utils.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace mmltk::testsupport;

void test_evaluate_aliases_require_compiled() {
    const std::string cli_path = mmltk_cli_path();
    for (const char* alias : {"evaluate", "eval", "val"}) {
        const SubprocessResult result = run_subprocess_capture_output({
            cli_path,
            "rfdetr",
            alias,
        });
        assert(result.exit_code == 1);
        assert(result.output_text.find("rfdetr evaluate requires --compiled") != std::string::npos);
    }
}

void test_validate_still_routes_to_validate() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "validate",
    });
    assert(result.exit_code == 1);
    assert(result.output_text.find("rfdetr validate requires --compiled") != std::string::npos);
}

void test_validate_help_lists_recompile_compile_options() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "validate",
        "--help",
    });
    assert(result.exit_code == 0);
    assert(result.output_text.find("--recompile") != std::string::npos);
    assert(result.output_text.find("--compile-workers") != std::string::npos);
    assert(result.output_text.find("--compile-cuda-mask-batch-size") != std::string::npos);
    assert(result.output_text.find("--compile-cuda-device-id") != std::string::npos);
}

void test_top_level_help_lists_primary_commands() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--help",
    });
    assert(result.exit_code == 0);
    assert(result.output_text.find("compile") != std::string::npos);
    assert(result.output_text.find("bench") != std::string::npos);
    assert(result.output_text.find("info") != std::string::npos);
    assert(result.output_text.find("rfdetr") != std::string::npos);
}

void test_predict_help_lists_model_inputs() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "predict",
        "--help",
    });
    assert(result.exit_code == 0);
    assert(result.output_text.find("--compiled") != std::string::npos);
    assert(result.output_text.find("--output") != std::string::npos);
    assert(result.output_text.find("--weights") != std::string::npos);
    assert(result.output_text.find("--onnx") != std::string::npos);
    assert(result.output_text.find("--tensorrt") != std::string::npos);
}

void test_info_requires_exactly_one_model_input() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "info",
    });
    assert(result.exit_code == 1);
    assert(result.output_text.find("rfdetr info requires exactly one of --onnx or --tensorrt") != std::string::npos);
}

void test_normalize_weights_requires_paths() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "normalize-weights",
    });
    assert(result.exit_code == 1);
    assert(result.output_text.find("rfdetr normalize-weights requires --input and --output") != std::string::npos);
}

void test_train_help_lists_optimizer_controls() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "rfdetr",
        "train",
        "--help",
    });
    assert(result.exit_code == 0);
    assert(result.output_text.find("--optimizer") != std::string::npos);
    assert(result.output_text.find("--momentum") != std::string::npos);
    assert(result.output_text.find("--warmup-momentum") != std::string::npos);
    assert(result.output_text.find("adamw or muon") != std::string::npos);
    assert(result.output_text.find("fused AdamW backend") != std::string::npos);
    assert(result.output_text.find("AdamW only") != std::string::npos);
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
