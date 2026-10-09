module;
// Native cleanup and raw declaration evidence.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <openssl/evp.h>
#include <sched.h>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <climits>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <unicode/ucol.h>
#include <unicode/uloc.h>
#include <unicode/utypes.h>
export module mmltk.tools.generate_cleanup_json;
import mmltk.tools.cpp_time;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
import mmltk.tools.cleanup.cpd_patterns;
import mmltk.tools.cleanup.declaration_patterns;
import mmltk.tools.cleanup.review_patterns;
import mmltk.tools.cpp_locale;
import mmltk.tools.cpp_digest;
export {
 namespace mmltk::tools::cleanup {
 using Json = nlohmann::ordered_json;
 using SourceReader = std::function<std::string(const std::string&)>;
 using PathAvailable = std::function<bool(const std::string&)>;
 using Clock = std::function<std::string()>;
 using Range = std::array<std::int64_t, 2>;
 using RangesByFile = std::map<std::string, std::vector<Range>>;
 using Suppressions = std::map<std::string, Json>;
 extern const std::filesystem::path REPO_ROOT;
 extern const Json INVENTORY_COMMANDS;
 extern const Json CLEANUP_PROFILES;
 extern const Json AUTHORED_PROFILE;
 std::size_t availableThreads();
 std::string usageText();
 Json rawCpdProfile(double minTokens = 12, std::string_view profileName = "cpp");
 Json parseArgs(const std::vector<std::string>& args);
 std::optional<std::string> authoredLanguage(std::string_view path);
 bool profileIncludesPath(const Json& profile, std::string_view path);
 struct DetectorOptions {
  std::optional<std::size_t> threads;
  std::optional<std::string> fileList;
 };
 Json detectorArguments(const Json& profile, const DetectorOptions& options = {});
 Json collectInventoryInputs();
 Json buildInventory(const Json& profile, const Json& inputs, const PathAvailable& pathAvailable = {});
 std::string decodeXmlAttribute(std::string_view value);
 Json parseXmlAttributes(std::string_view tag);
 struct ParseXmlOptions {
  std::optional<std::vector<std::string>> expectedPaths{};
  std::function<std::string(const std::string&)> normalizePath{};
 };
 Json parseCpdXml(std::string_view xml, const ParseXmlOptions& options = {});
 struct SnapshotEntry {
  std::string source;
  std::string sha256;
 };
 using Snapshot = std::map<std::string, SnapshotEntry>;
 Snapshot sourceSnapshot(const std::vector<std::string>& paths);
 void requireUnchangedSources(const Snapshot& snapshot);
 Json runCpd(const Json& profile, const std::vector<std::string>& paths);
 Json runCpd(const Json& profile, const std::vector<std::string>& paths, const Snapshot& snapshot);
 std::string repoRelativePath(std::string_view path, std::string_view repoRoot = REPO_ROOT.native());
 std::vector<Range> mergeOverlappingRanges(std::vector<Range> ranges);
 std::string commonDirectory(const std::vector<std::string>& paths);
 Json makeHit(std::int64_t lineCount, std::optional<std::int64_t> tokenCount, const RangesByFile& rangesByFile, bool mergeRanges);
 Json structuralHits(const Json& profile, const Json& duplications, const std::vector<std::string>& scannedPaths, std::string_view repoRoot = REPO_ROOT.native());
 std::int64_t duploLineCount(const Json& hit);
 Json duploOccurrence(const Json& hit, int side);
 std::string stripCommentsAndStrings(std::string_view source);
 Json filterDuploCandidates(const Json& profile, const Json& rawHits, const SourceReader& sourceReader = {});
 Json compactDuploHits(const Json& profile, const Json& rawHits, const std::vector<std::string>& scannedPaths);
 int compareHits(const Json& left, const Json& right);
 Json summarize(const Json& hits);
 Suppressions parseInlineSuppressions(std::string_view path, std::string_view source);
 Json applyInlineSuppressions(const Json& hits, const Suppressions& suppressions, std::string_view detector);
 void requireAtomicSuppressionUse(const Json& suppressedOccurrences);
 Json parseDuploJson(std::string_view text);
 // Input contains the original report fields, with an optional integer threads.
 // The injected clock supplies an ISO-8601 timestamp (UTC, millisecond precision).
 Json buildReport(const Json& input, const Clock& clock = {});
 std::string serializeReport(const Json& report);
 Json rejectionReport(const Json& previous, const Json& report, const Json& filteredCpd);
 void writeAtomic(const std::filesystem::path& path, std::string_view contents);
 struct GenerateReportOptions {
  Clock clock{};
  std::optional<Json> inventoryInputs{};
  PathAvailable pathAvailable{};
 };
 Json generateReport(const Json& profile, const GenerateReportOptions& options = {});
 Json rawCpdReport(const Json& profile, const Json& files, const Json& cpd, const SourceReader& sourceReader = {});
 struct GenerateRawCpdOptions {
  bool review = false;
  std::optional<Json> inventoryInputs{};
  // A JSON return value retains validation of non-string renderer results.
  std::function<Json(const Json&)> renderMarkdown{};
 };
 Json generateRawCpd(const Json& profile, std::string_view output, const GenerateRawCpdOptions& options = {});
 int main(const std::vector<std::string>& args);
 }  // namespace mmltk::tools::cleanup
}
namespace mmltk::tools::cleanup {
namespace {
constexpr double maxSafeInteger = 9007199254740991.0;
const std::vector<std::string> cppSuffixes{".c", ".cc", ".cpp", ".cppm", ".cxx", ".c++", ".def", ".h", ".hh", ".hpp", ".hxx", ".h++", ".ipp", ".inl", ".inc", ".tcc", ".tpp", ".txx", ".cu", ".cuh"};
const std::vector<std::string> nonCppPrefixes{
 "third_party/rapidgzip/src/external/isa-l/crc/aarch64/", "third_party/rapidgzip/src/external/isa-l/igzip/aarch64/", "third_party/rapidgzip/src/external/isa-l/include/aarch64_multibinary.h",
 "third_party/rapidgzip/src/external/isa-l/include/riscv64_multibinary.h"
};
[[noreturn]] void fail(std::string message) { throw ToolError(std::move(message)); }
using cpd::jsTrim;
using text::trimUnits;
std::string lowerAscii(std::string value) {
 for (auto& ch : value)
  if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
 return value;
}
bool startsAny(std::string_view value, const Json& prefixes) {
 return std::any_of(prefixes.begin(), prefixes.end(), [&](const Json& prefix) { return value.starts_with(prefix.get_ref<const std::string&>()); });
}
bool containsString(const Json& values, std::string_view needle) {
 return std::any_of(values.begin(), values.end(), [&](const Json& value) { return value.is_string() && value.get_ref<const std::string&>() == needle; });
}
std::string joined(const std::vector<std::string>& values, std::string_view separator) {
 std::string result;
 for (const auto& value : values) {
  if (&value != &values.front()) result += separator;
  result += value;
 }
 return result;
}
std::vector<std::string> strings(const Json& values) { return values.get<std::vector<std::string>>(); }
// Array.sort() uses UTF-16 lexical order; localeCompare() uses ICU collation.
// Keep these distinct for inventories and evidence ordering respectively.
bool lexicalLess(const std::string& a, const std::string& b) { return cpd::toUtf16(a) < cpd::toUtf16(b); }
double numberText(std::string_view value) {
 const auto trimmed = jsTrim(value);
 if (trimmed.empty()) return 0;
 if (trimmed.size() > 2 && trimmed[0] == '0') {
  const char marker = static_cast<char>(trimmed[1] | 32);
  const int base = marker == 'x' ? 16 : marker == 'o' ? 8 : marker == 'b' ? 2 : 0;
  if (base != 0) {
   double result = 0;
   for (std::size_t i = 2; i < trimmed.size(); ++i) {
    const char ch = trimmed[i];
    const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    if (digit < 0 || digit >= base) return std::numeric_limits<double>::quiet_NaN();
    result = result * base + digit;
   }
   return result;
  }
 }
 static const std::regex decimal(R"(^[+-]?(?:(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?|Infinity)$)");
 if (!std::regex_match(trimmed, decimal)) return std::numeric_limits<double>::quiet_NaN();
 char* end = nullptr;
 const double result = std::strtod(trimmed.c_str(), &end);
 return end == trimmed.c_str() + trimmed.size() ? result : std::numeric_limits<double>::quiet_NaN();
}
double number(const Json& value) {
 if (value.is_number()) return value.get<double>();
 if (value.is_null()) return 0;
 if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
 if (value.is_string()) return numberText(value.get_ref<const std::string&>());
 if (value.is_array()) {
  if (value.empty()) return 0;
  if (value.size() == 1) return value[0].is_null() ? 0 : number(value[0]);
 }
 return std::numeric_limits<double>::quiet_NaN();
}
double fieldNumber(const Json& value, std::string_view key) {
 const auto found = value.find(std::string(key));
 return found == value.end() ? std::numeric_limits<double>::quiet_NaN() : number(*found);
}
bool safeInteger(double value) { return std::isfinite(value) && std::trunc(value) == value && std::abs(value) <= maxSafeInteger; }
bool representableInteger(double value) {
 return std::isfinite(value) && std::trunc(value) == value && value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        value < -static_cast<double>(std::numeric_limits<std::int64_t>::min());
}
std::string fieldString(const Json& value, std::string_view key, std::string fallback = {}) {
 const auto found = value.find(std::string(key));
 return found == value.end() || found->is_null() ? fallback : mmltk::tools::js_string(*found);
}
std::string absolutePath(std::string_view path) { return (REPO_ROOT / std::filesystem::path(path)).lexically_normal().string(); }
std::filesystem::path repositoryJoin(std::string_view path) {
 // Node path.join concatenates subsequent rooted components; path.resolve,
 // used for detector identities above, instead replaces the preceding root.
 return std::filesystem::path(REPO_ROOT.string() + '/' + std::string(path)).lexically_normal();
}
bool isCppPath(std::string_view path) {
 const auto normalized = lowerAscii(std::string(path));
 return std::any_of(cppSuffixes.begin(), cppSuffixes.end(), [&](const std::string& suffix) { return normalized.ends_with(suffix) || normalized.ends_with(suffix + ".in"); });
}
Json cppProfile() {
 auto suffixes = cppSuffixes;
 for (const auto& suffix : cppSuffixes) suffixes.push_back(suffix + ".in");
 return {
  {"name", "cpp"}, {"selector", "--cpp"}, {"output", "cleanup/cpp-code-deduplication.json"}, {"purpose", "Exhaustive first-party tracked C/C++/CUDA textual and structural block-duplication cleanup"},
  {"includedDescription", "Git-tracked and untracked-unignored C/C++/CUDA source and header suffixes"}, {"exclusionRules", {"third_party/"}}, {"includedSuffixes", suffixes}, {"includedPrefix", ""},
  {"excludedPrefixes", {"third_party/"}}, {"detectorExcludedPrefixes", nonCppPrefixes},
  {"duplo",
   {{"binary", "/bin/duplo"}, {"minLines", 6}, {"flags", {"-ip"}}, {"contentFilter", "comments and preprocessor-only lines excluded; multiline literal content retained"}, {"filterNonCode", true}}},
  {"cpd", {{"binary", env("PMD_BIN", "pmd")}, {"language", "cpp"}, {"minTokens", 39}, {"flags", {"--ignore-identifiers", "--ignore-literal-sequences"}}}},
  {"inventoryLabels", {{"all", "tracked_cpp_file_count"}, {"deleted", "worktree_deleted_cpp_files"}, {"missing", "missing_tracked_cpp_files"}}}
 };
}
Json toolsProfile() {
 auto profile = cppProfile();
 profile["name"] = "tools";
 profile["selector"] = "--tools";
 profile["output"] = "cleanup/tools-code-deduplication.json";
 profile["purpose"] = "Native tool and wrapper duplication cleanup for shared helpers, classes, and factories";
 profile["includedDescription"] = "Git-tracked and untracked-unignored native modules, sources, and headers under tools/, including the driver and fixtures";
 profile["includedPrefix"] = "tools/";
 profile["includedFiles"] = Json::array();
 profile["exclusionRules"] = {"worktree-deleted paths", "Git-ignored paths", "third-party, build, target, dist, and generated output"};
 profile["detectorExcludedPrefixes"] = Json::array();
 profile["retainedGeneratedPaths"] = Json::array();
 profile["generatedOutputPatterns"] = {R"((^|\/)(?:third_party|build|target|dist|generated)(?:\/|$))"};
 profile["inventoryLabels"] = {{"all", "candidate_tools_file_count"}, {"deleted", "worktree_deleted_tools_files"}, {"missing", "missing_candidate_tools_files"}};
 return profile;
}
Json frontendProfile() {
 return {
  {"name", "frontend"}, {"selector", "--frontend"}, {"output", "cleanup/frontend-code-deduplication.json"},
  {"purpose", "Exhaustive first-party Iced Rust textual and structural block-duplication cleanup"}, {"includedDescription", "Git-tracked and untracked-unignored src/frontend/iced/**/*.rs"},
  {"exclusionRules", {"worktree-deleted paths", "Git-ignored paths", "build, target, dist, and generated output", "third_party"}}, {"includedSuffixes", {".rs"}},
  {"includedPrefix", "src/frontend/iced/"},
  {"excludedPrefixes", {"third_party/", "build/", "src/frontend/iced/build/", "src/frontend/iced/target/", "src/frontend/iced/dist/", "src/frontend/iced/third_party/"}},
  {"detectorExcludedPrefixes", Json::array()}, {"retainedGeneratedPaths", {"src/frontend/iced/src/generated.rs"}},
  {"generatedOutputPatterns", {R"((^|\/)(?:build|target|dist)(?:\/|$))", R"((^|\/)generated(?:\/|$))"}},
  {"duplo", {{"binary", "/bin/duplo"}, {"minLines", 9}, {"flags", Json::array()}, {"contentFilter", "none"}, {"filterNonCode", false}}},
  {"cpd", {{"binary", env("PMD_BIN", "pmd")}, {"language", "rust"}, {"minTokens", 75}, {"flags", Json::array()}}},
  {"inventoryLabels", {{"all", "candidate_rust_file_count"}, {"deleted", "worktree_deleted_rust_files"}, {"missing", "missing_candidate_rust_files"}}}
 };
}
ProcessResult runCommand(const std::string& command, const std::vector<std::string>& arguments, const std::optional<std::string>& input = {}, const std::vector<int>& accepted = {}) {
 std::vector<std::string> argv{command};
 argv.insert(argv.end(), arguments.begin(), arguments.end());
 RunOptions options;
 options.cwd = REPO_ROOT;
 options.input = input.value_or("");
 auto result = capture(argv, options);
 if (result.exit_code != 0 && std::find(accepted.begin(), accepted.end(), result.exit_code) == accepted.end())
  fail(command + " exited with status " + (result.signal ? std::string("null") : std::to_string(result.exit_code)) + "\n" + result.err);
 return result;
}
}  // namespace
const std::filesystem::path REPO_ROOT = std::filesystem::current_path();
const Json INVENTORY_COMMANDS{{"tracked", {"git", "ls-files", "-z"}}, {"untracked", {"git", "ls-files", "--others", "--exclude-standard", "-z"}}, {"deleted", {"git", "ls-files", "--deleted", "-z"}}};
const Json CLEANUP_PROFILES{{"cpp", cppProfile()}, {"frontend", frontendProfile()}, {"tools", toolsProfile()}};
const Json AUTHORED_PROFILE{
 {"name", "authored"}, {"excludedPrefixes", {"third_party/"}}, {"retainedGeneratedPaths", CLEANUP_PROFILES["frontend"]["retainedGeneratedPaths"]},
 {"generatedOutputPatterns", {R"(^(?:cleanup|output|outputs|compiled)(?:\/|$))", R"((^|\/)(?:third_party|build|target|dist|generated|__pycache__|node_modules|\.cache|\.git)(?:\/|$))"}}
};
std::size_t availableThreads() {
 const long configured = ::sysconf(_SC_NPROCESSORS_CONF);
 const auto count = configured > 0 ? static_cast<std::size_t>(configured) : std::size_t{1024};
 const auto bytes = CPU_ALLOC_SIZE(count);
 std::unique_ptr<cpu_set_t, decltype(&std::free)> set(CPU_ALLOC(count), &std::free);
 if (set && ::sched_getaffinity(0, bytes, set.get()) == 0) {
  const auto permitted = CPU_COUNT_S(bytes, set.get());
  if (permitted > 0) return static_cast<std::size_t>(permitted);
 }
 return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}
std::string usageText() {
 return "Usage:\n"
        "  tools/generate_cleanup_json [--cpp|--frontend|--tools]\n\n"
        "With no profile selector, --cpp is used. Each invocation scans the\n"
        "selected first-party source inventory with Duplo and PMD CPD, then writes\n"
        "the corresponding cleanup/*-code-deduplication.json report.\n\n"
        "  --cpp       C/C++/CUDA: Duplo 6 lines; C++ CPD 39 tokens\n"
        "  --frontend  Iced Rust: Duplo 9 lines; Rust CPD 75 tokens\n"
        "  --tools     Native tools/ modules, sources, headers, driver, and fixtures; C++ detector settings\n"
        "              Writes cleanup/tools-code-deduplication.json for shared-helper, class, and factory cleanup\n"
        "  --raw-cpd [--tools] [--review] [--min-tokens N] [--output PATH]  Standalone unfiltered C++ CPD (default 12 tokens)\n"
        "  Raw mode retains every match, ignores no literal sequences/blocks or inline suppressions,\n"
        "  and reports original source spans separately from declaration classification.\n"
        "  --review adds authored-file triage and unreviewed lexical families, plus a Markdown sibling.\n"
        "  --raw-cpd --tools keeps evidence and review within the tool inventory (cleanup/tools-raw.json or tools-review.json).\n"
        "  -h, --help  Show this help";
}
Json rawCpdProfile(double minTokens, std::string_view profileName) {
 if (!safeInteger(minTokens) || minTokens < 2) fail("--min-tokens must be an integer >= 2");
 auto profile = CLEANUP_PROFILES.at(std::string(profileName));
 if (profile.at("cpd").at("language") != "cpp") fail("raw CPD requires a C++ profile");
 profile["cpd"]["minTokens"] = static_cast<std::int64_t>(minTokens);
 profile["cpd"]["raw"] = true;
 profile["cpd"]["flags"] = {"--skip-blocks-pattern", ""};
 return profile;
}
Json parseArgs(const std::vector<std::string>& args) {
 if (args.size() == 2 && args[0] == "--raw-cpd" && (args[1] == "--help" || args[1] == "-h")) return {{"kind", "help"}};
 if (!args.empty() && args[0] == "--raw-cpd") {
  double minTokens = 12;
  std::optional<std::string> output;
  bool review = false, tools = false;
  std::set<std::string> seen;
  for (std::size_t index = 1; index < args.size(); ++index) {
   const auto& option = args[index];
   if ((option != "--tools" && option != "--review" && option != "--min-tokens" && option != "--output") || !seen.insert(option).second)
    fail("--raw-cpd accepts --tools, --review, --min-tokens N and --output PATH once each");
   if (option == "--tools") {
    tools = true;
    continue;
   }
   if (option == "--review") {
    review = true;
    continue;
   }
   if (index + 1 >= args.size() || args[index + 1].empty() || args[index + 1].starts_with("--")) fail("missing value for " + option);
   const auto& value = args[++index];
   if (option == "--min-tokens")
    minTokens = numberText(value);
   else
    output = value;
  }
  const auto stem = tools ? "cleanup/tools-" : "cleanup/declarations-";
  return {
   {"kind", "raw-cpd"}, {"profile", rawCpdProfile(minTokens, tools ? "tools" : "cpp")}, {"review", review}, {"output", output.value_or(std::string(stem) + (review ? "review.json" : "raw.json"))}
  };
 }
 if (args.empty()) return {{"kind", "profile"}, {"profile", CLEANUP_PROFILES.at("cpp")}};
 if (args.size() != 1) fail("expected zero arguments or exactly one of --cpp, --frontend, and --tools");
 if (args[0] == "--help" || args[0] == "-h") return {{"kind", "help"}};
 for (const auto& profile : CLEANUP_PROFILES)
  if (profile.at("selector") == args[0]) return {{"kind", "profile"}, {"profile", profile}};
 fail("unknown argument: " + args[0]);
}
std::optional<std::string> authoredLanguage(std::string_view path) {
 if (isCppPath(path)) return "cpp";
 if (path == "mmltk" || path.ends_with(".sh") || path.ends_with(".bash")) return "shell";
 const auto slash = path.rfind('/');
 const auto name = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
 if (name == "CMakeLists.txt" || (name.size() > 6 && name.ends_with(".cmake")) || (name.size() > 9 && name.ends_with(".cmake.in"))) return "cmake";
 if (name == "Dockerfile" || (name.starts_with("Dockerfile.") && name.size() > 11)) return "dockerfile";
 static const std::map<std::string_view, std::string_view> languages{
  {"rs", "rust"}, {"js", "javascript"}, {"mjs", "javascript"}, {"cjs", "javascript"}, {"jsx", "javascript"}, {"ts", "typescript"}, {"tsx", "typescript"}, {"py", "python"}, {"html", "html"},
  {"css", "css"}, {"scss", "css"}
 };
 const auto dot = path.rfind('.');
 const auto suffix = path.substr(dot == std::string_view::npos ? 0 : dot + 1);
 const auto found = languages.find(suffix);
 return found == languages.end() ? std::nullopt : std::optional<std::string>(found->second);
}
bool profileIncludesPath(const Json& profile, std::string_view path) {
 if (profile.at("name") == "authored") return authoredLanguage(path).has_value();
 if (!path.starts_with(profile.at("includedPrefix").get_ref<const std::string&>()) && !(profile.contains("includedFiles") && containsString(profile.at("includedFiles"), path))) return false;
 if (profile.at("cpd").at("language") == "cpp") return isCppPath(path);
 return lowerAscii(std::string(path)).ends_with(".rs");
}
Json detectorArguments(const Json& profile, const DetectorOptions& options) {
 Json duplo = {"-j", std::to_string(options.threads.value_or(availableThreads())), "-ml", mmltk::tools::js_string(profile.at("duplo").at("minLines"))};
 for (const auto& flag : profile.at("duplo").at("flags")) duplo.push_back(flag);
 for (const auto* arg : {"-json", "-", "-"}) duplo.push_back(arg);
 Json cpd = {
  "cpd", "--file-list", options.fileList.value_or("<temporary-file-list>"), "--language", profile.at("cpd").at("language"), "--minimum-tokens",
  mmltk::tools::js_string(profile.at("cpd").at("minTokens"))
 };
 for (const auto& flag : profile.at("cpd").at("flags")) cpd.push_back(flag);
 for (const auto* arg : {"--format", "xml", "--no-fail-on-violation"}) cpd.push_back(arg);
 return {{"duplo", std::move(duplo)}, {"cpd", std::move(cpd)}};
}
Json collectInventoryInputs() {
 Json inputs = Json::object();
 for (const auto& [kind, command] : INVENTORY_COMMANDS.items()) {
  auto argv = strings(command);
  auto executable = argv.front();
  argv.erase(argv.begin());
  inputs[kind] = split(runCommand(executable, argv).out, '\0', false);
 }
 return inputs;
}
Json buildInventory(const Json& profile, const Json& inputs, const PathAvailable& pathAvailable) {
 const auto tracked = strings(inputs.at("tracked"));
 const auto untracked = strings(inputs.at("untracked"));
 const auto deleted = strings(inputs.at("deleted"));
 const std::unordered_set<std::string> deletedSet(deleted.begin(), deleted.end());
 std::unordered_set<std::string> seen;
 std::vector<std::string> candidates;
 std::size_t trackedCount = 0, untrackedCount = 0;
 auto add = [&](const std::vector<std::string>& paths, std::size_t& count) {
  for (const auto& path : paths)
   if (profileIncludesPath(profile, path)) {
    ++count;
    if (!deletedSet.contains(path) && seen.insert(path).second) candidates.push_back(path);
   }
 };
 add(tracked, trackedCount);
 add(untracked, untrackedCount);
 std::sort(candidates.begin(), candidates.end(), lexicalLess);
 std::vector<std::regex> generatedPatterns;
 if (profile.contains("generatedOutputPatterns"))
  for (const auto& pattern : profile.at("generatedOutputPatterns")) generatedPatterns.emplace_back(pattern.get<std::string>());
 std::vector<std::string> byPrefix, generated, scanned, newlines, unavailable, deletedFiles;
 for (const auto& path : candidates) {
  if (startsAny(path, profile.at("excludedPrefixes"))) {
   byPrefix.push_back(path);
   continue;
  }
  const bool retained = profile.contains("retainedGeneratedPaths") && containsString(profile.at("retainedGeneratedPaths"), path);
  if (!retained && std::any_of(generatedPatterns.begin(), generatedPatterns.end(), [&](const auto& pattern) { return std::regex_search(path, pattern); })) {
   generated.push_back(path);
   continue;
  }
  scanned.push_back(path);
  if (path.find('\n') != std::string::npos) newlines.push_back(path);
 }
 if (!newlines.empty()) fail("Duplo/CPD file lists cannot represent these paths:\n" + joined(newlines, "\n"));
 for (const auto& path : scanned) {
  const bool available = pathAvailable ? pathAvailable(path) : std::filesystem::is_regular_file(repositoryJoin(path));
  if (!available) unavailable.push_back(path);
 }
 const auto name = profile.at("name").get<std::string>();
 if (!unavailable.empty()) fail(name + " inventory paths are unavailable:\n" + joined(unavailable, "\n"));
 if (byPrefix.size() + generated.size() + scanned.size() != candidates.size()) fail(name + " inventory is incomplete");
 for (const auto& path : deletedSet)
  if (profileIncludesPath(profile, path)) deletedFiles.push_back(path);
 std::sort(deletedFiles.begin(), deletedFiles.end(), lexicalLess);
 return {
  {"candidates", candidates}, {"excludedByPrefix", byPrefix}, {"excludedGeneratedOutput", generated}, {"scannedFiles", scanned}, {"deletedFiles", deletedFiles},
  {"trackedCandidateCount", trackedCount}, {"untrackedCandidateCount", untrackedCount}
 };
}
namespace {
bool entityName(std::string_view name) {
 const auto normalized = lowerAscii(std::string(name));
 if (normalized == "lt" || normalized == "gt" || normalized == "amp" || normalized == "quot" || normalized == "apos") return true;
 if (!normalized.starts_with('#')) return false;
 const bool hexadecimal = normalized.starts_with("#x");
 const auto digits = std::string_view(normalized).substr(hexadecimal ? 2 : 1);
 return !digits.empty() && std::all_of(digits.begin(), digits.end(), [&](char ch) { return (ch >= '0' && ch <= '9') || (hexadecimal && ch >= 'a' && ch <= 'f'); });
}
std::optional<std::size_t> entityEnd(std::string_view text, std::size_t start) {
 const auto end = text.find(';', start + 1);
 if (end == std::string_view::npos || !entityName(text.substr(start + 1, end - start - 1))) return {};
 return end + 1;
}
void appendCodePoint(std::string& text, std::uint32_t point) {
 if (point > 0x10ffff) fail("CPD returned an invalid XML character reference");
 if (point <= 0xffff) {
  const char16_t unit = static_cast<char16_t>(point);
  text += cpd::fromUtf16(std::u16string_view(&unit, 1));
 } else {
  point -= 0x10000;
  const char16_t units[]{static_cast<char16_t>(0xd800 + (point >> 10)), static_cast<char16_t>(0xdc00 + (point & 0x3ff))};
  text += cpd::fromUtf16(std::u16string_view(units, 2));
 }
}
std::size_t skipWhitespace(std::string_view text, std::size_t cursor) {
 while (cursor < text.size()) {
  const auto ch = static_cast<unsigned char>(text[cursor]);
  if (ch < 0x80) {
   if (!text::whitespace(ch)) break;
   ++cursor;
   continue;
  }
  const auto length = ch < 0xe0 ? 2U : ch < 0xf0 ? 3U : 4U;
  if (cursor + length > text.size()) break;
  const auto units = cpd::toUtf16(text.substr(cursor, length));
  if (units.size() != 1 || !text::whitespace(units[0])) break;
  cursor += length;
 }
 return cursor;
}
bool tagStarts(std::string_view text, std::size_t offset, std::string_view tag) {
 return text.substr(offset).starts_with(tag) && (offset + tag.size() == text.size() || !text::word(text[offset + tag.size()]));
}
// Collapse only complete codefragment payloads. Source inside CDATA must never
// be interpreted as detector error records or report markup.
std::string fragmentPlaceholders(std::string_view body) {
 std::string result;
 std::size_t copied = 0, search = 0;
 while (true) {
  const auto start = body.find("<codefragment>", search);
  if (start == std::string_view::npos) break;
  auto cursor = start + std::string_view("<codefragment>").size();
  bool valid = false;
  while (cursor < body.size()) {
   if (body.substr(cursor).starts_with("</codefragment>")) {
    cursor += std::string_view("</codefragment>").size();
    valid = true;
    break;
   }
   if (body.substr(cursor).starts_with("<![CDATA[")) {
    const auto close = body.find("]]>", cursor + 9);
    if (close == std::string_view::npos) break;
    cursor = close + 3;
   } else if (body[cursor] == '<')
    break;
   else if (body[cursor] == '&') {
    const auto end = entityEnd(body, cursor);
    if (!end) break;
    cursor = *end;
   } else
    ++cursor;
  }
  if (valid) {
   result.append(body.substr(copied, start - copied));
   result += "<codefragment/>";
   copied = cursor;
   search = cursor;
  } else
   search = start + 1;
 }
 result.append(body.substr(copied));
 return result;
}
std::string cpdDocument(std::string_view xml) {
 std::string document = jsTrim(xml);
 if (document.starts_with("<?xml") && skipWhitespace(document, 5) > 5) {
  const auto firstQuestion = document.find('?', 5);
  if (firstQuestion != std::string::npos && document.substr(firstQuestion, 2) == "?>") document.erase(0, skipWhitespace(document, firstQuestion + 2));
 }
 if (!tagStarts(document, 0, "<pmd-cpd")) fail("CPD returned an incomplete or unrelated XML document");
 const auto open = document.find('>', 8);
 if (open == std::string::npos) fail("CPD returned an incomplete or unrelated XML document");
 const bool selfClosing = open > 0 && document[open - 1] == '/';
 std::string_view attributes(document.data() + 8, open - 8 - (selfClosing ? 1 : 0));
 parseXmlAttributes(attributes);
 std::string body;
 if (selfClosing) {
  if (open + 1 != document.size()) fail("CPD returned an incomplete or unrelated XML document");
 } else {
  constexpr std::string_view close = "</pmd-cpd>";
  if (!document.ends_with(close) || document.size() < open + 1 + close.size()) fail("CPD returned an incomplete or unrelated XML document");
  body = fragmentPlaceholders(std::string_view(document).substr(open + 1, document.size() - open - 1 - close.size()));
 }
 static const std::regex errorTag(R"(<(?:error|processing[-_]?error)\b)", std::regex::icase);
 if (std::regex_search(body, errorTag)) {
  const auto units = cpd::toUtf16(body);
  fail("CPD reported incomplete source coverage: " + cpd::fromUtf16(std::u16string_view(units).substr(0, 1000)));
 }
 return body;
}
// Reads the exact restricted tag syntax used by PMD's XML writer. This is not
// an XML document loader: DTDs, external entities, and arbitrary markup are
// never resolved or accepted.
Json selfClosingAttributes(std::string_view text, std::size_t& cursor, std::string_view tag) {
 const auto end = text.find('>', cursor + tag.size());
 if (end == std::string_view::npos || end == 0 || text[end - 1] != '/') fail("CPD returned malformed or unexpected XML content");
 const auto begin = cursor + tag.size();
 const auto fields = parseXmlAttributes(text.substr(begin, end - begin - 1));
 cursor = end + 1;
 return fields;
}
std::string normalizeCpdPath(const std::string& path, const ParseXmlOptions& options) { return options.normalizePath ? options.normalizePath(path) : path; }
bool validUtf8(std::string_view text) {
 for (std::size_t index = 0; index < text.size();) {
  const auto lead = static_cast<unsigned char>(text[index++]);
  if (lead < 0x80) continue;
  unsigned length = 0;
  std::uint32_t point = 0;
  if (lead >= 0xc2 && lead <= 0xdf) {
   length = 1;
   point = lead & 0x1f;
  } else if (lead >= 0xe0 && lead <= 0xef) {
   length = 2;
   point = lead & 0x0f;
  } else if (lead >= 0xf0 && lead <= 0xf4) {
   length = 3;
   point = lead & 0x07;
  } else
   return false;
  if (index + length > text.size()) return false;
  for (unsigned part = 0; part < length; ++part) {
   const auto ch = static_cast<unsigned char>(text[index++]);
   if ((ch & 0xc0) != 0x80) return false;
   point = (point << 6) | (ch & 0x3f);
  }
  if ((length == 1 && point < 0x80) || (length == 2 && point < 0x800) || (length == 3 && point < 0x10000) || (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) return false;
 }
 return true;
}
std::string sha256(std::string_view bytes) { return mmltk::tools::sha256({reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()}, "cannot calculate source SHA-256"); }
std::string unsuppressedCpdSource(std::string_view path, const std::string& source) {
 if (source.find("CPD-OFF") == std::string::npos && source.find("CPD-ON") == std::string::npos) return source;
 auto units = cpd::toUtf16(source);
 for (const auto& comment : cpd::sourceComments(path, source)) {
  for (auto position = comment.offset; position < comment.end; ++position) {
   const auto tail = std::u16string_view(units).substr(position, comment.end - position);
   if (tail.starts_with(u"CPD-OFF") || tail.starts_with(u"CPD-ON")) units[position + 2] = u'X';
  }
 }
 return cpd::fromUtf16(units);
}
std::vector<std::string> detectorPaths(const Json& profile, const std::vector<std::string>& paths) {
 std::vector<std::string> result;
 for (const auto& path : paths)
  if (!startsAny(path, profile.at("detectorExcludedPrefixes"))) result.push_back(path);
 return result;
}
Json runDuplo(const Json& profile, const std::vector<std::string>& paths) {
 const auto arguments = detectorArguments(profile).at("duplo");
 if (paths.empty()) return {{"hits", Json::array()}, {"arguments", arguments}, {"status", 0}, {"stderr", ""}};
 const auto result = runCommand(profile.at("duplo").at("binary").get<std::string>(), strings(arguments), joined(paths, "\n"), {1});
 const auto stderr = jsTrim(result.err);
 for (std::size_t line = 0; line < stderr.size();) {
  const auto start = skipWhitespace(stderr, line);
  const auto prefix = lowerAscii(stderr.substr(start, 6));
  if (prefix.starts_with("error:") || prefix.starts_with("fatal:")) fail("Duplo reported a scan failure:\n" + stderr);
  const auto end = stderr.find('\n', line);
  if (end == std::string::npos) break;
  line = end + 1;
 }
 return {{"hits", parseDuploJson(result.out)}, {"arguments", arguments}, {"status", result.exit_code}, {"stderr", stderr}};
}
}  // namespace
std::string decodeXmlAttribute(std::string_view value) {
 std::string output;
 for (std::size_t index = 0; index < value.size();) {
  if (value[index] != '&') {
   output += value[index++];
   continue;
  }
  const auto end = entityEnd(value, index);
  if (!end) {
   output += value[index++];
   continue;
  }
  const auto name = lowerAscii(std::string(value.substr(index + 1, *end - index - 2)));
  if (name == "lt")
   output += '<';
  else if (name == "gt")
   output += '>';
  else if (name == "amp")
   output += '&';
  else if (name == "quot")
   output += '"';
  else if (name == "apos")
   output += '\'';
  else {
   const bool hexadecimal = name.starts_with("#x");
   std::uint32_t point = 0;
   for (const auto ch : std::string_view(name).substr(hexadecimal ? 2 : 1)) {
    const unsigned digit = ch <= '9' ? ch - '0' : ch - 'a' + 10;
    point = point * (hexadecimal ? 16 : 10) + digit;
    if (point > 0x10ffff) fail("CPD returned an invalid XML character reference");
   }
   appendCodePoint(output, point);
  }
  index = *end;
 }
 return output;
}
Json parseXmlAttributes(std::string_view tag) {
 Json result = Json::object();
 std::size_t cursor = 0;
 while (cursor < tag.size()) {
  const auto start = skipWhitespace(tag, cursor);
  if (start == tag.size()) break;
  if (start == cursor) fail("CPD returned malformed XML attributes");
  cursor = start;
  while (cursor < tag.size() && (text::word(tag[cursor]) || tag[cursor] == ':' || tag[cursor] == '-')) ++cursor;
  if (cursor == start || cursor + 1 >= tag.size() || tag[cursor] != '=' || (tag[cursor + 1] != '"' && tag[cursor + 1] != '\'')) fail("CPD returned malformed XML attributes");
  const std::string name(tag.substr(start, cursor - start));
  const char quote = tag[++cursor];
  const auto valueStart = ++cursor;
  while (cursor < tag.size() && tag[cursor] != quote && tag[cursor] != '<') ++cursor;
  if (cursor == tag.size() || tag[cursor] != quote) fail("CPD returned malformed XML attributes");
  const auto value = tag.substr(valueStart, cursor - valueStart);
  if (result.contains(name)) fail("CPD returned invalid XML attributes");
  for (std::size_t index = 0; index < value.size(); ++index)
   if (value[index] == '&' && !entityEnd(value, index)) fail("CPD returned invalid XML attributes");
  result[name] = decodeXmlAttribute(value);
  ++cursor;
 }
 return result;
}
Json parseCpdXml(std::string_view xml, const ParseXmlOptions& options) {
 const auto body = cpdDocument(xml);
 Json duplications = Json::array();
 std::vector<std::string> coverage;
 std::unordered_set<std::string> covered;
 for (std::size_t cursor = 0; (cursor = skipWhitespace(body, cursor)) < body.size();) {
  if (tagStarts(body, cursor, "<duplication")) {
   const auto headerEnd = body.find('>', cursor + 12);
   const auto end = body.find("</duplication>", headerEnd == std::string::npos ? body.size() : headerEnd + 1);
   if (headerEnd == std::string::npos || end == std::string::npos) fail("CPD returned malformed or unexpected XML content");
   const auto fields = parseXmlAttributes(std::string_view(body).substr(cursor + 12, headerEnd - cursor - 12));
   const double lineCount = fieldNumber(fields, "lines"), tokenCount = fieldNumber(fields, "tokens");
   if (!safeInteger(lineCount) || !safeInteger(tokenCount) || lineCount <= 0 || tokenCount <= 0) fail("CPD returned an invalid duplication header");
   const auto contents = std::string_view(body).substr(headerEnd + 1, end - headerEnd - 1);
   Json occurrences = Json::array();
   bool fragment = false;
   for (std::size_t offset = 0; (offset = skipWhitespace(contents, offset)) < contents.size();) {
    if (tagStarts(contents, offset, "<file")) {
     const auto attributes = selfClosingAttributes(contents, offset, "<file");
     const double start = fieldNumber(attributes, "line"), finish = fieldNumber(attributes, "endline");
     if (!attributes.contains("path") || attributes.at("path").get_ref<const std::string&>().empty() || !safeInteger(start) || !safeInteger(finish) || start < 1 || finish < start)
      fail("CPD returned an invalid source range or column");
     Json occurrence{{"path", normalizeCpdPath(attributes.at("path").get<std::string>(), options)}, {"start", static_cast<std::int64_t>(start)}, {"end", static_cast<std::int64_t>(finish)}};
     for (const auto& [field, output] : {std::pair{"column", "column"}, std::pair{"endcolumn", "endColumn"}}) {
      if (!attributes.contains(field)) continue;
      const auto column = fieldNumber(attributes, field);
      if (!safeInteger(column) || column < 1) fail("CPD returned an invalid source range or column");
      occurrence[output] = static_cast<std::int64_t>(column);
     }
     occurrences.push_back(std::move(occurrence));
    } else if (!fragment && contents.substr(offset).starts_with("<codefragment/>")) {
     fragment = true;
     offset += std::string_view("<codefragment/>").size();
    } else
     fail("CPD returned an incomplete duplication record");
   }
   if (occurrences.size() < 2) fail("CPD returned an incomplete duplication record");
   std::unordered_set<std::string> coordinates;
   for (const auto& occurrence : occurrences)
    if (!coordinates.insert(occurrence.dump()).second) fail("CPD returned duplicate occurrence coordinates");
   duplications.push_back({{"lineCount", static_cast<std::int64_t>(lineCount)}, {"tokenCount", static_cast<std::int64_t>(tokenCount)}, {"occurrences", std::move(occurrences)}});
   cursor = end + std::string_view("</duplication>").size();
  } else if (tagStarts(body, cursor, "<file")) {
   const auto fields = selfClosingAttributes(body, cursor, "<file");
   const double count = fieldNumber(fields, "totalNumberOfTokens");
   if (!fields.contains("path") || fields.at("path").get_ref<const std::string&>().empty() || !safeInteger(count) || count < 0) fail("CPD returned invalid source coverage");
   const auto path = normalizeCpdPath(fields.at("path").get<std::string>(), options);
   if (!covered.insert(path).second) fail("CPD returned duplicate source coverage: " + path);
   coverage.push_back(path);
  } else
   fail("CPD returned malformed or unexpected XML content");
 }
 if (options.expectedPaths) {
  const std::unordered_set<std::string> expected(options.expectedPaths->begin(), options.expectedPaths->end());
  std::vector<std::string> missing, unexpected;
  std::unordered_set<std::string> encountered;
  for (const auto& path : *options.expectedPaths)
   if (encountered.insert(path).second && !covered.contains(path) && missing.size() < 12) missing.push_back(path);
  for (const auto& path : coverage)
   if (!expected.contains(path) && unexpected.size() < 12) unexpected.push_back(path);
  if (!missing.empty() || !unexpected.empty())
   fail("CPD reported incomplete inventory coverage (" + std::to_string(covered.size()) + "/" + std::to_string(expected.size()) + "); missing: " + joined(missing, ", ") +
        "; unexpected: " + joined(unexpected, ", "));
  for (const auto& match : duplications)
   for (const auto& occurrence : match.at("occurrences")) {
    const auto path = occurrence.at("path").get<std::string>();
    if (!expected.contains(path)) fail("CPD returned an out-of-inventory path: " + path);
   }
 }
 return duplications;
}
Snapshot sourceSnapshot(const std::vector<std::string>& paths) {
 Snapshot snapshot;
 for (const auto& path : paths) {
  auto source = read_text(path);
  if (!validUtf8(source)) fail("source is not valid UTF-8: " + path);
  auto hash = sha256(source);
  snapshot.insert_or_assign(path, SnapshotEntry{std::move(source), std::move(hash)});
 }
 return snapshot;
}
void requireUnchangedSources(const Snapshot& snapshot) {
 for (const auto& [path, entry] : snapshot) {
  if (!std::filesystem::exists(path) || sha256(read_text(path)) != entry.sha256) fail("source changed during analysis: " + path);
 }
}
Json runCpd(const Json& profile, const std::vector<std::string>& paths) { return runCpd(profile, paths, sourceSnapshot(paths)); }
Json runCpd(const Json& profile, const std::vector<std::string>& paths, const Snapshot& snapshot) {
 const auto lexable = detectorPaths(profile, paths);
 if (lexable.empty()) return {{"duplications", Json::array()}, {"arguments", detectorArguments(profile).at("cpd")}, {"excludedLexerCount", paths.size()}, {"status", 0}, {"stderr", ""}};
 std::map<std::string, std::string> originalPaths;
 ProcessResult result;
 Json arguments;
 {
  TemporaryDirectory temporary("cpd-file-list-");
  const auto fileList = temporary.path() / "files.txt";
  DetectorOptions options;
  options.fileList = fileList.string();
  arguments = detectorArguments(profile, options).at("cpd");
  std::vector<std::string> inputs;
  const std::string suffix = profile.at("cpd").at("language") == "rust" ? ".rs" : ".cpp";
  for (std::size_t i = 0; i < lexable.size(); ++i) {
   const auto& path = lexable[i];
   const auto copy = temporary.path() / (std::to_string(i) + suffix);
   write_text(copy, unsuppressedCpdSource(path, snapshot.at(path).source));
   originalPaths.emplace(copy.string(), absolutePath(path));
   inputs.push_back(copy.string());
  }
  write_text(fileList, joined(inputs, "\n") + "\n");
  const auto binary = profile.at("cpd").at("binary").get<std::string>();
  // capture preserves the diagnostic stderr on nonzero exits. Missing exec
  // is reported separately, as with spawn's ENOENT result in the script.
  std::vector<std::string> argv{binary};
  const auto detectorArgs = strings(arguments);
  argv.insert(argv.end(), detectorArgs.begin(), detectorArgs.end());
  RunOptions launch;
  launch.cwd = REPO_ROOT;
  launch.input = "";
  result = capture(argv, launch);
  if (result.exec_errno == ENOENT) fail(binary + " is not installed; install PMD 7.7.0+ or set PMD_BIN");
  if (result.exit_code != 0) fail(binary + " exited with status " + (result.signal ? std::string("null") : std::to_string(result.exit_code)) + "\n" + result.err);
 }
 const auto stderr = jsTrim(result.err);
 static const std::regex failure(R"(\b(?:error|fatal|\w*exception)\b)", std::regex::icase);
 if (std::regex_search(stderr, failure)) fail("CPD could not lex a " + profile.at("name").get<std::string>() + " source outside the configured exclusions:\n" + stderr);
 ParseXmlOptions parser;
 parser.expectedPaths.emplace();
 for (const auto& path : lexable) parser.expectedPaths->push_back(absolutePath(path));
 parser.normalizePath = [&](const std::string& path) {
  const auto found = originalPaths.find(path);
  if (found == originalPaths.end()) fail("CPD returned an unlisted detector input: " + path);
  return found->second;
 };
 auto duplications = parseCpdXml(result.out, parser);
 struct IndexedSource {
  const SnapshotEntry* entry;
  std::vector<std::size_t> lines;
 };
 std::map<std::string, IndexedSource> indexedSources;
 for (const auto& [path, entry] : snapshot) indexedSources.insert_or_assign(absolutePath(path), IndexedSource{&entry, declarations::lineStarts(entry.source)});
 for (const auto& match : duplications)
  for (const auto& occurrence : match.at("occurrences")) {
   const auto path = occurrence.at("path").get<std::string>();
   const auto& entry = indexedSources.at(path);
   declarations::sourceOccurrence(path, entry.entry->source, occurrence, entry.lines);
  }
 requireUnchangedSources(snapshot);
 return {{"duplications", std::move(duplications)}, {"arguments", arguments}, {"excludedLexerCount", paths.size() - lexable.size()}, {"status", result.exit_code}, {"stderr", stderr}};
}
std::string repoRelativePath(std::string_view path, std::string_view repoRoot) {
 const auto prefix = std::string(repoRoot) + '/';
 if (path.starts_with(prefix)) return std::string(path.substr(prefix.size()));
 if (!path.starts_with('/')) return std::string(path);
 fail("CPD returned a path outside the repository: " + std::string(path));
}
std::vector<Range> mergeOverlappingRanges(std::vector<Range> ranges) {
 std::sort(ranges.begin(), ranges.end());
 std::vector<Range> merged;
 for (const auto& range : ranges) {
  if (!merged.empty() && range[0] <= merged.back()[1])
   merged.back()[1] = std::max(merged.back()[1], range[1]);
  else
   merged.push_back(range);
 }
 return merged;
}
std::string commonDirectory(const std::vector<std::string>& paths) {
 std::vector<std::string> common;
 bool first = true;
 for (const auto& path : paths) {
  const auto directory = std::filesystem::path(path).parent_path().generic_string();
  auto parts = directory.empty() || directory == "." ? std::vector<std::string>{} : split(directory, '/');
  if (first) {
   common = std::move(parts);
   first = false;
   continue;
  }
  std::size_t count = 0;
  while (count < std::min(common.size(), parts.size()) && common[count] == parts[count]) ++count;
  common.resize(count);
 }
 return joined(common, "/");
}
Json makeHit(std::int64_t lineCount, std::optional<std::int64_t> tokenCount, const RangesByFile& rangesByFile, bool mergeRanges) {
 Json files = Json::array();
 std::size_t count = 0;
 for (const auto& [path, ranges] : rangesByFile) {
  auto sorted = ranges;
  if (mergeRanges)
   sorted = mergeOverlappingRanges(std::move(sorted));
  else
   std::sort(sorted.begin(), sorted.end());
  count += sorted.size();
  files.push_back({{"path", path}, {"lines", sorted}});
 }
 std::stable_sort(files.begin(), files.end(), [](const Json& a, const Json& b) { return locale_compare(a.at("path").get_ref<const std::string&>(), b.at("path").get_ref<const std::string&>()) < 0; });
 std::vector<std::string> paths;
 for (const auto& file : files) paths.push_back(file.at("path").get<std::string>());
 const auto shared = commonDirectory(paths);
 const bool crossFile = files.size() > 1;
 Json result{{"kind", crossFile ? "cross-file" : "within-file"}, {"line_count", lineCount}};
 if (tokenCount) result["token_count"] = *tokenCount;
 result["file_count"] = files.size();
 result["occurrence_count"] = count;
 if (crossFile) {
  result["highest_shared_directory"] = shared.empty() ? "." : shared;
  result["shared_code_directory"] = shared.empty() ? "shared" : shared;
 }
 result["files"] = std::move(files);
 return result;
}
Json structuralHits(const Json& profile, const Json& duplications, const std::vector<std::string>& scannedPaths, std::string_view repoRoot) {
 const std::unordered_set<std::string> scanned(scannedPaths.begin(), scannedPaths.end());
 Json hits = Json::array();
 for (const auto& duplication : duplications) {
  RangesByFile ranges;
  for (const auto& occurrence : duplication.at("occurrences")) {
   const auto path = repoRelativePath(occurrence.at("path").get_ref<const std::string&>(), repoRoot);
   if (!scanned.contains(path) || !profileIncludesPath(profile, path)) fail("CPD returned a path outside its scope: " + path);
   ranges[path].push_back({occurrence.at("start").get<std::int64_t>(), occurrence.at("end").get<std::int64_t>()});
  }
  std::optional<std::int64_t> tokens;
  if (duplication.contains("tokenCount")) tokens = duplication.at("tokenCount").get<std::int64_t>();
  auto hit = makeHit(duplication.at("lineCount").get<std::int64_t>(), tokens, ranges, true);
  if (duplication.contains("patterns")) hit["patterns"] = duplication.at("patterns");
  if (hit.at("occurrence_count").get<std::size_t>() >= 2) hits.push_back(std::move(hit));
 }
 return hits;
}
std::int64_t duploLineCount(const Json& hit) {
 const double explicitCount = fieldNumber(hit, "LineCount");
 if (representableInteger(explicitCount) && explicitCount > 0) return static_cast<std::int64_t>(explicitCount);
 const double start = fieldNumber(hit, "StartLineNumber1"), end = fieldNumber(hit, "EndLineNumber1");
 return representableInteger(start) && representableInteger(end) && end >= start && representableInteger(end - start + 1) ? static_cast<std::int64_t>(end - start + 1) : 0;
}
Json duploOccurrence(const Json& hit, int side) {
 const auto suffix = std::to_string(side);
 const auto path = fieldString(hit, "SourceFile" + suffix);
 const double start = fieldNumber(hit, "StartLineNumber" + suffix), end = fieldNumber(hit, "EndLineNumber" + suffix);
 if (path.empty() || !representableInteger(start) || !representableInteger(end) || start < 1 || end < start) fail("Duplo returned an invalid source range: " + hit.dump());
 return {{"path", path}, {"start", static_cast<std::int64_t>(start)}, {"end", static_cast<std::int64_t>(end)}};
}
std::string stripCommentsAndStrings(std::string_view source) {
 const auto units = cpd::toUtf16(source);
 std::u16string result;
 std::size_t index = 0;
 auto at = [&](std::size_t offset) { return offset < units.size() ? units[offset] : char16_t{}; };
 static const std::regex rawPrefix(R"((?:^|[^A-Za-z0-9_])(?:u8|[uUL])?R$)");
 while (index < units.size()) {
  const auto current = at(index), next = at(index + 1);
  if (current == u'/' && next == u'/') {
   while (index < units.size() && at(index) != u'\n') ++index;
  } else if (current == u'/' && next == u'*') {
   index += 2;
   while (index < units.size() && !(at(index) == u'*' && at(index + 1) == u'/')) {
    if (at(index) == u'\n') result += u'\n';
    ++index;
   }
   index = std::min(units.size(), index + 2);
  } else if (current == u'"' && std::regex_search(cpd::fromUtf16(std::u16string_view(result).substr(result.size() > 3 ? result.size() - 3 : 0)), rawPrefix)) {
   ++index;
   std::u16string delimiter;
   while (index < units.size() && at(index) != u'(') delimiter += at(index++);
   if (index < units.size()) ++index;
   const auto closer = std::u16string(u")") + delimiter + u'"';
   const auto end = units.find(closer, index);
   const auto limit = end == std::u16string::npos ? units.size() : end;
   result.append(static_cast<std::size_t>(std::count(units.begin() + static_cast<std::ptrdiff_t>(index), units.begin() + static_cast<std::ptrdiff_t>(limit), u'\n')), u'\n');
   index = end == std::u16string::npos ? units.size() : end + closer.size();
   result += u' ';
  } else if (current == u'"' || current == u'\'') {
   ++index;
   while (index < units.size() && at(index) != current && at(index) != u'\n') {
    if (at(index) == u'\\') ++index;
    ++index;
   }
   if (index < units.size() && at(index) == current) ++index;
   result += u' ';
  } else {
   result += current;
   ++index;
  }
 }
 return cpd::fromUtf16(result);
}
Json filterDuploCandidates(const Json& profile, const Json& rawHits, const SourceReader& sourceReader) {
 if (!profile.at("duplo").at("filterNonCode").get<bool>()) return {{"hits", rawHits}, {"filteredCount", 0}};
 const SourceReader reader = sourceReader ? sourceReader : SourceReader([](const std::string& path) { return read_text(repositoryJoin(path)); });
 struct Lines {
  std::vector<std::string> code;
  std::set<std::size_t> literals;
 };
 std::map<std::string, Lines> cache;
 auto codeCount = [&](const Json& occurrence, bool literal = false) {
  const auto path = occurrence.at("path").get<std::string>();
  auto entry = cache.find(path);
  if (entry == cache.end()) {
   const auto source = reader(path);
   const auto units = cpd::toUtf16(source);
   Lines lines{split(stripCommentsAndStrings(source), '\n'), {}};
   std::size_t offset = 0, line = 0;
   for (const auto& token : cpd::tokenizeCpp(std::u16string_view(units), true)) {
    while (offset < token.offset)
     if (units[offset++] == u'\n') ++line;
    const bool multiline = token.kind == "literal" && token.text.find('\n') != std::string::npos;
    while (offset < token.end) {
     if (multiline && !text::whitespace(units[offset])) lines.literals.insert(line);
     if (units[offset++] == u'\n') ++line;
    }
   }
   entry = cache.emplace(path, std::move(lines)).first;
  }
  const auto& lines = entry->second.code;
  std::int64_t count = 0;
  const auto end = std::min<std::int64_t>(occurrence.at("end").get<std::int64_t>(), static_cast<std::int64_t>(lines.size()));
  for (auto index = occurrence.at("start").get<std::int64_t>() - 1; index < end; ++index) {
   const auto line = jsTrim(lines[static_cast<std::size_t>(index)]);
   if (literal ? entry->second.literals.contains(static_cast<std::size_t>(index)) : !line.empty() && !line.starts_with('#')) ++count;
  }
  return count;
 };
 Json hits = Json::array(), embedded = Json::array(), nonCodeRejected = Json::array();
 const auto minimum = profile.at("duplo").at("minLines").get<std::int64_t>();
 for (const auto& hit : rawHits) {
  const std::array<Json, 2> occurrences{duploOccurrence(hit, 1), duploOccurrence(hit, 2)};
  if (codeCount(occurrences[0]) >= minimum && codeCount(occurrences[1]) >= minimum)
   hits.push_back(hit);
  else if (codeCount(occurrences[0], true) >= minimum && codeCount(occurrences[1], true) >= minimum)
   embedded.push_back(hit);
  else
   nonCodeRejected.push_back({{"occurrences", occurrences}, {"reason", "insufficient authored code or multiline literal content"}});
 }
 const auto filteredCount = nonCodeRejected.size();
 if (profile.at("cpd").at("language") != "cpp") return {{"hits", hits}, {"filteredCount", filteredCount}};
 Json candidates = Json::array();
 for (std::size_t index = 0; index < hits.size(); ++index)
  candidates.push_back({{"sourceHitIndex", index}, {"lineCount", duploLineCount(hits[index])}, {"occurrences", {duploOccurrence(hits[index], 1), duploOccurrence(hits[index], 2)}}});
 cpd::FilterOptions options;
 options.sourceReader = reader;
 options.consolidate = false;
 auto contextual = cpd::filterCpdCandidates(candidates, options);
 std::set<std::size_t> retained;
 for (const auto& hit : contextual.at("duplications")) retained.insert(hit.at("sourceHitIndex").get<std::size_t>());
 Json selected = Json::array();
 for (std::size_t index = 0; index < hits.size(); ++index)
  if (retained.contains(index)) selected.push_back(hits[index]);
 for (const auto& hit : embedded) selected.push_back(hit);
 auto rejected = contextual.at("filtered");
 for (auto& hit : rejected) hit.erase("sourceHitIndex");
 return {{"hits", std::move(selected)}, {"filteredCount", filteredCount}, {"nonCodeRejected", std::move(nonCodeRejected)}, {"contextRejected", std::move(rejected)}};
}
namespace {
std::string duploDuplicateKey(const Json& hit) {
 if (hit.contains("Lines") && hit.at("Lines").is_array() && !hit.at("Lines").empty()) {
  std::vector<std::string> lines;
  for (const auto& line : hit.at("Lines")) lines.push_back(line.is_null() ? "" : mmltk::tools::js_string(line));
  return std::to_string(duploLineCount(hit)) + '\0' + joined(lines, "\n");
 }
 std::vector<std::string> fields{"locations", std::to_string(duploLineCount(hit))};
 for (const auto* key : {"SourceFile1", "StartLineNumber1", "EndLineNumber1", "SourceFile2", "StartLineNumber2", "EndLineNumber2"}) fields.push_back(fieldString(hit, key));
 return joined(fields, std::string_view("\0", 1));
}
template <class T>
int comparison(const T& a, const T& b) {
 return a < b ? -1 : a > b ? 1 : 0;
}
void sortHits(Json& hits) {
 std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return compareHits(a, b) < 0; });
}
}  // namespace
Json compactDuploHits(const Json& profile, const Json& rawHits, const std::vector<std::string>& scannedPaths) {
 const std::unordered_set<std::string> scanned(scannedPaths.begin(), scannedPaths.end());
 struct Group {
  std::int64_t lineCount = 0;
  RangesByFile ranges;
  std::set<std::string> coordinates;
 };
 std::map<std::string, std::size_t> groupIndex;
 std::vector<Group> groups;
 for (const auto& hit : rawHits) {
  const std::array<Json, 2> occurrences{duploOccurrence(hit, 1), duploOccurrence(hit, 2)};
  for (const auto& occurrence : occurrences) {
   const auto path = occurrence.at("path").get<std::string>();
   if (!scanned.contains(path) || !profileIncludesPath(profile, path)) fail("Duplo returned a path outside its scope: " + path);
  }
  const auto key = duploDuplicateKey(hit);
  const auto [found, inserted] = groupIndex.emplace(key, groups.size());
  if (inserted) groups.emplace_back();
  auto& group = groups[found->second];
  group.lineCount = std::max(group.lineCount, duploLineCount(hit));
  for (const auto& occurrence : occurrences) {
   const auto path = occurrence.at("path").get<std::string>();
   const auto start = occurrence.at("start").get<std::int64_t>(), end = occurrence.at("end").get<std::int64_t>();
   const auto identity = path + '\0' + std::to_string(start) + '\0' + std::to_string(end);
   if (group.coordinates.insert(identity).second) group.ranges[path].push_back({start, end});
  }
 }
 Json result = Json::array();
 for (const auto& group : groups) result.push_back(makeHit(group.lineCount, {}, group.ranges, false));
 return result;
}
int compareHits(const Json& left, const Json& right) {
 if (left.at("kind") != right.at("kind")) return left.at("kind") == "cross-file" ? -1 : 1;
 for (const auto* field : {"file_count", "occurrence_count", "line_count"}) {
  const int order = comparison(right.at(field).get<std::int64_t>(), left.at(field).get<std::int64_t>());
  if (order) return order;
 }
 const int paths = locale_compare(left.at("files")[0].at("path").get_ref<const std::string&>(), right.at("files")[0].at("path").get_ref<const std::string&>());
 if (paths) return paths;
 return comparison(left.at("files")[0].at("lines")[0][0].get<std::int64_t>(), right.at("files")[0].at("lines")[0][0].get<std::int64_t>());
}
Json summarize(const Json& hits) {
 std::set<std::string> files;
 std::size_t cross = 0, within = 0;
 for (const auto& hit : hits) {
  for (const auto& file : hit.at("files")) files.insert(file.at("path").get<std::string>());
  if (hit.at("kind") == "cross-file") ++cross;
  if (hit.at("kind") == "within-file") ++within;
 }
 return {{"hit_count", hits.size()}, {"cross_file_hits", cross}, {"within_file_hits", within}, {"affected_file_count", files.size()}};
}
Suppressions parseInlineSuppressions(std::string_view path, std::string_view source) {
 const auto lines = split(source, '\n');
 const auto units = cpd::toUtf16(source);
 const auto starts = declarations::lineStarts(source);
 Json suppressions = Json::array();
 std::optional<Json> open;
 std::size_t line = 0;
 auto marker = [](std::u16string_view comment, std::u16string_view prefix) -> std::optional<std::string> {
  if (!comment.starts_with(u"//")) return {};
  comment = trimUnits(comment.substr(2));
  if (!comment.starts_with(prefix)) return {};
  comment.remove_prefix(prefix.size());
  // The original marker expression's dot does not span a line terminator.
  const auto reason = trimUnits(comment);
  if (reason.empty() || reason.find_first_of(u"\r\n\u2028\u2029") != std::u16string_view::npos) return {};
  return cpd::fromUtf16(reason);
 };
 auto terminator = [](std::u16string_view comment, std::u16string_view prefix) {
  comment = trimUnits(comment.substr(2));
  if (!comment.starts_with(prefix)) return false;
  if (comment.size() == prefix.size()) return true;
  const auto next = comment[prefix.size()];
  return !text::word(next);
 };
 const auto location = [&](std::size_t number) { return std::string(path) + ':' + std::to_string(number); };
 for (const auto& comment : cpd::sourceComments(path, source)) {
  if (!comment.text.starts_with("//")) continue;
  while (line + 1 < starts.size() && starts[line + 1] <= comment.offset) ++line;
  const auto text = cpd::toUtf16(comment.text);
  if (const auto reason = marker(text, u"CLEANUP-IGNORE:")) {
   const auto prefix = trimUnits(std::u16string_view(units).substr(starts[line], comment.offset - starts[line]));
   auto target = line + 1;
   if (prefix.empty()) {
    auto index = line + 1;
    while (index < lines.size()) {
     const auto candidate = jsTrim(lines[index]);
     if (!candidate.empty() && !candidate.starts_with("//")) break;
     ++index;
    }
    target = index + 1;
   }
   suppressions.push_back({{"kind", "single"}, {"detectors", {"duplo", "cpd"}}, {"marker_line", line + 1}, {"start_line", target}, {"end_line", target}, {"reason", *reason}});
   continue;
  }
  auto reason = marker(text, u"CLEANUP-OFF:");
  std::string family = "CLEANUP";
  if (!reason) {
   reason = marker(text, u"CPD-OFF:");
   family = "CPD";
  }
  if (reason) {
   if (open) fail("nested cleanup suppression at " + location(line + 1));
   open = Json{{"family", family}, {"detectors", family == "CPD" ? Json::array({"cpd"}) : Json::array({"duplo", "cpd"})}, {"marker_line", line + 1}, {"reason", *reason}};
   continue;
  }
  const bool cleanupOn = terminator(text, u"CLEANUP-ON"), cpdOn = terminator(text, u"CPD-ON");
  if (cleanupOn || cpdOn) {
   if (!open || open->at("family") != (cleanupOn ? "CLEANUP" : "CPD")) fail("unmatched cleanup suppression terminator at " + location(line + 1));
   suppressions.push_back(
    {{"kind", "range"}, {"detectors", open->at("detectors")}, {"marker_line", open->at("marker_line")}, {"start_line", open->at("marker_line")}, {"end_line", line + 1},
     {"reason", open->at("reason")}});
   open.reset();
  }
 }
 if (open) fail("unterminated cleanup suppression at " + location(open->at("marker_line").get<std::size_t>()));
 std::stable_sort(suppressions.begin(), suppressions.end(),
  [](const Json& a, const Json& b) { return a.at("start_line") < b.at("start_line") || (a.at("start_line") == b.at("start_line") && a.at("end_line") < b.at("end_line")); });
 for (std::size_t index = 1; index < suppressions.size(); ++index)
  if (suppressions[index].at("start_line") <= suppressions[index - 1].at("end_line"))
   fail("overlapping atomic cleanup suppressions at " + location(suppressions[index].at("marker_line").get<std::size_t>()));
 Suppressions result;
 if (!suppressions.empty()) result.emplace(std::string(path), std::move(suppressions));
 return result;
}
Json applyInlineSuppressions(const Json& hits, const Suppressions& suppressions, std::string_view detector) {
 Json remaining = Json::array(), suppressed = Json::array();
 for (const auto& hit : hits) {
  RangesByFile ranges;
  std::size_t occurrences = 0;
  for (const auto& file : hit.at("files")) {
   const auto path = file.at("path").get<std::string>();
   const auto candidates = suppressions.find(path);
   for (const auto& range : file.at("lines")) {
    const auto start = range[0].get<std::int64_t>(), end = range[1].get<std::int64_t>();
    const Json* suppression = nullptr;
    if (candidates != suppressions.end())
     for (const auto& candidate : candidates->second) {
      if (!containsString(candidate.at("detectors"), detector)) continue;
      if ((candidate.at("kind") == "single" && candidate.at("start_line") == start) ||
          (candidate.at("kind") == "range" && candidate.at("start_line").get<std::int64_t>() <= start && end <= candidate.at("end_line").get<std::int64_t>())) {
       suppression = &candidate;
       break;
      }
     }
    if (!suppression) {
     ranges[path].push_back({start, end});
     ++occurrences;
    } else
     suppressed.push_back(
      {{"detector", detector}, {"path", path}, {"lines", range}, {"marker_line", suppression->at("marker_line")}, {"suppression_kind", suppression->at("kind")},
       {"suppression_lines", {suppression->at("start_line"), suppression->at("end_line")}}, {"reason", suppression->at("reason")}});
   }
  }
  if (occurrences >= 2) {
   std::optional<std::int64_t> tokens;
   if (hit.contains("token_count")) tokens = hit.at("token_count").get<std::int64_t>();
   auto retained = makeHit(hit.at("line_count").get<std::int64_t>(), tokens, ranges, false);
   if (hit.contains("patterns")) retained["patterns"] = hit.at("patterns");
   remaining.push_back(std::move(retained));
  }
 }
 return {{"hits", std::move(remaining)}, {"suppressed", std::move(suppressed)}};
}
void requireAtomicSuppressionUse(const Json& suppressedOccurrences) {
 std::map<std::string, std::set<std::int64_t>> uses;
 for (const auto& occurrence : suppressedOccurrences) {
  const auto detector = occurrence.at("detector").get<std::string>(), path = occurrence.at("path").get<std::string>();
  const auto marker = mmltk::tools::js_string(occurrence.at("marker_line"));
  auto& starts = uses[detector + '\0' + path + '\0' + marker];
  starts.insert(occurrence.at("lines")[0].get<std::int64_t>());
  if (starts.size() > 1) fail("cleanup suppression at " + path + ':' + marker + " hides multiple local " + detector + " starts; split it into atomic inline suppressions");
 }
}
Json parseDuploJson(std::string_view text) {
 // JSON.parse does not admit a BOM before a value (even though XML does).
 // nlohmann's parser otherwise accepts a leading UTF-8 BOM automatically.
 if (text.starts_with("\xef\xbb\xbf") && !jsTrim(text).empty()) fail("Duplo returned invalid JSON: unexpected byte order mark");
 auto hits = jsTrim(text).empty() ? Json::array() : Json::parse(text);
 if (hits.is_null()) hits = Json::array();
 if (!hits.is_array()) fail("Duplo JSON output is not an array");
 return hits;
}
namespace {
Json scopeMetadata(const Json& profile, const Json& files) {
 const Json commands{{"tracked", INVENTORY_COMMANDS.at("tracked")}, {"untracked_unignored", INVENTORY_COMMANDS.at("untracked")}, {"worktree_deleted", INVENTORY_COMMANDS.at("deleted")}};
 if (profile.at("name") == "cpp") {
  return {
   {"tracked_file_source", INVENTORY_COMMANDS.at("tracked")}, {"included_suffixes", profile.at("includedSuffixes")}, {"excluded_path_prefixes", profile.at("excludedPrefixes")},
   {"tracked_cpp_file_count", files.at("candidates").size()}, {"excluded_third_party_file_count", files.at("excludedByPrefix").size()}, {"scanned_file_count", files.at("scannedFiles").size()},
   {"worktree_deleted_cpp_files", files.at("deletedFiles")}, {"missing_tracked_cpp_files", Json::array()}, {"inventory_commands", commands},
   {"tracked_candidate_file_count", files.at("trackedCandidateCount")}, {"untracked_unignored_candidate_file_count", files.at("untrackedCandidateCount")},
   {"excluded_generated_output_files", Json::array()}
  };
 }
 Json result{
  {"inventory_commands", commands}, {"inclusion_rule", profile.at("includedDescription")}, {"exclusion_rules", profile.at("exclusionRules")},
  {"excluded_path_prefixes", profile.at("excludedPrefixes")}, {"tracked_candidate_file_count", files.at("trackedCandidateCount")},
  {"untracked_unignored_candidate_file_count", files.at("untrackedCandidateCount")}, {"worktree_deleted_candidate_file_count", files.at("deletedFiles").size()},
  {"excluded_by_prefix_file_count", files.at("excludedByPrefix").size()}, {"excluded_generated_output_file_count", files.at("excludedGeneratedOutput").size()},
  {"excluded_generated_output_files", files.at("excludedGeneratedOutput")}, {"scanned_file_count", files.at("scannedFiles").size()}
 };
 result[profile.at("inventoryLabels").at("deleted").get<std::string>()] = files.at("deletedFiles");
 result[profile.at("inventoryLabels").at("missing").get<std::string>()] = Json::array();
 result["included_prefix"] = profile.at("includedPrefix");
 if (profile.contains("includedFiles")) result["included_files"] = profile.at("includedFiles");
 result["included_suffixes"] = profile.at("includedSuffixes");
 result["retained_generated_paths"] = profile.at("retainedGeneratedPaths");
 result["generated_output_exclusion_patterns"] = profile.at("generatedOutputPatterns");
 result["excluded_by_prefix_files"] = files.at("excludedByPrefix");
 result[profile.at("inventoryLabels").at("all").get<std::string>()] = files.at("candidates").size();
 return result;
}
std::string elapsedSeconds(std::chrono::steady_clock::time_point start) {
 const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
 std::ostringstream formatted;
 formatted << std::fixed << std::setprecision(1) << elapsed;
 return formatted.str();
}
}  // namespace
Json buildReport(const Json& input, const Clock& clock) {
 const auto& profile = input.at("profile");
 const auto& duplo = input.at("duplo");
 const auto& cpd = input.at("cpd");
 const auto& codeOnly = input.at("codeOnlyDuplo");
 Json duploReport{
  {"binary", profile.at("duplo").at("binary")}, {"arguments", duplo.at("arguments")}, {"min_lines", profile.at("duplo").at("minLines")}, {"content_filter", profile.at("duplo").at("contentFilter")},
  {"non_code_hits_filtered", codeOnly.at("filteredCount")}, {"non_code_rejected", codeOnly.value("nonCodeRejected", Json::array())},
  {"context_filter_rejected", codeOnly.value("contextRejected", Json::array())}, {"threads", input.value("threads", Json(availableThreads()))}, {"status", duplo.at("status")},
  {"stderr", duplo.at("stderr")}
 };
 if (duplo.contains("hits")) duploReport["raw_hits"] = duplo.at("hits");
 Json cpdReport{
  {"binary", profile.at("cpd").at("binary")}, {"arguments", cpd.at("arguments")}, {"language", profile.at("cpd").at("language")}, {"min_tokens", profile.at("cpd").at("minTokens")},
  {"flags", profile.at("cpd").at("flags")}
 };
 if (profile.at("cpd").at("language") == "cpp") {
  cpdReport["excluded_non_cpp_prefixes"] = profile.at("detectorExcludedPrefixes");
  cpdReport["excluded_non_cpp_file_count"] = cpd.at("excludedLexerCount");
 } else {
  cpdReport["excluded_lexer_prefixes"] = profile.at("detectorExcludedPrefixes");
  cpdReport["excluded_lexer_file_count"] = cpd.at("excludedLexerCount");
 }
 cpdReport["status"] = cpd.at("status");
 cpdReport["stderr"] = cpd.at("stderr");
 return {
  {"generated_at", clock ? clock() : utc_timestamp(TimestampFormat::javascript, "cannot format cleanup report time")}, {"profile", profile.at("name")}, {"purpose", profile.at("purpose")},
  {"scope", scopeMetadata(profile, input.at("files"))}, {"duplo", std::move(duploReport)}, {"cpd", std::move(cpdReport)},
  {"hit_order", {"cross-file before within-file", "higher file count", "higher occurrence count", "higher line count"}}, {"summary", summarize(input.at("hits"))},
  {"structural_summary", summarize(input.at("structuralHitsAfterSuppression"))},
  {"inline_duplication_suppression",
   {{"markers", {"// CLEANUP-IGNORE: <false-positive reason>", "// CLEANUP-OFF: <false-positive reason> ... // CLEANUP-ON", "// CPD-OFF: <false-positive reason> ... // CPD-ON"}},
    {"policy", "Inline and atomic: one comment targets one occurrence, and one bounded range encloses exactly one multi-line occurrence."},
    {"suppressed_duplo_occurrences", input.at("duploSuppression").at("suppressed")}, {"suppressed_cpd_occurrences", input.at("cpdSuppression").at("suppressed")}}},
  {"hits", input.at("hits")}, {"structural_hits", input.at("structuralHitsAfterSuppression")}
 };
}
std::string serializeReport(const Json& report) {
 Json result{{"hits", Json::array()}, {"structural_hits", Json::array()}};
 for (const auto* kind : {"hits", "structural_hits"})
  for (const auto& hit : report.at(kind)) {
   Json concise = Json::object();
   if (hit.contains("token_count"))
    concise["token_count"] = hit.at("token_count");
   else
    concise["line_count"] = hit.at("line_count");
   if (hit.contains("patterns")) concise["patterns"] = hit.at("patterns");
   concise["files"] = hit.at("files");
   result[kind].push_back(std::move(concise));
  }
 return json_dump(result, 2) + '\n';
}
Json rejectionReport(const Json& previous, const Json& report, const Json& filteredCpd) {
 auto diagnostics = report;
 diagnostics.erase("hits");
 diagnostics.erase("structural_hits");
 Json rejected = Json::array();
 for (const auto& duplication : filteredCpd.at("filtered")) {
  auto occurrences = duplication.at("occurrences");
  for (auto& occurrence : occurrences) occurrence["path"] = repoRelativePath(occurrence.at("path").get_ref<const std::string&>());
  Json entry{{"reason", duplication.at("reason")}};
  if (duplication.contains("tokenCount")) entry["token_count"] = duplication.at("tokenCount");
  entry["occurrences"] = std::move(occurrences);
  rejected.push_back(std::move(entry));
 }
 diagnostics["cpd_candidate_filter"] = {
  {"enabled", report.at("cpd").at("language") == "cpp"}, {"maximum_tokens", nullptr}, {"indexed_files", filteredCpd.at("indexedFiles")}, {"rejected", std::move(rejected)}
 };
 Json result = previous.is_object() ? previous : Json::object();
 result[report.at("profile").get<std::string>()] = std::move(diagnostics);
 return result;
}
void writeAtomic(const std::filesystem::path& path, std::string_view contents) {
 const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
 std::filesystem::create_directories(parent);
 std::string pattern = (parent / ".cleanup-XXXXXX").string();
 if (!::mkdtemp(pattern.data())) throw std::system_error(errno, std::generic_category(), "mkdtemp cleanup publication");
 struct RemoveDirectory {
  std::filesystem::path path;
  ~RemoveDirectory() {
   std::error_code error;
   std::filesystem::remove_all(path, error);
  }
 } remove{pattern};
 const auto temporary = remove.path / path.filename();
 write_text(temporary, contents);
 std::filesystem::rename(temporary, path);
}
namespace {
Json nativeProducer() {
 const auto executable = executable_path();
 FileHasher digest;
 Json result{{"executable", executable.string()}, {"executable_sha256", digest(executable)}, {"module", "mmltk.tools.generate_cleanup_json"}, {"source", "tools/cleanup/generate_cleanup_json.cpp"}};
 const auto inputs = executable.parent_path() / ".inputs.sha256";
 if (std::filesystem::is_regular_file(inputs)) result["tools_inputs_sha256"] = trim(read_text(inputs));
 return result;
}
}  // namespace
Json generateReport(const Json& profile, const GenerateReportOptions& options) {
 const auto started = std::chrono::steady_clock::now();
 const auto inputs = options.inventoryInputs ? *options.inventoryInputs : collectInventoryInputs();
 const auto files = buildInventory(profile, inputs, options.pathAvailable);
 const auto paths = strings(files.at("scannedFiles"));
 const auto snapshot = sourceSnapshot(paths);
 const auto duploPaths = detectorPaths(profile, paths);
 const auto externalStarted = std::chrono::steady_clock::now();
 // Both detector processes run concurrently. Future destruction joins even
 // when CPD rejects coverage, so no process outlives this analysis invocation.
 auto duploFuture = std::async(std::launch::async, [&] { return runDuplo(profile, duploPaths); });
 const auto cpdResult = runCpd(profile, paths, snapshot);
 const auto duplo = duploFuture.get();
 const auto externalSeconds = elapsedSeconds(externalStarted);
 const SourceReader relativeReader = [&](const std::string& path) { return snapshot.at(path).source; };
 std::map<std::string, const SnapshotEntry*> absoluteSources;
 for (const auto& [path, entry] : snapshot) absoluteSources.emplace(absolutePath(path), &entry);
 const auto codeOnly = filterDuploCandidates(profile, duplo.at("hits"), relativeReader);
 auto rawDuplo = compactDuploHits(profile, codeOnly.at("hits"), paths);
 sortHits(rawDuplo);
 Json filteredCpd;
 if (profile.at("cpd").at("language") == "cpp") {
  cpd::FilterOptions filter;
  filter.sourceReader = [&](const std::string& path) { return absoluteSources.at(path)->source; };
  filteredCpd = cpd::filterCpdCandidates(cpdResult.at("duplications"), filter);
 } else
  filteredCpd = {{"duplications", cpdResult.at("duplications")}, {"filtered", Json::array()}, {"reasons", Json::object()}, {"indexedFiles", 0}};
 auto rawCpd = structuralHits(profile, filteredCpd.at("duplications"), paths);
 sortHits(rawCpd);
 Suppressions suppressions;
 for (const auto& path : paths) {
  auto parsed = parseInlineSuppressions(path, snapshot.at(path).source);
  suppressions.merge(parsed);
 }
 auto duploSuppression = applyInlineSuppressions(rawDuplo, suppressions, "duplo");
 auto cpdSuppression = applyInlineSuppressions(rawCpd, suppressions, "cpd");
 auto suppressed = duploSuppression.at("suppressed");
 for (const auto& occurrence : cpdSuppression.at("suppressed")) suppressed.push_back(occurrence);
 requireAtomicSuppressionUse(suppressed);
 sortHits(duploSuppression["hits"]);
 sortHits(cpdSuppression["hits"]);
 const Json input{
  {"profile", profile}, {"files", files}, {"duplo", duplo}, {"cpd", cpdResult}, {"codeOnlyDuplo", codeOnly}, {"hits", duploSuppression.at("hits")},
  {"structuralHitsAfterSuppression", cpdSuppression.at("hits")}, {"duploSuppression", duploSuppression}, {"cpdSuppression", cpdSuppression}
 };
 auto report = buildReport(input, options.clock);
 report["producer"] = nativeProducer();
 const auto output = profile.at("output").get<std::string>();
 const auto outputPath = repositoryJoin(output);
 const auto rejectedPath = outputPath.parent_path() / "rejected.json";
 const auto previous = std::filesystem::exists(rejectedPath) ? read_json(rejectedPath) : Json::object();
 const auto rejectedContents = json_dump(rejectionReport(previous, report, filteredCpd), 2) + '\n';
 const auto contents = serializeReport(report);
 requireUnchangedSources(snapshot);
 writeAtomic(rejectedPath, rejectedContents);
 writeAtomic(outputPath, contents);
 std::cout << "wrote " << output << '\n';
 if (profile.at("name") == "cpp") {
  std::cout << "scanned " << paths.size() << " tracked C/C++/CUDA files\n"
            << "excluded " << files.at("excludedByPrefix").size() << " tracked third-party files\n";
 } else {
  std::cout << "scanned " << paths.size() << (profile.at("cpd").at("language") == "cpp" ? " native tool and wrapper files\n" : " Iced Rust files\n") << "excluded "
            << files.at("excludedByPrefix").size() + files.at("excludedGeneratedOutput").size() << " out-of-scope files\n";
 }
 std::cout << "Duplo: " << report.at("summary").at("cross_file_hits") << " cross-file and " << report.at("summary").at("within_file_hits") << " within-file groups\n"
           << "CPD: " << report.at("structural_summary").at("cross_file_hits") << " cross-file and " << report.at("structural_summary").at("within_file_hits") << " within-file groups\n";
 if (profile.at("cpd").at("language") == "cpp") std::cout << "CPD candidate filter: " << filteredCpd.at("filtered").size() << " rejected; details in cleanup/rejected.json\n";
 std::cout << "timing: detectors " << externalSeconds << "s, total " << elapsedSeconds(started) << "s\n";
 return report;
}
Json rawCpdReport(const Json& profile, const Json& files, const Json& cpdResult, const SourceReader& sourceReader) {
 if (cpdResult.at("status") != 0) fail("CPD raw scan exited with status " + mmltk::tools::js_string(cpdResult.at("status")) + '\n' + cpdResult.at("stderr").get<std::string>());
 const SourceReader reader = sourceReader ? sourceReader : SourceReader([](const std::string& path) { return read_text(path); });
 std::map<std::string, std::string> sources;
 std::map<std::string, std::vector<std::size_t>> lines;
 const auto paths = strings(files.at("scannedFiles"));
 for (const auto& path : paths) sources.insert_or_assign(path, reader(path));
 for (const auto& [path, source] : sources) lines.emplace(path, declarations::lineStarts(source));
 declarations::Options classification;
 classification.macroConflicts = declarations::inventoryMacroConflicts(sources);
 Json classified = Json::array();
 for (const auto& path : paths) {
  const auto result = declarations::classifyDeclarations(path, sources.at(path), classification);
  for (const auto& candidate : result.at("candidates")) classified.push_back(candidate);
 }
 auto matches = cpdResult.at("duplications");
 auto compareOccurrence = [](const Json& a, const Json& b) {
  const auto paths = locale_compare(a.at("path").get_ref<const std::string&>(), b.at("path").get_ref<const std::string&>(), true);
  if (paths) return paths;
  for (const auto& [field, fallback] : {std::pair{"start", 0}, std::pair{"column", 1}, std::pair{"end", 0}, std::pair{"endColumn", 0}}) {
   const auto value = comparison(a.value(field, std::int64_t{fallback}), b.value(field, std::int64_t{fallback}));
   if (value) return value;
  }
  return 0;
 };
 for (auto& match : matches) std::stable_sort(match["occurrences"].begin(), match["occurrences"].end(), [&](const Json& a, const Json& b) { return compareOccurrence(a, b) < 0; });
 std::stable_sort(matches.begin(), matches.end(), [](const Json& a, const Json& b) {
  const auto& left = a.at("occurrences")[0];
  const auto& right = b.at("occurrences")[0];
  const auto paths = locale_compare(left.at("path").get_ref<const std::string&>(), right.at("path").get_ref<const std::string&>(), true);
  if (paths) return paths < 0;
  const auto starts = comparison(left.at("start").get<std::int64_t>(), right.at("start").get<std::int64_t>());
  if (starts) return starts < 0;
  const auto columns = comparison(left.value("column", std::int64_t{1}), right.value("column", std::int64_t{1}));
  if (columns) return columns < 0;
  const auto tokens = comparison(a.at("tokenCount").get<std::int64_t>(), b.at("tokenCount").get<std::int64_t>());
  if (tokens) return tokens < 0;
  return locale_compare(json_dump(a), json_dump(b), true) < 0;
 });
 Json rawMatches = Json::array();
 for (const auto& match : matches) {
  Json occurrences = Json::array();
  for (const auto& occurrence : match.at("occurrences")) {
   const auto path = repoRelativePath(occurrence.at("path").get_ref<const std::string&>());
   const auto found = sources.find(path);
   if (found == sources.end()) fail("CPD returned an out-of-inventory path: " + path);
   occurrences.push_back(declarations::sourceOccurrence(path, found->second, occurrence, lines.at(path)));
  }
  rawMatches.push_back({{"id", rawMatches.size() + 1}, {"token_count", match.at("tokenCount")}, {"line_count", match.at("lineCount")}, {"occurrences", std::move(occurrences)}});
 }
 return {
  {"format", 1}, {"purpose", "Unfiltered CPD evidence and independent declaration context; not a behavior-duplication verdict"}, {"inventory", files.at("scannedFiles")},
  {"detector", {{"binary", profile.at("cpd").at("binary")}, {"arguments", detectorArguments(profile).at("cpd")}, {"min_tokens", profile.at("cpd").at("minTokens")}, {"status", cpdResult.at("status")},
                {"stderr", cpdResult.at("stderr")}, {"context_filter", false}, {"inline_suppressions", false}, {"sequence_skipping", false}}},
  {"raw_matches", std::move(rawMatches)}, {"classified_context", classified},
  {"summary", {{"files", sources.size()}, {"raw_matches", cpdResult.at("duplications").size()}, {"classified_candidates", classified.size()}}}
 };
}
Json generateRawCpd(const Json& profile, std::string_view output, const GenerateRawCpdOptions& options) {
 if (!output.ends_with(".json")) fail("raw CPD output must be a .json report path");
 const auto inputs = options.inventoryInputs ? *options.inventoryInputs : collectInventoryInputs();
 const auto files = buildInventory(profile, inputs);
 const auto& reviewProfile = profile.at("includedPrefix").get_ref<const std::string&>().empty() ? AUTHORED_PROFILE : profile;
 const auto authored = options.review ? strings(buildInventory(reviewProfile, inputs).at("scannedFiles")) : std::vector<std::string>{};
 const auto paths = strings(files.at("scannedFiles"));
 std::vector<std::string> all = paths;
 std::set<std::string> included(all.begin(), all.end());
 for (const auto& path : authored)
  if (included.insert(path).second) all.push_back(path);
 const auto snapshot = sourceSnapshot(all);
 const auto result = runCpd(profile, paths, snapshot);
 auto report = rawCpdReport(profile, files, result, [&](const std::string& path) { return snapshot.at(path).source; });
 report["producer"] = nativeProducer();
 if (options.review) {
  Json reviewFiles = Json::array();
  for (const auto& path : authored) {
   const auto& entry = snapshot.at(path);
   reviewFiles.push_back({{"path", path}, {"language", *authoredLanguage(path)}, {"source", entry.source}, {"sha256", entry.sha256}});
  }
  report["review"] = review::buildReview(reviewFiles, report);
 }
 const auto contents = json_dump(report, 2) + '\n';
 Json markdown = nullptr;
 if (options.review) markdown = options.renderMarkdown ? options.renderMarkdown(report.at("review")) : Json(review::renderReviewMarkdown(report.at("review")));
 if (options.review && !markdown.is_string()) fail("review renderer did not produce Markdown text");
 requireUnchangedSources(snapshot);
 writeAtomic(std::filesystem::path(output), contents);
 if (options.review) writeAtomic(std::string(output.substr(0, output.size() - 5)) + ".md", markdown.get_ref<const std::string&>());
 std::cout << "wrote " << output << ": " << report.at("summary").at("files") << " files, " << report.at("summary").at("raw_matches") << " raw CPD groups, "
           << report.at("summary").at("classified_candidates") << " source candidates\n";
 return report;
}
int main(const std::vector<std::string>& args) {
 try {
  const auto selection = parseArgs(args);
  if (selection.at("kind") == "help")
   std::cout << usageText() << '\n';
  else if (selection.at("kind") == "raw-cpd") {
   GenerateRawCpdOptions options;
   options.review = selection.at("review").get<bool>();
   generateRawCpd(selection.at("profile"), selection.at("output").get_ref<const std::string&>(), options);
  } else
   generateReport(selection.at("profile"));
  return 0;
 } catch (const std::exception& error) {
  std::cerr << "generate_cleanup_json: " << error.what() << '\n';
  return 1;
 }
}
}  // namespace mmltk::tools::cleanup
export int mmltk_tool_generate_cleanup_json(int argc, char** argv) { return mmltk::tools::cleanup::main(std::vector<std::string>(argv + 1, argv + argc)); }
