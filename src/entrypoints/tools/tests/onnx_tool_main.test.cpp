#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>
#include "src/test_support/async_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include "detail/onnx_tool_main.h"
#include "src/test_support/filesystem_test_utils.hpp"
import mmltk.common.logging.mmltk_logging;
namespace {
std::filesystem::path captured_model_path;
void capture_model_path(const std::filesystem::path& path) { captured_model_path = path; }
constexpr mmltk::entrypoints::tools::OnnxToolMainConfig kTestConfig{
    .usage = "usage: mmltk-rfdetr-onnx-info [logging options] MODEL.onnx",
    .application_name = "mmltk-rfdetr-onnx-info-test",
    .logger_name = "onnx-info-test",
    .error_prefix = "onnx-info-test: ",
};
[[nodiscard]] int run_onnx_tool(std::vector<std::string> arguments, mmltk::entrypoints::tools::OnnxToolOperation operation = &capture_model_path) {
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) argv.push_back(argument.data());
    mmltk::common::logging::initialize(mmltk::common::logging::default_config("onnx-test"));
    captured_model_path.clear();
    return mmltk::entrypoints::tools::run_onnx_tool_main(static_cast<int>(argv.size()), argv.data(), kTestConfig, operation);
}
void test_onnx_tool_main_routes_one_model_after_logging_options() {
    CHECK(run_onnx_tool({"mmltk-rfdetr-onnx-info", "--log-level=off", "--log-file", "/tmp/mmltk-onnx-info-test.log", "--log-dir", "/tmp", "/tmp/model.onnx"}) ==
          0);
    CHECK(captured_model_path == "/tmp/model.onnx");
}
void test_onnx_tool_main_rejects_a_missing_model() {
    CHECK(run_onnx_tool({"mmltk-rfdetr-onnx-info", "--log-level", "off"}) == 1);
    CHECK(captured_model_path.empty());
}
std::size_t error_text_reads = 0U;
struct ObservedFailure final : std::exception {
    const char* what() const noexcept override {
        ++error_text_reads;
        return "observed model operation failure";
    }
};
void fail_model_operation(const std::filesystem::path& path) {
    captured_model_path = path;
    throw ObservedFailure{};
}
void test_onnx_fatal_errors_remain_visible_with_diagnostics_off() {
    namespace logging = mmltk::common::logging;
    const mmltk::testsupport::ScopedTempDir root("mmltk_onnx_failure");
    const auto sink = root.path() / "failure.log";
    // An empty level resets an explicit choice to inherited configuration.
    // Clear inherited logging for the default branch and restore it on exit.
    const std::array names{"MMLTK_LOG_LEVEL", "MMLTK_LOG_FILE", "MMLTK_LOG_DIR"};
    std::array<std::optional<std::string>, 3> inherited;
    const mmltk::testsupport::ScopedTestCleanup restore([&] {
        logging::initialize(logging::default_config("onnx-test"));
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (inherited[i])
                ::setenv(names[i], inherited[i]->c_str(), 1);
            else
                ::unsetenv(names[i]);
        }
    });
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (const char* value = std::getenv(names[i])) inherited[i] = value;
        REQUIRE(::unsetenv(names[i]) == 0);
    }
    for (const bool explicit_off : {false, true}) {
        std::vector<std::string> arguments{"onnx-test", "/tmp/model.onnx"};
        if (explicit_off) {
            arguments.push_back("--log-level=off");
            arguments.push_back("--log-file=" + sink.string());
        }
        error_text_reads = 0U;
        CHECK(run_onnx_tool(std::move(arguments), &fail_model_operation) == 1);
        CHECK(captured_model_path == "/tmp/model.onnx");
        CHECK(error_text_reads == 1U);
        CHECK_FALSE(std::filesystem::exists(sink));
    }
    error_text_reads = 0U;
    CHECK(run_onnx_tool({"onnx-test", "--log-level=info", "--log-file=" + sink.string(), "/tmp/model.onnx"}, &fail_model_operation) == 1);
    logging::flush();
    CHECK(error_text_reads == 1U);
    std::ifstream input(sink);
    REQUIRE(input.is_open());
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(text.find("[onnx-info-test]") != std::string::npos);
    CHECK(text.find("onnx-info-test: observed model operation failure") != std::string::npos);
}
}  // namespace
TEST_CASE("test_onnx_tool_main_routes_one_model_after_logging_options", "[entrypoints][tools][onnx]") {
    test_onnx_tool_main_routes_one_model_after_logging_options();
}
TEST_CASE("test_onnx_tool_main_rejects_a_missing_model", "[entrypoints][tools][onnx]") { test_onnx_tool_main_rejects_a_missing_model(); }
TEST_CASE("test_onnx_fatal_errors_remain_visible_with_diagnostics_off", "[entrypoints][tools][onnx]") { test_onnx_fatal_errors_remain_visible_with_diagnostics_off(); }
