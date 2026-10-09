module;
#include <nlohmann/json.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
export module mmltk.tools.build_time_trace;
import std;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace {
namespace support = mmltk::tools;
namespace fs = std::filesystem;
using support::json;
using support::RunOptions;
using support::ToolError;
void log(std::string_view message) { std::cerr << "build_time_trace: " << message << '\n'; }
void usage() {
 std::cout << R"(Usage: ./mmltk --tool build-time-trace -- [--cache-key NAME] [--warm-rebuild|--no-op] [--label LABEL]

Runs ./mmltk --build with an isolated repository-local compile cache and
staged release. Normal .cache/cmake, Cargo, ccache, Firefox, browser, and
build/release state is never cleared or overwritten.

Options:
  --cache-key NAME  Reuse .cache/build-time-trace/NAME across runs.
                    Omit for a new cold cache.
  --warm-rebuild    Remove only isolated CMake/Firefox/staged products,
                    preserving compiler, Cargo, mozbuild, browser, and
                    BuildKit caches.
                    Requires a completed prior run with --cache-key.
  --no-op           Request an unchanged no-op measurement from a
                    completed prior run with --cache-key.
  --label LABEL     Store a human-readable label in run.json.
  --help            Show this help.
)";
}
struct Options {
 std::string cache_key;
 std::string label;
 bool warm = false;
 bool noop = false;
 bool help = false;
};
Options parse_options(int argc, char** argv) {
 Options result;
 for (int index = 1; index < argc; ++index) {
  const std::string argument = argv[index];
  if (argument == "--help" || argument == "-h") {
   result.help = true;
   return result;
  }
  if (argument == "--cache-key" || argument == "--label") {
   if (++index == argc) throw ToolError(argument + " requires a value");
   (argument == "--cache-key" ? result.cache_key : result.label) = argv[index];
  } else if (argument == "--warm-rebuild") {
   result.warm = true;
  } else if (argument == "--no-op") {
   result.noop = true;
  } else {
   throw ToolError("unsupported argument: " + argument);
  }
 }
 if (result.warm && result.noop) throw ToolError("--warm-rebuild and --no-op are mutually exclusive");
 return result;
}
std::string utc_stamp(const char* format) {
 const auto now = std::time(nullptr);
 std::tm utc{};
 if (!::gmtime_r(&now, &utc)) throw ToolError("cannot read UTC time");
 std::array<char, 64> buffer{};
 if (!std::strftime(buffer.data(), buffer.size(), format, &utc)) throw ToolError("cannot format UTC time");
 return buffer.data();
}
std::string checked_output(const std::vector<std::string>& command, RunOptions options = {}) {
 options.capture_stdout = true;
 const auto result = support::run(command, options);
 if (result.exit_code != 0) throw ToolError(command.front() + " failed with status " + std::to_string(result.exit_code), result.exit_code);
 return mmltk::tools::text::trim_newlines(std::move(result.out));
}
fs::path resolved(const fs::path& path) {
 // GNU realpath -m also resolves dangling symlinks in a missing suffix.
 return checked_output({"realpath", "-m", "--", path.string()});
}
void require_exact_path(const fs::path& path, const fs::path& expected, std::string_view label) {
 if (path != expected) throw ToolError(std::string(label) + " resolves to " + path.string() + ", expected " + expected.string());
}
void require_descendant(const fs::path& path, const fs::path& parent, std::string_view label) {
 if (!path.string().starts_with(parent.string() + '/')) throw ToolError(std::string(label) + " must stay beneath " + parent.string());
}
void require_unaliased(const fs::path& path, std::string_view label) { require_exact_path(resolved(path), path, label); }
struct Paths {
 fs::path cache;
 fs::path trace;
 fs::path workspace;
 fs::path release;
 fs::path browser;
 fs::path report;
 fs::path phases;
 fs::path firefox_phases;
 fs::path build_log;
 fs::path evidence;
 fs::path summary;
 fs::path layers;
 Paths(const fs::path& repository, const std::string& key) {
  const auto cache_namespace = repository / ".cache/build-time-trace";
  const auto trace_namespace = repository / "build/time-trace";
  const auto expected_cache = cache_namespace / key;
  const auto expected_trace = trace_namespace / key;
  cache = resolved(expected_cache);
  trace = resolved(expected_trace);
  const auto expected_workspace = trace / "workspace";
  workspace = resolved(expected_workspace);
  require_descendant(cache, cache_namespace, "trace cache root");
  require_descendant(trace, trace_namespace, "trace report root");
  require_descendant(workspace, trace, "trace workspace");
  require_exact_path(cache, expected_cache, "trace cache root");
  require_exact_path(trace, expected_trace, "trace report root");
  require_exact_path(workspace, expected_workspace, "trace workspace");
  release = workspace / "release";
  browser = workspace / "browser-app";
  fs::create_directories(cache);
  fs::create_directories(workspace);
  const std::pair<const char*, const char*> cache_paths[] = {
   {"cmake", "trace CMake root"},
   {"cmake/release", "trace release Ninja root"},
   {"cargo", "trace Cargo root"},
   {"cargo/home", "trace Cargo home"},
   {"cargo/target", "trace Cargo target root"},
   {"cargo/target/browser-app", "trace browser Cargo target"},
   {"browser-app", "trace browser state root"},
   {"buildkit", "trace BuildKit cache root"},
   {"ccache", "trace ccache root"},
   {"ccache/tmp", "trace ccache temporary root"},
   {"container-home", "trace container home"},
   {"firefox", "trace Firefox cache root"},
   {"firefox/mozbuild", "trace Firefox mozbuild root"},
   {"firefox/sccache", "trace Firefox sccache root"},
   {"firefox/obj-minimal-opt", "trace Firefox object root"},
   {"image-fingerprints", "trace image fingerprint root"},
   {"locks", "trace cache lock root"},
  };
  for (const auto& [suffix, label] : cache_paths) require_unaliased(cache / suffix, label);
  require_unaliased(release, "trace release stage");
  require_unaliased(release.string() + ".next", "trace next release stage");
  require_unaliased(release.string() + ".previous", "trace previous release stage");
  require_unaliased(browser, "trace browser stage");
  require_unaliased(browser / "dist", "trace browser distribution");
  require_unaliased(browser / "dist.next", "trace next browser distribution");
 }
 bool completed_run_available() const {
  return fs::is_regular_file(cache / "cmake/release/.ninja_log") && fs::is_regular_file(cache / "firefox/obj-minimal-opt/dist/.mmltk-runtime-stage.sha256") &&
         fs::is_regular_file(release / ".mmltk-package-fingerprint");
 }
 void reset_warm_products() const {
  const std::pair<fs::path, const char*> targets[] = {
   {cache / "cmake", "warm CMake reset target"},
   {cache / "firefox/obj-minimal-opt", "warm Firefox reset target"},
   {release, "warm release reset target"},
   {browser, "warm browser reset target"},
  };
  // Check every target before removing any of them.
  for (const auto& [path, label] : targets) require_unaliased(path, label);
  bool failed = false;
  for (const auto& [path, label] : targets) {
   static_cast<void>(label);
   std::error_code error;
   fs::remove_all(path, error);
   if (error) {
    log("cannot remove " + path.string() + ": " + error.message());
    failed = true;
   }
  }
  if (failed) throw ToolError("failed to reset isolated build products");
 }
 void create_report(const std::string& run_id) {
  const auto parent = trace / "runs";
  require_unaliased(parent, "trace report parent");
  fs::create_directories(parent);
  const auto expected = parent / run_id;
  report = resolved(expected);
  require_descendant(report, trace, "trace run report");
  require_exact_path(report, expected, "trace run report");
  std::error_code error;
  if (!fs::create_directory(report, error)) throw ToolError("trace run report already exists: " + report.string());
  phases = report / "phases.jsonl";
  firefox_phases = report / "phases-firefox-stage.jsonl";
  build_log = report / "build.log";
  evidence = report / "cache-evidence.jsonl";
  summary = report / "trace-summary.json";
  layers = report / "runtime-layers.jsonl";
 }
};
class TraceLock {
public:
 TraceLock(const fs::path& cache, const std::string& key) {
  const auto expected = cache / ".run.lock";
  const auto path = resolved(expected);
  require_descendant(path, cache, "trace lock file");
  require_exact_path(path, expected, "trace lock file");
  if (fs::exists(path) && !fs::is_regular_file(path)) throw ToolError("trace lock path is not a regular file: " + path.string());
  descriptor_.reset(::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666));
  if (descriptor_.get() < 0) throw ToolError("cannot open " + path.string() + ": " + std::strerror(errno));
  if (::flock(descriptor_.get(), LOCK_EX | LOCK_NB) != 0) throw ToolError("another timing trace is already using cache key '" + key + "' in this checkout");
 }

private:
 support::FileDescriptor descriptor_;
};
class BuilderCleanup {
public:
 explicit BuilderCleanup(std::string name) : name_(std::move(name)) { cleanup(); }
 ~BuilderCleanup() { cleanup(); }
 BuilderCleanup(const BuilderCleanup&) = delete;
 BuilderCleanup& operator=(const BuilderCleanup&) = delete;

private:
 void cleanup() const noexcept {
  try {
   static_cast<void>(support::capture({"docker", "buildx", "rm", name_}));
  } catch (...) {
   // The original EXIT cleanup is best effort and never replaces status.
  }
 }
 std::string name_;
};
class BuildTee {
public:
 explicit BuildTee(const fs::path& path) : path_(path.string()) {
  file_.reset(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666));
  if (file_.get() < 0) std::cerr << "tee: " << path_ << ": " << std::strerror(errno) << '\n';
 }
 void operator()(std::string_view chunk) {
  if (stdout_valid_) stdout_valid_ = write_chunk(STDOUT_FILENO, chunk, "standard output");
  if (file_.get() >= 0 && !write_chunk(file_.get(), chunk, path_)) file_.reset();
 }

private:
 static bool write_chunk(int descriptor, std::string_view chunk, std::string_view name) {
  if (const int error = support::try_write_all(descriptor, chunk)) {
   std::cerr << "tee: " << name << ": " << std::strerror(error) << '\n';
   return false;
  }
  return true;
 }
 std::string path_;
 support::FileDescriptor file_;
 bool stdout_valid_ = true;
};
void touch(const fs::path& path) {
 support::FileDescriptor descriptor(::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666));
 if (descriptor.get() < 0 || ::futimens(descriptor.get(), nullptr) != 0) throw ToolError("cannot touch " + path.string() + ": " + std::strerror(errno));
}
void command_report(const fs::path& destination, const std::vector<std::string>& command, bool suppress_stderr = true) {
 std::ofstream output(destination, std::ios::binary | std::ios::trunc);
 if (!output) {
  log("cannot write " + destination.string());
  return;
 }
 RunOptions options;
 if (suppress_stderr) options.stderr_sink = [](std::string_view) {};
 options.stdout_sink = [&](std::string_view chunk) { output.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); };
 // Evidence commands intentionally retain partial output even on failure.
 static_cast<void>(support::run(command, options));
 output.close();
 if (!output && !suppress_stderr) log("write failed: " + destination.string());
}
std::int64_t integer(std::string value) {
 value = support::trim(value);
 std::size_t consumed = 0;
 const auto result = std::stoll(value, &consumed);
 if (consumed != value.size()) throw ToolError("invalid integer: " + value);
 return result;
}
std::int64_t integer(const json& value) {
 if (value.is_string()) return integer(value.get<std::string>());
 if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
 if (value.is_number_unsigned()) {
  const auto number = value.get<std::uint64_t>();
  if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) throw ToolError("trace integer is out of range");
  return static_cast<std::int64_t>(number);
 }
 if (value.is_number_integer()) return value.get<std::int64_t>();
 if (value.is_number_float()) {
  const auto number = value.get<double>();
  if (std::isfinite(number) && number >= -9223372036854775808.0 && number < 9223372036854775808.0) return static_cast<std::int64_t>(number);
 }
 throw ToolError("invalid integer in phase trace: " + value.dump());
}
std::string path_bytes(const fs::path& path) {
 if (!fs::exists(path)) return "0";
 RunOptions options;
 options.capture_stdout = true;
 const auto result = support::run({"du", "-sb", "--", path.string()}, options);
 // du may print a useful partial total and fail. The shell's metric call
 // retains that output, including an empty raw value if du prints nothing.
 auto value = result.out.substr(0, result.out.find('\t'));
 return support::text::trim_newlines(std::move(value));
}
std::int64_t file_count(const fs::path& path) {
 if (!fs::is_directory(path)) return 0;
 std::int64_t count = 0;
 RunOptions options;
 options.stdout_sink = [&](std::string_view chunk) { count += static_cast<std::int64_t>(chunk.size()); };
 static_cast<void>(support::run({"find", path.string(), "-type", "f", "-printf", "."}, options));
 return count;
}
struct CmakeArtifacts {
 std::int64_t ninja_lines = 0;
 std::int64_t pch_files = 0;
 std::int64_t pch_bytes = 0;
};
CmakeArtifacts inspect_cmake(const fs::path& path) {
 CmakeArtifacts result;
 if (!fs::is_directory(path)) return result;
 RunOptions options;
 options.capture_stdout = true;
 const auto logs = support::run({"find", path.string(), "-type", "f", "-name", ".ninja_log", "-print0"}, options);
 for (const auto& filename : support::split(logs.out, '\0', false)) {
  std::ifstream input(filename);
  if (!input) {
   log("cannot read Ninja log: " + filename);
   continue;
  }
  std::string line;
  // AWK's FNR > 1 counts every record after each file's header.
  std::getline(input, line);
  while (std::getline(input, line)) ++result.ninja_lines;
  if (input.bad()) log("read failed: " + filename);
 }
 const auto pch = support::run({"find", path.string(), "-type", "f", "(", "-name", "*.gch", "-o", "-name", "*.pch", ")", "-printf", "%s\n"}, options);
 for (const auto& line : support::lines(pch.out)) {
  ++result.pch_files;
  result.pch_bytes += integer(line);
 }
 return result;
}
void record_metric(std::ostream& output, std::string_view name, std::string_view value, std::string_view unit) {
 output << "{\"metric\":" << json(name).dump() << ",\"value\":" << value << ",\"unit\":" << json(unit).dump() << "}\n";
 if (!output) throw ToolError("cannot write cache evidence");
}
void record_metric(std::ostream& output, std::string_view name, std::int64_t value, std::string_view unit) { record_metric(output, name, std::to_string(value), unit); }
void collect_evidence(const Paths& paths, const std::string& image, int build_status) {
 std::ofstream evidence(paths.evidence, std::ios::binary | std::ios::trunc);
 if (!evidence) throw ToolError("cannot write " + paths.evidence.string());
 touch(paths.phases);
 touch(paths.firefox_phases);
 support::write_text(paths.layers, "");
 std::int64_t image_bytes = 0;
 if (support::capture({"docker", "image", "inspect", image}).exit_code == 0) {
  image_bytes = integer(checked_output({"docker", "image", "inspect", image, "--format", "{{.Size}}"}));
  command_report(paths.layers, {"docker", "image", "history", "--no-trunc", "--format", "{\"id\":{{json .ID}},\"size\":{{json .Size}},\"created_by\":{{json .CreatedBy}}}", image}, false);
 }
 record_metric(evidence, "build_exit_status", build_status, "code");
 record_metric(evidence, "cache_total", path_bytes(paths.cache), "bytes");
 record_metric(evidence, "browser_state_total", path_bytes(paths.cache / "browser-app"), "bytes");
 record_metric(evidence, "buildkit_total", path_bytes(paths.cache / "buildkit"), "bytes");
 record_metric(evidence, "buildkit_files", file_count(paths.cache / "buildkit"), "files");
 record_metric(evidence, "ccache_total", path_bytes(paths.cache / "ccache"), "bytes");
 record_metric(evidence, "ccache_files", file_count(paths.cache / "ccache"), "files");
 record_metric(evidence, "cargo_total", path_bytes(paths.cache / "cargo"), "bytes");
 record_metric(evidence, "cargo_files", file_count(paths.cache / "cargo"), "files");
 record_metric(evidence, "firefox_obj_total", path_bytes(paths.cache / "firefox/obj-minimal-opt"), "bytes");
 record_metric(evidence, "firefox_mozbuild_total", path_bytes(paths.cache / "firefox/mozbuild"), "bytes");
 record_metric(evidence, "firefox_sccache_total", path_bytes(paths.cache / "firefox/sccache"), "bytes");
 record_metric(evidence, "firefox_sccache_files", file_count(paths.cache / "firefox/sccache"), "files");
 record_metric(evidence, "image_fingerprint_files", file_count(paths.cache / "image-fingerprints"), "files");
 record_metric(evidence, "cmake_total", path_bytes(paths.cache / "cmake"), "bytes");
 const auto cmake = inspect_cmake(paths.cache / "cmake");
 record_metric(evidence, "ninja_log_lines", cmake.ninja_lines, "lines");
 record_metric(evidence, "pch_files", cmake.pch_files, "files");
 record_metric(evidence, "pch_total", cmake.pch_bytes, "bytes");
 record_metric(evidence, "runtime_image_total", image_bytes, "bytes");
 const auto layers = support::read_text(paths.layers);
 record_metric(evidence, "runtime_layer_records", std::count(layers.begin(), layers.end(), '\n'), "layers");
 evidence.close();
 if (!evidence) throw ToolError("write failed: " + paths.evidence.string());
 const auto fingerprint = paths.cache / "firefox/obj-minimal-opt/dist/.mmltk-runtime-stage.sha256";
 const auto captured_fingerprint = paths.report / "firefox-runtime-stage.sha256";
 if (fs::is_regular_file(fingerprint))
  fs::copy_file(fingerprint, captured_fingerprint, fs::copy_options::overwrite_existing);
 else
  support::write_text(captured_fingerprint, "missing\n");
 command_report(paths.report / "pch-artifacts.txt", {"find", (paths.cache / "cmake").string(), "-type", "f", "(", "-name", "*.gch", "-o", "-name", "*.pch", ")", "-printf", "%p\t%s\n"});
 command_report(paths.report / "cargo-rust-work.txt", {"rg", "-n", "--", "(^|[[:space:]])Compiling |rustc|cargo|trunk|wasm-bindgen|wasm-opt", paths.build_log.string()});
 command_report(paths.report / "buildkit-evidence.txt", {"rg", "-n", "--", "CACHED|cache-from|cache-to|exporting cache|importing cache|buildkit", paths.build_log.string()});
 const std::pair<const char*, const char*> stats[] = {
  {"ccache-stats.txt", "ccache stats unavailable: core compilation failed before capture.\n"},
  {"sccache-stats.txt", "sccache stats unavailable: Firefox compilation was skipped or failed before capture.\n"},
 };
 for (const auto& [name, missing] : stats) {
  const auto path = paths.report / name;
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size == 0) support::write_text(path, missing);
 }
 command_report(paths.report / "mold-link-commands.txt", {"rg", "-n", "--glob", "build.ninja", "--", "-fuse-ld=mold", (paths.cache / "cmake").string()});
 command_report(paths.report / "pch-commands.txt", {"rg", "-n", "--glob", "build.ninja", "--", "cmake_pch|fpch-preprocess", (paths.cache / "cmake").string()});
 command_report(paths.report / "firefox-mold-config.txt",
  {"rg", "-n", "--", "mold|-fuse-ld", (paths.cache / "firefox/obj-minimal-opt/config.status").string(), (paths.cache / "firefox/obj-minimal-opt/config/autoconf.mk").string()});
}
// Python's read_text(errors="replace") also applies universal-newline decoding.
// Keep malformed log bytes from discarding an otherwise useful JSONL record.
std::string decoded_text(const fs::path& path) { return support::text::decode_utf8(support::read_text(path), true); }
std::vector<json> load_jsonl(const fs::path& path) {
 std::vector<json> records;
 for (const auto& line : support::lines(decoded_text(path))) {
  auto record = json::parse(line, nullptr, false);
  if (record.is_discarded()) continue;
  if (!record.is_object()) throw ToolError("phase trace record is not an object: " + path.string());
  records.push_back(std::move(record));
 }
 return records;
}
bool field_is(const json& record, std::string_view key, std::string_view value) {
 const auto found = record.find(std::string(key));
 return found != record.end() && *found == value;
}
std::int64_t event_count(const std::vector<json>& records, std::string_view phase, std::string_view event) {
 return std::count_if(records.begin(), records.end(), [&](const auto& record) { return field_is(record, "phase", phase) && field_is(record, "event", event); });
}
std::string cache_status(const std::vector<json>& records, std::string_view phase) {
 if (event_count(records, phase, "cache_hit")) return "hit";
 if (event_count(records, phase, "cache_miss")) return "miss";
 return "unknown";
}
std::optional<std::pair<std::int64_t, std::int64_t>> phase_span(const std::vector<json>& records, std::string_view phase) {
 std::optional<std::int64_t> start;
 std::optional<std::int64_t> finish;
 for (const auto& record : records) {
  if (!field_is(record, "phase", phase)) continue;
  if (field_is(record, "event", "start")) {
   const auto timestamp = integer(record.at("timestamp_ns"));
   start = start ? std::min(*start, timestamp) : timestamp;
  } else if (field_is(record, "event", "finish")) {
   const auto timestamp = integer(record.at("timestamp_ns"));
   finish = finish ? std::max(*finish, timestamp) : timestamp;
  }
 }
 if (!start || !finish) return std::nullopt;
 return std::pair{*start, *finish};
}
std::int64_t regex_count(const std::string& pattern, const std::string& text) {
 const std::regex expression(pattern, std::regex_constants::ECMAScript | std::regex_constants::multiline);
 return std::distance(std::sregex_iterator(text.begin(), text.end(), expression), std::sregex_iterator());
}
struct BuildLogCounts {
 std::int64_t trunk_lines = 0;
 std::int64_t native_edges = 0;
 std::int64_t cuda_edges = 0;
 std::int64_t buildkit_cached = 0;
 std::int64_t buildkit_imports = 0;
 std::int64_t buildkit_exports = 0;
 std::int64_t buildkit_lines = 0;
};
BuildLogCounts count_build_lines(std::string_view text) {
 BuildLogCounts counts;
 // These anchored patterns need only one pass. Avoid a backtracking .* over
 // potentially very long compiler/linker command lines in build transcripts.
 for (std::size_t begin = 0; begin < text.size();) {
  const auto newline = text.find('\n', begin);
  const bool terminated = newline != std::string_view::npos;
  const auto line = text.substr(begin, terminated ? newline - begin : text.size() - begin);
  begin = terminated ? newline + 1 : text.size();
  counts.trunk_lines += line.find("trunk") != std::string_view::npos || line.find("wasm-bindgen") != std::string_view::npos || line.find("wasm-opt") != std::string_view::npos;
  const auto digits = [&](std::size_t& position) {
   const auto start = position;
   while (position < line.size() && line[position] >= '0' && line[position] <= '9') ++position;
   return position != start;
  };
  std::size_t position = 1;
  if (line.starts_with('[') && digits(position) && position < line.size() && line[position++] == '/' && digits(position) && position < line.size() && line[position++] == ']') {
   const auto suffix = line.substr(position);
   counts.native_edges += suffix.find("Building C object") != std::string_view::npos || suffix.find("Building CXX object") != std::string_view::npos;
   counts.cuda_edges += suffix.find("Building CUDA object") != std::string_view::npos;
  }
  position = 1;
  if (line.starts_with('#') && digits(position) && position < line.size() && line[position++] == ' ') {
   const auto suffix = line.substr(position);
   ++counts.buildkit_lines;
   counts.buildkit_cached += suffix == "CACHED";
   counts.buildkit_imports += suffix.find("importing cache") != std::string_view::npos;
   counts.buildkit_exports += suffix.find("exporting cache") != std::string_view::npos;
  }
 }
 return counts;
}
std::int64_t parsed_stat(const std::string& pattern, const std::string& text) {
 const std::regex expression(pattern, std::regex_constants::ECMAScript | std::regex_constants::multiline | std::regex_constants::icase);
 std::int64_t value = 0;
 for (auto it = std::sregex_iterator(text.begin(), text.end(), expression); it != std::sregex_iterator(); ++it) {
  auto digits = (*it)[1].str();
  std::erase(digits, ',');
  value = integer(digits);
 }
 return value;
}
void write_sorted_json(const fs::path& path, const json& value) {
 // Python's sort_keys and ensure_ascii defaults are part of both report formats.
 const nlohmann::json sorted = value;
 support::write_text(path, sorted.dump(2, ' ', true, nlohmann::json::error_handler_t::replace) + '\n');
}
json summarize(const Paths& paths, int build_status) {
 const auto phase_records = load_jsonl(paths.phases);
 const auto stage_records = load_jsonl(paths.firefox_phases);
 const auto build_log = decoded_text(paths.build_log);
 const auto log_counts = count_build_lines(build_log);
 const auto ccache_stats = decoded_text(paths.report / "ccache-stats.txt");
 const auto sccache_stats = decoded_text(paths.report / "sccache-stats.txt");
 json configure = "unknown";
 json jobs = json::object();
 for (const auto& record : phase_records) {
  if (field_is(record, "phase", "configure:release") && field_is(record, "event", "finish")) configure = record.value("result", json(nullptr));
  if (field_is(record, "phase", "jobs") && field_is(record, "event", "assigned")) jobs = record;
 }
 const auto ninja_span = phase_span(phase_records, "ninja_build");
 const auto firefox_span = phase_span(phase_records, "firefox_build");
 std::int64_t overlap_ns = 0;
 if (ninja_span && firefox_span) {
  const auto begin = std::max(ninja_span->first, firefox_span->first);
  const auto end = std::min(ninja_span->second, firefox_span->second);
  if (end > begin) {
   if (begin < 0 && end > std::numeric_limits<std::int64_t>::max() + begin) throw ToolError("phase overlap is out of range");
   overlap_ns = end - begin;
  }
 }
 std::int64_t image_hits = 0;
 std::int64_t image_misses = 0;
 for (const auto& record : phase_records) {
  const auto phase = record.find("phase");
  if (phase == record.end() || !phase->is_string()) continue;
  const auto& name = phase->get_ref<const std::string&>();
  if (!name.starts_with("image:") && name != "runtime_image") continue;
  image_hits += field_is(record, "event", "cache_hit");
  image_misses += field_is(record, "event", "cache_miss");
 }
 const json metrics = {
  {"configure_executed", static_cast<int>(configure == "executed")},
  {"configure_skipped", static_cast<int>(configure == "skipped")},
  {"ninja_firefox_overlap", static_cast<int>(overlap_ns > 0)},
  {"ninja_firefox_overlap_ns", overlap_ns},
  {"ninja_jobs", integer(jobs.value("ninja", json(0)))},
  {"cargo_jobs", integer(jobs.value("cargo", json(0)))},
  {"firefox_jobs", integer(jobs.value("firefox", json(0)))},
  {"cargo_compile_units", regex_count(R"(^\s*Compiling\s+)", build_log)},
  {"cargo_finished_lines", regex_count(R"(^\s*Finished\s+)", build_log)},
  {"rustc_invocations", regex_count(R"((?:^|[/\s])rustc(?:[\s"']|$))", build_log)},
  {"trunk_activity_lines", log_counts.trunk_lines},
  {"native_compile_edges", log_counts.native_edges},
  {"cuda_compile_edges", log_counts.cuda_edges},
  {"image_fingerprint_cache_hits", image_hits},
  {"image_fingerprint_cache_misses", image_misses},
  {"buildkit_cached_steps", log_counts.buildkit_cached},
  {"buildkit_cache_imports", log_counts.buildkit_imports},
  {"buildkit_cache_exports", log_counts.buildkit_exports},
  {"buildkit_step_log_lines", log_counts.buildkit_lines},
  {"cmake_regeneration_lines", regex_count("Re-running CMake|re-running CMake", build_log)},
  {"firefox_stage_cache_hits", event_count(stage_records, "firefox_runtime_stage", "cache_hit")},
  {"firefox_stage_cache_misses", event_count(stage_records, "firefox_runtime_stage", "cache_miss")},
  {"package_cache_hits", event_count(phase_records, "package", "cache_hit")},
  {"package_cache_misses", event_count(phase_records, "package", "cache_miss")},
  {"ccache_hits", parsed_stat(R"(^\s*Hits:\s*([0-9,]+))", ccache_stats)},
  {"sccache_hits", parsed_stat(R"(^\s*Cache hits\s+([0-9,]+))", sccache_stats)},
 };
 std::ofstream evidence(paths.evidence, std::ios::binary | std::ios::app);
 if (!evidence) throw ToolError("cannot append " + paths.evidence.string());
 for (const auto& [name, value] : metrics.items()) {
  const auto unit = name == "ninja_firefox_overlap_ns" ? "nanoseconds" : name == "ninja_jobs" || name == "cargo_jobs" || name == "firefox_jobs" ? "workers" : "count";
  record_metric(evidence, name, integer(value), unit);
 }
 evidence.close();
 if (!evidence) throw ToolError("write failed: " + paths.evidence.string());
 json summary = {
  {"buildkit", {{"cache_exports", metrics.at("buildkit_cache_exports")}, {"cache_imports", metrics.at("buildkit_cache_imports")}, {"cached_steps", metrics.at("buildkit_cached_steps")},
                {"step_log_lines", metrics.at("buildkit_step_log_lines")}}},
  {"cargo_rust", {{"compile_units", metrics.at("cargo_compile_units")}, {"rustc_invocations", metrics.at("rustc_invocations")}, {"trunk_activity_lines", metrics.at("trunk_activity_lines")}}},
  {"compiler_cache_hits", {{"ccache", metrics.at("ccache_hits")}, {"sccache", metrics.at("sccache_hits")}}},
  {"configure", configure},
  {"firefox_runtime_stage_cache", cache_status(stage_records, "firefox_runtime_stage")},
  {"jobs", {{"cargo", metrics.at("cargo_jobs")}, {"firefox", metrics.at("firefox_jobs")}, {"ninja", metrics.at("ninja_jobs")}}},
  {"ninja_firefox_overlap_ns", overlap_ns},
  {"package_cache", cache_status(phase_records, "package")},
  {"runtime_image_cache", cache_status(phase_records, "runtime_image")},
  {"wrapper_image_fingerprint_cache", {{"hits", image_hits}, {"misses", image_misses}}},
 };
 summary["observed_noop"] = build_status == 0 && configure == "skipped" && metrics.at("cmake_regeneration_lines") == 0 && metrics.at("native_compile_edges") == 0 &&
                            metrics.at("cuda_compile_edges") == 0 && metrics.at("cargo_compile_units") == 0 && metrics.at("rustc_invocations") == 0 && metrics.at("trunk_activity_lines") == 0 &&
                            image_misses == 0 && summary.at("firefox_runtime_stage_cache") == "hit" && summary.at("package_cache") == "hit" && summary.at("runtime_image_cache") == "hit";
 write_sorted_json(paths.summary, summary);
 return summary;
}
int execute(int argc, char** argv) {
 const auto repository = support::repository_root(argv[0], __FILE__);
 auto options = parse_options(argc, argv);
 if (options.help) {
  usage();
  return 0;
 }
 if (options.cache_key.empty()) options.cache_key = "cold-" + utc_stamp("%Y%m%dt%H%M%S") + "z-" + std::to_string(::getpid());
 if (!std::regex_match(options.cache_key, std::regex("^[a-z0-9][a-z0-9._-]*$")))
  throw ToolError("--cache-key must start with a lowercase letter or digit and contain only a-z, 0-9, dot, underscore, or dash");
 Paths paths(repository, options.cache_key);
 TraceLock lock(paths.cache, options.cache_key);
 const bool completed = paths.completed_run_available();
 std::string mode = "cold";
 if (options.warm) {
  if (!completed) throw ToolError("--warm-rebuild requires a successful prior run using the same --cache-key");
  log("resetting isolated build products while preserving compiler caches");
  paths.reset_warm_products();
  mode = "warm";
 } else if (options.noop) {
  if (!completed) throw ToolError("--no-op requires a successful prior run using the same --cache-key");
  mode = "noop";
 } else if (completed) {
  mode = "reuse";
 }
 const auto run_id = utc_stamp("%Y%m%dT%H%M%SZ") + '-' + std::to_string(::getpid());
 paths.create_report(run_id);
 RunOptions hash_options;
 hash_options.input = repository.string() + std::string(1, '\0') + options.cache_key;
 const auto identity = checked_output({"sha256sum"}, hash_options).substr(0, 16);
 auto runtime_image = support::env("MMLTK_TRACE_IMAGE");
 if (runtime_image.empty()) runtime_image = "mmltk-trace:" + identity;
 const auto container = "mmltk-trace-" + identity;
 const auto builder = container + "-buildx";
 const auto build_image = "mmltk-trace-build:" + identity;
 const auto runtime_base = "mmltk-trace-runtime-base:" + identity;
 BuilderCleanup cleanup(builder);
 log("cache key: " + options.cache_key);
 log("isolated cache: " + paths.cache.string());
 log("report: " + paths.report.string());
 log("mode: " + mode);
 log("reuse --cache-key " + options.cache_key + " unchanged for a no-op comparison");
 int build_status = 0;
 {
  BuildTee tee(paths.build_log);
  RunOptions build_options;
  build_options.merge_stderr = true;
  build_options.stdout_sink = [&](std::string_view chunk) { tee(chunk); };
  build_options.env = {
   {"MMLTK_BROWSER_STAGE_ROOT", paths.browser.string()},
   {"MMLTK_BUILD_TRACE_FILE", paths.phases.string()},
   {"MMLTK_BUILD_IMAGE", build_image},
   {"MMLTK_BUILDX_BUILDER", builder},
   {"MMLTK_CACHE_ROOT", paths.cache.string()},
   {"MMLTK_CCACHE_STATS_FILE", (paths.report / "ccache-stats.txt").string()},
   {"MMLTK_CONTAINER_NAME", container},
   {"MMLTK_FIREFOX_SCCACHE_STATS_FILE", (paths.report / "sccache-stats.txt").string()},
   {"MMLTK_IMAGE", runtime_image},
   {"MMLTK_RELEASE_STAGE_ROOT", paths.release.string()},
   {"MMLTK_RUNTIME_BASE_IMAGE", runtime_base},
  };
  build_status = support::run({(repository / "mmltk").string(), "--build"}, build_options).exit_code;
 }
 collect_evidence(paths, runtime_image, build_status);
 const auto summary = summarize(paths, build_status);
 const json run = {
  {"build_exit_status", build_status},
  {"build_image", build_image},
  {"buildx_builder", builder},
  {"cache_key", options.cache_key},
  {"cache_root", paths.cache.string()},
  {"evidence_file", paths.evidence.string()},
  {"firefox_stage_trace_file", paths.firefox_phases.string()},
  {"label", options.label},
  {"mode", mode},
  {"observed_mode", summary.at("observed_noop").get<bool>() ? "noop" : "work"},
  {"report_dir", paths.report.string()},
  {"runtime_layers_file", paths.layers.string()},
  {"runtime_image", runtime_image},
  {"runtime_base_image", runtime_base},
  {"run_id", run_id},
  {"summary_file", paths.summary.string()},
 };
 write_sorted_json(paths.report / "run.json", run);
 log("build exit status: " + std::to_string(build_status));
 log("phase trace: " + paths.phases.string());
 log("Firefox stage trace: " + paths.firefox_phases.string());
 log("cache evidence: " + paths.evidence.string());
 log("trace summary: " + paths.summary.string());
 log("runtime layers: " + paths.layers.string());
 return build_status;
}
}  // namespace
export int mmltk_tool_build_time_trace(int argc, char** argv) {
 try {
  return execute(argc, argv);
 } catch (const ToolError& error) {
  log(error.what());
  return error.status;
 } catch (const std::exception& error) {
  log(error.what());
  return 1;
 }
}
