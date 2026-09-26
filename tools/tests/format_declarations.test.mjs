import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { ANNOTATION_HEADER, classifyDeclarations, conflictingMacros, formatDeclarationSource, sourceOccurrence, lineStarts } from "../cleanup/declaration_patterns.mjs";
import { tokenizeCpp } from "../cleanup/cpd_patterns.mjs";
import { declarationFormatReport, parseFormatArgs } from "../format_declarations.mjs";
import { CLEANUP_PROFILES, buildInventory, detectorArguments, parseArgs, parseCpdXml, rawCpdReport } from "../generate_cleanup_json.mjs";

const fixture = (name) => readFileSync(new URL(`fixtures/declarations/${name}.txt`, import.meta.url), "utf8");
const canonical = fixture("canonical"), ambiguous = fixture("ambiguous");
const sourcePath = "src/fixture.h";

test("declaration formatter preserves defaults, comments, literals and preprocessing", () => {
  const result = formatDeclarationSource(sourcePath, canonical);
  assert.equal(result.changed, true);
  assert.match(result.output, /MMLTK_MAX_ITEMS\(kTrainingSourceCapacity\) std::vector<MetricSource> available/u);
  assert.match(result.output, /MMLTK_MAX_ITEMS\(kMaximumTrainingModels \+ 1U\)/u);
  assert.match(result.output, /MMLTK_MAX_BYTES\(64\) MMLTK_MIN_BYTES\(1\) std::string identity/u);
  assert.match(result.output, /MMLTK_MAX_PATH_BYTES std::filesystem::path/u);
  assert.match(result.output, /MMLTK_MINIMUM\(double, 0.0\) MMLTK_MAXIMUM\(double, 1.0\) MMLTK_FINITE double fraction = 0.5/u);
  assert.match(result.output, /MMLTK_CATALOG\(Catalog<int, float>\)/u);
  assert.match(result.output, /MMLTK_MAX_ITEMS\(std::integral_constant<std::size_t, 2>::value\)/u);
  const trailer = canonical.slice(canonical.indexOf("// [[="));
  assert.ok(result.output.endsWith(trailer));
  assert.ok(result.output.includes('identity = "MMLTK_MAX_BYTES(99)"'));
  assert.ok(result.output.includes(`bool operator==(const TrainingSourceCatalog&) const = default;`));
  assert.ok(result.output.includes(`MMLTK_REFLECT_FIELDS(TrainingSourceCatalog)`));
  assert.equal(result.output.split(ANNOTATION_HEADER).length, 2);
});

test("declaration formatting is idempotent and keeps declaration identity visible", () => {
  const first = formatDeclarationSource(sourcePath, canonical);
  const second = formatDeclarationSource(sourcePath, first.output);
  assert.equal(second.output, first.output);
  assert.equal(second.edits.length, 0);
  assert.equal(second.changed, false);
  const available = first.candidates.find((item) => item.replacement === "MMLTK_MAX_ITEMS(kTrainingSourceCapacity)");
  assert.equal(available.context.owner, "TrainingSourceCatalog");
  assert.equal(available.context.member, "available");
  assert.ok(available.categories.includes("typed_limit"));
  assert.equal(canonical.slice(available.start_offset, available.end_offset), available.text);
  assert.ok(second.candidates.some((item) => item.categories.includes("shortened_annotation")));
});

test("uncertain declarations stay byte-identical with concrete manual reasons", () => {
  const result = formatDeclarationSource(sourcePath, ambiguous);
  assert.equal(result.output, ambiguous);
  assert.equal(result.edits.length, 0);
  const annotations = result.candidates.filter((item) => item.categories.includes("reflection_annotation"));
  assert.equal(annotations.length, 7);
  assert.ok(annotations.every((item) => item.disposition === "manual" && item.reason.length));
  assert.ok(annotations.some((item) => item.reason.includes("composition")));
  assert.ok(annotations.some((item) => item.reason.includes("conditional")));
  assert.ok(annotations.some((item) => item.reason.includes("comment")));
});

test("namespace lookup requires a visible global alias with absolute spelling and no competing type", () => {
  const declaration = "[[= ::policy::MaxBytes{8}]] std::string value;";
  for (const source of [
    declaration + "\nnamespace policy = ::mmltk::frameworks::reflection;",
    "namespace outer { namespace policy = ::mmltk::frameworks::reflection; }\n" + declaration,
    "namespace policy = ::mmltk::frameworks::reflection;\nstruct policy {};\n" + declaration,
    "namespace policy = mmltk::frameworks::reflection;\n" + declaration,
  ]) assert.equal(formatDeclarationSource(sourcePath, source).changed, false);
  assert.equal(formatDeclarationSource(sourcePath, "namespace policy = ::mmltk::frameworks::reflection;\nnamespace owner {\n" + declaration + "\n}").changed, true);
});

test("relative policies and aliases remain manual under namespace or inherited shadowing", () => {
  for (const source of [
    "[[= mmltk::frameworks::reflection::MaxBytes{8}]] std::string value;\n",
    "namespace owner { namespace mmltk::frameworks::reflection { struct MaxBytes { int value; }; }\n" +
      "[[= mmltk::frameworks::reflection::MaxBytes{8}]] std::string value; }\n",
    "namespace owner { namespace policy = mmltk::frameworks::reflection;\n[[= policy::MaxBytes{8}]] std::string value; }\n",
    "namespace policy = ::mmltk::frameworks::reflection;\nstruct Owner : ForeignBase { [[= policy::MaxBytes{8}]] std::string value; };\n",
  ]) {
    const result = formatDeclarationSource(sourcePath, source);
    assert.equal(result.output, source);
    assert.ok(result.candidates.some((candidate) => candidate.reason.includes("not proven global")));
  }
  const source = "namespace owner { namespace mmltk {}\n[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] std::string value; }\n";
  assert.ok(formatDeclarationSource(sourcePath, source).output.includes("MMLTK_MAX_BYTES(8)"));
});

test("named bounds retain relative lookup while only proven global bounds use a shorthand", () => {
  const source = "[[= ::mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string value;\n";
  const result = formatDeclarationSource(sourcePath, source);
  assert.ok(result.output.includes("MMLTK_MAX_BYTES(mmltk::frameworks::reflection::kMaximumPathBytes)"));
  assert.equal(formatDeclarationSource(sourcePath, result.output).changed, false);
  const global = source.replace("{mmltk", "{::mmltk");
  assert.ok(formatDeclarationSource(sourcePath, global).output.includes("MMLTK_MAX_PATH_BYTES std::string value;"));
});

test("macro definitions and undefinitions block automatic rewrites across inventory", () => {
  const definition = "#define MMLTK_MAX_BYTES(...) other(__VA_ARGS__)\n";
  assert.deepEqual(conflictingMacros("src/conflict.h", definition), ["MMLTK_MAX_BYTES"]);
  assert.deepEqual(conflictingMacros(ANNOTATION_HEADER, definition), []);
  const annotation = "[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] std::string value;\n";
  for (const conflict of [definition, "#undef MMLTK_MAX_BYTES\n"]) {
    const report = declarationFormatReport(["src/conflict.h", sourcePath], new Map([["src/conflict.h", conflict], [sourcePath, annotation]]));
    assert.equal(report.summary.rewrite, 0);
    assert.deepEqual(report.macro_conflicts, ["MMLTK_MAX_BYTES"]);
    assert.match(report.files.find((file) => file.path === sourcePath).candidates[0].reason, /conflicting/u);
  }
});

test("direct annotation includes are unconditional and preserve CRLF", () => {
  const source = '#pragma once\r\n#if ENABLE\r\n#include "' + ANNOTATION_HEADER + '"\r\n#endif\r\n' +
    "[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] std::string value;\r\n";
  const result = formatDeclarationSource(sourcePath, source);
  assert.ok(result.output.startsWith(`#pragma once\r\n#include "${ANNOTATION_HEADER}"\r\n#if ENABLE`));
  assert.equal(formatDeclarationSource(sourcePath, result.output).changed, false);
  assert.equal(formatDeclarationSource("src/fixture.cppm", source).changed, false);
  assert.equal(formatDeclarationSource("src/fixture.cppm.in", source).changed, false);
  assert.equal(formatDeclarationSource("src/fixture.cppm.in", "MMLTK_MAX_BYTES(8) std::string value;\n").changed, false);
});

test("declaration categories do not depend on namespace depth", () => {
  const result = classifyDeclarations(sourcePath, canonical + "\n[[nodiscard]] __device__ __forceinline__ float convert(int n) { return static_cast<float>(n); }\n" +
    "template<class T> void declared(T value);\nauto x = option<Root, member_path<&Root::inner, &Inner::value>>(name, help);\n");
  const categories = new Set(result.candidates.flatMap((item) => item.categories));
  for (const kind of ["reflection_annotation", "typed_limit", "adjacent_annotation_composition", "registration_macro", "defaulted_equality", "optional_declaration", "collection_declaration", "attribute_qualifier", "cast", "nested_descriptor", "template_declaration"]) assert.ok(categories.has(kind), kind);
});

test("shared tokenizer retains literal and comment source ranges without interpreting their contents", () => {
  const source = 'R"x([[= fake{}]] /* literal */)x" /* comment */ [[= actual{}]]';
  const tokens = tokenizeCpp(source, { trivia: true });
  for (const token of tokens) assert.equal(source.slice(token.offset, token.end), token.text);
  assert.equal(tokens.filter((token) => token.kind === "literal").length, 1);
  assert.equal(tokens.filter((token) => token.kind === "comment").length, 1);
});

test("raw short CPD uses existing XML parser and retains exact evidence separately from context", () => {
  const source = "// CLEANUP-IGNORE: still raw\n[[= p::MaxBytes{8}]] std::string a;\n[[= p::MaxBytes{8}]] std::string b;\n";
  const parsed = parseCpdXml('<pmd-cpd><duplication lines="1" tokens="12"><file line="2" endline="2" column="1" endcolumn="20" path="src/fixture.h"/><file line="3" endline="3" column="1" endcolumn="20" path="src/fixture.h"/></duplication></pmd-cpd>');
  const selection = parseArgs(["--raw-cpd", "--min-tokens", "12"]);
  const report = rawCpdReport(selection.profile, { scannedFiles: [sourcePath] }, { duplications: parsed, status: 0, stderr: "" }, () => source);
  assert.equal(report.raw_matches.length, 1);
  assert.equal(report.raw_matches[0].occurrences.length, 2);
  assert.equal(report.raw_matches[0].occurrences[0].text, source.split("\n")[1].slice(0, 19));
  assert.equal(report.detector.inline_suppressions, false);
  assert.equal(report.detector.sequence_skipping, false);
  assert.ok(report.classified_context.every((item) => item.disposition !== "rewrite"));
  const options = detectorArguments(selection.profile).cpd;
  for (const flag of ["--ignore-identifiers", "--ignore-literal-sequences", "--skip-sequences", "--ignore-literals"]) assert.ok(!options.includes(flag));
  assert.equal(options[options.indexOf("--skip-blocks-pattern") + 1], "");
  assert.equal(CLEANUP_PROFILES.cpp.cpd.minTokens, 39);
  assert.throws(() => rawCpdReport(selection.profile, { scannedFiles: [sourcePath] }, { duplications: parsed, status: 5, stderr: "cannot lex file" }), /status 5/u);
});

test("raw evidence ordering is deterministic without inventing or coalescing matches", () => {
  const source = "one();\ntwo();\n";
  const selection = parseArgs(["--raw-cpd"]);
  const occurrence = (start) => ({ path: sourcePath, start, end: start });
  const matches = [{ tokenCount: 12, lineCount: 1, occurrences: [occurrence(2), occurrence(1)] },
    { tokenCount: 13, lineCount: 1, occurrences: [occurrence(1), occurrence(2)] }];
  const report = (duplications) => rawCpdReport(selection.profile, { scannedFiles: [sourcePath] }, { duplications, status: 0, stderr: "" }, () => source);
  assert.deepEqual(report(matches), report([...matches].reverse().map((match) => ({ ...match, occurrences: [...match.occurrences].reverse() }))));
  assert.equal(report(matches).raw_matches.length, 2);
});

test("multiline source evidence uses exclusive CPD end columns and exact offsets", () => {
  const source = "before\n [[= x{\n  4}]]\nafter\n";
  const result = sourceOccurrence(sourcePath, source, { start: 2, end: 3, column: 2, endColumn: 7 }, lineStarts(source));
  assert.equal(result.text, "[[= x{\n  4}]]");
});

test("formatter inventory reuses CPD exclusions and never accepts paths outside it", () => {
  const files = buildInventory(CLEANUP_PROFILES.cpp, { tracked: ["src/a.h", "third_party/vendor.h"], untracked: ["src/b.cpp"], deleted: [] }, () => true);
  assert.deepEqual(files.scannedFiles, ["src/a.h", "src/b.cpp"]);
  const sources = new Map(files.scannedFiles.map((path) => [path, ""]));
  assert.throws(() => declarationFormatReport(files.scannedFiles, sources, { files: ["../outside.h"] }), /inventory/u);
  const options = parseFormatArgs(["preview", "--file", "src/a.h", "--report", "cleanup/preview.json"]);
  assert.equal(options.mode, "preview");
  assert.deepEqual(options.files, ["src/a.h"]);
  for (const args of [[], ["invalid"], ["fix", "--file"], ["check", "--report", "a", "--report", "b"]]) assert.throws(() => parseFormatArgs(args));
});

test("formatter reports deterministic original edits in both repeated previews", () => {
  const inventory = [sourcePath], sources = new Map([[sourcePath, canonical]]);
  const report = declarationFormatReport(inventory, sources);
  assert.deepEqual(report, declarationFormatReport(inventory, sources));
  assert.deepEqual(report.changed_files, [sourcePath]);
  assert.equal(report.files[0].before, canonical);
  const fixedSources = new Map([[sourcePath, report.files[0].after]]);
  assert.deepEqual(declarationFormatReport(inventory, fixedSources).changed_files, []);
});


test("real training declarations retain shared policy structure and every distinct member", () => {
  const path = "src/backend/models/rfdetr/contract/training_metrics.h";
  const source = readFileSync(new URL(`../../${path}`, import.meta.url), "utf8");
  const candidates = classifyDeclarations(path, source).candidates;
  const expected = {
    TrainingSourceCatalog: { available: "MaxItems" },
    TrainingSources: { observations: "MaxItems", failures: "MaxItems", distributions: "MaxItems" },
    TrainingRun: { run_id: "MaxBytes", attempt_id: "MaxBytes", checkpoint_attempt_id: "MaxBytes",
      source_checkpoint_attempt_id: "MaxBytes", original_weights: "MaxBytes", original_class_descriptor: "MaxBytes" },
  };
  for (const [owner, members] of Object.entries(expected)) for (const [member, policy] of Object.entries(members)) {
    const matches = candidates.filter((candidate) => candidate.context.owner === owner && candidate.context.member === member && candidate.policy === policy);
    assert.equal(matches.length, 1, `${owner}.${member}`);
    assert.ok(matches[0].categories.includes("typed_limit"));
    assert.ok(matches[0].text.startsWith("MMLTK_"));
  }
  assert.ok(candidates.some((candidate) => candidate.context.owner === "TrainingSources" && candidate.text.includes("kMaximumTrainingModels + 1U")));
  assert.equal(formatDeclarationSource(path, source).changed, false);
});

test("CTAD bounds preserve their original construction and deduced policy type", () => {
  const source = "[[= ::mmltk::frameworks::reflection::Minimum{std::uint64_t{1U}}]] std::uint64_t identity = 0;\n";
  const result = formatDeclarationSource(sourcePath, source);
  assert.ok(result.output.includes("MMLTK_MINIMUM_VALUE(std::uint64_t{1U}) std::uint64_t identity = 0;"));
  assert.equal(formatDeclarationSource(sourcePath, result.output).changed, false);
});


test("preprocessing after comments and in continued definitions stays byte-identical", () => {
  const source = "/* lead */ #define EXAMPLE \\\n [[= ::mmltk::frameworks::reflection::MaxBytes{8}]]\n" +
    "/* multi\n line */ #if ENABLE\n[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] std::string value;\n#endif\n";
  assert.equal(formatDeclarationSource(sourcePath, source).output, source);
});


test("already shortened authoring acquires its direct dependency exactly once", () => {
  const source = "#pragma once\nMMLTK_MAX_BYTES(4) std::string value;\n";
  const result = formatDeclarationSource(sourcePath, source);
  assert.equal(result.edits.length, 1);
  assert.ok(result.output.includes(`#include "${ANNOTATION_HEADER}"`));
  assert.ok(result.output.endsWith("MMLTK_MAX_BYTES(4) std::string value;\n"));
  assert.equal(formatDeclarationSource(sourcePath, result.output).changed, false);
});


test("preprocessed or conditional aliases never authorize an outside rewrite", () => {
  const declaration = "[[= policy::MaxBytes{8}]] std::string value;\n";
  for (const prefix of ["#define ALIAS namespace policy = mmltk::frameworks::reflection;\n",
    "#if ENABLE\nnamespace policy = mmltk::frameworks::reflection;\n#endif\n"]) {
    assert.equal(formatDeclarationSource(sourcePath, prefix + declaration).changed, false);
  }
});
