#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::cli_support {

struct TestBundleSpec {
    const char* name;
    const char* executable_name;
    const char* description;
};

struct BundledTestBundle {
    const TestBundleSpec* spec = nullptr;
    std::filesystem::path executable_path;
};

struct ParsedTestRequest {
    bool requested = false;
    std::string bundle_name;
    std::vector<std::string> forwarded_args;
};

inline constexpr std::array<TestBundleSpec, 3> kBundledTestBundleSpecs{{
    {"core", "mmltk_tests_core", "Core dataset/runtime/compiler Catch2 suite"},
    {"gui", "mmltk_tests_gui", "GUI state/options/runtime Catch2 suite"},
    {"rfdetr", "mmltk_tests_model_rfdetr", "RF-DETR native/model Catch2 suite"},
}};

ParsedTestRequest parse_test_request(int argc, char** argv);
std::vector<BundledTestBundle> discover_bundled_test_bundles();
std::string bundled_test_help_text(const std::vector<BundledTestBundle>& bundles);
void print_bundled_test_bundles(FILE* stream, const std::vector<BundledTestBundle>& bundles);
int handle_bundled_test_request(const ParsedTestRequest& request, const std::vector<BundledTestBundle>& bundles);

}  // namespace mmltk::cli_support
