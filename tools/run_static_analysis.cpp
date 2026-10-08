module;
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <initializer_list>
#include <optional>
#include <array>
#include <chrono>
#include <climits>
#include <cstring>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
export module mmltk.tools.run_static_analysis;
import mmltk.tools.cpp_support;
import mmltk.tools.tool_command;
import mmltk.tools.cpp_text;
namespace {
using namespace mmltk::tools;
using Arguments = std::vector<std::string>;
// Keep project selection ready for a cppcheck version that implements the
// repository's C++26 reflection syntax. This is deliberately not an option.
constexpr bool cppcheck_enabled = false;
constexpr std::string_view default_checks =
 "-*,clang-analyzer-*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,"
 "bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-macro-parentheses,"
 "performance-*,portability-*,-portability-avoid-pragma-once,-portability-simd-intrinsics,"
 "modernize-use-auto,modernize-use-using";
std::string nonempty_env(std::string_view name, std::string_view fallback = {}) {
 const auto value = env(name);
 return value.empty() ? std::string(fallback) : value;
}
void set_environment(const char* name, const std::string& value) {
 if (::setenv(name, value.c_str(), 1) != 0) throw std::system_error(errno, std::generic_category(), "setenv");
}
class Output {
public:
 void setup(const std::string& repo_root) {
  auto log_path = env("MMLTK_TIDY_LOG_FILE");
  if (log_path.empty()) {
   const auto now = std::time(nullptr);
   std::tm local{};
   if (!::localtime_r(&now, &local)) throw ToolError("cannot obtain tidy log timestamp");
   char stamp[32]{};
   if (!std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local)) throw ToolError("cannot format tidy log timestamp");
   log_path = repo_root + "/build/logs/mmltk-tidy-" + stamp + ".log";
  } else if (log_path.front() != '/') {
   log_path = repo_root + '/' + log_path;
  }
  set_environment("MMLTK_TIDY_LOG_FILE", log_path);
  if (nonempty_env("MMLTK_TIDY_TEE_STARTED", "0") != "1") {
   const auto parent = fs::path(log_path).parent_path();
   if (!parent.empty()) fs::create_directories(parent);
   log_file_.reset(::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666));
   if (log_file_.get() < 0) throw std::system_error(errno, std::generic_category(), "open " + log_path);
   set_environment("MMLTK_TIDY_TEE_STARTED", "1");
  }
  log("writing tidy log to " + log_path);
  log("tail with: tail -f " + log_path);
 }
 void write(std::string_view text, bool error = false) {
  const std::lock_guard lock(mutex_);
  // The script's own tee merges stderr into stdout. When the outer
  // wrapper has already started capture, preserve its separate streams.
  mmltk::tools::write_all(error && log_file_.get() < 0 ? STDERR_FILENO : STDOUT_FILENO, text, "write tidy output");
  if (log_file_.get() >= 0) mmltk::tools::write_all(log_file_.get(), text, "write tidy output");
 }
 void log(std::string_view text) { write("mmltk-tidy: " + std::string(text) + '\n', true); }

private:
 FileDescriptor log_file_;
 std::mutex mutex_;
};
static_assert(std::atomic_int::is_always_lock_free);
std::atomic_int termination_requested{0};
std::atomic_int termination_write_fd{-1};
extern "C" void request_termination(int signal) {
 const int saved_errno = errno;
 termination_requested.store(signal, std::memory_order_relaxed);
 const unsigned char byte = 1;
 const int descriptor = termination_write_fd.load(std::memory_order_relaxed);
 if (descriptor >= 0) {
  const auto ignored = ::write(descriptor, &byte, sizeof(byte));
  static_cast<void>(ignored);
 }
 errno = saved_errno;
}
class Termination {
public:
 Termination() {
  int descriptors[2];
  if (::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) throw std::system_error(errno, std::generic_category(), "pipe tidy cancellation");
  reader_.reset(descriptors[0]);
  writer_.reset(descriptors[1]);
  termination_requested = 0;
  termination_write_fd = writer_.get();
  struct sigaction action{};
  action.sa_handler = request_termination;
  ::sigemptyset(&action.sa_mask);
  if (::sigaction(SIGINT, &action, &old_int_) != 0) {
   termination_write_fd = -1;
   throw std::system_error(errno, std::generic_category(), "install tidy SIGINT handler");
  }
  if (::sigaction(SIGTERM, &action, &old_term_) != 0) {
   const int error = errno;
   ::sigaction(SIGINT, &old_int_, nullptr);
   termination_write_fd = -1;
   throw std::system_error(error, std::generic_category(), "install tidy SIGTERM handler");
  }
 }
 ~Termination() {
  ::sigaction(SIGINT, &old_int_, nullptr);
  ::sigaction(SIGTERM, &old_term_, nullptr);
  termination_write_fd = -1;
 }
 Termination(const Termination&) = delete;
 Termination& operator=(const Termination&) = delete;
 int descriptor() const { return reader_.get(); }
 void check() const {
  if (termination_requested.load(std::memory_order_relaxed)) throw ToolError("received termination; stopping static-analysis workers", 130);
 }

private:
 FileDescriptor reader_;
 FileDescriptor writer_;
 struct sigaction old_int_{};
 struct sigaction old_term_{};
};
enum class Capture { Live, Stdout, Silent, File };
class Commands {
public:
 Commands(Output& output, const Termination& termination) : output_(output), termination_(termination) {}
 ProcessResult invoke(const Arguments& arguments, const fs::path& cwd = {}, Capture capture = Capture::Live, const fs::path& output_file = {}) const {
  termination_.check();
  RunOptions options;
  options.cwd = cwd;
  options.new_process_group = true;
  options.cancel_fd = termination_.descriptor();
  options.cancel_signal = [] { return SIGTERM; };
  FileDescriptor spool;
  if (capture == Capture::File) {
   spool.reset(::open(output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
   if (spool.get() < 0) throw std::system_error(errno, std::generic_category(), "open " + output_file.string());
   options.merge_stderr = true;
   options.stdout_sink = [&](std::string_view text) { mmltk::tools::write_all(spool.get(), text, "write tidy output"); };
  } else {
   if (capture == Capture::Live)
    options.stdout_sink = [&](std::string_view text) { output_.write(text); };
   else
    options.capture_stdout = true;
   if (capture == Capture::Silent)
    options.capture_stderr = true;
   else
    options.stderr_sink = [&](std::string_view text) { output_.write(text, true); };
  }
  auto result = run(arguments, options);
  termination_.check();
  return result;
 }
 std::string require(const Arguments& arguments, const fs::path& cwd = {}, Capture capture = Capture::Live) const {
  auto result = invoke(arguments, cwd, capture);
  if (result.exit_code) throw ToolError({}, result.exit_code);
  return std::move(result.out);
 }

private:
 Output& output_;
 const Termination& termination_;
};
template <class Function>
void parallel(std::size_t count, std::size_t workers, Function operation) {
 std::atomic_size_t next{0};
 std::atomic_bool stop{false};
 std::exception_ptr failure;
 std::mutex failure_mutex;
 std::vector<std::jthread> threads;
 threads.reserve(std::min(count, workers));
 for (std::size_t worker = 0; worker < std::min(count, workers); ++worker) {
  threads.emplace_back([&] {
   try {
    while (!stop.load(std::memory_order_relaxed)) {
     const auto index = next.fetch_add(1, std::memory_order_relaxed);
     if (index >= count) break;
     if (!operation(index)) stop.store(true, std::memory_order_relaxed);
    }
   } catch (...) {
    stop.store(true, std::memory_order_relaxed);
    const std::lock_guard lock(failure_mutex);
    if (!failure) failure = std::current_exception();
   }
  });
 }
 // Destruction joins all in-flight commands before rethrowing or deleting
 // their output files. Cancellation stays readable for every child waiter.
 threads.clear();
 if (failure) std::rethrow_exception(failure);
}
void usage(Output& output) {
 output.write(R"(Usage: mmltk --tool run-static-analysis -- [--tools-only] [--third_party] [--file <source>...] [--start-at <translation-unit>] [--rust-only] [--cppcheck-only]

Runs the Docker-backed static-analysis pass for this repo:
1. Formats tracked first-party C/C++/CUDA files and, on full passes, the owned Rust frontend
2. Refreshes the cached Docker-side Ninja analysis compile database
3. Runs clang-tidy over tracked first-party C/C++/CUDA translation units
4. Reports cppcheck disabled while its parser lacks C++26 reflection support

Options:
  --tools-only                  Analyze the standalone native tools graph
  --third_party                 Include tracked third_party sources except Firefox
  --file <source>...            Format a space-delimited set of tracked C/C++/CUDA
                                sources or modules and analyze their translation units
  --start-at <translation-unit>  Resume analysis from the given file in
                                 compile-database order
  --cppcheck-only                Unavailable while cppcheck lacks C++26 reflection support
  --rust-only                    Format only the first-party Iced Rust package
  --help                         Show this help

Environment:
  MMLTK_REPO_ROOT         Repo root mounted inside the container
  MMLTK_TIDY_BUILD_DIR    Build dir for the Docker-side compile database
  MMLTK_CLANG_FORMAT      clang-format binary override
  MMLTK_CLANG_TIDY        clang-tidy binary override
  MMLTK_CLANG_TIDY_CHECKS Override the clang-tidy check filter
)");
}
struct Options {
 std::string repo_root = nonempty_env("MMLTK_REPO_ROOT", fs::current_path().string());
 std::string build_dir = nonempty_env("MMLTK_TIDY_BUILD_DIR", repo_root + "/.cache/cmake/analysis");
 std::string start_at;
 Arguments files;
 bool cppcheck_only = false;
 bool rust_only = false;
 bool third_party = false;
 bool tools_only = false;
};
bool parse(Options& options, int argc, char** argv, Output& output) {
 for (int index = 1; index < argc; ++index) {
  const std::string_view argument(argv[index]);
  if (argument == "--help" || argument == "-h") {
   usage(output);
   return false;
  }
  if (argument == "--start-at") {
   if (++index == argc) throw ToolError("--start-at requires a file path");
   options.start_at = argv[index];
  } else if (argument == "--file") {
   if (!options.files.empty()) throw ToolError("--file may only be specified once");
   if (++index == argc) throw ToolError("--file requires a source path");
   for (; index < argc && !std::string_view(argv[index]).starts_with("--"); ++index) options.files.emplace_back(argv[index]);
   if (options.files.empty()) throw ToolError("--file requires at least one source path");
   --index;
  } else if (argument == "--cppcheck-only") {
   options.cppcheck_only = true;
  } else if (argument == "--rust-only") {
   options.rust_only = true;
  } else if (argument == "--tools-only") {
   options.tools_only = true;
  } else if (argument == "--third_party") {
   options.third_party = true;
  } else {
   throw ToolError("unsupported argument: " + std::string(argument));
  }
 }
 if (!options.files.empty() && !options.start_at.empty()) throw ToolError("--file and --start-at cannot be combined");
 if (options.cppcheck_only && !cppcheck_enabled) throw ToolError("--cppcheck-only is unavailable because cppcheck 2.21 cannot parse C++26 reflection");
 if (options.tools_only && (options.rust_only || options.third_party)) throw ToolError("--tools-only cannot select Rust or third_party sources");
 if (options.rust_only && (!options.files.empty() || options.third_party || !options.start_at.empty())) throw ToolError("--rust-only cannot be combined with native analysis selections");
 return true;
}
std::string normalize_repo_path(std::string candidate, const std::string& repo_root) {
 if (candidate.starts_with("/host")) candidate.erase(0, 5);
 const auto prefix = (repo_root.starts_with("/host") ? repo_root.substr(5) : repo_root) + '/';
 if (candidate.starts_with(prefix)) return candidate.substr(prefix.size());
 if (candidate.starts_with('/')) throw ToolError("path '" + candidate + "' is outside " + repo_root);
 if (candidate.starts_with("./")) candidate.erase(0, 2);
 return candidate;
}
bool executable_available(const std::string& name) {
 const auto executable = [](const fs::path& path) {
  std::error_code error;
  return ::access(path.c_str(), X_OK) == 0 && !fs::is_directory(path, error);
 };
 if (name.find('/') != std::string::npos) return executable(name);
 for (const auto& directory : split(env("PATH"), ':'))
  if (executable(fs::path(directory) / name)) return true;
 return false;
}
void format_rust(const Options& options, const Commands& commands, Output& output) {
 if (!executable_available("cargo")) throw ToolError("cargo is unavailable; rebuild the analysis image");
 output.log("running cargo fmt over the first-party Iced frontend");
 commands.require({"cargo", "fmt", "--manifest-path", options.repo_root + "/src/frontend/iced/Cargo.toml", "--package", "mmltk-browser-app"});
}
const std::unordered_set<std::string> format_extensions{".c", ".cc", ".cpp", ".cppm", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp", ".inl", ".inc", ".cu", ".cuh"};
const std::unordered_set<std::string> host_extensions{".c", ".cc", ".cpp", ".cppm", ".cxx"};
void normalize_targets(Options& options, const Commands& commands) {
 Arguments normalized;
 std::unordered_set<std::string> seen;
 for (const auto& supplied : options.files) {
  auto file = normalize_repo_path(supplied, options.repo_root);
  if (std::none_of(format_extensions.begin(), format_extensions.end(), [&](const auto& extension) { return file.ends_with(extension); }))
   throw ToolError("--file requires a C/C++/CUDA source, module, or header");
  if (options.tools_only && !file.starts_with("tools/")) throw ToolError("--tidy-tools requires a source under tools/");
  if (file.starts_with("third_party/firefox/")) throw ToolError("--file does not support owned Firefox sources");
  if (!options.third_party && file.starts_with("third_party/")) throw ToolError("--file requires --third_party for third_party sources");
  if (commands.invoke({"git", "-C", options.repo_root, "ls-files", "--error-unmatch", "--", file}, {}, Capture::Silent).exit_code) throw ToolError("--file target '" + file + "' is not tracked");
  if (seen.insert(file).second) normalized.push_back(std::move(file));
 }
 options.files = std::move(normalized);
}
Arguments tracked_files(const Options& options, const Commands& commands, bool headers) {
 Arguments arguments{"git", "-C", options.repo_root, "ls-files", "-z", "--", "*.c", "*.cc", "*.cpp", "*.cppm", "*.cxx"};
 if (headers) arguments.insert(arguments.end(), {"*.h", "*.hh", "*.hpp", "*.hxx", "*.ipp", "*.inl", "*.inc"});
 arguments.emplace_back("*.cu");
 if (headers) arguments.emplace_back("*.cuh");
 arguments.emplace_back(":(exclude)third_party/firefox/**");
 if (!options.third_party) arguments.emplace_back(":(exclude)third_party/**");
 auto files = split(commands.require(arguments, {}, Capture::Stdout), '\0', false);
 if (options.tools_only) std::erase_if(files, [](const auto& path) { return !path.starts_with("tools/"); });
 return files;
}
Arguments remove_deleted(Arguments files, const Options& options, const Commands& commands) {
 const auto deleted = split(commands.require({"git", "-C", options.repo_root, "ls-files", "-z", "--deleted"}, {}, Capture::Stdout), '\0', false);
 const std::unordered_set<std::string> removed(deleted.begin(), deleted.end());
 std::erase_if(files, [&](const auto& file) { return removed.contains(file); });
 return files;
}
std::size_t worker_count(const Commands& commands) {
 const auto value = trim(commands.require({"nproc"}, {}, Capture::Stdout));
 std::size_t consumed = 0;
 const auto count = std::stoull(value, &consumed);
 if (!count || consumed != value.size()) throw ToolError("nproc did not report a positive worker count");
 return static_cast<std::size_t>(count);
}
void format_native(const Options& options, const Commands& commands, Output& output, const std::string& formatter, std::size_t workers) {
 const auto files = remove_deleted(options.files.empty() ? tracked_files(options, commands, true) : options.files, options, commands);
 if (files.empty()) throw ToolError("no tracked C/C++/CUDA source files found");
 output.log("running clang-format (" + formatter + ") over " + std::to_string(files.size()) + " tracked C/C++/CUDA files with " + std::to_string(workers) + " workers");
 std::atomic_int status{0};
 parallel(files.size(), workers, [&](std::size_t index) {
  const auto result = commands.invoke({formatter, "-i", "--style=file", files[index]}, options.repo_root);
  if (!result.exit_code) return true;
  const int code = result.exec_errno ? result.exit_code : result.signal ? 125 : result.exit_code == 255 ? 124 : 123;
  int expected = 0;
  if (code == 123)
   status.compare_exchange_strong(expected, code);
  else
   status.store(code);
  return code == 123;
 });
 if (status.load()) throw ToolError({}, status.load());
}
Arguments command_arguments(const json& entry) {
 if (entry.contains("arguments")) return entry.at("arguments").get<Arguments>();
 return shell_words(entry.at("command").get_ref<const std::string&>());
}
// Keep memory proportional to one compile entry plus selected first-party
// entries, rather than loading the (potentially large) vendor database.
template <class Function>
void compile_entries(const fs::path& path, Function visit) {
 std::ifstream stream(path, std::ios::binary);
 if (!stream) throw ToolError("cannot read " + path.string());
 stream >> std::ws;
 if (stream.get() != '[') throw ToolError(path.string() + " is not a JSON array");
 while (true) {
  stream >> std::ws;
  const int next = stream.peek();
  if (next == ']') return;
  if (next == std::char_traits<char>::eof()) {
   if (stream.bad()) throw ToolError("read failed: " + path.string());
   return;
  }
  if (next == ',') {
   stream.get();
   continue;
  }
  if (next != '{') throw ToolError("compile database entry is not an object: " + path.string());
  std::string entry;
  std::size_t depth = 0;
  mmltk::tools::text::JsonStringState string;
  char ch;
  do {
   if (!stream.get(ch)) throw ToolError("incomplete compile database entry: " + path.string());
   entry += ch;
   if (string.consume(ch)) continue;
   if (ch == '{' || ch == '[') {
    ++depth;
   } else if (ch == '}' || ch == ']') {
    --depth;
   }
  } while (depth || string.quoted());
  visit(json::parse(entry));
 }
}
std::string relative_within(const fs::path& path, const fs::path& root) {
 const auto relative = path.lexically_relative(root);
 if (relative.empty() || (relative.begin() != relative.end() && *relative.begin() == "..")) return {};
 return relative.generic_string();
}
void collect_nvcc_removals(const Arguments& arguments, std::set<std::string>& removed) {
 static const std::unordered_set<std::string> standalone{
  "-forward-unknown-to-host-compiler", "--forward-unknown-to-host-compiler", "--allow-unsupported-compiler", "-allow-unsupported-compiler", "-lineinfo", "--Werror", "-Werror", "all-warnings"
 };
 static const std::unordered_set<std::string> valued{"-ccbin", "--compiler-bindir", "-Xcompiler", "--compiler-options", "--generate-code", "-gencode", "-rdc", "--relocatable-device-code"};
 for (std::size_t index = 1; index < arguments.size(); ++index) {
  const auto& argument = arguments[index];
  const auto equals = argument.find('=');
  if (standalone.contains(argument) || (equals != std::string::npos && valued.contains(argument.substr(0, equals)))) {
   removed.insert(argument);
  } else if (valued.contains(argument)) {
   removed.insert(argument);
   if (index + 1 < arguments.size()) removed.insert(arguments[++index]);
  }
 }
}
struct Selection {
 Arguments clang_units;
 Arguments cppcheck_units;
 json cppcheck_commands = json::array();
 Arguments reflection_units;
 std::set<std::string> reflection_targets;
 std::map<std::string, std::set<std::string>> reflection_objects;
 std::set<std::string> cuda_removed_args;
};
Selection select_units(const Options& options, const Commands& commands, const fs::path& database, const Termination& termination) {
 const auto root = fs::weakly_canonical(fs::absolute(options.repo_root));
 const auto compile_db = fs::weakly_canonical(fs::absolute(database));
 const auto tracked = remove_deleted(tracked_files(options, commands, false), options, commands);
 std::unordered_set<std::string> eligible(tracked.begin(), tracked.end());
 if (!options.files.empty()) {
  const std::unordered_set<std::string> selected(options.files.begin(), options.files.end());
  std::erase_if(eligible, [&](const auto& file) { return !selected.contains(file); });
 }
 Selection selection;
 std::unordered_set<std::string> seen_clang;
 std::unordered_set<std::string> seen_cppcheck;
 compile_entries(compile_db, [&](const json& entry) {
  termination.check();
  // Match pathlib's original resolve(): relative file names are relative
  // to the invocation directory, not the entry's command directory.
  const auto file = fs::weakly_canonical(fs::absolute(entry.at("file").get<std::string>()));
  const auto relative = relative_within(file, root);
  if (!eligible.contains(relative)) return;
  auto extension = file.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  const auto arguments = command_arguments(entry);
  if (host_extensions.contains(extension) && seen_cppcheck.insert(relative).second) {
   selection.cppcheck_units.push_back(relative);
   selection.cppcheck_commands.push_back(entry);
  }
  if (std::find(arguments.begin(), arguments.end(), "-freflection") != arguments.end()) {
   selection.reflection_units.push_back(relative);
   const auto output = entry.value("output", std::string{});
   if (!output.empty()) {
    fs::path target(output);
    if (target.is_absolute()) {
     const auto rel = relative_within(fs::weakly_canonical(target), compile_db.parent_path());
     if (rel.empty()) throw ToolError("reflection output '" + output + "' is outside " + compile_db.parent_path().string());
     target = rel;
    }
    selection.reflection_targets.insert(target.generic_string());
    selection.reflection_objects[relative].insert(target.generic_string());
   }
   return;
  }
  if ((host_extensions.contains(extension) || extension == ".cu") && seen_clang.insert(relative).second) {
   selection.clang_units.push_back(relative);
   if (extension == ".cu") collect_nvcc_removals(arguments, selection.cuda_removed_args);
  }
 });
 std::sort(selection.clang_units.begin(), selection.clang_units.end());
 std::sort(selection.cppcheck_units.begin(), selection.cppcheck_units.end());
 std::sort(selection.reflection_units.begin(), selection.reflection_units.end());
 return selection;
}
Arguments tidy_arguments(const Options& options, const Selection& selection, const std::string& tidy, const std::string& checks, const std::string& cuda_home, const std::string& file) {
 std::string unit_checks = checks;
 std::string header_filter = '^' + options.repo_root + '/';
 if (file.starts_with("src/")) {
  header_filter += "src/";
 } else if (file.starts_with("third_party/")) {
  const auto component_end = file.find('/', std::string_view("third_party/").size());
  header_filter += file.substr(0, component_end) + '/';
  if (file.starts_with("third_party/rapidgzip/src/external/isa-l/"))
   unit_checks +=
    ",-bugprone-branch-clone,-bugprone-implicit-widening-of-multiplication-result,"
    "-bugprone-narrowing-conversions,-bugprone-suspicious-string-compare,"
    "-bugprone-switch-missing-default-case,-bugprone-too-small-loop-variable,"
    "-clang-analyzer-core.uninitialized.Assign,-clang-analyzer-deadcode.DeadStores,"
    "-performance-no-int-to-ptr";
 }
 Arguments arguments{
  tidy, "-p", options.build_dir, options.repo_root + '/' + file, "--checks=" + unit_checks, "--warnings-as-errors=*", "--extra-arg=-Wno-unknown-warning-option", "--extra-arg=-DMMLTK_CLANG_TIDY=1",
  "--header-filter=" + header_filter
 };
 if (file.ends_with(".cu")) {
  for (const auto& argument : selection.cuda_removed_args)
   if (!argument.empty()) arguments.push_back("--removed-arg=" + argument);
  arguments.insert(
   arguments.end(), {"--extra-arg-before=--gcc-toolchain=/opt/gcc-16.2", "--extra-arg=--cuda-path=" + cuda_home, "--extra-arg=--cuda-gpu-arch=sm_86", "--extra-arg=-Wno-unknown-cuda-version"});
 } else {
  arguments.insert(arguments.end(), {"--removed-arg=-fhardened", "--removed-arg=-Werror=hardened", "--removed-arg=-std=c++26", "--extra-arg-before=--gcc-toolchain=/opt/gcc-16.2",
                                     "--extra-arg=-std=c++23", "--extra-arg=-D_FORTIFY_SOURCE=3", "--extra-arg=-D_GLIBCXX_ASSERTIONS", "--extra-arg=-ftrivial-auto-var-init=zero",
                                     "--extra-arg=-fstack-protector-strong", "--extra-arg=-fstack-clash-protection", "--extra-arg=-fcf-protection=full"});
 }
 arguments.emplace_back("--quiet");
 return arguments;
}
void report_tidy_output(const fs::path& path, Output& output) {
 static const std::regex generated(R"(^[0-9]+ warnings?( and [0-9]+ errors?)? generated\.$)");
 std::ifstream stream(path, std::ios::binary);
 if (!stream) throw ToolError("cannot read " + path.string());
 std::string line;
 while (std::getline(stream, line)) {
  if (std::regex_match(line, generated)) continue;
  if (!stream.eof()) line += '\n';
  output.write(line, true);
 }
 if (stream.bad()) throw ToolError("read failed: " + path.string());
}
int analyze(Options& options, Output& output, const Termination& termination) {
 const Commands commands(output, termination);
 if (options.rust_only) {
  format_rust(options, commands, output);
  output.log("Rust formatting completed cleanly");
  return 0;
 }
 normalize_targets(options, commands);
 const auto cuda_home = env("CUDA_HOME");
 if (!options.tools_only && cuda_home.empty()) throw ToolError("CUDA_HOME: container must provide the canonical CUDA root");
 if (env("LD_LIBRARY_PATH").empty()) throw ToolError("LD_LIBRARY_PATH: container must provide the canonical loader search order");
 const auto formatter = nonempty_env("MMLTK_CLANG_FORMAT", "clang-format");
 if (!executable_available(formatter)) throw ToolError("clang-format is unavailable; rebuild the analysis image");
 const auto tidy = nonempty_env("MMLTK_CLANG_TIDY", "clang-tidy");
 if (!executable_available(tidy)) throw ToolError("clang-tidy is unavailable; rebuild the analysis image");
 const auto workers = worker_count(commands);
 if (!options.tools_only) {
  format_native(options, commands, output, formatter, workers);
  if (options.files.empty()) format_rust(options, commands, output);
 }
 const auto cppcheck_build = options.build_dir + "/cppcheck";
 const auto database = options.build_dir + "/compile_commands.json";
 fs::create_directories(options.build_dir);
 fs::create_directories(cppcheck_build);
 output.log("refreshing cached Ninja analysis build tree at " + options.build_dir);
 if (options.tools_only)
  commands.require({"cmake", "-S", options.repo_root + "/tools", "-B", options.build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DMMLTK_TOOLS_BUILD_DRIVER=ON"});
 else
  commands.require({"cmake", "--fresh", "--preset", "analysis", "-B", options.build_dir}, options.repo_root);
 if (!fs::is_regular_file(database)) throw ToolError("missing compile_commands.json at " + database);
 const auto invariants = command::self_invocation(command::Command::check_toolchain_invariants, {"--repo-root", options.repo_root, "--build-dir", options.build_dir});
 if (!options.tools_only) commands.require(invariants);
 TemporaryDirectory scratch("mmltk-tidy-");
 output.log("selecting tracked " + std::string(options.third_party ? "first-party and non-Firefox third_party" : "first-party") + " translation units from " + database);
 auto selection = select_units(options, commands, database, termination);
 if (options.tools_only) {
  auto formatting = options;
  formatting.files = selection.clang_units;
  formatting.files.insert(formatting.files.end(), selection.reflection_units.begin(), selection.reflection_units.end());
  for (const auto& requested : options.files)
   if (std::find(formatting.files.begin(), formatting.files.end(), requested) == formatting.files.end())
    throw ToolError("file '" + requested + "' is not registered in the standalone driver graph; use its product or isolated image owner");
  if (formatting.files.empty()) throw ToolError("no registered tools translation units found in " + database);
  format_native(formatting, commands, output, formatter, workers);
 }
 if (options.tools_only && !options.start_at.empty()) {
  const auto start = normalize_repo_path(options.start_at, options.repo_root);
  const auto found = std::find(selection.reflection_units.begin(), selection.reflection_units.end(), start);
  if (found == selection.reflection_units.end()) throw ToolError("start file '" + start + "' is not a registered tools translation unit");
  selection.reflection_units.erase(selection.reflection_units.begin(), found);
  selection.reflection_targets.clear();
  for (const auto& file : selection.reflection_units) {
   const auto& objects = selection.reflection_objects.at(file);
   selection.reflection_targets.insert(objects.begin(), objects.end());
  }
 }
 const auto project_path = scratch.path() / "cppcheck-project.json";
 write_json(project_path, selection.cppcheck_commands, -1);
 if (selection.clang_units.empty() && selection.reflection_units.empty()) throw ToolError("no translation units found in " + database);
 const auto cuda_count = static_cast<std::size_t>(std::count_if(selection.clang_units.begin(), selection.clang_units.end(), [](const auto& file) { return file.ends_with(".cu"); }));
 output.log("selected " + std::to_string(selection.clang_units.size()) + " clang-tidy units (" + std::to_string(selection.clang_units.size() - cuda_count) + " host, " + std::to_string(cuda_count) +
            " CUDA) and " + std::to_string(selection.cppcheck_units.size()) + " cppcheck units");
 bool reflection_failed = false;
 if (!selection.reflection_units.empty()) {
  if (selection.reflection_targets.empty()) throw ToolError("reflection translation units have no owning CMake target");
  output.log("validating " + std::to_string(selection.reflection_units.size()) + " GCC-reflection units through their exact supported compiler objects");
  Arguments build{"cmake", "--build", options.build_dir, "--parallel", std::to_string(workers), "--target"};
  build.insert(build.end(), selection.reflection_targets.begin(), selection.reflection_targets.end());
  build.insert(build.end(), {"--", "-k", "0"});
  reflection_failed = commands.invoke(build).exit_code != 0;
  if (!options.tools_only) commands.require(invariants);
 }
 if (!options.tools_only && !options.start_at.empty()) {
  const auto start = normalize_repo_path(options.start_at, options.repo_root);
  if (!options.third_party && start.starts_with("third_party/")) throw ToolError("--start-at requires --third_party for third_party sources");
  const auto found = std::find(selection.clang_units.begin(), selection.clang_units.end(), start);
  if (found == selection.clang_units.end()) throw ToolError("start file '" + start + "' is not a clang-tidy translation unit");
  selection.clang_units.erase(selection.clang_units.begin(), found);
 }
 Arguments failed_units;
 if (!options.cppcheck_only && !selection.clang_units.empty()) {
  const auto checks = nonempty_env("MMLTK_CLANG_TIDY_CHECKS", default_checks);
  output.log("running clang-tidy across " + std::to_string(selection.clang_units.size()) + " translation units with " + std::to_string(workers) + " workers");
  std::size_t completed = 0;
  std::mutex progress;
  parallel(selection.clang_units.size(), workers, [&](std::size_t index) {
   const auto& file = selection.clang_units[index];
   const auto output_file = scratch.path() / ("clang-tidy-" + std::to_string(index) + ".log");
   output.log("clang-tidy start " + file);
   const auto result = commands.invoke(tidy_arguments(options, selection, tidy, checks, cuda_home, file), {}, Capture::File, output_file);
   const std::lock_guard lock(progress);
   if (result.exit_code) {
    report_tidy_output(output_file, output);
    failed_units.push_back(file);
   }
   fs::remove(output_file);
   ++completed;
   output.log("clang-tidy [" + std::to_string(completed) + '/' + std::to_string(selection.clang_units.size()) + "] " + (result.exit_code ? "FAILED " : "ok ") + file);
   return true;
  });
 } else if (!options.cppcheck_only) {
  output.log("skipping clang-tidy because Clang does not implement the selected GCC reflection syntax");
 }
 bool cppcheck_failed = false;
 if (cppcheck_enabled && !selection.cppcheck_units.empty()) {
  output.log("running cppcheck across " + std::to_string(selection.cppcheck_units.size()) + " translation units");
  cppcheck_failed = commands
                     .invoke(
                      {"cppcheck", "--project=" + project_path.string(), "--cppcheck-build-dir=" + cppcheck_build, "--suppressions-list=" + options.repo_root + "/tools/cppcheck.suppressions",
                       "--suppress=*:/src/workspace/deps/*", "--suppress=*:*/workspace/deps/*", "--inline-suppr", "--quiet", "--enable=warning,performance,portability", "--inconclusive",
                       "--check-level=exhaustive", "--error-exitcode=1", "--platform=unix64", "--relative-paths=" + options.repo_root, "-j", std::to_string(workers)})
                     .exit_code != 0;
 } else if (!cppcheck_enabled) {
  output.log("skipping cppcheck because cppcheck 2.21 cannot parse C++26 reflection");
 } else {
  output.log("skipping cppcheck because the selected target has no eligible host C/C++ translation unit");
 }
 constexpr std::string_view separator = "--------------------------------------------------------------------------------";
 if (!failed_units.empty()) {
  output.log(separator);
  output.log("clang-tidy FAILED for the following " + std::to_string(failed_units.size()) + " units:");
  for (const auto& file : failed_units) output.log("  " + file);
 }
 if (cppcheck_failed) {
  output.log(separator);
  output.log("cppcheck FAILED");
 }
 if (reflection_failed) {
  output.log(separator);
  output.log("GCC reflection compiler validation FAILED");
 }
 if (!failed_units.empty() || cppcheck_failed || reflection_failed) {
  output.log(separator);
  throw ToolError("static analysis failed");
 }
 termination.check();
 output.log("static analysis completed cleanly");
 return 0;
}
void report_failure(Output& output, std::string_view message) noexcept {
 if (message.empty()) return;
 try {
  output.log(message);
 } catch (...) {
  static constexpr char fallback[] = "mmltk-tidy: failed to write diagnostic output\n";
  static_cast<void>(::write(STDERR_FILENO, fallback, sizeof(fallback) - 1));
 }
}
}  // namespace
export int mmltk_tool_run_static_analysis(int argc, char** argv) {
 Output output;
 try {
  const Termination termination;
  Options options;
  output.setup(options.repo_root);
  if (!parse(options, argc, argv, output)) return 0;
  return analyze(options, output, termination);
 } catch (const ToolError& error) {
  report_failure(output, error.what());
  return error.status;
 } catch (const std::exception& error) {
  report_failure(output, error.what());
  return 1;
 }
}
