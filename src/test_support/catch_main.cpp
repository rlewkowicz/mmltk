#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
import mmltk.common.logging.mmltk_logging;
namespace {
class TestProgressListener final : public Catch::EventListenerBase {
public:
 using EventListenerBase::EventListenerBase;
 void testCaseStarting(const Catch::TestCaseInfo& test_info) override {
  std::fprintf(stderr, "[ RUN      ] %s\n", test_info.name.c_str());
  std::fflush(stderr);
 }
 void testCaseEnded(const Catch::TestCaseStats& test_stats) override {
  const bool failed = test_stats.aborting || test_stats.totals.testCases.failed != 0U;
  const char* outcome = failed ? "[  FAILED  ]" : test_stats.totals.testCases.skipped != 0U ? "[  SKIPPED ]" : "[       OK ]";
  std::fprintf(stderr, "%s %s\n", outcome, test_stats.testInfo->name.c_str());
  std::fflush(stderr);
 }
};
bool is_logging_option(std::string_view arg, std::string_view name) {
 const std::string prefix = std::string(name) + "=";
 return arg == name || (arg.size() >= prefix.size() && arg.compare(0, prefix.size(), prefix) == 0);
}
std::vector<std::string> filter_logging_args(int argc, char** argv) {
 std::vector<std::string> filtered;
 filtered.reserve(static_cast<size_t>(argc));
 if (argc > 0 && argv[0] != nullptr) { filtered.emplace_back(argv[0]); }
 for (int index = 1; index < argc; ++index) {
  const std::string_view arg = argv[index];
  if (is_logging_option(arg, "--log-level") || is_logging_option(arg, "--log-file") || is_logging_option(arg, "--log-dir")) {
   if ((arg == "--log-level" || arg == "--log-file" || arg == "--log-dir") && index + 1 < argc) { ++index; }
   continue;
  }
  filtered.emplace_back(argv[index]);
 }
 return filtered;
}
}  // namespace
CATCH_REGISTER_LISTENER(TestProgressListener)
int main(int argc, char** argv) {
 try {
  const std::string app_name = argc > 0 && argv[0] != nullptr ? std::filesystem::path(argv[0]).filename().string() : std::string("mmltk_tests");
  const mmltk::common::logging::CliOverrides overrides = mmltk::common::logging::scan_cli_overrides(argc, argv);
  mmltk::common::logging::initialize(mmltk::common::logging::merge(mmltk::common::logging::config_from_env(app_name), overrides));
  const std::vector<std::string> filtered_args = filter_logging_args(argc, argv);
  std::vector<char*> raw_args;
  raw_args.reserve(filtered_args.size());
  for (const std::string& arg : filtered_args) { raw_args.push_back(const_cast<char*>(arg.c_str())); }
  Catch::Session session;
  return session.run(static_cast<int>(raw_args.size()), raw_args.data());
 } catch (const std::exception& error) {
  std::fprintf(stderr, "catch main error: %s\n", error.what());
  return 1;
 }
}
