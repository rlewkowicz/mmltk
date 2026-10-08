module;
// Repeat one generated native link into independent diagnostic storage.
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <initializer_list>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
export module mmltk.tools.diagnose_native_link;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace {
using namespace mmltk::tools;
struct Arguments {
 std::string target;
 std::string graph = "release";
 bool save_temps = true;
 std::string linker_directory;
 std::vector<std::string> trace_symbols;
};
void usage(std::ostream& stream, std::string_view program) {
 stream << "usage: " << program
        << " [-h] [--graph {release,analysis}] [--save-temps | --no-save-temps]\n"
           "       [--linker-directory LINKER_DIRECTORY] [--trace-symbol TRACE_SYMBOL] target\n";
}
[[noreturn]] void argument_error(std::string message) { throw ToolError(std::move(message), 2); }
std::optional<Arguments> parse_arguments(int argc, char** argv) {
 Arguments args;
 bool positional_only = false;
 bool have_target = false;
 constexpr std::string_view options[] = {"--help", "--graph", "--save-temps", "--no-save-temps", "--linker-directory", "--trace-symbol"};
 for (int index = 1; index < argc; ++index) {
  std::string token = argv[index];
  if (!positional_only && token == "--") {
   positional_only = true;
   continue;
  }
  if (!positional_only && token.starts_with('-') && token != "-") {
   const auto equals = token.find('=');
   std::string option = token.substr(0, equals);
   if (option == "-h") option = "--help";
   if (option.starts_with("--")) {
    std::vector<std::string_view> matches;
    for (const auto candidate : options) {
     if (candidate == option) {
      matches = {candidate};
      break;
     }
     if (candidate.starts_with(option)) matches.push_back(candidate);
    }
    if (matches.size() > 1) argument_error("ambiguous option: " + option);
    if (matches.size() == 1)
     option = matches.front();
    else
     argument_error("unrecognized arguments: " + token);
   } else {
    argument_error("unrecognized arguments: " + token);
   }
   if (option == "--help" || option == "--save-temps" || option == "--no-save-temps") {
    if (equals != std::string::npos) argument_error("argument " + option + ": ignored explicit argument");
    if (option == "--help") {
     usage(std::cout, fs::path(argv[0]).filename().string());
     std::cout << "\nRepeat one generated native link into independent diagnostic storage.\n\n"
                  "positional arguments:\n  target                Existing native executable target\n\n"
                  "options:\n  -h, --help            show this help message and exit\n"
                  "  --graph {release,analysis}\n                        Cached CMake graph\n"
                  "  --save-temps, --no-save-temps\n                        Retain LTO intermediates\n"
                  "  --linker-directory LINKER_DIRECTORY\n"
                  "                        Repository-relative directory containing a candidate ld.mold\n"
                  "  --trace-symbol TRACE_SYMBOL\n                        Trace one mangled symbol (repeatable)\n";
     return std::nullopt;
    }
    args.save_temps = option == "--save-temps";
    continue;
   }
   std::string value;
   if (equals != std::string::npos)
    value = token.substr(equals + 1);
   else {
    if (index + 1 == argc || (std::string_view(argv[index + 1]).starts_with('-') && std::string_view(argv[index + 1]) != "-")) argument_error("argument " + option + ": expected one argument");
    value = argv[++index];
   }
   if (option == "--graph") {
    if (value != "release" && value != "analysis") argument_error("argument --graph: invalid choice: " + value + " (choose from release, analysis)");
    args.graph = std::move(value);
   } else if (option == "--linker-directory")
    args.linker_directory = std::move(value);
   else
    args.trace_symbols.push_back(std::move(value));
  } else {
   if (have_target) argument_error("unrecognized arguments: " + token);
   args.target = std::move(token);
   have_target = true;
  }
 }
 if (!have_target) argument_error("the following arguments are required: target");
 return args;
}
std::string checked_output(const std::vector<std::string>& command, const fs::path& cwd = {}) {
 RunOptions options;
 options.cwd = cwd;
 options.capture_stdout = true;
 const auto result = run(command, options);
 if (result.exit_code != 0) throw ToolError(command.front() + " failed with " + (result.signal ? "signal " + std::to_string(result.signal) : "exit status " + std::to_string(result.exit_code)));
 return result.out;
}
std::string find_linker() {
 // shutil.which uses the platform default search path when PATH is unset.
 const auto path = env("PATH", "/bin:/usr/bin");
 if (path.empty()) return {};
 std::set<fs::path> visited;
 for (const auto& directory : split(path, ':')) {
  if (!visited.insert(directory).second) continue;
  const fs::path candidate = fs::path(directory) / "ld.mold";
  if (fs::exists(candidate) && !fs::is_directory(candidate) && ::access(candidate.c_str(), X_OK) == 0) return candidate.string();
 }
 return {};
}
int diagnose(const Arguments& args) {
 constexpr std::string_view name_characters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";
 if (args.target.empty() || args.target.find_first_not_of(name_characters) != std::string::npos) argument_error("expected an ordinary native target name");
 const fs::path workspace = "/workspace";
 fs::path linker;
 if (!args.linker_directory.empty()) {
  const fs::path relative = args.linker_directory;
  linker = fs::weakly_canonical(workspace / relative);
  if (relative.is_absolute() || !mmltk::tools::path_within(linker, workspace) || !fs::is_regular_file(linker / "ld.mold"))
   argument_error("linker directory must remain within the repository and contain ld.mold");
  if (!mmltk::tools::path_within(fs::canonical(linker / "ld.mold"), workspace)) argument_error("candidate linker must remain within the repository");
 }
 for (const auto& symbol : args.trace_symbols) {
  if (symbol.empty() || symbol.find_first_not_of(std::string(name_characters) + ".$@") != std::string::npos) argument_error("expected a mangled symbol without linker option separators");
 }
 const auto graph = workspace / ".cache/cmake" / args.graph;
 std::string rule;
 const auto prefix = "build " + args.target + ": ";
 for (const auto& line : lines(read_text(graph / "build.ninja"))) {
  if (!line.starts_with(prefix)) continue;
  rule = line.substr(prefix.size(), line.find(' ', prefix.size()) - prefix.size());
  break;
 }
 if (!rule.starts_with("CXX_EXECUTABLE_LINKER__")) argument_error("target has no generated C++ executable link");
 const auto entries = json::parse(checked_output({"ninja", "-t", "compdb", rule}, graph));
 std::vector<std::string> command;
 bool found = false;
 for (const auto& entry : entries) {
  if (entry.at("output").get<std::string>() != args.target) continue;
  command = shell_words(entry.at("command").get<std::string>());
  found = true;
  break;
 }
 if (!found) throw ToolError("generated compilation database has no output for " + args.target);
 if (command.size() < 4 || command[0] != ":" || command[1] != "&&" || command[command.size() - 2] != "&&" || command.back() != ":") argument_error("unsupported generated link command shape");
 command.erase(command.end() - 2, command.end());
 command.erase(command.begin(), command.begin() + 2);
 if (command.empty() || command.front() != "/opt/gcc-16.2/bin/g++" || std::any_of(command.begin(), command.end(), [](const auto& token) { return token == "&&" || token == ";" || token == "|"; }))
  argument_error("expected one direct GCC link command");
 const fs::path output = "/diagnostic/output";
 const auto output_option = std::find(command.begin(), command.end(), "-o");
 if (output_option == command.end() || std::next(output_option) == command.end()) throw ToolError("generated link command has no output following -o");
 *std::next(output_option) = (output / args.target).string();
 for (auto& token : command) {
  if (token.starts_with("-Wl,--dependency-file=")) token = "-Wl,--dependency-file=" + (output / "link.d").string();
 }
 if (args.save_temps) {
  const auto inputs = output / "inputs";
  if (!fs::create_directory(inputs)) throw ToolError("diagnostic inputs directory already exists: " + inputs.string());
  std::map<fs::path, fs::path> borrowed;
  for (auto& token : command) {
   auto source = graph / token;
   if (token.starts_with('-') || !fs::is_regular_file(source)) continue;
   if (source.extension() == ".a" || source.extension() == ".o") {
    const auto [entry, inserted] = borrowed.try_emplace(source, inputs / (std::to_string(borrowed.size()) + "-" + source.filename().string()));
    if (inserted) fs::create_symlink(source, entry->second);
    source = entry->second;
   }
   token = source.string();
  }
  command.push_back("-save-temps=obj");
 }
 command.push_back("-Wl,-Map," + (output / "link.map").string());
 if (!linker.empty()) command.insert(command.begin() + 1, "-B" + linker.string() + "/");
 for (const auto& symbol : args.trace_symbols) command.push_back("-Wl,--trace-symbol=" + symbol);
 const auto selected_linker = linker.empty() ? find_linker() : (linker / "ld.mold").string();
 if (selected_linker.empty()) argument_error("the development image has no ld.mold");
 const auto compiler_version = lines(checked_output({command.front(), "--version"}));
 if (compiler_version.empty()) throw ToolError("compiler version output is empty");
 const json versions = {{"compiler", compiler_version.front()}, {"linker", trim(checked_output({selected_linker, "--version"}))}};
 const auto directory = args.save_temps ? output : graph;
 write_json(output / "command.json", {{"graph", graph.string()}, {"directory", directory.string()}, {"arguments", command}, {"versions", versions}});
 std::cout << json({{"owner", "native-link"}, {"event", "toolchain"}, {"compiler", versions.at("compiler")}, {"linker", versions.at("linker")}}).dump() << std::endl;
 RunOptions options;
 options.cwd = directory;
 const auto result = run(command, options);
 if (result.exec_errno) return 1;
 // Python subprocess returns negative signal numbers; sys.exit preserves
 // their low eight bits as the diagnostic helper's process exit status.
 return result.signal ? (256 - result.signal) : result.exit_code;
}
}  // namespace
export int mmltk_tool_diagnose_native_link(int argc, char** argv) {
 try {
  const auto args = parse_arguments(argc, argv);
  return args ? diagnose(*args) : 0;
 } catch (const mmltk::tools::ToolError& error) {
  if (error.status == 2) usage(std::cerr, std::filesystem::path(argv[0]).filename().string());
  std::cerr << std::filesystem::path(argv[0]).filename().string() << ": error: " << error.what() << '\n';
  return error.status;
 } catch (const std::exception& error) {
  std::cerr << "native-link: " << error.what() << '\n';
  return 1;
 }
}
