module;
#include "cpp_test_macros.hpp"
// Native counterpart of audit_includes.test.mjs: all 14 original cases.
// Fixtures are in memory; no JavaScript runtime or external commands are used.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <initializer_list>
#include <optional>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <exception>
#include <regex>

export module mmltk.tools.tests.audit_includes_test;
import mmltk.tools.audit_includes;
import mmltk.tools.tests.cpp_test_support;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;

namespace include_audit_test {
using namespace mmltk::tools::include_audit;
using namespace mmltk::tools::testing;
const std::string& pch() { static const std::string value = "src/pch_std.h"; return value; }
const Json& context() {
    static const Json value{{"target", "owner"}, {"modules", false}, {"pch", Json::array({pch()})},
        {"include_dirs", Json::array()}, {"quote_dirs", Json::array()}};
    return value;
}
const Json& coverage() {
    static const Json value{{pch(), Json::array({"vector", "string", "memory"})}};
    return value;
}
Json rewrite(std::string_view path, std::string_view source, const Json& contexts = Json::array({context()})) {
  return pchReplacement(path, source, scanIncludes(source), contexts, coverage());
}
Json targetEntry(const std::string& target, const std::string& extra) {
  return {{"directory", "/repo/build"}, {"file", "/repo/src/a.cpp"},
          {"command", "g++ -I '/repo/include dir' -isystem /opt/lib " + extra +
                      " -o CMakeFiles/" + target + ".dir/a.o -c /repo/src/a.cpp"}};
}
Json targetEntry(const std::string& target) {
  return targetEntry(target, "-include CMakeFiles/" + target + ".dir/cmake_pch.hxx");
}
const std::string& registeredPolicy() {
    static const std::string value = "header\t/repo/src/pch_std.h\nuse\t/repo/src/a.cpp\n";
    return value;
}
Json readContexts(const Json& entries, const std::string& policy = registeredPolicy()) {
  return compilationContexts(entries, "/repo", [&](const std::string&) { return policy; },
                             [&](const std::string&) { return !policy.empty(); }).at("src/a.cpp");
}
std::string replaceFirst(std::string source, std::string_view from, std::string_view to) {
  const auto offset = source.find(from);
  if (offset == std::string::npos) throw std::runtime_error("fixture replacement missing");
  source.replace(offset, from.size(), to);
  return source;
}
const Json& byPath(const Json& entries, std::string_view path) {
  const auto found = std::find_if(entries.begin(), entries.end(), [&](const Json& entry) { return entry.at("path") == path; });
  if (found == entries.end()) throw std::runtime_error("missing report path: " + std::string(path));
  return *found;
}
std::vector<std::string> keys(const Json& object) {
  std::vector<std::string> result;
  for (const auto& item : object.items()) result.push_back(item.key());
  return result;
}
using mmltk::tools::includes;

void lexical_includes() {
  const std::string source = "// #include <fake>\n/* #include <fake> */\nconst auto s = R\"x(\n#include <fake>\n)x\";\n"
    "# include <vector> // comment\n#if FLAG\n#include \"own.h\"\n#else\n#include HEADER\n#endif\n#include \\\n <string>\n# /* legal */ include <memory>\n";
  const auto scan = scanIncludes(source);
  auto actual = Json::array();
  for (const auto& include : scan.at("includes")) actual.push_back(Json::array({include.at("header"), include.at("spelling"), include.at("conditional")}));
  CHECK(actual == R"json([["vector","angle",false],["own.h","quote",true],["HEADER","macro",true],["string","angle",false],["memory","angle",false]])json"_fixture);
  CHECK(scan.at("includes").at(0).at("line") == 6);
  CHECK(scan.at("includes").at(3).at("plain") == false);
}

void attribution() {
  const std::set<std::string> paths{"src/a/own.h", "src/a/detail.h", "src/b/detail.h", "third_party/vendor.h"};
  const auto attr = [&](std::string_view header, std::string_view spelling = "quote", const Json& contexts = Json::array({context()})) {
    return attributeInclude({{"header", header}, {"spelling", spelling}}, "src/a/file.cpp", paths, contexts, "/repo");
  };
  CHECK(attr("own.h").at("resolved") == "src/a/own.h");
  CHECK(attr("vector", "angle").at("category") == "standard");
  CHECK(attr("linux/fs.h", "angle").at("category") == "linux-posix");
  CHECK(attr("cuda.h", "angle").at("category") == "external");
  CHECK(attr("HEADER", "macro").at("category") == "macro");
  CHECK(attr("vendor.h").at("category") == "vendored");
  auto other = context();
  other["include_dirs"] = Json::array({"/repo/src/b"});
  CHECK(attr("detail.h", "quote", Json::array({other})).at("resolution") == "ambiguous");
  CHECK(attr("missing.h").at("category") == "unresolved");
}

void compile_environments() {
  const auto contexts = readContexts(Json::array({targetEntry("one"), targetEntry("two", "-fmodules")}));
  CHECK(contexts.size() == 2);
  CHECK(contexts.at(0).at("include_dirs") == Json::array({"/repo/include dir", "/opt/lib"}));
  CHECK(contexts.at(0).at("pch") == Json::array({pch()}));
  CHECK(contexts.at(1).at("modules") == true);
  CHECK(commandWords(R"cmd(g++ -DNAME=\"literal\" "a b.cpp")cmd") == std::vector<std::string>({"g++", "-DNAME=\"literal\"", "a b.cpp"}));
  throws([] { commandWords("'unfinished"); });
}

void line_endings() {
  const std::string source = "#include \"own.h\"\r\n#include <vector>\r\n// reason\r\n#include <string>\r\n#if FLAG\r\n#include <memory>\r\n#endif\r\nint main() {}\r\n";
  const auto result = rewrite("src/a.cpp", source);
  CHECK(result.at("after") == "#include \"own.h\"\r\n#include \"src/pch_std.h\"\r\n// reason\r\n#if FLAG\r\n#include \"src/pch_std.h\"\r\n#endif\r\nint main() {}\r\n");
  CHECK(result.at("removed").get<int>() - result.at("added").get<int>() == 1);
  CHECK(result.at("retained").empty());
  CHECK(!rewrite("src/a.cpp", result.at("after").get<std::string>()).contains("after"));
}

void compiled_consumers() {
  const std::string source = "#include <vector>\n#include <string>\n";
  for (const auto* path : {"src/a.h", "src/a.cu", "src/a.c", "src/a.cppm", "src/a.cpp.in"}) CHECK(!rewrite(path, source).contains("after"));
  CHECK(!rewrite("src/a.cpp", source, Json::array()).contains("after"));
  auto noPch = context();
  noPch["pch"] = Json::array();
  CHECK(!rewrite("src/a.cpp", source, Json::array({context(), noPch})).contains("after"));
  auto module = context();
  module["modules"] = true;
  CHECK(!rewrite("src/a.cpp", source, Json::array({module})).contains("after"));
  CHECK(rewrite("src/a.cpp", "#define _GNU_SOURCE\n" + source).at("after") == "#define _GNU_SOURCE\n#include \"" + pch() + "\"\n");
  CHECK(!rewrite(pch(), source).contains("after"));
  const std::string json = "nlohmann/json.hpp";
  const auto extendedSource = source + "#include <" + json + ">\n";
  auto extendedCoverage = coverage();
  extendedCoverage["src/pch_json.h"] = Json::array({json});
  const auto result = pchReplacement("src/a.cpp", extendedSource, scanIncludes(extendedSource), Json::array({context()}), extendedCoverage);
  CHECK(result.at("after") == "#include \"" + pch() + "\"\n#include <" + json + ">\n");
}

void continuations() {
  CHECK(rewrite("src/a.cpp", "#include <vector>\n").at("after") == "#include \"" + pch() + "\"\n");
  const std::string source = "#include \"src/pch_std.h\"\n#include <vector>\n#include <string> // intent\n#include \\\n <memory>\n";
  auto expected = replaceFirst(source, "#include <vector>\n", "");
  expected = replaceFirst(expected, "<string>", "\"" + pch() + "\"");
  expected = replaceFirst(expected, "<memory>", "\"" + pch() + "\"");
  CHECK(rewrite("src/a.cpp", source).at("after") == expected);
}

void branches() {
  const std::string source = "# /* legal */ if FIRST\n#include <vector>\n#else\n#include <string>\n#endif\n#include_next <memory>\n";
  const auto result = rewrite("src/a.cpp", source);
  CHECK(result.at("after") == replaceFirst(replaceFirst(source, "<vector>", "\"" + pch() + "\""), "<string>", "\"" + pch() + "\""));
  CHECK(result.at("retained").at(0).at("reason") == "include_next changes lookup");
}

void bundles() {
  const auto file = [](std::string_view path, const std::vector<std::string>& names) {
    auto entries = Json::array();
    for (const auto& name : names) entries.push_back({{"category", "project"}, {"resolved", "src/lib/" + name + ".h"}});
    return Json{{"path", path}, {"includes", entries}};
  };
  const auto bundles = repeatedBundles(Json::array({file("src/a.cpp", {"a", "b", "c", "c"}), file("src/lib/use.cpp", {"a", "b", "c"}), file("src/b.cpp", {"a", "b"})}));
  CHECK(bundles.size() == 2);
  CHECK(bundles.at(0).at("headers") == Json::array({"src/lib/a.h", "src/lib/b.h"}));
  CHECK(bundles.at(0).at("consumers").size() == 3);
  CHECK(bundles.at(1).at("headers").size() == 3);
  CHECK(bundles.at(1).at("outside_folder") == Json::array({"src/a.cpp"}));
}

void frequencies() {
  const Json sources{{pch(), "#include <vector>\n#include <string>\n"},
    {"src/a.cpp", "#include <vector>\n#include <vector>\n#include <string>\n"},
    {"src/b.h", "#include <vector>\n"}, {"src/c.cu", "#if FLAG\n#include <vector>\n#endif\n"}};
  auto paths = keys(sources);
  for (int index = 0; index < 11; ++index) paths.push_back("docs/page" + std::to_string(index) + ".md");
  const auto report = buildIncludeReport(paths, sources, {{"src/a.cpp", Json::array({context()})}}, "/repo");
  auto candidates = Json::array();
  for (const auto& entry : report.at("pch_candidates")) candidates.push_back(Json::array({entry.at("header"), entry.at("files").size(), entry.at("occurrences")}));
  CHECK(candidates == R"json([["vector",3,4]])json"_fixture);
  auto folders = Json::array();
  for (const auto& folder : report.at("folders")) folders.push_back(Json::array({folder.at("path"), folder.at("file_count")}));
  CHECK(folders == R"json([["docs",11]])json"_fixture);
  CHECK(report.at("summary").at("changed_files") == 1);
  CHECK(byPath(report.at("files"), "src/b.h").at("includes").at(0).at("category") == "standard");
}

void arguments() {
  CHECK(parseIncludeArgs({}).at("mode") == "report");
  CHECK(parseIncludeArgs({}).at("output") == "output/include-audit");
  CHECK(parseIncludeArgs({"fix", "--output", "output/custom"}).at("output") == "output/custom");
  throws([] { parseIncludeArgs({"sort"}); });
  throws([] { parseIncludeArgs({"fix", "--build-dir"}); });
}

void pch_evidence() {
  const auto valid = readContexts(Json::array({targetEntry("one")})).at(0);
  CHECK(valid.at("forced_pch") == R"json([{"option":"-include","path":"build/CMakeFiles/one.dir/cmake_pch.hxx"}])json"_fixture);
  CHECK(valid.at("pch_reason") == "target-local forced PCH");
  CHECK(valid.at("pch_issues") == Json::array());
  for (const std::string extra : {"", "-include CMakeFiles/two.dir/cmake_pch.hxx", "-include-pch CMakeFiles/one.dir/cmake_pch.hxx",
      "-include CMakeFiles/one.dir/cmake_pch.hxx -fmodules", "-include CMakeFiles/one.dir/cmake_pch.hxx @hidden-options"}) {
    const auto found = readContexts(Json::array({targetEntry("one", extra)})).at(0);
    CHECK(found.at("pch") == Json::array());
    CHECK(found.at("pch_reason") == "inconsistent PCH evidence");
    CHECK(!found.at("pch_issues").empty());
  }
  const auto skippedPolicy = replaceFirst(registeredPolicy(), "use\t", "skip\t");
  const auto skipped = readContexts(Json::array({targetEntry("one", "")}), skippedPolicy).at(0);
  CHECK(skipped.at("pch_reason") == "source is excluded by its target PCH policy");
  CHECK(skipped.at("pch_issues") == Json::array());
  const auto module = readContexts(Json::array({targetEntry("one", "-fmodules-ts")}), skippedPolicy).at(0);
  CHECK(module.at("pch_reason") == "module compilation does not consume a PCH");
  CHECK(module.at("pch") == Json::array());
  const auto missing = readContexts(Json::array({targetEntry("one")}), "").at(0);
  CHECK(missing.at("pch_reason") == "inconsistent PCH evidence");
  const auto undeclared = readContexts(Json::array({targetEntry("one", "")}), "").at(0);
  CHECK(undeclared.at("pch_reason") == "target has no PCH registration");
  auto entry = targetEntry("one");
  entry["output"] = "CMakeFiles/two.dir/a.o";
  const auto mismatch = readContexts(Json::array({entry})).at(0);
  CHECK(includes(mismatch.at("pch_issues"), "compile output metadata differs from its compiler argument"));
  const auto contradictory = readContexts(Json::array({targetEntry("one")}), registeredPolicy() + "skip\t/repo/src/a.cpp\n").at(0);
  CHECK(includes(contradictory.at("pch_issues"), "source is both used and excluded by its target policy"));
}

void secondary_contexts() {
  auto unknown = targetEntry("other", "");
  unknown["command"] = replaceFirst(unknown.at("command").get<std::string>(), "CMakeFiles/other.dir/a.o", "objects/a.o");
  const auto contexts = readContexts(Json::array({targetEntry("one"), unknown}));
  CHECK(contexts.size() == 2);
  CHECK(contexts.at(1).at("target") == "(unregistered)");
  CHECK(contexts.at(1).at("pch_reason") == "compile output has no registered CMake owner");
  const std::string source = "#include <vector>\n";
  const auto result = rewrite("src/a.cpp", source, contexts);
  CHECK(!result.contains("after"));
  CHECK(result.at("retained").at(0).at("reason") == contexts.at(1).at("pch_reason"));
  const auto mixed = readContexts(Json::array({targetEntry("one"), targetEntry("two", "")}));
  CHECK(mixed.size() == 2);
  CHECK(!rewrite("src/a.cpp", source, mixed).contains("after"));
  CHECK(mixed.at(1).at("pch_reason") == "inconsistent PCH evidence");
}

void retention() {
  const Json reasons{{"src/a.h", "declaration header requires direct dependencies"},
    {"src/a.cu", "CUDA source does not consume an ordinary C++ PCH"},
    {"src/a.cppm", "module source does not consume a PCH"}, {"src/a.c", "not an ordinary C++ translation unit"}};
  for (const auto& [path, reason] : reasons.items()) {
    const auto result = rewrite(path, "#include <vector>\n");
    CHECK(result.at("retained").size() == 1);
    CHECK(result.at("retained").at(0).at("reason") == reason);
  }
  const auto absent = rewrite("src/a.cpp", "#include <vector>\n", Json::array());
  CHECK(absent.at("retained").at(0).at("reason") == "source has no compile command in this build graph");
  const auto uncovered = rewrite("src/a.cpp", "#include <unistd.h>\n#include <array>\n");
  CHECK(!uncovered.contains("after"));
  auto categories = Json::array();
  for (const auto& include : uncovered.at("retained")) categories.push_back(include.at("category"));
  CHECK(categories == Json::array({"linux-posix", "standard"}));
  CHECK(std::all_of(uncovered.at("retained").begin(), uncovered.at("retained").end(), [](const Json& include) {
    return include.at("reason") == "header is not provided by a PCH shared by every compile context";
  }));
  const auto quoted = rewrite("src/a.cpp", "#include \"vector\"\n");
  CHECK(quoted.at("retained").at(0).at("reason") == "non-angle include retains explicit lookup");
}

void report_evidence() {
  const Json sources{{pch(), "#include <vector>\n"}, {"src/a.cpp", "#include <vector>\n#include <unistd.h>\n"},
                     {"src/b.h", "#include <vector>\n#include <array>\n"}};
  const auto contexts = compilationContexts(Json::array({targetEntry("one")}), "/repo",
    [](const std::string&) { return registeredPolicy(); }, [](const std::string&) { return true; });
  const auto report = buildIncludeReport(keys(sources), sources, contexts, "/repo");
  const auto& file = byPath(report.at("files"), "src/a.cpp");
  CHECK(file.at("compile_contexts").size() == 1);
  CHECK(file.at("compile_contexts").at(0).at("forced_pch").at(0).at("path") == "build/CMakeFiles/one.dir/cmake_pch.hxx");
  CHECK(report.at("pch_groups").at(0).at("consumers") == Json::array());
  CHECK(report.at("pch_groups").at(0).at("compiled_consumers") == Json::array({"src/a.cpp"}));
  auto counts = Json::array();
  for (const auto& retained : report.at("retained_system_includes")) counts.push_back(retained.at("includes"));
  CHECK(counts == Json::array({2, 1}));
  CHECK(byPath(report.at("replacements"), "src/a.cpp").at("retained").at(0).at("header") == "unistd.h");
  CHECK(byPath(report.at("replacements"), "src/b.h").at("retained").size() == 2);
}
constexpr mmltk::tools::testing::Case cases[]{
    {"include audit ignores comments and literals, retains branches, macros and continued directives", lexical_includes},
    {"include audit attributes project paths before system names and exposes ambiguity", attribution},
    {"include audit verifies target-local forced PCH across every compile environment", compile_environments},
    {"PCH replacement preserves local includes, comments, conditional branches and line endings", line_endings},
    {"PCH replacement stays limited to actual compiled PCH consumers", compiled_consumers},
    {"PCH replacement covers single includes and preserves comments and continuations", continuations},
    {"PCH replacement retains separate branches and include_next lookup semantics", branches},
    {"include bundles count distinct consumers, close repeated pairs into triples and retain outside consumers", bundles},
    {"include frequencies use distinct files, exclude PCH definitions and include inactive evidence", frequencies},
    {"include audit defaults to reports under output and rejects unknown modes", arguments},
    {"PCH evidence distinguishes registered use, exclusion, and contradictory compiler commands", pch_evidence},
    {"include audit retains unknown and ineligible secondary compile contexts", secondary_contexts},
    {"PCH retention accounts for source kind and actual group coverage", retention},
    {"include reports expose compiler evidence and every retained standard or Linux directive", report_evidence},
};

}  // namespace include_audit_test

export int mmltk_tool_audit_includes_test(int argc, char** argv) {
  return mmltk::tools::testing::run_cases(include_audit_test::cases, argc, argv);
}
