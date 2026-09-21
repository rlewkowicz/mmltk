#include "src/entrypoints/cli/tests/support/cli_path.h"
#include <filesystem>
#include <system_error>
#include <stdexcept>
#include "src/common/system/runtime_paths.h"
namespace mmltk::testsupport {
std::string mmltk_cli_path() {
#ifndef MMLTK_TEST_MMLTK_CLI_PATH
#error "MMLTK_TEST_MMLTK_CLI_PATH must be defined for CLI subprocess tests"
#endif
 const std::filesystem::path configured_path = MMLTK_TEST_MMLTK_CLI_PATH;
 std::error_code error;
 if (std::filesystem::exists(configured_path, error) && !error) { return configured_path.string(); }
 std::filesystem::path executable_path;
 try {
  executable_path = mmltk::common::system::runtime_paths::current_executable_path();
 } catch (const std::runtime_error&) {
  // Discovery failure leaves the installed and configured fallbacks available.
 }
 if (!executable_path.empty()) {
  const std::filesystem::path sibling_cli = executable_path.parent_path() / "mmltk";
  error.clear();
  if (std::filesystem::exists(sibling_cli, error) && !error) { return sibling_cli.string(); }
 }
 const std::filesystem::path installed_cli = "/opt/mmltk/bin/mmltk";
 error.clear();
 if (std::filesystem::exists(installed_cli, error) && !error) { return installed_cli.string(); }
 return configured_path.string();
}
}  // namespace mmltk::testsupport
