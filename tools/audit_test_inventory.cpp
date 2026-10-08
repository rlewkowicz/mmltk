module;
// Audit tracked native tests against CMake ownership and the all-suite inventory.
#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
export module mmltk.tools.audit_test_inventory;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace {
using namespace mmltk::tools;
using Names = std::set<std::string>;
void usage(std::ostream& out, std::string_view program) {
 out << "usage: " << program
     << " [-h] --compile-database COMPILE_DATABASE --repo-root REPO_ROOT\n"
        "       --expected-targets EXPECTED_TARGETS\n";
}
std::optional<std::array<std::string, 3>> parse_arguments(int argc, char** argv) {
 constexpr std::array<std::string_view, 4> options{"--compile-database", "--repo-root", "--expected-targets", "--help"};
 std::array<std::string, 3> values;
 std::array<bool, 3> supplied{};
 for (int index = 1; index < argc; ++index) {
  const std::string token = argv[index];
  const auto equals = token.find('=');
  const std::string name = token.substr(0, equals);
  std::optional<std::size_t> selected;
  for (std::size_t candidate = 0; candidate < options.size(); ++candidate) {
   if ((name.starts_with("--") && name != "--" && options[candidate].starts_with(name)) || (name == "-h" && candidate == 3)) {
    if (selected) throw ToolError("ambiguous option: " + name, 2);
    selected = candidate;
   }
  }
  if (!selected) throw ToolError("unrecognized arguments: " + token, 2);
  if (*selected == 3) {
   if (equals != std::string::npos) throw ToolError("argument --help: ignored explicit argument", 2);
   usage(std::cout, fs::path(argv[0]).filename().string());
   std::cout << "\nAudit tracked native test sources, configured CMake targets, and the mmltk all-suite inventory.\n\n"
                "options:\n  -h, --help            show this help message and exit\n"
                "  --compile-database COMPILE_DATABASE\n"
                "  --repo-root REPO_ROOT\n"
                "  --expected-targets EXPECTED_TARGETS\n";
   return std::nullopt;
  }
  if (equals != std::string::npos)
   values[*selected] = token.substr(equals + 1);
  else {
   if (index + 1 == argc || (std::string_view(argv[index + 1]).starts_with('-') && std::string_view(argv[index + 1]) != "-"))
    throw ToolError("argument " + std::string(options[*selected]) + ": expected one argument", 2);
   values[*selected] = argv[++index];
  }
  supplied[*selected] = true;
 }
 std::string missing;
 for (std::size_t index = 0; index < supplied.size(); ++index) {
  if (!supplied[index]) {
   if (!missing.empty()) missing += ", ";
   missing += options[index];
  }
 }
 if (!missing.empty()) throw ToolError("the following arguments are required: " + missing, 2);
 return values;
}
Names tracked_test_sources(const fs::path& root) {
 RunOptions options;
 options.capture_stdout = true;
 const auto result = run({"git", "-C", root.string(), "ls-files", "-z", "--", "src"}, options);
 if (result.exit_code != 0) throw ToolError("git ls-files failed with " + (result.signal ? "signal " + std::to_string(result.signal) : "exit status " + std::to_string(result.exit_code)));
 Names sources;
 constexpr std::string_view suffixes[]{".test.c", ".test.cc", ".test.cpp", ".test.cxx", ".test.cu"};
 for (const auto& path : split(result.out, '\0', false)) {
  for (const auto suffix : suffixes) {
   if (path.ends_with(suffix)) {
    sources.insert(path);
    break;
   }
  }
 }
 return sources;
}
int audit(const std::array<std::string, 3>& args) {
 const auto root = fs::weakly_canonical(fs::absolute(args[1]));
 Names expected_targets;
 std::istringstream words(args[2]);
 for (std::string word; words >> word;) expected_targets.insert(std::move(word));
 const auto sources = tracked_test_sources(root);
 std::map<std::string, Names> source_targets;
 Names graph_targets;
 const std::regex target_pattern(R"((?:^|/)CMakeFiles/([^/]+)\.dir/)");
 const auto commands = read_json(args[0]);
 if (!commands.is_array()) throw ToolError("compile database must contain an array");
 for (const auto& command : commands) {
  const auto output = command.value("output", std::string{});
  std::smatch match;
  if (!std::regex_search(output, match, target_pattern)) continue;
  const auto target = match[1].str();
  graph_targets.insert(target);
  // Match Path.resolve(): relative filenames use the invocation directory,
  // deliberately not the compilation entry's directory field.
  const auto path = fs::weakly_canonical(fs::absolute(command.at("file").get<std::string>()));
  const auto relative = path.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..") continue;
  const auto source = relative.generic_string();
  if (sources.contains(source)) source_targets[source].insert(target);
 }
 Names missing_sources;
 std::vector<std::string> multiply_owned;
 Names discovered;
 for (const auto& source : sources)
  if (!source_targets.contains(source)) missing_sources.insert(source);
 for (const auto& [source, targets] : source_targets) {
  if (targets.size() == 1)
   discovered.insert(*targets.begin());
  else {
   std::string line = source + ": ";
   for (auto target = targets.begin(); target != targets.end(); ++target) {
    if (target != targets.begin()) line += ", ";
    line += *target;
   }
   multiply_owned.push_back(std::move(line));
  }
 }
 const auto difference = [](const Names& left, const Names& right) {
  Names result;
  for (const auto& name : left)
   if (!right.contains(name)) result.insert(name);
  return result;
 };
 bool failed = false;
 const auto finding = [&failed](std::string_view title, const auto& entries) {
  if (entries.empty()) return;
  if (!failed) std::cerr << "mmltk-test-inventory: FAILED\n";
  failed = true;
  std::cerr << "mmltk-test-inventory: " << title << ":\n  ";
  bool first = true;
  for (const auto& entry : entries) {
   if (!first) std::cerr << "\n  ";
   first = false;
   std::cerr << entry;
  }
  std::cerr << '\n';
 };
 finding("tracked test sources missing from the configured compile graph", missing_sources);
 finding("test sources without exactly one owning CMake target", multiply_owned);
 finding("configured test targets missing from the mmltk all-suite inventory", difference(discovered, expected_targets));
 finding("mmltk all-suite targets missing from the configured CMake graph", difference(expected_targets, graph_targets));
 finding("mmltk all-suite targets without a tracked *.test source", difference(expected_targets, discovered));
 if (failed) return 1;
 std::cout << "mmltk-test-inventory: " << sources.size() << " tracked test sources map one-to-one into " << discovered.size() << " configured all-suite executables\n";
 return 0;
}
}  // namespace
export int mmltk_tool_audit_test_inventory(int argc, char** argv) {
 try {
  const auto args = parse_arguments(argc, argv);
  return args ? audit(*args) : 0;
 } catch (const mmltk::tools::ToolError& error) {
  if (error.status == 2) usage(std::cerr, std::filesystem::path(argv[0]).filename().string());
  std::cerr << "mmltk-test-inventory: " << error.what() << '\n';
  return error.status;
 } catch (const std::exception& error) {
  std::cerr << "mmltk-test-inventory: " << error.what() << '\n';
  return 1;
 }
}
