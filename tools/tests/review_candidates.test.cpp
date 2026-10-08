module;
#include "cpp_test_macros.hpp"
// Native counterpart of review_candidates.test.mjs: all 15 original cases.
// Run from the repository root with real PMD
// configured for the suffix-copy case. Failure fixtures reexecute this binary;
// no JavaScript runtime is used.
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <unistd.h>
#include <array>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <utility>
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
export module mmltk.tools.tests.review_candidates_test;
import mmltk.tools.generate_cleanup_json;
import mmltk.tools.cleanup.cpd_patterns;
import mmltk.tools.cleanup.declaration_patterns;
import mmltk.tools.cleanup.review_patterns;
import mmltk.tools.tests.cpp_test_support;
import mmltk.tools.tool_command;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace review_test {
namespace fs = std::filesystem;
using namespace mmltk::tools::cleanup;
using mmltk::tools::cpd::SourceIndex;
using mmltk::tools::declarations::classifyDeclarations;
using mmltk::tools::declarations::sourceOccurrence;
using mmltk::tools::review::buildReview;
using mmltk::tools::review::physicalLines;
using mmltk::tools::review::renderReviewMarkdown;
using namespace mmltk::tools::testing;
using mmltk::tools::includes;
Json select(const Json& values, const std::function<bool(const Json&)>& predicate) {
 auto result = Json::array();
 for (const auto& value : values)
  if (predicate(value)) result.push_back(value);
 return result;
}
const Json& family(const Json& review, std::string_view kind) {
 const auto& families = review.at("families");
 const auto found = std::find_if(families.begin(), families.end(), [&](const Json& entry) { return entry.at("kind") == kind; });
 CHECK(found != families.end());
 return *found;
}
std::string repeat(std::string_view value, int count) {
 std::string result;
 for (int index = 0; index < count; ++index) result += value;
 return result;
}
Json rawProfile() { return parseArgs({"--raw-cpd"}).at("profile"); }
Json inventory(const Json& tracked) { return {{"tracked", tracked}, {"untracked", Json::array()}, {"deleted", Json::array()}}; }
struct ReviewResult {
 Json raw;
 Json review;
};
ReviewResult reviewOf(const Json& files, const Json& matches = Json::array()) {
 auto sources = Json::object();
 auto native = Json::array();
 for (const auto& file : files) {
  sources[file.at("path").get<std::string>()] = file.at("source");
  if (file.at("language") == "cpp") native.push_back(file.at("path"));
 }
 auto raw =
  rawCpdReport(rawProfile(), {{"scannedFiles", native}}, {{"duplications", matches}, {"status", 0}, {"stderr", ""}}, [&](const std::string& path) { return sources.at(path).get<std::string>(); });
 return {raw, buildReview(files, raw)};
}
void raw_selection() {
 CHECK(parseArgs({"--raw-cpd"}).at("output") == "cleanup/declarations-raw.json");
 CHECK(parseArgs({"--raw-cpd", "--review"}).at("output") == "cleanup/declarations-review.json");
 const auto selection = parseArgs({"--raw-cpd", "--output", "chosen.json", "--review", "--min-tokens", "27"});
 CHECK(selection.at("output") == "chosen.json");
 CHECK(selection.at("profile").at("cpd").at("minTokens") == 27);
 CHECK(selection.at("review") == true);
 for (const auto& args : std::vector<std::vector<std::string>>{{"--review"}, {"--raw-cpd", "--review", "--review"}, {"--raw-cpd", "--output"}, {"--raw-cpd", "--min-tokens", "--review"}})
  throws([&] { parseArgs(args); });
}
void authored_inventory() {
 auto retained =
  R"json(["src/a.cpp","src/template.cppm.in","src/app.rs","tools/check.py","tools/cleanup/patterns.mjs","src/output/value.h","web/a.tsx","web/b.mjs","web/style.css","web/index.html","cmake/A.cmake","CMakeLists.txt","docker/Dockerfile.dependencies","mmltk","tools/run.sh","src/frontend/iced/src/generated.rs"])json"_fixture;
 const auto excluded = R"json(["third_party/a.cpp","src/third_party/a.rs","build/a.cpp",".cache/a.py","web/node_modules/a.ts","src/generated/a.h","compiled/a.h","output/a.py"])json"_fixture;
 auto tracked = retained;
 for (const auto& value : excluded) tracked.push_back(value);
 for (const auto* value : {"README.md", "model.onnx", "src/deleted.cpp"}) tracked.push_back(value);
 const auto files = buildInventory(AUTHORED_PROFILE, {{"tracked", tracked}, {"untracked", {"tools/new.js"}}, {"deleted", {"src/deleted.cpp"}}}, [](const std::string&) { return true; });
 retained.push_back("tools/new.js");
 std::sort(retained.begin(), retained.end());
 CHECK(files.at("scannedFiles") == retained);
 CHECK(files.at("excludedByPrefix").size() + files.at("excludedGeneratedOutput").size() == excluded.size());
 CHECK(files.at("deletedFiles") == Json::array({"src/deleted.cpp"}));
 CHECK(!authoredLanguage("README.md"));
 throws([&] { buildInventory(AUTHORED_PROFILE, inventory({"src/missing.rs"}), [](const std::string&) { return false; }); }, "unavailable");
}
void size_triage() {
 auto files = Json::array();
 for (int lines : {499, 500, 501}) files.push_back({{"path", "tools/" + std::to_string(lines) + ".py"}, {"language", "python"}, {"source", repeat("# source\n", lines)}});
 files.push_back({{"path", "src/tests/embedded.rs"}, {"language", "rust"}, {"source", repeat("#[cfg(test)] mod tests {}\n", 501)}});
 const auto review = reviewOf(files).review;
 CHECK(review.at("queue") == Json::array({"src/tests/embedded.rs", "tools/501.py"}));
 CHECK(review.at("families") == Json::array());
 CHECK(review.at("inventory").at(0).at("test_evidence").at("dedicated") == true);
 CHECK(review.at("inventory").at(0).at("test_evidence").at("embedded") == true);
 CHECK(physicalLines("") == 0);
 CHECK(physicalLines("\n") == 1);
 CHECK(physicalLines("a\r\nb") == 2);
 CHECK(physicalLines(repeat("line\n", 500) + "last") == 501);
}
void nested_bindings() {
 const auto expressions = Json::array({
  R"cpp(option<Root, member_path<&Root::inner, &Inner::value>>("--value", "Help", "Group", {}, "--no-value"))cpp",
  R"cpp(custom_option<Root, member_path<&Root::inner, &Inner::items>, Codec<std::pair<int, int>>>("--ids", make_help(1, nested(2, 3)), "Execution"))cpp",
  R"cpp(negative_flag<Root, &Root::enabled>("--disabled", "Negative polarity", "Execution"))cpp",
  R"cpp(option_with_item_policy<Root, &Root::items, Policy<Limit{4}>>("--items", "Bounded", "Data"))cpp",
  R"cpp(unexposed<Root, member_path<&Root::inner, &Inner::value>>("owned by JSON"))cpp",
  R"cpp(MMLTK_CLI_OPTION(Scope, value, "Help", "Group"))cpp",
  R"cpp(MMLTK_CLI_NAMED(Scope, items, "--item-list", "Help", "Data", {}, "--no-items"))cpp",
 });
 std::string source = "constexpr std::array kOptions{\n";
 for (std::size_t index = 0; index < expressions.size(); ++index) {
  if (index) source += ",\n";
  source += expressions.at(index).get<std::string>();
 }
 source += "\n};\n";
 const auto classified = select(classifyDeclarations("src/options.h", source).at("candidates"),
  [](const Json& value) { return includes(value.at("categories"), "nested_descriptor") || includes(value.at("categories"), "descriptor_exclusion"); });
 auto text = Json::array();
 for (const auto& value : classified) {
  text.push_back(value.at("text"));
  CHECK(value.at("complete") == true);
  CHECK(value.at("context").at("owner") == "kOptions");
 }
 CHECK(text == expressions);
 const auto broken =
  select(classifyDeclarations("src/options.h", "option<Root, &Root::broken>(\n").at("candidates"), [](const Json& value) { return includes(value.at("categories"), "nested_descriptor"); });
 CHECK(broken.at(0).at("complete") == false);
}
void indexed_families() {
 const std::string path = "src/options.h";
 const std::string source =
  "option<Root, &Root::first>(\"--first\", \"One\", \"Input\");\noption<Root, &Root::second>(\"--second\", \"Two\", \"Input\");\noption<Root, &Root::third>(\"--third\", \"Three\", \"Input\");";
 auto files = Json::array(
  {{{"path", path}, {"language", "cpp"}, {"source", source}},
   {{"path", "src/more.h"}, {"language", "cpp"}, {"source", R"fixture(option<Other, &Other::fourth>("--fourth", "Four", "Output");)fixture"}}});
 auto occurrences = Json::array();
 for (int line : {1, 2}) occurrences.push_back({{"path", path}, {"start", line}, {"end", line}, {"column", 1}, {"endColumn", 10}});
 const Json match{{"tokenCount", 12}, {"lineCount", 1}, {"occurrences", occurrences}};
 const auto first = reviewOf(files, Json::array({match, match}));
 const auto& binding = family(first.review, "cli_binding");
 CHECK(binding.at("occurrence_count") == 4);
 CHECK(binding.at("file_count") == 2);
 CHECK(binding.at("covered_lines") == 4);
 CHECK(binding.at("status") == "unreviewed");
 CHECK(includes(binding.at("invariant_tokens"), "option"));
 CHECK(!select(binding.at("occurrences"), [](const Json& value) { return includes(value.at("varying_values"), "\"Output\""); }).empty());
 CHECK(select(binding.at("occurrences"), [&](const Json& value) { return value.at("path") == path; }).at(0).at("raw_match_ids") == Json::array({1, 2}));
 CHECK(first.raw.at("raw_matches").size() == 2);
 std::reverse(files.begin(), files.end());
 CHECK(first.review == reviewOf(files, Json::array({match, match})).review);
 const auto markdown = renderReviewMarkdown(first.review);
 CHECK(markdown.find("unreviewed") != std::string::npos);
 CHECK(markdown.find("src/options.h:1:1") != std::string::npos);
}
void trait_mapping() {
 const std::vector<std::string> names{"Minimum", "Maximum", "MaxBytes"};
 const std::vector<std::string> conversions{"long double", "unsigned long", "signed short"};
 std::string source;
 std::string primary;
 for (std::size_t index = 0; index < names.size(); ++index) {
  if (index) {
   source += '\n';
   primary += '\n';
  }
  source +=
   "template<class A> struct " + names[index] + "Annotation : std::bool_constant<A::marker_" + std::to_string(index) + " && std::is_convertible_v<decltype(A::value), " + conversions[index] + ">> {};";
  primary += "template<class A, class = void> struct " + names[index] + "Annotation : std::false_type {};";
 }
 for (const std::string name : {"alpha", "beta", "gamma"}) source += "\ndestination." + name + " = source." + name + ";\nfield(\"" + name + "\", settings." + name + ");";
 const auto review = reviewOf(Json::array({{{"path", "src/traits.h"}, {"source", source}, {"language", "cpp"}}})).review;
 // Use equal-shape primary traits to complement differing conversion shapes.
 const auto traits = reviewOf(Json::array({{{"path", "src/primary.h"}, {"source", primary}, {"language", "cpp"}}})).review;
 CHECK(family(traits, "trait_declaration").at("occurrence_count") == 3);
 const auto& specialization = family(review, "trait_declaration");
 CHECK(specialization.at("occurrence_count") == 3);
 CHECK(includes(specialization.at("occurrences").at(0).at("varying_values"), "marker_0"));
 CHECK(includes(specialization.at("occurrences").at(0).at("varying_values"), "double"));
 CHECK(select(review.at("families"), [](const Json& value) { return value.at("kind") == "same_name_mapping"; }).size() == 2);
 CHECK(review.at("summary").at("covered_lines") == 9);
}
void overlapping_names() {
 std::string source;
 for (const std::string member : {"first", "second", "third"}) {
  if (!source.empty()) source += '\n';
  source += "option<mmltk::reflection::Root, &mmltk::reflection::Root::" + member + ">(\"--" + member + "\", \"Help\", \"Group\");";
 }
 const auto review = reviewOf(Json::array({{{"path", "src/overlap.h"}, {"language", "cpp"}, {"source", source}}})).review;
 family(review, "cli_binding");
 family(review, "qualification");
 CHECK(review.at("summary").at("covered_lines") == 3);
 std::size_t count = 0;
 for (const auto& value : review.at("families")) count += value.at("covered_lines").get<std::size_t>();
 CHECK(count > 3);
}
void raw_conflicts() {
 const auto raw = reviewOf(Json::array({
                            {{"path", "src/a.h"}, {"language", "cpp"}, {"source", "[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] int value;"}},
                            {{"path", "src/b.h"}, {"language", "cpp"}, {"source", "#define MMLTK_MAX_BYTES(x) unsafe(x)\n"}},
                           }))
                   .raw;
 const auto classified = select(raw.at("classified_context"), [](const Json& value) { return includes(value.at("categories"), "reflection_annotation"); });
 CHECK(classified.at(0).at("reason").get<std::string>().find("conflicting") != std::string::npos);
 auto keys = Json::array();
 for (const auto& [key, value] : raw.items()) keys.push_back(key);
 CHECK(keys == Json::array({"format", "purpose", "inventory", "detector", "raw_matches", "classified_context", "summary"}));
}
void cpd_completion() {
 for (const std::string xml : {
       "",
       "garbage",
       "<unrelated/>",
       "<pmd-cpd>",
       "<pmd-cpd><duplication></pmd-cpd>",
       R"xml(<pmd-cpd><duplication lines="1" tokens="2"><file line="1" endline="1" path="a"/></duplication></pmd-cpd>)xml",
       R"xml(<pmd-cpd><file path="a" totalNumberOfTokens="-1"/></pmd-cpd>)xml",
       R"xml(<pmd-cpd><file path="a" totalNumberOfTokens="0"/><file path="a" totalNumberOfTokens="0"/></pmd-cpd>)xml",
       "<pmd-cpd><processing-error/></pmd-cpd>",
       "<pmd-cpd><codefragment><![CDATA[unterminated</codefragment></pmd-cpd>",
       R"xml(<pmd-cpd><file path="a" garbage totalNumberOfTokens="1"/></pmd-cpd>)xml",
      })
  throws([&] { parseCpdXml(xml); });
 throws([] { parseCpdXml("<pmd-cpd/>", {.expectedPaths = std::vector<std::string>{"a"}}); }, "coverage");
 CHECK(parseCpdXml(R"xml(<pmd-cpd><file path="empty.cpp" totalNumberOfTokens="0"/></pmd-cpd>)xml", {.expectedPaths = std::vector<std::string>{"empty.cpp"}}) == Json::array());
 throws([] { parseCpdXml(R"xml(<pmd-cpd><file path="outside.cpp" totalNumberOfTokens="0"/></pmd-cpd>)xml", {.expectedPaths = std::vector<std::string>{"inside.cpp"}}); }, "coverage");
 const std::string files = R"xml(<file path="a" line="1" endline="1"/><file path="b" line="1" endline="1"/>)xml";
 CHECK(parseCpdXml(R"xml(<pmd-cpd><duplication lines="1" tokens="2">)xml" + files + "<codefragment><![CDATA[<error> is source & text]]></codefragment></duplication></pmd-cpd>").size() == 1);
}
void coordinates() {
 const std::string source = "ab\r\n😀xy\r\nlast";
 CHECK(sourceOccurrence("file.cpp", source, {{"start", 2}, {"end", 2}, {"column", 1}, {"endColumn", 5}}).at("text") == "😀xy");
 CHECK(sourceOccurrence("file.cpp", source, {{"start", 3}, {"end", 3}}).at("text") == "last");
 for (const auto& occurrence :
  R"json([{"start":0,"end":1},{"start":1,"end":4},{"start":1,"end":2,"column":4},{"start":1,"end":1,"endColumn":4},{"start":2,"end":2,"column":2},{"start":2,"end":2,"endColumn":2},{"start":3,"end":3,"endColumn":6}])json"_fixture) {
  throws([&] { sourceOccurrence("file.cpp", source, occurrence); }, "span");
  throws([&] { SourceIndex(source).range(occurrence); }, "span");
 }
 throws([] { sourceOccurrence("file.cpp", "a\n", {{"start", 2}, {"end", 2}}); }, "span");
}
void suppression_comments() {
 const std::string cpp =
  "const char* a = \"// CLEANUP-IGNORE: not a comment\";\nconst char* b = R\"tag(\n// CLEANUP-OFF: literal\n// CLEANUP-ON\n)tag\";\n// CLEANUP-IGNORE: inspected occurrence\nwork();\n";
 CHECK(parseInlineSuppressions("a.cpp", cpp).at("a.cpp").size() == 1);
 const std::string rust =
  "let a = r###\"\n// CLEANUP-OFF: literal\n// CLEANUP-ON\n\"###;\nlet b = br#\"// CLEANUP-IGNORE: bytes\"#;\nlet c = cr\"// CLEANUP-IGNORE: c string\";\n/* nested /* // CLEANUP-OFF: comment content "
  "*/ // CLEANUP-ON */\nfn borrow<'a>(x: &'a str) {}\n// CLEANUP-IGNORE: inspected occurrence\nwork();\n";
 CHECK(parseInlineSuppressions("a.rs", rust).at("a.rs").size() == 1);
 const auto native = parseInlineSuppressions("a.cpp", "// CPD-OFF: inspected single occurrence\none();\n// CPD-ON\n").at("a.cpp");
 CHECK(native.at(0).at("detectors") == Json::array({"cpd"}));
 auto uses = Json::array();
 for (int start : {1, 2}) uses.push_back({{"detector", "cpd"}, {"path", "a.cpp"}, {"marker_line", 1}, {"lines", {start, 3}}});
 throws([&] { requireAtomicSuppressionUse(uses); }, "multiple local");
}
// External detector protocol fixture only. The production runner constructs
// the copy inventory and validates the resulting evidence.
int fakeDetector(int argc, char** argv, const char* configuration) {
 if (fs::path(argv[0]).filename() == "duplo.mjs") {
  std::cout << "[]";
  return 0;
 }
 const auto config = Json::parse(configuration);
 const std::vector<std::string> args(argv, argv + argc);
 const auto paths = detector_paths(args);
 std::string coverage;
 const auto coveragePaths = config.contains("coveragePath") ? std::vector<std::string>{config.at("coveragePath").get<std::string>()} : paths;
 for (const auto& value : coveragePaths) coverage += "<file path=\"" + value + "\" totalNumberOfTokens=\"5\"/>";
 std::string duplication;
 if (config.contains("duplicationPath"))
  duplication = "<duplication lines=\"1\" tokens=\"2\"><file path=\"" + config.at("duplicationPath").get<std::string>() + "\" line=\"1\" endline=\"1\" column=\"1\" endcolumn=\"4\"/><file path=\"" +
                paths.at(0) + "\" line=\"1\" endline=\"1\" column=\"5\" endcolumn=\"10\"/></duplication>";
 if (config.value("change", false)) mmltk::tools::write_text(config.at("path").get<std::string>(), "int changed = 2;\n");
 std::cout << config.value("xml", "<pmd-cpd>" + coverage + duplication + "</pmd-cpd>");
 std::cerr << config.value("stderr", "");
 return config.value("status", 0);
}
struct DetectorFixture {
 static constexpr const char* variable = "MMLTK_REVIEW_TEST_PMD_FIXTURE";
 mmltk::tools::TemporaryDirectory directory{(fs::current_path() / ".cleanup-evidence-fixture-").string()};
 std::string path = fs::relative(directory.path() / "input.cpp", fs::current_path()).string();
 fs::path binary = directory.path() / "pmd.mjs";
 fs::path output = directory.path() / "selected.json";
 fs::path markdown = directory.path() / "selected.md";
 Json profile = rawProfile();
 mmltk::tools::ScopedEnvironment savedEnvironment{variable};
 DetectorFixture() {
  mmltk::tools::write_text(path, "int value = 1;\n");
  write_fixture_program(binary, mmltk::tools::command::Command::test_review_candidates);
  profile["cpd"]["binary"] = binary.string();
 }
 DetectorFixture(const DetectorFixture&) = delete;
 DetectorFixture& operator=(const DetectorFixture&) = delete;
 void configure(Json config = Json::object()) const {
  config["path"] = path;
  if (::setenv(variable, config.dump().c_str(), 1) != 0) throw std::runtime_error("setenv failed");
 }
};
Json unlistedDetectorPaths(const std::string& path) {
 auto failures = Json::array();
 for (const auto& value : {fs::absolute(path).string(), fs::absolute(path + ".unlisted").string()}) {
  failures.push_back({{"coveragePath", value}});
  failures.push_back({{"duplicationPath", value}});
 }
 return failures;
}
void publication() {
 const DetectorFixture fixture;
 const GenerateRawCpdOptions options{.review = true, .inventoryInputs = inventory({fixture.path})};
 auto failures = R"json([{"status":4},{"status":5},{"stderr":"scanner ERROR"},{"stderr":"LexerException"},{"xml":"<pmd-cpd>"},{"xml":"<pmd-cpd/>"},{"change":true}])json"_fixture;
 for (const auto& failure : unlistedDetectorPaths(fixture.path)) failures.push_back(failure);
 for (const auto& failure : failures) {
  mmltk::tools::write_text(fixture.output, "previous json");
  mmltk::tools::write_text(fixture.markdown, "previous markdown");
  fixture.configure(failure);
  throws([&] { generateRawCpd(fixture.profile, fixture.output.string(), options); });
  CHECK(mmltk::tools::read_text(fixture.output) == "previous json");
  CHECK(mmltk::tools::read_text(fixture.markdown) == "previous markdown");
 }
 fixture.configure();
 auto rendering = options;
 rendering.renderMarkdown = [](const Json&) -> Json { throw std::runtime_error("render failed"); };
 throws([&] { generateRawCpd(fixture.profile, fixture.output.string(), rendering); }, "render failed");
 CHECK(mmltk::tools::read_text(fixture.output) == "previous json");
 CHECK(mmltk::tools::read_text(fixture.markdown) == "previous markdown");
 std::string source;
 for (const std::string name : {"first", "second", "third"}) {
  if (!source.empty()) source += '\n';
  source += "option<Root, &Root::" + name + ">(\"--" + name + "\", \"Help\");";
 }
 mmltk::tools::write_text(fixture.path, source);
 generateRawCpd(fixture.profile, fixture.output.string(), options);
 CHECK(Json::parse(mmltk::tools::read_text(fixture.output)).at("review").at("inventory").at(0).at("sha256").get<std::string>().size() == 64);
 CHECK(Json::parse(mmltk::tools::read_text(fixture.output)).at("review").at("families").at(0).at("occurrence_count") == 3);
 CHECK(mmltk::tools::read_text(fixture.markdown).find("unreviewed") != std::string::npos);
}
void detector_copies() {
 const DetectorFixture fixture;
 std::vector<std::string> paths;
 for (const char* name : {"a/same.cppm", "b/same.cppm", "kernel.cu", "detail.cuh", "template.cppm.in", "header.hpp.in"}) {
  const auto path = fixture.directory.path() / name;
  fs::create_directories(path.parent_path());
  mmltk::tools::write_text(path, "int repeated(int value) { return value * value + value + 1; }\n");
  paths.push_back(path.string());
 }
 const auto result = runCpd(rawProfile(), paths);
 CHECK(result.at("excludedLexerCount") == 0);
 std::set<std::string> actual;
 for (const auto& match : result.at("duplications"))
  for (const auto& occurrence : match.at("occurrences")) actual.insert(occurrence.at("path").get<std::string>());
 std::sort(paths.begin(), paths.end());
 CHECK(std::vector<std::string>(actual.begin(), actual.end()) == paths);
 for (const auto& match : result.at("duplications"))
  for (const auto& occurrence : match.at("occurrences")) {
   const auto path = occurrence.at("path").get<std::string>();
   CHECK(sourceOccurrence(path, mmltk::tools::read_text(path), occurrence).at("text").get<std::string>().find("repeated") != std::string::npos);
  }
}
void shared_completion() {
 const DetectorFixture fixture;
 auto normal = CLEANUP_PROFILES.at("cpp");
 normal["cpd"]["binary"] = fixture.profile.at("cpd").at("binary");
 for (const auto& failure : R"json([{"status":4},{"status":5},{"xml":"<unrelated/>"},{"xml":"<pmd-cpd/>"},{"stderr":"source ERROR"}])json"_fixture) {
  fixture.configure(failure);
  for (const auto& selected : {normal, fixture.profile}) throws([&] { runCpd(selected, {fixture.path}); });
 }
 for (const auto& failure : unlistedDetectorPaths(fixture.path)) {
  fixture.configure(failure);
  for (const auto& selected : {normal, fixture.profile}) throws([&] { runCpd(selected, {fixture.path}); }, "unlisted detector input");
 }
}
void ordinary_publication() {
 const DetectorFixture fixture;
 const auto duplo = fixture.directory.path() / "duplo.mjs";
 write_fixture_program(duplo, mmltk::tools::command::Command::test_review_candidates);
 const auto output = fs::relative(fixture.directory.path() / "ordinary.json", fs::current_path()).string();
 const auto rejected = fixture.directory.path() / "rejected.json";
 mmltk::tools::write_text(output, "previous ordinary");
 mmltk::tools::write_text(rejected, "{\"previous\":\"rejected\"}");
 auto failures = Json::array({{{"xml", "<pmd-cpd/>"}}});
 for (const auto& failure : unlistedDetectorPaths(fixture.path)) failures.push_back(failure);
 for (const auto& failure : failures) {
  fixture.configure(failure);
  auto profile = CLEANUP_PROFILES.at("cpp");
  profile["output"] = output;
  profile["cpd"] = fixture.profile.at("cpd");
  profile["duplo"]["binary"] = duplo.string();
  throws([&] { generateReport(profile, {.inventoryInputs = inventory({fixture.path})}); });
  CHECK(mmltk::tools::read_text(output) == "previous ordinary");
  CHECK(mmltk::tools::read_text(rejected) == "{\"previous\":\"rejected\"}");
 }
}
constexpr mmltk::tools::testing::Case cases[]{
 {"review is an opt-in raw operation with stable default and selected basenames", raw_selection},
 {"authored inventory covers code languages and preserves Git boundaries", authored_inventory},
 {"size triage is strictly over 500 physical lines and does not lex non-C++ files", size_triage},
 {"complete nested CLI bindings and exclusions retain each exceptional argument", nested_bindings},
 {"indexed families retain invariant and varying slots, cross-file anchors and unique raw associations", indexed_families},
 {"trait and same-name mapping families retain marker, conversion and field differences", trait_mapping},
 {"overlapping qualified names and complete bindings count physical evidence lines once", overlapping_names},
 {"raw classification shares whole-inventory conflicts and the default raw shape", raw_conflicts},
 {"CPD completion rejects malformed records, unrelated documents and missing coverage", cpd_completion},
 {"coordinates validate each original line, CRLF, Unicode and the final physical line", coordinates},
 {"suppression accounting admits actual comments and ignores C++ and Rust literal contents", suppression_comments},
 {"review outputs are prepared before publication and prior outputs survive detector, source and rendering failures", publication},
 {"detector copies cover module, CUDA and template suffixes without basename collisions", detector_copies},
 {"ordinary and raw detector modes share completion and source coverage checks", shared_completion},
 {"failed ordinary completion preserves both prior reports", ordinary_publication},
};
}  // namespace review_test
export int mmltk_tool_review_candidates_test(int argc, char** argv) {
 try {
  if ((argc > 1 && std::string_view(argv[1]) == "cpd") || std::filesystem::path(argv[0]).filename() == "duplo.mjs") {
   if (const char* config = std::getenv(review_test::DetectorFixture::variable)) return review_test::fakeDetector(argc, argv, config);
   throw std::runtime_error("review test executable takes no arguments");
  }
  return mmltk::tools::testing::run_cases(review_test::cases, argc, argv);
 } catch (const std::exception& error) {
  std::cerr << "review_candidates.test: " << error.what() << '\n';
  return 1;
 }
}
