module;
#include "cpp_test_macros.hpp"
// Native counterpart of generate_cleanup_json.test.mjs: all 40 original cases.
// Run from the repository root. The first
// case requires the configured real PMD; the failure fixture reexecutes this
// native test binary as a fake detector. No JavaScript runtime is used.
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <set>
#include <initializer_list>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <span>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <exception>
export module mmltk.tools.tests.generate_cleanup_json_test;
import mmltk.tools.generate_cleanup_json;
import mmltk.tools.cleanup.cpd_patterns;
import mmltk.tools.cleanup.declaration_patterns;
import mmltk.tools.tests.cpp_test_support;
import mmltk.tools.tool_command;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace cleanup_test {
namespace fs = std::filesystem;
using namespace mmltk::tools::cleanup;
using mmltk::tools::cpd::filterCpdCandidates;
using mmltk::tools::declarations::sourceOccurrence;
const auto& cpp() { return CLEANUP_PROFILES.at("cpp"); }
const auto& frontend() { return CLEANUP_PROFILES.at("frontend"); }
using namespace mmltk::tools::testing;
using Environment = mmltk::tools::ScopedEnvironment;
// An external process fixture only: capture argv and emit the prescribed PMD
// result, deriving complete coverage from the production-created file list.
int fakeDetector(int argc, char** argv, const char* configuration) {
 const auto config = Json::parse(configuration);
 std::vector<std::string> args(argv, argv + argc);
 mmltk::tools::write_text(config.at("capture").get<std::string>(), Json(args).dump());
 if (config.value("complete", false)) {
  const auto paths = detector_paths(args);
  std::cout << "<pmd-cpd>";
  for (const auto& path : paths) std::cout << "<file path=\"" << path << "\" totalNumberOfTokens=\"1\"/>";
  std::cout << "</pmd-cpd>";
 } else
  std::cout << config.at("stdout").get<std::string>();
 std::cerr << config.at("stderr").get<std::string>();
 return config.at("status").get<int>();
}
Json fieldValues(const Json& rows, std::string_view field) {
 auto result = Json::array();
 for (const auto& row : rows) result.push_back(row.at(std::string(field)));
 return result;
}
Json spans(const Json& occurrences) {
 auto result = Json::array();
 for (const auto& occurrence : occurrences) result.push_back(Json::array({occurrence.at("start"), occurrence.at("end")}));
 return result;
}
using mmltk::tools::includes;
std::string replaceFirst(std::string source, std::string_view from, std::string_view to) {
 const auto offset = source.find(from);
 CHECK(offset != std::string::npos);
 source.replace(offset, from.size(), to);
 return source;
}
Json classifySources(const Json& sources, const Json& ranges = Json::object(), int tokenCount = 39) {
 auto occurrences = Json::array();
 for (const auto& [path, value] : sources.items()) {
  const auto& source = value.get_ref<const std::string&>();
  Json occurrence{{"path", path}, {"start", 1}, {"end", 1 + std::count(source.begin(), source.end(), '\n')}};
  if (ranges.contains(path)) occurrence.update(ranges.at(path));
  occurrences.push_back(std::move(occurrence));
 }
 std::map<std::string, int> reads;
 const auto result = filterCpdCandidates(Json::array({{
                                          {"lineCount", 5},
                                          {"tokenCount", tokenCount},
                                          {"occurrences", occurrences},
                                         }}),
  {.sourceReader = [&](const std::string& path) {
  ++reads[path];
  return sources.at(path).get<std::string>();
 }});
 CHECK(reads.size() == sources.size());
 for (const auto& [path, source] : sources.items()) CHECK(reads.at(path) == 1);
 return result;
}
void copiesRemoved(const Json& arguments) {
 const auto option = std::find(arguments.begin(), arguments.end(), Json("--file-list"));
 CHECK(option != arguments.end());
 CHECK(std::next(option) != arguments.end());
 CHECK(!fs::exists(fs::path(std::next(option)->get<std::string>()).parent_path()));
}
void real_pmd() {
 const mmltk::tools::TemporaryDirectory directory("mmltk-cpd-source-");
 const std::string body = "int repeated(int value) {\r\n  const char* marker = \"CPD-OFF\";\r\n  int result = value * value + 3;\r\n  return result + value + marker[0];\r\n}\r\n";
 const std::vector<std::string> paths{(directory.path() / "suppressed.cpp").string(), (directory.path() / "ordinary.cpp").string()};
 const std::vector<std::string> sources{"// CPD-OFF\r\n" + body + "// CPD-ON\r\n", "// ordinary source\r\n" + body};
 for (std::size_t index = 0; index < paths.size(); ++index) mmltk::tools::write_text(paths[index], sources[index]);
 const auto raw = parseArgs({"--raw-cpd"}).at("profile");
 auto normal = cpp();
 normal["cpd"]["minTokens"] = 12;
 normal["cpd"]["flags"] = Json::array();
 CHECK(!runCpd(normal, paths).at("duplications").empty());
 const auto result = runCpd(raw, paths);
 const auto& duplications = result.at("duplications");
 const auto match =
  std::find_if(duplications.begin(), duplications.end(), [](const Json& candidate) { return candidate.at("occurrences").size() == 2 && candidate.at("tokenCount").get<int>() >= 30; });
 CHECK(match != duplications.end());
 auto actual = fieldValues(match->at("occurrences"), "path").get<std::vector<std::string>>();
 auto expected = paths;
 std::sort(actual.begin(), actual.end());
 std::sort(expected.begin(), expected.end());
 CHECK(actual == expected);
 for (const auto& occurrence : match->at("occurrences")) {
  const auto path = occurrence.at("path").get<std::string>();
  const auto position = std::find(paths.begin(), paths.end(), path);
  CHECK(position != paths.end());
  const auto& source = sources.at(static_cast<std::size_t>(position - paths.begin()));
  const auto evidence = sourceOccurrence(path, source, occurrence);
  const auto text = evidence.at("text").get<std::string>();
  CHECK(occurrence.at("start") == 2);
  CHECK(occurrence.at("column") == 1);
  CHECK(text.starts_with("int repeated("));
  CHECK(text.find("\"CPD-OFF\"") != std::string::npos);
  CHECK(text.find("\r\n") != std::string::npos);
  const auto start = evidence.at("start_offset").get<std::size_t>();
  CHECK(source.substr(start, evidence.at("end_offset").get<std::size_t>() - start) == text);
  CHECK(mmltk::tools::read_text(path) == source);
 }
 copiesRemoved(result.at("arguments"));
 const auto invalid = directory.path() / "invalid.cpp";
 mmltk::tools::write_text(invalid, "int invalid() { return \"unterminated; }\n");
 throws([&] { runCpd(raw, {invalid.string()}); }, "status 5|could not lex|incomplete raw source coverage");
}
void partial_detector() {
 const mmltk::tools::TemporaryDirectory directory("mmltk-cpd-failure-");
 const auto capture = directory.path() / "arguments.json";
 const auto output = directory.path() / "report.json";
 const std::string original = "{\"previous\":\"complete\"}\n";
 mmltk::tools::write_text(output, original);
 const auto results = R"json([
    {"status":5,"stdout":"<pmd-cpd/>","stderr":"Cannot lex src/broken.cpp: unterminated literal"},
    {"status":0,"stdout":"<pmd-cpd><error filename=\"src/broken.cpp\" msg=\"lexical failure\"/></pmd-cpd>","stderr":""},
    {"status":0,"stdout":"<pmd-cpd/>","stderr":"source scanner: ERROR in src/broken.cpp"},
    {"status":0,"stdout":"<pmd-cpd/>","stderr":"","complete":true}
  ])json"_fixture;
 for (auto result : results) {
  result["capture"] = capture.string();
  const Environment fixture("MMLTK_CLEANUP_TEST_PMD_FIXTURE", result.dump());
  auto profile = parseArgs({"--raw-cpd"}).at("profile");
  const auto binary = directory.path() / "pmd";
  write_fixture_program(binary, mmltk::tools::command::Command::test_generate_cleanup_json);
  profile["cpd"]["binary"] = binary.string();
  if (result.value("complete", false))
   runCpd(profile, {"src/frameworks/reflection/declaration_annotations.h"});
  else
   throws([&] {
    GenerateRawCpdOptions options;
    options.inventoryInputs = R"json({"tracked":["src/frameworks/reflection/declaration_annotations.h"],"untracked":[],"deleted":[]})json"_fixture;
    generateRawCpd(profile, output.string(), options);
   }, "src/broken\\.cpp");
  CHECK(mmltk::tools::read_text(output) == original);
  copiesRemoved(Json::parse(mmltk::tools::read_text(capture)));
 }
}
void empty_inventory() {
 auto profile = parseArgs({"--raw-cpd"}).at("profile");
 profile["cpd"]["binary"] = "/nonexistent/pmd";
 const auto result = runCpd(profile, {});
 CHECK(result.at("status") == 0);
 CHECK(result.at("duplications") == Json::array());
}
void profiles() {
 CHECK(parseArgs({}).at("profile") == cpp());
 CHECK(parseArgs({"--cpp"}).at("profile") == cpp());
 CHECK(parseArgs({"--frontend"}).at("profile") == frontend());
 CHECK(parseArgs({"--tools"}).at("profile") == CLEANUP_PROFILES.at("tools"));
 CHECK(parseArgs({"--help"}).at("kind") == "help");
 static_assert(std::is_const_v<std::remove_reference_t<decltype(CLEANUP_PROFILES)>>);
 static_assert(std::is_const_v<std::remove_reference_t<decltype(frontend().at("cpd"))>>);
 const auto snapshot = CLEANUP_PROFILES;
 auto selected = parseArgs({"--frontend"});
 selected["profile"]["cpd"]["minTokens"] = 1;
 CHECK(CLEANUP_PROFILES == snapshot);
 for (const auto& args : std::vector<std::vector<std::string>>{{"--cpp", "--frontend"}, {"--cpp", "--cpp"}, {"--tools", "--cpp"}, {"frontend"}, {"--unknown"}}) throws([&] { parseArgs(args); });
}
void inventory_commands() {
 CHECK(INVENTORY_COMMANDS.at("tracked") == Json::array({"git", "ls-files", "-z"}));
 CHECK(INVENTORY_COMMANDS.at("untracked") == Json::array({"git", "ls-files", "--others", "--exclude-standard", "-z"}));
 CHECK(INVENTORY_COMMANDS.at("deleted") == Json::array({"git", "ls-files", "--deleted", "-z"}));
 static_assert(std::is_const_v<std::remove_reference_t<decltype(INVENTORY_COMMANDS)>>);
 static_assert(std::is_const_v<std::remove_reference_t<decltype(INVENTORY_COMMANDS.at("untracked"))>>);
}
void detector_commands() {
 CHECK(detectorArguments(cpp(), {.threads = 3, .fileList = "/f"}) == R"json({
    "duplo":["-j","3","-ml","6","-ip","-json","-","-"],
    "cpd":["cpd","--file-list","/f","--language","cpp","--minimum-tokens","39","--ignore-identifiers","--ignore-literal-sequences","--format","xml","--no-fail-on-violation"]
  })json"_fixture);
 CHECK(detectorArguments(frontend(), {.threads = 4, .fileList = "/rust"}) == R"json({
    "duplo":["-j","4","-ml","9","-json","-","-"],
    "cpd":["cpd","--file-list","/rust","--language","rust","--minimum-tokens","75","--format","xml","--no-fail-on-violation"]
  })json"_fixture);
}
void frontend_inventory() {
 // Ignored files are absent from git ls-files --others --exclude-standard.
 const auto inventory = buildInventory(frontend(), R"json({
    "tracked":["src/frontend/iced/src/app.rs","src/frontend/iced/src/deleted.rs","src/frontend/iced/src/generated.rs","src/frontend/iced/target/debug/generated.rs","src/frontend/iced/third_party/dependency.rs","src/controller/not_frontend.rs"],
    "untracked":["src/frontend/iced/src/new.rs","src/frontend/iced/generated/bindings.rs"],
    "deleted":["src/frontend/iced/src/deleted.rs"]
  })json"_fixture,
  [](const std::string&) { return true; });
 CHECK(inventory.at("scannedFiles") == Json::array({"src/frontend/iced/src/app.rs", "src/frontend/iced/src/generated.rs", "src/frontend/iced/src/new.rs"}));
 CHECK(inventory.at("deletedFiles") == Json::array({"src/frontend/iced/src/deleted.rs"}));
 CHECK(inventory.at("excludedByPrefix") == Json::array({"src/frontend/iced/target/debug/generated.rs", "src/frontend/iced/third_party/dependency.rs"}));
 CHECK(inventory.at("excludedGeneratedOutput") == Json::array({"src/frontend/iced/generated/bindings.rs"}));
 CHECK(inventory.at("untrackedCandidateCount") == 2);
 const auto tools = buildInventory(CLEANUP_PROFILES.at("tools"), R"json({
    "tracked":["tools/mmltk.cpp","tools/mmltk_container_commands.cpp","tools/a.cpp","tools/module.cppm","tools/deleted.cpp","src/a.cpp","tools/a.mjs","tools/build/a.cpp","tools/generated/a.hpp"],
    "untracked":["tools/new.hpp","tools/tests/new.cpp","tools/third_party/a.cpp"],
    "deleted":["tools/deleted.cpp"]
  })json"_fixture,
  [](const std::string&) { return true; });
 CHECK(tools.at("scannedFiles") == Json::array({"tools/a.cpp", "tools/mmltk.cpp", "tools/mmltk_container_commands.cpp", "tools/module.cppm", "tools/new.hpp", "tools/tests/new.cpp"}));
}
void invalid_inventory() {
 const auto input = R"json({"tracked":["src/frontend/iced/src/missing.rs"],"untracked":[],"deleted":[]})json"_fixture;
 throws([&] { buildInventory(frontend(), input, [](const std::string&) { return false; }); });
 throws([&] { buildInventory(frontend(), R"json({"tracked":["src/frontend/iced/src/bad\nname.rs"],"untracked":[],"deleted":[]})json"_fixture, [](const std::string&) { return true; }); });
}
void parsers() {
 const auto duplo = parseDuploJson(
  R"json([{"LineCount":9,"SourceFile1":"src/frontend/iced/src/a.rs","StartLineNumber1":2,"EndLineNumber1":10,"SourceFile2":"src/frontend/iced/src/b.rs","StartLineNumber2":4,"EndLineNumber2":12,"Lines":["x"]}])json");
 const auto compacted = compactDuploHits(frontend(), duplo, {"src/frontend/iced/src/a.rs", "src/frontend/iced/src/b.rs"});
 CHECK(compacted.at(0).at("kind") == "cross-file");
 CHECK(compacted.at(0).at("line_count") == 9);
 const auto cpd = parseCpdXml(
  R"xml(<?xml version="1.0"?><pmd-cpd><duplication lines="8" tokens="75"><file line="2" endline="9" path="/repo/src/frontend/iced/src/a.rs"/><file line="6" endline="13" path="/repo/src/frontend/iced/src/b.rs"/><codefragment><![CDATA[x]]></codefragment></duplication></pmd-cpd>)xml");
 const auto structural = structuralHits(frontend(), cpd, {"src/frontend/iced/src/a.rs", "src/frontend/iced/src/b.rs"}, "/repo");
 CHECK(structural.at(0).at("token_count") == 75);
 CHECK(structural.at(0).at("files").at(1).at("lines") == Json::array({Json::array({6, 13})}));
}
void sorting_and_suppression() {
 const auto within = R"json({"kind":"within-file","line_count":10,"file_count":1,"occurrence_count":2,"files":[{"path":"src/frontend/iced/src/z.rs","lines":[[1,10],[20,29]]}]})json"_fixture;
 const auto cross =
  R"json({"kind":"cross-file","line_count":9,"file_count":3,"occurrence_count":3,"files":[{"path":"src/frontend/iced/src/a.rs","lines":[[2,10]]},{"path":"src/frontend/iced/src/b.rs","lines":[[2,10]]},{"path":"src/frontend/iced/src/c.rs","lines":[[2,10]]}]})json"_fixture;
 auto sorted = Json::array({within, cross});
 std::sort(sorted.begin(), sorted.end(), [](const Json& left, const Json& right) { return compareHits(left, right) < 0; });
 CHECK(sorted == Json::array({cross, within}));
 const auto suppressions = parseInlineSuppressions("src/frontend/iced/src/a.rs", "// CLEANUP-OFF: fixture false positive\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n// CLEANUP-ON\n");
 const auto applied = applyInlineSuppressions(Json::array({cross}), suppressions, "duplo");
 CHECK(applied.at("suppressed").size() == 1);
 CHECK(applied.at("hits").at(0).at("occurrence_count") == 2);
}
void reports() {
 const auto files = buildInventory(frontend(), R"json({"tracked":["src/frontend/iced/src/generated.rs","src/frontend/iced/third_party/dependency.rs"],"untracked":[],"deleted":[]})json"_fixture,
  [](const std::string&) { return true; });
 const Json input{
  {"profile", frontend()},
  {"files", files},
  {"duplo", {{"arguments", detectorArguments(frontend(), {.threads = 2, .fileList = "/unused"}).at("duplo")}, {"status", 0}, {"stderr", ""}}},
  {"cpd", {{"arguments", detectorArguments(frontend(), {.threads = 2, .fileList = "/tmp/cpd-fixture/files.txt"}).at("cpd")}, {"status", 0}, {"stderr", ""}, {"excludedLexerCount", 0}}},
  {"codeOnlyDuplo", {{"filteredCount", 0}}},
  {"hits", Json::array()},
  {"structuralHitsAfterSuppression", Json::array()},
  {"duploSuppression", {{"suppressed", Json::array()}}},
  {"cpdSuppression", {{"suppressed", Json::array()}}},
  {"threads", 2},
 };
 const auto clock = [] { return "2026-08-31T12:34:56.000Z"; };
 const auto first = serializeReport(buildReport(input, clock));
 const auto second = serializeReport(buildReport(input, clock));
 CHECK(first == second);
 CHECK(Json::parse(first) == R"json({"hits":[],"structural_hits":[]})json"_fixture);
 const auto report = buildReport(input, clock);
 CHECK(report.at("generated_at") == "2026-08-31T12:34:56.000Z");
 CHECK(report.at("profile") == "frontend");
 CHECK(report.at("scope").at("retained_generated_paths") == Json::array({"src/frontend/iced/src/generated.rs"}));
 CHECK(report.at("scope").at("excluded_by_prefix_files") == Json::array({"src/frontend/iced/third_party/dependency.rs"}));
 CHECK(report.at("scope").at("inventory_commands") ==
       Json{{"tracked", INVENTORY_COMMANDS.at("tracked")}, {"untracked_unignored", INVENTORY_COMMANDS.at("untracked")}, {"worktree_deleted", INVENTORY_COMMANDS.at("deleted")}});
 CHECK(report.at("duplo").at("arguments") == Json::array({"-j", "2", "-ml", "9", "-json", "-", "-"}));
 const Json priorCpp{{"generated_at", "previous cpp pass"}};
 const auto rejected = rejectionReport({{"cpp", priorCpp}}, report, {{"indexedFiles", 0}, {"filtered", Json::array()}});
 CHECK(rejected.at("cpp") == priorCpp);
 CHECK(rejected.at("frontend").at("generated_at") == report.at("generated_at"));
 CHECK(!rejected.at("frontend").contains("hits"));
 CHECK(first.find("suppressed_duplo_occurrences") == std::string::npos);
}
void match_sizes() {
 for (const int tokenCount : {39, 99, 100, 101, 640}) {
  const auto unrelated = classifySources(
   {
    {"a.h", "struct A { int width; int height; };"},
    {"b.h", "struct B { bool active; bool pending; };"},
   },
   Json::object(), tokenCount);
  CHECK(unrelated.at("duplications") == Json::array());
  CHECK(unrelated.at("filtered").size() == 1);
  CHECK(unrelated.at("indexedFiles") == 2);
  CHECK(classifySources(
         {
          {"a.cpp", "void A() { auto item = acquire(); inspect(item); publish(item); }"},
          {"b.cpp", "void B() { auto value = acquire(); inspect(value); publish(value); }"},
         },
         Json::object(), tokenCount)
         .at("duplications")
         .size() == 1);
 }
 const std::string predicate = "bool space(char16_t c) { return c == 32 || c == 160 || (c >= 8192 && c <= 8202); }";
 CHECK(classifySources({{"a.cpp", predicate}, {"b.cpp", std::regex_replace(predicate, std::regex(R"(\bc\b)"), "x")}}).at("duplications").size() == 1);
 CHECK(classifySources({{"a.cpp", predicate}, {"b.cpp", replaceFirst(predicate, "160", "161")}}).at("duplications").empty());
 CHECK(classifySources({
                        {"a.cpp", "void previous() { first(); }\nvoid shared() { auto item = acquire(); inspect(item); publish(item); }\nvoid next() { first(); }"},
                        {"b.cpp", "void previous() { second(); }\nvoid shared() { auto item = acquire(); inspect(item); publish(item); }\nvoid next() { second(); }"},
                       })
        .at("duplications")
        .size() == 1);
 CHECK(classifySources({
                        {"a.cpp", "void A(const Options& options = {}) { auto item = acquire(); inspect(item); publish(item); }"},
                        {"b.cpp", "void B(const Options& options = {}) { auto value = acquire(); inspect(value); publish(value); }"},
                       })
        .at("duplications")
        .size() == 1);
 const auto neighbors = [](const std::string& edge) { return "void work() { " + edge + "(); inspect(value); publish(value); " + edge + "(); }"; };
 CHECK(classifySources({{"a.cpp", neighbors("first")}, {"b.cpp", neighbors("second")}}).at("duplications").size() == 1);
}
void conversion_setup() {
 const auto result = classifySources({
  {"a.cpp", R"cpp(void operator()(const char* key, Kind& value) const {
                   int index = static_cast<int>(value);
                   get_optional(json, key, index);
                   value = kind_from_index(index, value);
                  })cpp"},
  {"b.cpp", R"cpp(void operator()(const char* key, Order& value) const {
                   int index = static_cast<int>(value);
                   get_optional(json, key, index);
                   value = index == 1 ? Order::Shuffled : Order::Sequential;
                  })cpp"},
 });
 CHECK(result.at("duplications").size() == 1);
 CHECK(includes(result.at("duplications").at(0).at("patterns"), "same function, first parameter and opening statement"));
}
void overload_identity() {
 CHECK(classifySources({
                        {"a.cpp", "void read(const std::map<int, First>& input, Kind& value) { int index = static_cast<int>(value); first(input, index); }"},
                        {"b.cpp", "void read(const std::map<int, Second>& input, Order& value) { int index = static_cast<int>(value); second(input, index); }"},
                       })
   .at("duplications")
   .empty());
}
void equal_signatures() {
 CHECK(classifySources({
                        {"a.cpp", "void read(const Input& input, Value& value) { auto index = select(input); first(index, value); }"},
                        {"b.cpp", "void read(const Input& input, Value& value) { auto index = select(input); second(index, value); }"},
                       })
   .at("duplications")
   .empty());
}
void aliases_and_transfers() {
 for (const std::string body : {
       "const int threads = launch::kThreads; const int blocks = blocks_for(count, threads);",
       "PixelThread pixel; if (!locate_pixel(width, height, pixel)) return;",
       "store(pixel, color); return;",
       "continue; } store(pixel, color);",
      }) {
  const std::string nested = body.starts_with("continue") ? "for (;;) { if (skip) { " : "";
  const std::string closing = nested.empty() ? "" : " }";
  auto sources = Json::object();
  auto ranges = Json::object();
  for (const std::string name : {"a", "b"}) {
   const auto source = "void " + name + "() { " + nested + body + closing + " }";
   sources[name + ".cpp"] = source;
   ranges[name + ".cpp"] = {{"column", source.find(body) + 1}};
  }
  CHECK(classifySources(sources, ranges).at("duplications").empty());
 }
 CHECK(classifySources({
                        {"a.cpp", "void A() { prepare(); store(pixel, color); return; }"},
                        {"b.cpp", "void B() { prepare(); store(pixel, color); return; }"},
                       })
        .at("duplications")
        .size() == 1);
}
void scalar_calculations() {
 const auto ranges = R"json({"a.cpp":{"start":2,"end":3},"b.cpp":{"start":2,"end":3}})json"_fixture;
 CHECK(classifySources(
        {
         {"a.cpp", "void A() {\nconst float transformed_x = matrix[0] * x + matrix[1] * y;\nconst float transformed_y = matrix[2] * x + matrix[3] * y;\n}"},
         {"b.cpp", "void B() {\nconst float sample_x = matrix[0] * x + matrix[1] * y;\nconst float sample_y = matrix[2] * x + matrix[3] * y;\n}"},
        },
        ranges)
        .at("duplications")
        .size() == 1);
 for (const auto& [first, second] : std::vector<std::pair<std::string, std::string>>{
       {"const int area = width * height;\nconst int bytes = area * 4;", "const int count = width * height;\nconst int size = count * 4;"},
       {"auto first = acquire();\nauto second = acquire();", "auto left = acquire();\nauto right = acquire();"},
      }) {
  CHECK(classifySources(
         {
          {"a.cpp", "void A() {\n" + first + "\n}"},
          {"b.cpp", "void B() {\n" + second + "\n}"},
         },
         ranges)
         .at("duplications")
         .size() == 1);
 }
}
void admission_guards() {
 for (const bool braced : {false, true}) {
  const std::string failure = braced ? "{ return Invalid; }" : "return Invalid;";
  const auto make = [&](std::string name, std::string input) {
   return "void " + name + "(void* " + input + ", int pitch) {\nif (" + input + " == nullptr) " + failure + "\nif (pitch < minimum) " + failure + "\n}";
  };
  CHECK(classifySources({{"a.cpp", make("A", "scratch")}, {"b.cpp", make("B", "target")}}).at("duplications").size() == 1);
  CHECK(classifySources({{"a.cpp", make("A", "scratch")}, {"b.cpp", make("B", "scratch")}}).at("duplications").size() == 1);
 }
}
void cuda_setup() {
 const auto make = [](std::string name, std::string operation) {
  return "__global__ void " + name +
         "(int* output, int count) {\n"
         "const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);\n"
         "if (index >= count) return;\noutput[index] = " +
         operation + "(index);\n}";
 };
 const auto prefix = R"json({"a.cu":{"end":3},"b.cu":{"end":3}})json"_fixture;
 CHECK(classifySources({{"a.cu", make("A", "first")}, {"b.cu", make("B", "second")}}, prefix).at("duplications").size() == 1);
 CHECK(classifySources({{"a.cu", make("A", "first")}, {"b.cu", make("B", "first")}}).at("duplications").size() == 1);
 CHECK(classifySources(
        {
         {"a.cu", "__global__ void A() {\nconst int area = width * height;\nconst int bytes = area * 4;\nwrite(bytes);\n}"},
         {"b.cu", "__global__ void B() {\nconst int area = width * height;\nconst int bytes = area * 4;\nwrite(bytes);\n}"},
        },
        R"json({"a.cu":{"start":2,"end":3},"b.cu":{"start":2,"end":3}})json"_fixture)
        .at("duplications")
        .size() == 1);
 const std::string projection = "__device__ int column() { return static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x); }\n";
 auto projected = Json::object();
 auto guarded = Json::object();
 auto numerical = Json::object();
 auto admissions = Json::object();
 for (const std::string name : {"a", "b"}) {
  projected[name + ".cu"] = projection + replaceFirst(make(name, name), "static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x)", "column()");
  guarded[name + ".cu"] = replaceFirst(make(name, name), "index >= count", "!admit(index) || !advance(count)");
  numerical[name + ".cu"] = replaceFirst(make(name, name), "output[index] =", "const int value = input[index] * gain;\nconst int result = value * value;\noutput[index] =");
  admissions[name + ".cu"] = replaceFirst(make(name, name), "if (index >= count) return;", "if (!admit(index)) return;\nif (!advance(count)) return;");
 }
 CHECK(classifySources(projected, R"json({"a.cu":{"start":2,"end":4},"b.cu":{"start":2,"end":4}})json"_fixture).at("duplications").size() == 1);
 CHECK(classifySources(guarded, prefix).at("duplications").size() == 1);
 CHECK(classifySources(numerical, R"json({"a.cu":{"end":5},"b.cu":{"end":5}})json"_fixture).at("duplications").size() == 1);
 CHECK(classifySources(admissions, R"json({"a.cu":{"end":4},"b.cu":{"end":4}})json"_fixture).at("duplications").size() == 1);
}
void duplo_context() {
 const auto hit = R"json({"LineCount":6,"SourceFile1":"a.cpp","StartLineNumber1":2,"EndLineNumber1":7,"SourceFile2":"b.cpp","StartLineNumber2":2,"EndLineNumber2":7})json"_fixture;
 const auto initializer = [](std::string value) {
  return "void publish_fact() {\nconst Fact fact{\n.kind = Kind::Ready,\n.width = width,\n.height = height,\n.epoch = epoch,\n.generation = generation,\n.capacity = " + value +
         ",\n};\npublish(fact);\n}";
 };
 const auto rejected = filterDuploCandidates(cpp(), Json::array({hit}), [&](const std::string& path) { return initializer(path == "a.cpp" ? "width" : "capacity"); });
 CHECK(rejected.at("hits") == Json::array());
 CHECK(rejected.at("filteredCount") == 0);
 CHECK(rejected.at("contextRejected").size() == 1);
 CHECK(spans(rejected.at("contextRejected").at(0).at("occurrences")) == R"json([[2,7],[2,7]])json"_fixture);
 const std::string body = "void work() {\nauto item = acquire();\ninspect(item);\nprepare(item);\nwrite(item);\npublish(item);\nrelease(item);\n}";
 CHECK(filterDuploCandidates(cpp(), Json::array({hit}), [&](const std::string&) { return body; }).at("hits") == Json::array({hit}));
 const std::string embedded = "const auto script = R\"SH(\nconfigure\nvalidate\ninspect\nbuild\npackage\nreport\n)SH\";";
 CHECK(filterDuploCandidates(CLEANUP_PROFILES.at("tools"), Json::array({hit}), [&](const std::string&) { return embedded; }).at("hits") == Json::array({hit}));
 const auto comments = filterDuploCandidates(cpp(), Json::array({hit}), [](const std::string&) { return "// header\n// a\n// b\n// c\n// d\n// e\n// f"; });
 CHECK(comments.at("hits").empty());
 CHECK(comments.at("nonCodeRejected").size() == 1);
}
void declarations_and_getters() {
 for (const auto& sources :
  std::vector<Json>{
   {{"a.h", "struct Input { int width; int height; void Read(int); void Stop(); };"}, {"b.h", "struct Output { int count; int capacity; void Write(int); void Cancel(); };"}},
   {{"a.cpp", "int A::first() { return first_.load(); }\nint A::second() { return second_.load(); }"}, {"b.cpp", "int B::count() { return count_.load(); }\nint B::limit() { return limit_.load(); }"}},
  }) {
  const auto result = classifySources(sources);
  CHECK(result.at("duplications").empty());
  CHECK(result.at("filtered").size() == 1);
 }
}
void executable_sequences() {
 const auto repeated = classifySources({
  {"a.cpp", "void StartA() { auto x = acquire(); inspect(x); publish(x); }"},
  {"b.cpp", "void StartB() { auto y = acquire(); inspect(y); publish(y); }"},
 });
 CHECK(repeated.at("duplications").size() == 1);
 const auto unrelated = classifySources({
  {"a.cpp", "void Start() { auto x = open(); read(x); }"},
  {"b.cpp", "void Finish() { auto y = flush(); close(y); }"},
 });
 CHECK(unrelated.at("duplications").empty());
}
void named_fields() {
 const auto result = classifySources({
  {"a.h", "struct A { unsigned width = 0; unsigned height = 0; unsigned stride = 0; };"},
  {"b.h", "struct B { unsigned width = 0; unsigned height = 0; unsigned stride = 0; };"},
 });
 CHECK(result.at("duplications").size() == 1);
 CHECK(includes(result.at("duplications").at(0).at("patterns"), "same complete record declaration"));
}
void namespace_aliases() {
 for (const auto& sources : std::vector<Json>{
       {{"a.cpp", "namespace runtime = application::runtime;\nnamespace gpu = application::gpu;"}, {"b.cpp", "namespace runtime = application::runtime;\nnamespace gpu = application::gpu;"}},
       {{"a.h", "struct A { int x = 0; int y = 0; bool valid() const { return x != 0 && y != 0; } };"}, {"b.h", "struct B { int x = 0; int y = 0; bool valid() const { return x != 0 || y != 0; } };"}},
      })
  CHECK(classifySources(sources).at("duplications").empty());
}
void lock_and_forward() {
 for (const std::string guard : {"lock(mutex_)", "lock{mutex_}"}) {
  CHECK(classifySources({
                         {"a.cpp", "void A::SetPeer(int epoch) { std::scoped_lock " + guard + "; input_.SetPeer(epoch); }"},
                         {"b.cpp", "void B::SetPeer(int epoch) { std::scoped_lock " + guard + "; input_.SetPeer(epoch); }"},
                        })
    .at("duplications")
    .empty());
 }
}
void member_identity() {
 for (const std::string changed : {
       "void B(State& value) { inspect(value.height); publish(value, 4); }",
       "void B(State& value) { inspect(value.width); publish(value, 8); }",
      }) {
  CHECK(classifySources({
                         {"a.cpp", "void A(State& state) { inspect(state.width); publish(state, 4); }"},
                         {"b.cpp", changed},
                        })
    .at("duplications")
    .empty());
 }
 CHECK(classifySources({
                        {"a.cpp", "void A(State& first, State& second) { inspect(first, first); publish(second); }"},
                        {"b.cpp", "void B(State& left, State& right) { inspect(left, right); publish(right); }"},
                       })
   .at("duplications")
   .empty());
}
void template_and_assignment() {
 CHECK(classifySources({
                        {"a.cpp", "template<class T> void A(T& first) { inspect(first); publish(first); }"},
                        {"b.cpp", "template<class T> void B(T& second) { inspect(second); publish(second); }"},
                       })
        .at("duplications")
        .size() == 1);
 CHECK(classifySources({
                        {"a.cpp", "First& operator=(const First& value) { inspect(value); publish(value); return *this; }"},
                        {"b.cpp", "Second& operator=(const Second& other) { inspect(other); publish(other); return *this; }"},
                       })
        .at("duplications")
        .size() == 1);
}
void partial_spans() {
 const Json sources{
  {"a.cpp", "void A(int fd) { auto count = write(fd, &value, sizeof(value)); check(count); }"},
  {"b.cpp", "void B(int fd) { auto count = read(fd, &value, sizeof(value)); check(count); }"},
 };
 auto ranges = Json::object();
 for (const auto& [path, value] : sources.items()) {
  const auto& source = value.get_ref<const std::string&>();
  ranges[path] = {{"column", source.find("(fd,") + 1}, {"endColumn", source.find("check(count);") + 13}};
 }
 CHECK(classifySources(sources, ranges).at("duplications").empty());
 const Json changed{
  {"a.cpp", "void A() { start(); Config config{.count = 2, .device = 0, .size = 8}; publish(config); }"},
  {"b.cpp", "void B() { start(); Config config{.count = 3, .device = 0, .size = 8}; publish(config); }"},
 };
 for (const std::string edge : {"column", "endColumn"}) {
  auto partial = Json::object();
  for (const auto& [path, value] : changed.items()) {
   const auto& source = value.get_ref<const std::string&>();
   partial[path] = {{edge, edge == "column" ? source.find(".device") + 1 : source.find(".count")}};
  }
  CHECK(classifySources(changed, partial).at("duplications").empty());
 }
 auto same = sources;
 same["b.cpp"] = replaceFirst(sources.at("b.cpp").get<std::string>(), "read", "write");
 CHECK(classifySources(same, ranges).at("duplications").size() == 1);
}
void nested_for() {
 CHECK(classifySources({
                        {"a.cpp", "void A() { for (int i = 0; i < count; ++i) { inspect(i); publish(i); } }"},
                        {"b.cpp", "void B() { for (int n = 0; n < count; ++n) { inspect(n); publish(n); } }"},
                       })
        .at("duplications")
        .size() == 1);
}
void non_algorithm_prefixes() {
 for (const auto& sources : std::vector<Json>{
       {{"a.cpp", "void A() { for (int i = 0; i < count; ++i) { inspect(i); } }"}, {"b.cpp", "void B() { for (int i = 0; i < count; ++i) { publish(i); } }"}},
       {{"a.cpp", "void forward(Tensor x) { const auto residual = x; first(x); }"}, {"b.cpp", "void forward(Tensor x) { const auto residual = x; second(x); }"}},
       {{"a.cpp", "void apply(T& value) { static_assert(accepts<T>()); first(value); }"}, {"b.cpp", "void apply(T& value) { static_assert(accepts<T>()); second(value); }"}},
       {{"a.cpp", "void Start(Request& request) { if (!request.current()) return; first(request); }"}, {"b.cpp", "void Start(Request& request) { if (!request.current()) return; second(request); }"}},
       {{"a.cpp", "void A(T& value) { if constexpr (supported<T>()) { inspect(value); } first(value); }"},
        {"b.cpp", "void B(T& value) { if constexpr (supported<T>()) { inspect(value); } second(value); }"}},
      })
  CHECK(classifySources(sources).at("duplications").empty());
}
void tensor_dimensions() {
 CHECK(classifySources({
                        {"a.cpp", "void A(Tensor& value) { const int batch = value.size(0); const int heads = value.size(2); const int channels = value.size(3); }"},
                        {"b.cpp", "void B(Tensor& value) { const int batch = value.size(0); const int height = value.size(2); const int width = value.size(3); }"},
                       })
   .at("duplications")
   .empty());
}
void arithmetic_receivers() {
 CHECK(classifySources({
                        {"a.cpp", "void A(Tensor& masks) { const auto count = masks.size(-2) * masks.size(-1); auto points = random(count, masks.device()); }"},
                        {"b.cpp", "void B(Sparse& sparse) { const auto count = sparse.features.size(-2) * sparse.features.size(-1); auto points = random(count, sparse.features.device()); }"},
                       })
        .at("duplications")
        .size() == 1);
 CHECK(classifySources({
                        {"a.cpp", "void A(Tensor& first, Tensor& second) { inspect(first); const auto count = first.size(0) * second.size(0); publish(count); }"},
                        {"b.cpp", "void B(Tensor& first, Tensor& second) { inspect(first); const auto count = second.size(0) * second.size(0); publish(count); }"},
                       })
   .at("duplications")
   .empty());
}
void enclosing_declaration() {
 const std::string source = "struct State {\nint width;\nint height;\nint stride;\nint x;\nint y;\nint z;\n};";
 const auto result = filterCpdCandidates(R"json([
    {"lineCount":3,"tokenCount":39,"occurrences":[
      {"path":"state.h","start":2,"end":4},
      {"path":"state.h","start":5,"end":7}
    ]}
  ])json"_fixture,
  {.sourceReader = [&](const std::string&) { return source; }});
 CHECK(result.at("duplications").empty());
}
void independent_groups() {
 const auto result = classifySources({
  {"a.cpp", "void A() { prepare(); publish(); }"},
  {"b.cpp", "void B() { prepare(); publish(); }"},
  {"c.cpp", "void C() { inspect(); release(); }"},
  {"d.cpp", "void D() { inspect(); release(); }"},
 });
 CHECK(result.at("duplications").size() == 2);
 auto paths = Json::array();
 for (const auto& hit : result.at("duplications")) paths.push_back(fieldValues(hit.at("occurrences"), "path"));
 CHECK(paths == R"json([["a.cpp","b.cpp"],["c.cpp","d.cpp"]])json"_fixture);
}
void overlapping_spans() {
 const Json sources{
  {"a.cpp", "void A() { inspect(value); publish(value); }"},
  {"b.cpp", "void B() { inspect(value); publish(value); }"},
 };
 auto occurrences = Json::array();
 for (const auto& [path, value] : sources.items()) {
  const auto& source = value.get_ref<const std::string&>();
  occurrences.push_back({{"path", path}, {"start", 1}, {"end", 1}, {"column", source.find("inspect") + 1}, {"endColumn", source.find("; }") + 1}});
 }
 const Json candidate{{"lineCount", 1}, {"tokenCount", 39}, {"occurrences", occurrences}};
 auto larger = candidate;
 larger["tokenCount"] = 42;
 for (auto& occurrence : larger["occurrences"]) occurrence["column"] = occurrence.at("column").get<std::size_t>() + 7;
 const auto result = filterCpdCandidates(Json::array({candidate, larger}), {.sourceReader = [&](const std::string& path) { return sources.at(path).get<std::string>(); }});
 CHECK(result.at("duplications").size() == 1);
 CHECK(result.at("duplications").at(0).at("tokenCount") == 42);
 CHECK(result.at("filtered").at(0).at("reason").get<std::string>().find("already represented") != std::string::npos);
}
void overload_family() {
 const std::string source = R"cpp(void read(const char* key, A& value) {
                                   int index = static_cast<int>(value);
                                   decode(key, index);
                                   first(index, value);
                                  }
                                  void read(const char* key, B& value) {
                                   int index = static_cast<int>(value);
                                   decode(key, index);
                                   second(index, value);
                                  }
                                  void read(const char* key, C& value) {
                                   int index = static_cast<int>(value);
                                   decode(key, index);
                                   third(index, value);
                                  })cpp";
 const auto result = filterCpdCandidates(R"json([
    {"lineCount":3,"tokenCount":39,"occurrences":[
      {"path":"a.cpp","start":1,"end":3},{"path":"a.cpp","start":4,"end":6}
    ]},
    {"lineCount":3,"tokenCount":41,"occurrences":[
      {"path":"a.cpp","start":4,"end":6},{"path":"a.cpp","start":7,"end":9}
    ]}
  ])json"_fixture,
  {.sourceReader = [&](const std::string&) { return source; }});
 CHECK(result.at("duplications").size() == 1);
 CHECK(spans(result.at("duplications").at(0).at("occurrences")) == R"json([[1,2],[4,5],[7,8]])json"_fixture);
}
void assertion_facts() {
 const auto result = classifySources({
  {"a.cpp", "void A() { CHECK(frame.ready); CHECK(frame.width == 3); }"},
  {"b.cpp", "void B() { CHECK(run.active); CHECK(run.epoch == 3); }"},
 });
 CHECK(result.at("duplications").empty());
 const auto repeated = classifySources({
  {"a.cpp", "void A() { CHECK(frame.ready); CHECK(frame.width == 3); }"},
  {"b.cpp", "void B() { CHECK(frame.ready); CHECK(frame.width == 3); }"},
 });
 CHECK(repeated.at("duplications").size() == 1);
}
void strings_and_callbacks() {
 const auto result = classifySources({
  {"a.cpp", R"cpp(void A() {
                   const auto text = R"tag({ void fake() { })tag";
                   /* } arbitrary comment { */
                   callback([]() { inspect(); });
                   auto x = acquire();
                   inspect(x);
                   publish(x);
                  })cpp"},
  {"b.cpp", R"cpp(void B() {
                   const auto text = R"tag({ void fake() { })tag";
                   callback([]() { inspect(); });
                   auto y = acquire();
                   inspect(y);
                   publish(y);
                  })cpp"},
 });
 CHECK(result.at("duplications").size() == 1);
 CHECK(!includes(result.at("duplications").at(0).at("patterns"), "unclassified syntax requires manual review"));
}
void lambda_capture() {
 CHECK(classifySources({
                        {"a.cpp", "void A(Runtime& runtime) { owner.Defer(runtime, [value = std::move(candidate)]() mutable { runtime.Commit(std::move(value)); first(); }); }"},
                        {"b.cpp", "void B(Runtime& runtime) { owner.Defer(runtime, [value = std::move(candidate)]() mutable { runtime.Commit(std::move(value)); second(); }); }"},
                       })
   .at("duplications")
   .empty());
}
void raw_arguments() {
 const auto raw = parseArgs({"--raw-cpd", "--min-tokens", "16", "--output", "cleanup/raw.json"});
 CHECK(raw.at("profile").at("cpd").at("minTokens") == 16);
 CHECK(raw.at("output") == "cleanup/raw.json");
 CHECK(cpp().at("cpd").at("minTokens") == 39);
 CHECK(raw.at("profile").at("cpd").at("flags") == Json::array({"--skip-blocks-pattern", ""}));
 const auto tools = parseArgs({"--raw-cpd", "--tools", "--review"});
 CHECK(tools.at("profile").at("name") == "tools");
 CHECK(tools.at("output") == "cleanup/tools-review.json");
 CHECK(parseArgs({"--raw-cpd", "--tools"}).at("output") == "cleanup/tools-raw.json");
 for (const auto& args : std::vector<std::vector<std::string>>{
       {"--raw-cpd", "--min-tokens", "1"},
       {"--raw-cpd", "--min-tokens", "NaN"},
       {"--raw-cpd", "--min-tokens", "12.5"},
       {"--raw-cpd", "--output"},
       {"--raw-cpd", "--cpp"},
       {"--raw-cpd", "--output", "a", "--output", "b"},
      })
  throws([&] { parseArgs(args); });
}
constexpr mmltk::tools::testing::Case cases[]{
 {"raw CPD includes real PMD-suppressed code with original paths, columns, literals and CRLF", real_pmd},
 {"raw CPD rejects partial detector runs without replacing a complete report and removes scan copies", partial_detector},
 {"empty raw CPD inventories complete without launching a detector", empty_inventory},
 {"profiles and arguments select exactly one frozen configuration", profiles},
 {"inventory commands are exact and frozen", inventory_commands},
 {"detector commands exactly encode each profile", detector_commands},
 {"frontend inventory unions tracked and untracked-unignored files", frontend_inventory},
 {"inventory rejects unavailable and newline paths", invalid_inventory},
 {"Duplo and CPD fixture parsers preserve source ranges", parsers},
 {"shared sorting and inline suppression retain two occurrences", sorting_and_suppression},
 {"cleanup contains only targets while rejected output retains diagnostics by profile", reports},
 {"context filtering checks every match size and retains repeated implementations", match_sizes},
 {"CPD retains overloaded conversion setup despite distinct enum types and fallbacks", conversion_setup},
 {"overload identity includes the complete templated first parameter", overload_identity},
 {"equal signatures in separate implementations do not manufacture an overload family", equal_signatures},
 {"aliases and bare control transfers do not turn one call into a shared algorithm", aliases_and_transfers},
 {"scalar calculations remain candidates despite different local roles", scalar_calculations},
 {"repeated admission guards remain candidates despite different input roles", admission_guards},
 {"CUDA setup remains visible alongside complete kernels and numerical work", cuda_setup},
 {"Duplo compares complete initializer context and preserves real repeated operations", duplo_context},
 {"CPD does not present ordinary declarations or concatenated getters as extraction work", declarations_and_getters},
 {"CPD keeps repeated executable sequences and separates different operation names", executable_sequences},
 {"CPD retains repeated named fields that may duplicate a canonical declaration", named_fields},
 {"CPD excludes namespace aliases and records that only share a field prefix", namespace_aliases},
 {"a lock and one forwarded operation do not manufacture an overload algorithm", lock_and_forward},
 {"CPD preserves member identities, constants and alias relationships after local renaming", member_identity},
 {"CPD recognizes executable template and assignment-operator bodies", template_and_assignment},
 {"partial CPD spans retain the complete operation and initializer context", partial_spans},
 {"statement context handles for headers and compact nested blocks", nested_for},
 {"loop headers, simple aliases, and contract guards are not shared algorithms", non_algorithm_prefixes},
 {"independent tensor dimension reads preserve their distinct local roles", tensor_dimensions},
 {"shared arithmetic accepts local or member receiver inputs without losing alias relationships", arithmetic_receivers},
 {"multiple spans of one enclosing declaration are not independent duplicates", enclosing_declaration},
 {"independent operation groups stay separate within one CPD candidate", independent_groups},
 {"overlapping CPD spans of the same complete statements produce one target", overlapping_spans},
 {"one overload family consolidates matches without losing distinct function targets", overload_family},
 {"CPD preserves assertion facts rather than merging unrelated test expectations", assertion_facts},
 {"CPD skips strings and comments and handles outer code after a nested callback", strings_and_callbacks},
 {"a call in a lambda capture is not a function declaration or overload family", lambda_capture},
 {"raw CPD argument validation cannot alter ordinary profile thresholds", raw_arguments},
};
}  // namespace cleanup_test
export int mmltk_tool_generate_cleanup_json_test(int argc, char** argv) {
 try {
  if (argc > 1 && std::string_view(argv[1]) == "cpd") {
   if (const char* fixture = std::getenv("MMLTK_CLEANUP_TEST_PMD_FIXTURE")) return cleanup_test::fakeDetector(argc, argv, fixture);
   throw std::runtime_error("fake detector invoked without its fixture configuration");
  }
  return mmltk::tools::testing::run_cases(cleanup_test::cases, argc, argv);
 } catch (const std::exception& error) {
  std::cerr << "generate_cleanup_json.test: " << error.what() << '\n';
  return 1;
 }
}
