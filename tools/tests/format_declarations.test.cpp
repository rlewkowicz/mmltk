module;
#include "cpp_test_macros.hpp"
// Native counterpart of format_declarations.test.mjs: all 21 original cases.
// Run from the repository root (REPO_ROOT); the existing text fixtures and real
// training declaration source are read directly without interpreter execution.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <optional>
#include <utility>
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
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
#include <regex>

export module mmltk.tools.tests.format_declarations_test;
import mmltk.tools.cleanup.cpd_patterns;
import mmltk.tools.cleanup.declaration_patterns;
import mmltk.tools.format_declarations;
import mmltk.tools.generate_cleanup_json;
import mmltk.tools.tests.cpp_test_support;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;

namespace formatter_test {
using Json = nlohmann::ordered_json;
using namespace mmltk::tools::declarations;
using mmltk::tools::cpd::tokenizeCpp;
using mmltk::tools::formatter::declarationFormatReport;
using mmltk::tools::formatter::parseFormatArgs;
using mmltk::tools::cleanup::CLEANUP_PROFILES;
using mmltk::tools::cleanup::REPO_ROOT;
using mmltk::tools::cleanup::buildInventory;
using mmltk::tools::cleanup::detectorArguments;
using mmltk::tools::cleanup::parseArgs;
using mmltk::tools::cleanup::parseCpdXml;
using mmltk::tools::cleanup::rawCpdReport;

using namespace mmltk::tools::testing;

std::string fixture(std::string_view name) {
  return mmltk::tools::read_text(REPO_ROOT / "tools/tests/fixtures/declarations" / (std::string(name) + ".txt"));
}
const std::string& text(const Json& value) { return value.get_ref<const std::string&>(); }
using mmltk::tools::includes;
bool contains(std::string_view source, std::string_view value) {
  return source.find(value) != std::string_view::npos;
}
const std::string& sourcePath() { static const std::string value = "src/fixture.h"; return value; }

void registered_surface() {
  const std::string source = "#include \"" + std::string(DECLARATION_SURFACE) + "\"\nstruct Request { MMLTK_MAX_BYTES(64) std::string name; };\n";
  CHECK(formatDeclarationSource(sourcePath(), source).at("output") == source);
  const std::string conditional = "#if ENABLED\n#include \"" + std::string(DECLARATION_SURFACE) + "\"\n#endif\nstruct Request { MMLTK_MAX_BYTES(64) std::string name; };\n";
  CHECK(contains(text(formatDeclarationSource(sourcePath(), conditional).at("output")), "#include \"" + std::string(ANNOTATION_HEADER) + "\""));
}

void preservation() {
  const auto canonical = fixture("canonical");
  const auto result = formatDeclarationSource(sourcePath(), canonical);
  CHECK(result.at("changed") == true);
  const auto& output = text(result.at("output"));
  for (const std::string_view expected : {
    "MMLTK_MAX_ITEMS(kTrainingSourceCapacity) std::vector<MetricSource> available",
    "MMLTK_MAX_ITEMS(kMaximumTrainingModels + 1U)",
    "MMLTK_MAX_BYTES(64) MMLTK_MIN_BYTES(1) std::string identity",
    "MMLTK_MAX_PATH_BYTES std::filesystem::path",
    "MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0) MMLTK_FINITE double fraction = 0.5",
    "MMLTK_CATALOG(Catalog<int, float>)",
    "MMLTK_MAX_ITEMS(std::integral_constant<std::size_t, 2>::value)",
  }) CHECK(contains(output, expected));
  const auto trailer = canonical.substr(canonical.find("// [[="));
  CHECK(output.ends_with(trailer));
  CHECK(contains(output, "identity = \"MMLTK_MAX_BYTES(99)\""));
  CHECK(contains(output, "bool operator==(const TrainingSourceCatalog&) const = default;"));
  CHECK(contains(output, "MMLTK_REFLECT_FIELDS(TrainingSourceCatalog)"));
  const auto header = output.find(ANNOTATION_HEADER);
  CHECK(header != std::string::npos);
  CHECK(output.find(ANNOTATION_HEADER, header + ANNOTATION_HEADER.size()) == std::string::npos);
}

void idempotence() {
  const auto canonical = fixture("canonical");
  const auto first = formatDeclarationSource(sourcePath(), canonical);
  const auto second = formatDeclarationSource(sourcePath(), text(first.at("output")));
  CHECK(second.at("output") == first.at("output"));
  CHECK(second.at("edits").size() == 0);
  CHECK(second.at("changed") == false);
  const auto& candidates = first.at("candidates");
  const auto available = std::find_if(candidates.begin(), candidates.end(), [](const auto& item) {
    return item.value("replacement", "") == "MMLTK_MAX_ITEMS(kTrainingSourceCapacity)";
  });
  CHECK(available != candidates.end());
  CHECK(available->at("context").at("owner") == "TrainingSourceCatalog");
  CHECK(available->at("context").at("member") == "available");
  CHECK(includes(available->at("categories"), "typed_limit"));
  const auto start = available->at("start_offset").get<std::size_t>();
  CHECK(canonical.substr(start, available->at("end_offset").get<std::size_t>() - start) == available->at("text"));
  CHECK(std::any_of(second.at("candidates").begin(), second.at("candidates").end(), [](const auto& item) {
    return includes(item.at("categories"), "shortened_annotation");
  }));
}

void uncertain_declarations() {
  const auto ambiguous = fixture("ambiguous");
  const auto result = formatDeclarationSource(sourcePath(), ambiguous);
  CHECK(result.at("output") == ambiguous);
  CHECK(result.at("edits").size() == 0);
  auto annotations = Json::array();
  for (const auto& item : result.at("candidates")) {
    if (includes(item.at("categories"), "reflection_annotation")) annotations.push_back(item);
  }
  CHECK(annotations.size() == 7);
  CHECK(std::all_of(annotations.begin(), annotations.end(), [](const auto& item) {
    return item.at("disposition") == "manual" && !text(item.at("reason")).empty();
  }));
  for (const std::string_view reason : {"composition", "conditional", "comment"}) {
    CHECK(std::any_of(annotations.begin(), annotations.end(), [&](const auto& item) {
      return contains(text(item.at("reason")), reason);
    }));
  }
}

void namespace_lookup() {
  const std::string declaration = "[[= ::policy::MaxBytes{8}]] std::string value;";
  for (const auto& source : std::vector<std::string>{
    declaration + "\nnamespace policy = ::mmltk::frameworks::reflection;",
    "namespace outer { namespace policy = ::mmltk::frameworks::reflection; }\n" + declaration,
    "namespace policy = ::mmltk::frameworks::reflection;\nstruct policy {};\n" + declaration,
    "namespace policy = mmltk::frameworks::reflection;\n" + declaration,
  }) CHECK(formatDeclarationSource(sourcePath(), source).at("changed") == false);
  CHECK(formatDeclarationSource(sourcePath(), "namespace policy = ::mmltk::frameworks::reflection;\nnamespace owner {\n" + declaration + "\n}").at("changed") == true);
}

void relative_policies() {
  for (const std::string source : {
    "[[= mmltk::frameworks::reflection::MaxBytes{8}]] std::string value;\n",
    "namespace owner { namespace mmltk::frameworks::reflection { struct MaxBytes { int value; }; }\n"
      "[[= mmltk::frameworks::reflection::MaxBytes{8}]] std::string value; }\n",
    "namespace owner { namespace policy = mmltk::frameworks::reflection;\n[[= policy::MaxBytes{8}]] std::string value; }\n",
    "namespace policy = ::mmltk::frameworks::reflection;\nstruct Owner : ForeignBase { [[= policy::MaxBytes{8}]] std::string value; };\n",
  }) {
    const auto result = formatDeclarationSource(sourcePath(), source);
    CHECK(result.at("output") == source);
    CHECK(std::any_of(result.at("candidates").begin(), result.at("candidates").end(), [](const auto& candidate) {
      return contains(text(candidate.at("reason")), "not proven global");
    }));
  }
  const std::string source = "namespace owner { namespace mmltk {}\n[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] std::string value; }\n";
  CHECK(contains(text(formatDeclarationSource(sourcePath(), source).at("output")), "MMLTK_MAX_BYTES(8)"));
}

void named_bounds() {
  const std::string source = "[[= ::mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string value;\n";
  const auto result = formatDeclarationSource(sourcePath(), source);
  CHECK(contains(text(result.at("output")), "MMLTK_MAX_BYTES(mmltk::frameworks::reflection::kMaximumPathBytes)"));
  CHECK(formatDeclarationSource(sourcePath(), text(result.at("output"))).at("changed") == false);
  auto global = source;
  global.replace(global.find("{mmltk"), 6, "{::mmltk");
  CHECK(contains(text(formatDeclarationSource(sourcePath(), global).at("output")), "MMLTK_MAX_PATH_BYTES std::string value;"));
}

void macro_conflicts() {
  const std::string definition = "#define MMLTK_MAX_BYTES(...) other(__VA_ARGS__)\n";
  CHECK(conflictingMacros("src/conflict.h", definition) == std::vector<std::string>{"MMLTK_MAX_BYTES"});
  CHECK(conflictingMacros(ANNOTATION_HEADER, definition).empty());
  const std::string annotation = "[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] std::string value;\n";
  for (const auto& conflict : std::vector<std::string>{definition, "#undef MMLTK_MAX_BYTES\n"}) {
    const auto report = declarationFormatReport({"src/conflict.h", sourcePath()}, {{"src/conflict.h", conflict}, {sourcePath(), annotation}});
    CHECK(report.at("summary").at("rewrite") == 0);
    CHECK(report.at("macro_conflicts") == Json::array({"MMLTK_MAX_BYTES"}));
    const auto& files = report.at("files");
    const auto file = std::find_if(files.begin(), files.end(), [](const auto& item) { return item.at("path") == sourcePath(); });
    CHECK(file != files.end());
    CHECK(contains(text(file->at("candidates").at(0).at("reason")), "conflicting"));
  }
}

void unconditional_includes() {
  const std::string source = "#pragma once\r\n#if ENABLE\r\n#include \"" + std::string(ANNOTATION_HEADER) + "\"\r\n#endif\r\n" +
    "[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] std::string value;\r\n";
  const auto result = formatDeclarationSource(sourcePath(), source);
  CHECK(text(result.at("output")).starts_with("#pragma once\r\n#include \"" + std::string(ANNOTATION_HEADER) + "\"\r\n#if ENABLE"));
  CHECK(formatDeclarationSource(sourcePath(), text(result.at("output"))).at("changed") == false);
  CHECK(formatDeclarationSource("src/fixture.cppm", source).at("changed") == false);
  CHECK(formatDeclarationSource("src/fixture.cppm.in", source).at("changed") == false);
  CHECK(formatDeclarationSource("src/fixture.cppm.in", "MMLTK_MAX_BYTES(8) std::string value;\n").at("changed") == false);
}

void declaration_categories() {
  const auto result = classifyDeclarations(sourcePath(), fixture("canonical") + "\n[[nodiscard]] __device__ __forceinline__ float convert(int n) { return static_cast<float>(n); }\n" +
    "template<class T> void declared(T value);\nauto x = option<Root, member_path<&Root::inner, &Inner::value>>(name, help);\n");
  std::set<std::string> categories;
  for (const auto& item : result.at("candidates")) {
    for (const auto& category : item.at("categories")) categories.insert(text(category));
  }
  for (const std::string kind : {"reflection_annotation", "typed_limit", "adjacent_annotation_composition", "registration_macro", "defaulted_equality", "optional_declaration", "collection_declaration", "attribute_qualifier", "cast", "nested_descriptor", "template_declaration"}) {
    require(categories.contains(kind), kind, __LINE__);
  }
}

void tokenizer_ranges() {
  const std::string source = "R\"x([[= fake{}]] /* literal */)x\" /* comment */ [[= actual{}]]";
  const auto tokens = tokenizeCpp(source, true);
  for (const auto& token : tokens) CHECK(source.substr(token.offset, token.end - token.offset) == token.text);
  CHECK(std::count_if(tokens.begin(), tokens.end(), [](const auto& token) { return token.kind == "literal"; }) == 1);
  CHECK(std::count_if(tokens.begin(), tokens.end(), [](const auto& token) { return token.kind == "comment"; }) == 1);
}

void raw_cpd_evidence() {
  const std::string source = "// CLEANUP-IGNORE: still raw\n[[= p::MaxBytes{8}]] std::string a;\n[[= p::MaxBytes{8}]] std::string b;\n";
  const auto parsed = parseCpdXml("<pmd-cpd><duplication lines=\"1\" tokens=\"12\"><file line=\"2\" endline=\"2\" column=\"1\" endcolumn=\"20\" path=\"src/fixture.h\"/><file line=\"3\" endline=\"3\" column=\"1\" endcolumn=\"20\" path=\"src/fixture.h\"/></duplication></pmd-cpd>");
  const auto selection = parseArgs({"--raw-cpd", "--min-tokens", "12"});
  const auto report = rawCpdReport(selection.at("profile"), {{"scannedFiles", Json::array({sourcePath()})}},
    {{"duplications", parsed}, {"status", 0}, {"stderr", ""}}, [&](const std::string&) { return source; });
  CHECK(report.at("raw_matches").size() == 1);
  CHECK(report.at("raw_matches").at(0).at("occurrences").size() == 2);
  CHECK(report.at("raw_matches").at(0).at("occurrences").at(0).at("text") == source.substr(source.find('\n') + 1, 19));
  CHECK(report.at("detector").at("inline_suppressions") == false);
  CHECK(report.at("detector").at("sequence_skipping") == false);
  CHECK(std::all_of(report.at("classified_context").begin(), report.at("classified_context").end(), [](const auto& item) {
    return item.at("disposition") != "rewrite";
  }));
  const auto options = detectorArguments(selection.at("profile")).at("cpd");
  for (const std::string_view flag : {"--ignore-identifiers", "--ignore-literal-sequences", "--skip-sequences", "--ignore-literals"}) CHECK(!includes(options, flag));
  const auto skip = std::find(options.begin(), options.end(), Json("--skip-blocks-pattern"));
  CHECK(skip != options.end() && std::next(skip) != options.end());
  CHECK(*std::next(skip) == "");
  CHECK(CLEANUP_PROFILES.at("cpp").at("cpd").at("minTokens") == 39);
  throws([&] {
    rawCpdReport(selection.at("profile"), {{"scannedFiles", Json::array({sourcePath()})}},
      {{"duplications", parsed}, {"status", 5}, {"stderr", "cannot lex file"}});
  }, "status 5", ErrorMatch::substring);
}

void raw_ordering() {
  const std::string source = "one();\ntwo();\n";
  const auto selection = parseArgs({"--raw-cpd"});
  const auto occurrence = [](int start) { return Json{{"path", sourcePath()}, {"start", start}, {"end", start}}; };
  const auto matches = Json::array({
    {{"tokenCount", 12}, {"lineCount", 1}, {"occurrences", Json::array({occurrence(2), occurrence(1)})}},
    {{"tokenCount", 13}, {"lineCount", 1}, {"occurrences", Json::array({occurrence(1), occurrence(2)})}},
  });
  const auto report = [&](const Json& duplications) {
    return rawCpdReport(selection.at("profile"), {{"scannedFiles", Json::array({sourcePath()})}},
      {{"duplications", duplications}, {"status", 0}, {"stderr", ""}}, [&](const std::string&) { return source; });
  };
  auto reversed = matches;
  std::reverse(reversed.begin(), reversed.end());
  for (auto& match : reversed) std::reverse(match.at("occurrences").begin(), match.at("occurrences").end());
  CHECK(report(matches) == report(reversed));
  CHECK(report(matches).at("raw_matches").size() == 2);
}

void multiline_evidence() {
  const std::string source = "before\n [[= x{\n  4}]]\nafter\n";
  const auto result = sourceOccurrence(sourcePath(), source, {{"start", 2}, {"end", 3}, {"column", 2}, {"endColumn", 7}}, lineStarts(source));
  CHECK(result.at("text") == "[[= x{\n  4}]]");
}

void formatter_inventory() {
  const auto files = buildInventory(CLEANUP_PROFILES.at("cpp"),
    {{"tracked", Json::array({"src/a.h", "third_party/vendor.h"})}, {"untracked", Json::array({"src/b.cpp"})}, {"deleted", Json::array()}},
    [](const std::string&) { return true; });
  CHECK(files.at("scannedFiles") == Json::array({"src/a.h", "src/b.cpp"}));
  const auto inventory = files.at("scannedFiles").get<std::vector<std::string>>();
  std::map<std::string, std::string> sources;
  for (const auto& path : inventory) sources.emplace(path, "");
  throws([&] { declarationFormatReport(inventory, sources, {{"files", Json::array({"../outside.h"})}}); }, "inventory", ErrorMatch::substring);
  const auto options = parseFormatArgs({"preview", "--file", "src/a.h", "--report", "cleanup/preview.json"});
  CHECK(options.at("mode") == "preview");
  CHECK(options.at("files") == Json::array({"src/a.h"}));
  for (const auto& args : std::vector<std::vector<std::string>>{{}, {"invalid"}, {"fix", "--file"}, {"check", "--report", "a", "--report", "b"}}) {
    throws([&] { parseFormatArgs(args); });
  }
}

void deterministic_reports() {
  const auto canonical = fixture("canonical");
  const std::vector<std::string> inventory{sourcePath()};
  const std::map<std::string, std::string> sources{{sourcePath(), canonical}};
  const auto report = declarationFormatReport(inventory, sources);
  CHECK(report == declarationFormatReport(inventory, sources));
  CHECK(report.at("changed_files") == Json::array({sourcePath()}));
  CHECK(report.at("files").at(0).at("before") == canonical);
  const std::map<std::string, std::string> fixedSources{{sourcePath(), text(report.at("files").at(0).at("after"))}};
  CHECK(declarationFormatReport(inventory, fixedSources).at("changed_files") == Json::array());
}

void real_training_declarations() {
  const std::string path = "src/backend/models/rfdetr/contract/training_metrics.h";
  const auto source = mmltk::tools::read_text(REPO_ROOT / path);
  const auto candidates = classifyDeclarations(path, source).at("candidates");
  const Json expected{
    {"TrainingSourceCatalog", {{"available", "MaxItems"}}},
    {"TrainingSources", {{"observations", "MaxItems"}, {"failures", "MaxItems"}, {"distributions", "MaxItems"}}},
    {"TrainingRun", {{"run_id", "MaxBytes"}, {"attempt_id", "MaxBytes"}, {"checkpoint_attempt_id", "MaxBytes"},
      {"source_checkpoint_attempt_id", "MaxBytes"}, {"original_weights", "MaxBytes"}, {"original_class_descriptor", "MaxBytes"}}},
  };
  for (const auto& [owner, members] : expected.items()) {
    for (const auto& [member, policy] : members.items()) {
      auto matches = Json::array();
      for (const auto& candidate : candidates) {
        if (candidate.at("context").at("owner") == owner &&
            candidate.at("context").at("member") == member &&
            candidate.value("policy", Json()) == policy) matches.push_back(candidate);
      }
      require(matches.size() == 1, owner + "." + member, __LINE__);
      CHECK(includes(matches.at(0).at("categories"), "typed_limit"));
      CHECK(text(matches.at(0).at("text")).starts_with("MMLTK_"));
    }
  }
  CHECK(std::any_of(candidates.begin(), candidates.end(), [](const auto& candidate) {
    return candidate.at("context").at("owner") == "TrainingSources" &&
      contains(text(candidate.at("text")), "kMaximumTrainingModels + 1U");
  }));
  CHECK(formatDeclarationSource(path, source).at("changed") == false);
}

void ctad_bounds() {
  const std::string source = "[[= ::mmltk::frameworks::reflection::Minimum{std::uint64_t{1U}}]] std::uint64_t identity = 0;\n";
  const auto result = formatDeclarationSource(sourcePath(), source);
  CHECK(contains(text(result.at("output")), "MMLTK_MINIMUM_VALUE(std::uint64_t{1U}) std::uint64_t identity = 0;"));
  CHECK(formatDeclarationSource(sourcePath(), text(result.at("output"))).at("changed") == false);
}

void continued_preprocessing() {
  const std::string source = "/* lead */ #define EXAMPLE \\\n [[= ::mmltk::frameworks::reflection::MaxBytes{8}]]\n"
    "/* multi\n line */ #if ENABLE\n[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] std::string value;\n#endif\n";
  CHECK(formatDeclarationSource(sourcePath(), source).at("output") == source);
}

void shortened_dependency() {
  const std::string source = "#pragma once\nMMLTK_MAX_BYTES(4) std::string value;\n";
  const auto result = formatDeclarationSource(sourcePath(), source);
  CHECK(result.at("edits").size() == 1);
  CHECK(contains(text(result.at("output")), "#include \"" + std::string(ANNOTATION_HEADER) + "\""));
  CHECK(text(result.at("output")).ends_with("MMLTK_MAX_BYTES(4) std::string value;\n"));
  CHECK(formatDeclarationSource(sourcePath(), text(result.at("output"))).at("changed") == false);
}

void conditional_aliases() {
  const std::string declaration = "[[= policy::MaxBytes{8}]] std::string value;\n";
  for (const std::string prefix : {"#define ALIAS namespace policy = mmltk::frameworks::reflection;\n",
    "#if ENABLE\nnamespace policy = mmltk::frameworks::reflection;\n#endif\n"}) {
    CHECK(formatDeclarationSource(sourcePath(), prefix + declaration).at("changed") == false);
  }
}

constexpr mmltk::tools::testing::Case cases[]{
    {"declaration formatter uses the registered authoring surface without adding a narrow include", registered_surface},
    {"declaration formatter preserves defaults, comments, literals and preprocessing", preservation},
    {"declaration formatting is idempotent and keeps declaration identity visible", idempotence},
    {"uncertain declarations stay byte-identical with concrete manual reasons", uncertain_declarations},
    {"namespace lookup requires a visible global alias with absolute spelling and no competing type", namespace_lookup},
    {"relative policies and aliases remain manual under namespace or inherited shadowing", relative_policies},
    {"named bounds retain relative lookup while only proven global bounds use a shorthand", named_bounds},
    {"macro definitions and undefinitions block automatic rewrites across inventory", macro_conflicts},
    {"direct annotation includes are unconditional and preserve CRLF", unconditional_includes},
    {"declaration categories do not depend on namespace depth", declaration_categories},
    {"shared tokenizer retains literal and comment source ranges without interpreting their contents", tokenizer_ranges},
    {"raw short CPD uses existing XML parser and retains exact evidence separately from context", raw_cpd_evidence},
    {"raw evidence ordering is deterministic without inventing or coalescing matches", raw_ordering},
    {"multiline source evidence uses exclusive CPD end columns and exact offsets", multiline_evidence},
    {"formatter inventory reuses CPD exclusions and never accepts paths outside it", formatter_inventory},
    {"formatter reports deterministic original edits in both repeated previews", deterministic_reports},
    {"real training declarations retain shared policy structure and every distinct member", real_training_declarations},
    {"CTAD bounds preserve their original construction and deduced policy type", ctad_bounds},
    {"preprocessing after comments and in continued definitions stays byte-identical", continued_preprocessing},
    {"already shortened authoring acquires its direct dependency exactly once", shortened_dependency},
    {"preprocessed or conditional aliases never authorize an outside rewrite", conditional_aliases},
};

}  // namespace formatter_test

export int mmltk_tool_format_declarations_test(int argc, char** argv) {
  return mmltk::tools::testing::run_cases(formatter_test::cases, argc, argv);
}
