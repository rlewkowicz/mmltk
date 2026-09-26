import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join, relative, resolve } from "node:path";
import test from "node:test";
import { AUTHORED_PROFILE, CLEANUP_PROFILES, authoredLanguage, buildInventory, generateRawCpd, generateReport,
  parseArgs, parseCpdXml, parseInlineSuppressions, rawCpdReport, requireAtomicSuppressionUse, runCpd } from "../generate_cleanup_json.mjs";
import { classifyDeclarations, sourceOccurrence } from "../cleanup/declaration_patterns.mjs";
import { SourceIndex } from "../cleanup/cpd_patterns.mjs";
import { buildReview, physicalLines, renderReviewMarkdown } from "../cleanup/review_patterns.mjs";

const rawProfile = () => parseArgs(["--raw-cpd"]).profile;
const inventory = (tracked) => ({ tracked, untracked: [], deleted: [] });
const reviewOf = (files, matches = []) => {
  const sources = new Map(files.map(({ path, source }) => [path, source]));
  const native = files.filter(({ language }) => language === "cpp").map(({ path }) => path);
  const raw = rawCpdReport(rawProfile(), { scannedFiles: native }, { duplications: matches, status: 0, stderr: "" }, (path) => sources.get(path));
  return { raw, review: buildReview(files, raw) };
};

test("review is an opt-in raw operation with stable default and selected basenames", () => {
  assert.equal(parseArgs(["--raw-cpd"]).output, "cleanup/declarations-raw.json");
  assert.equal(parseArgs(["--raw-cpd", "--review"]).output, "cleanup/declarations-review.json");
  const selection = parseArgs(["--raw-cpd", "--output", "chosen.json", "--review", "--min-tokens", "27"]);
  assert.equal(selection.output, "chosen.json");
  assert.equal(selection.profile.cpd.minTokens, 27);
  assert.equal(selection.review, true);
  for (const args of [["--review"], ["--raw-cpd", "--review", "--review"], ["--raw-cpd", "--output"], ["--raw-cpd", "--min-tokens", "--review"]]) assert.throws(() => parseArgs(args));
});

test("authored inventory covers code languages and preserves Git boundaries", () => {
  const retained = ["src/a.cpp", "src/template.cppm.in", "src/app.rs", "tools/check.py", "tools/cleanup/patterns.mjs", "src/output/value.h", "web/a.tsx", "web/b.mjs", "web/style.css", "web/index.html", "cmake/A.cmake", "CMakeLists.txt", "docker/Dockerfile.dependencies", "mmltk", "tools/run.sh", "src/frontend/iced/src/generated.rs"];
  const excluded = ["third_party/a.cpp", "src/third_party/a.rs", "build/a.cpp", ".cache/a.py", "web/node_modules/a.ts", "src/generated/a.h", "compiled/a.h", "output/a.py"];
  const files = buildInventory(AUTHORED_PROFILE, { tracked: [...retained, ...excluded, "README.md", "model.onnx", "src/deleted.cpp"], untracked: ["tools/new.js"], deleted: ["src/deleted.cpp"] }, () => true);
  assert.deepEqual(files.scannedFiles, [...retained, "tools/new.js"].sort());
  assert.equal(files.excludedByPrefix.length + files.excludedGeneratedOutput.length, excluded.length);
  assert.deepEqual(files.deletedFiles, ["src/deleted.cpp"]);
  assert.equal(authoredLanguage("README.md"), null);
  assert.throws(() => buildInventory(AUTHORED_PROFILE, inventory(["src/missing.rs"]), () => false), /unavailable/u);
});

test("size triage is strictly over 500 physical lines and does not lex non-C++ files", () => {
  const files = [499, 500, 501].map((lines) => ({ path: `tools/${lines}.py`, language: "python", source: "# source\n".repeat(lines) }));
  files.push({ path: "src/tests/embedded.rs", language: "rust", source: "#[cfg(test)] mod tests {}\n".repeat(501) });
  const { review } = reviewOf(files);
  assert.deepEqual(review.queue, ["src/tests/embedded.rs", "tools/501.py"]);
  assert.deepEqual(review.families, []);
  assert.equal(review.inventory[0].test_evidence.dedicated, true);
  assert.equal(review.inventory[0].test_evidence.embedded, true);
  assert.equal(physicalLines(""), 0);
  assert.equal(physicalLines("\n"), 1);
  assert.equal(physicalLines("a\r\nb"), 2);
  assert.equal(physicalLines("line\n".repeat(500) + "last"), 501);
});

test("complete nested CLI bindings and exclusions retain each exceptional argument", () => {
  const expressions = [
    'option<Root, member_path<&Root::inner, &Inner::value>>("--value", "Help", "Group", {}, "--no-value")',
    'custom_option<Root, member_path<&Root::inner, &Inner::items>, Codec<std::pair<int, int>>>("--ids", make_help(1, nested(2, 3)), "Execution")',
    'negative_flag<Root, &Root::enabled>("--disabled", "Negative polarity", "Execution")',
    'option_with_item_policy<Root, &Root::items, Policy<Limit{4}>>("--items", "Bounded", "Data")',
    'unexposed<Root, member_path<&Root::inner, &Inner::value>>("owned by JSON")',
    'MMLTK_CLI_OPTION(Scope, value, "Help", "Group")',
    'MMLTK_CLI_NAMED(Scope, items, "--item-list", "Help", "Data", {}, "--no-items")',
  ];
  const source = "constexpr std::array kOptions{\n" + expressions.join(",\n") + "\n};\n";
  const classified = classifyDeclarations("src/options.h", source).candidates.filter(({ categories }) => categories.includes("nested_descriptor") || categories.includes("descriptor_exclusion"));
  assert.deepEqual(classified.map(({ text }) => text), expressions);
  assert.ok(classified.every(({ complete }) => complete));
  assert.ok(classified.every(({ context }) => context.owner === "kOptions"));
  assert.equal(classifyDeclarations("src/options.h", "option<Root, &Root::broken>(\n").candidates.find(({ categories }) => categories.includes("nested_descriptor")).complete, false);
});

test("indexed families retain invariant and varying slots, cross-file anchors and unique raw associations", () => {
  const path = "src/options.h";
  const source = ['option<Root, &Root::first>("--first", "One", "Input");', 'option<Root, &Root::second>("--second", "Two", "Input");', 'option<Root, &Root::third>("--third", "Three", "Input");'].join("\n");
  const files = [{ path, language: "cpp", source }, { path: "src/more.h", language: "cpp", source: 'option<Other, &Other::fourth>("--fourth", "Four", "Output");' }];
  const match = { tokenCount: 12, lineCount: 1, occurrences: [1, 2].map((line) => ({ path, start: line, end: line, column: 1, endColumn: 10 })) };
  const first = reviewOf(files, [match, match]);
  const family = first.review.families.find(({ kind }) => kind === "cli_binding");
  assert.equal(family.occurrence_count, 4);
  assert.equal(family.file_count, 2);
  assert.equal(family.covered_lines, 4);
  assert.equal(family.status, "unreviewed");
  assert.ok(family.invariant_tokens.includes("option"));
  assert.ok(family.occurrences.some(({ varying_values }) => varying_values.includes('"Output"')));
  assert.deepEqual(family.occurrences.filter(({ path: value }) => value === path)[0].raw_match_ids, [1, 2]);
  assert.equal(first.raw.raw_matches.length, 2);
  assert.deepEqual(first.review, reviewOf([...files].reverse(), [match, match]).review);
  const markdown = renderReviewMarkdown(first.review);
  assert.match(markdown, /unreviewed/u);
  assert.match(markdown, /src\/options.h:1:1/u);
});

test("trait and same-name mapping families retain marker, conversion and field differences", () => {
  const source = ["Minimum", "Maximum", "MaxBytes"].map((name, index) => `template<class A> struct ${name}Annotation : std::bool_constant<A::marker_${index} && std::is_convertible_v<decltype(A::value), ${["long double", "unsigned long", "signed short"][index]}>> {};`).join("\n") + "\n" +
    ["alpha", "beta", "gamma"].map((name) => `destination.${name} = source.${name};\nfield("${name}", settings.${name});`).join("\n");
  const { review } = reviewOf([{ path: "src/traits.h", source, language: "cpp" }]);
  // Use equal-shape primary traits to complement differing conversion shapes.
  const primary = ["Minimum", "Maximum", "MaxBytes"].map((name) => `template<class A, class = void> struct ${name}Annotation : std::false_type {};`).join("\n");
  const traits = reviewOf([{ path: "src/primary.h", source: primary, language: "cpp" }]).review;
  assert.equal(traits.families.find(({ kind }) => kind === "trait_declaration").occurrence_count, 3);
  const specialization = review.families.find(({ kind }) => kind === "trait_declaration");
  assert.equal(specialization.occurrence_count, 3);
  assert.ok(specialization.occurrences[0].varying_values.includes("marker_0"));
  assert.ok(specialization.occurrences[0].varying_values.includes("double"));
  assert.equal(review.families.filter(({ kind }) => kind === "same_name_mapping").length, 2);
  assert.equal(review.summary.covered_lines, 9);
});

test("overlapping qualified names and complete bindings count physical evidence lines once", () => {
  const source = ["first", "second", "third"].map((member) => `option<mmltk::reflection::Root, &mmltk::reflection::Root::${member}>("--${member}", "Help", "Group");`).join("\n");
  const { review } = reviewOf([{ path: "src/overlap.h", language: "cpp", source }]);
  assert.ok(review.families.some(({ kind }) => kind === "cli_binding"));
  assert.ok(review.families.some(({ kind }) => kind === "qualification"));
  assert.equal(review.summary.covered_lines, 3);
  assert.ok(review.families.reduce((count, family) => count + family.covered_lines, 0) > 3);
});

test("raw classification shares whole-inventory conflicts and the default raw shape", () => {
  const { raw } = reviewOf([{ path: "src/a.h", language: "cpp", source: "[[= ::mmltk::frameworks::reflection::MaxBytes{8}]] int value;" },
    { path: "src/b.h", language: "cpp", source: "#define MMLTK_MAX_BYTES(x) unsafe(x)\n" }]);
  assert.match(raw.classified_context.find(({ categories }) => categories.includes("reflection_annotation")).reason, /conflicting/u);
  assert.deepEqual(Object.keys(raw), ["format", "purpose", "inventory", "detector", "raw_matches", "classified_context", "summary"]);
});

test("CPD completion rejects malformed records, unrelated documents and missing coverage", () => {
  for (const xml of ["", "garbage", "<unrelated/>", "<pmd-cpd>", "<pmd-cpd><duplication></pmd-cpd>",
    '<pmd-cpd><duplication lines="1" tokens="2"><file line="1" endline="1" path="a"/></duplication></pmd-cpd>',
    '<pmd-cpd><file path="a" totalNumberOfTokens="-1"/></pmd-cpd>',
    '<pmd-cpd><file path="a" totalNumberOfTokens="0"/><file path="a" totalNumberOfTokens="0"/></pmd-cpd>',
    '<pmd-cpd><processing-error/></pmd-cpd>', '<pmd-cpd><codefragment><![CDATA[unterminated</codefragment></pmd-cpd>',
    '<pmd-cpd><file path="a" garbage totalNumberOfTokens="1"/></pmd-cpd>']) assert.throws(() => parseCpdXml(xml));
  assert.throws(() => parseCpdXml("<pmd-cpd/>", { expectedPaths: ["a"] }), /coverage/u);
  assert.deepEqual(parseCpdXml('<pmd-cpd><file path="empty.cpp" totalNumberOfTokens="0"/></pmd-cpd>', { expectedPaths: ["empty.cpp"] }), []);
  assert.throws(() => parseCpdXml('<pmd-cpd><file path="outside.cpp" totalNumberOfTokens="0"/></pmd-cpd>', { expectedPaths: ["inside.cpp"] }), /coverage/u);
  const files = '<file path="a" line="1" endline="1"/><file path="b" line="1" endline="1"/>';
  assert.equal(parseCpdXml(`<pmd-cpd><duplication lines="1" tokens="2">${files}<codefragment><![CDATA[<error> is source & text]]></codefragment></duplication></pmd-cpd>`).length, 1);
});

test("coordinates validate each original line, CRLF, Unicode and the final physical line", () => {
  const source = "ab\r\n😀xy\r\nlast";
  assert.equal(sourceOccurrence("file.cpp", source, { start: 2, end: 2, column: 1, endColumn: 5 }).text, "😀xy");
  assert.equal(sourceOccurrence("file.cpp", source, { start: 3, end: 3 }).text, "last");
  for (const occurrence of [{ start: 0, end: 1 }, { start: 1, end: 4 }, { start: 1, end: 2, column: 4 },
    { start: 1, end: 1, endColumn: 4 }, { start: 2, end: 2, column: 2 }, { start: 2, end: 2, endColumn: 2 }, { start: 3, end: 3, endColumn: 6 }]) {
    assert.throws(() => sourceOccurrence("file.cpp", source, occurrence), /span/u);
    assert.throws(() => new SourceIndex(source).range(occurrence), /span/u);
  }
  assert.throws(() => sourceOccurrence("file.cpp", "a\n", { start: 2, end: 2 }), /span/u);
});

test("suppression accounting admits actual comments and ignores C++ and Rust literal contents", () => {
  const cpp = 'const char* a = "// CLEANUP-IGNORE: not a comment";\nconst char* b = R"tag(\n// CLEANUP-OFF: literal\n// CLEANUP-ON\n)tag";\n// CLEANUP-IGNORE: inspected occurrence\nwork();\n';
  assert.equal(parseInlineSuppressions("a.cpp", cpp).get("a.cpp").length, 1);
  const rust = 'let a = r###"\n// CLEANUP-OFF: literal\n// CLEANUP-ON\n"###;\nlet b = br#"// CLEANUP-IGNORE: bytes"#;\nlet c = cr"// CLEANUP-IGNORE: c string";\n/* nested /* // CLEANUP-OFF: comment content */ // CLEANUP-ON */\nfn borrow<\'a>(x: &\'a str) {}\n// CLEANUP-IGNORE: inspected occurrence\nwork();\n';
  assert.equal(parseInlineSuppressions("a.rs", rust).get("a.rs").length, 1);
  const native = parseInlineSuppressions("a.cpp", "// CPD-OFF: inspected single occurrence\none();\n// CPD-ON\n").get("a.cpp");
  assert.deepEqual(native[0].detectors, ["cpd"]);
  assert.throws(() => requireAtomicSuppressionUse([1, 2].map((start) => ({ detector: "cpd", path: "a.cpp", marker_line: 1, lines: [start, 3] }))), /multiple local/u);
});

function detectorFixture(context) {
  const directory = mkdtempSync(join(process.cwd(), ".cleanup-evidence-fixture-"));
  context.after(() => rmSync(directory, { recursive: true, force: true }));
  const path = relative(process.cwd(), join(directory, "input.cpp"));
  writeFileSync(path, "int value = 1;\n");
  const binary = join(directory, "pmd.mjs"), output = join(directory, "selected.json"), markdown = join(directory, "selected.md");
  const profile = rawProfile();
  profile.cpd.binary = binary;
  const configure = ({ status = 0, xml, stderr = "", change = false, coveragePath, duplicationPath } = {}) => writeFileSync(binary,
    `#!/usr/bin/env node
import { readFileSync, writeFileSync } from 'node:fs';
const paths = readFileSync(process.argv[process.argv.indexOf('--file-list') + 1], 'utf8').trim().split('\\n');
const coverage = ${coveragePath === undefined ? "paths" : JSON.stringify([coveragePath])}.map(path => '<file path="' + path + '" totalNumberOfTokens="5"/>').join('');
const duplicatePath = ${JSON.stringify(duplicationPath ?? null)};
const duplication = duplicatePath === null ? '' : '<duplication lines="1" tokens="2"><file path="' + duplicatePath + '" line="1" endline="1" column="1" endcolumn="4"/><file path="' + paths[0] + '" line="1" endline="1" column="5" endcolumn="10"/></duplication>';
${change ? `writeFileSync(${JSON.stringify(path)}, 'int changed = 2;\\n');` : ""}
process.stdout.write(${xml === undefined ? "'<pmd-cpd>' + coverage + duplication + '</pmd-cpd>'" : JSON.stringify(xml)});
process.stderr.write(${JSON.stringify(stderr)});
process.exitCode = ${status};
`, { mode: 0o755 });
  return { directory, path, profile, output, markdown, configure };
}

function unlistedDetectorPaths(path) {
  return [resolve(path), resolve(`${path}.unlisted`)].flatMap((value) => [{ coveragePath: value }, { duplicationPath: value }]);
}

test("review outputs are prepared before publication and prior outputs survive detector, source and rendering failures", async (context) => {
  const fixture = detectorFixture(context), { path, profile, output, markdown, configure } = fixture;
  const options = { review: true, inventoryInputs: inventory([path]) };
  for (const failure of [{ status: 4 }, { status: 5 }, { stderr: "scanner ERROR" }, { stderr: "LexerException" }, { xml: "<pmd-cpd>" }, { xml: "<pmd-cpd/>" }, { change: true }, ...unlistedDetectorPaths(path)]) {
    writeFileSync(output, "previous json"); writeFileSync(markdown, "previous markdown");
    configure(failure);
    await assert.rejects(generateRawCpd(profile, output, options));
    assert.equal(readFileSync(output, "utf8"), "previous json");
    assert.equal(readFileSync(markdown, "utf8"), "previous markdown");
  }
  configure();
  await assert.rejects(generateRawCpd(profile, output, { ...options, renderMarkdown: () => { throw new Error("render failed"); } }), /render failed/u);
  assert.equal(readFileSync(output, "utf8"), "previous json");
  assert.equal(readFileSync(markdown, "utf8"), "previous markdown");
  writeFileSync(path, ["first", "second", "third"].map((name) => `option<Root, &Root::${name}>("--${name}", "Help");`).join("\n"));
  await generateRawCpd(profile, output, options);
  assert.equal(JSON.parse(readFileSync(output, "utf8")).review.inventory[0].sha256.length, 64);
  assert.equal(JSON.parse(readFileSync(output, "utf8")).review.families[0].occurrence_count, 3);
  assert.match(readFileSync(markdown, "utf8"), /unreviewed/u);
});

test("detector copies cover module, CUDA and template suffixes without basename collisions", async (context) => {
  const { directory } = detectorFixture(context);
  const paths = ["a/same.cppm", "b/same.cppm", "kernel.cu", "detail.cuh", "template.cppm.in", "header.hpp.in"].map((name) => {
    const path = join(directory, name);
    mkdirSync(join(path, ".."), { recursive: true });
    writeFileSync(path, "int repeated(int value) { return value * value + value + 1; }\n");
    return path;
  });
  const result = await runCpd(rawProfile(), paths);
  assert.equal(result.excludedLexerCount, 0);
  assert.deepEqual([...new Set(result.duplications.flatMap(({ occurrences }) => occurrences.map(({ path }) => path)))].sort(), [...paths].sort());
  for (const match of result.duplications) for (const occurrence of match.occurrences) {
    assert.match(sourceOccurrence(occurrence.path, readFileSync(occurrence.path, "utf8"), occurrence).text, /repeated/u);
  }
});

test("ordinary and raw detector modes share completion and source coverage checks", async (context) => {
  const { path, profile, configure } = detectorFixture(context);
  const normal = { ...CLEANUP_PROFILES.cpp, cpd: { ...CLEANUP_PROFILES.cpp.cpd, binary: profile.cpd.binary } };
  for (const failure of [{ status: 4 }, { status: 5 }, { xml: "<unrelated/>" }, { xml: "<pmd-cpd/>" }, { stderr: "source ERROR" }]) {
    configure(failure);
    for (const selected of [normal, profile]) await assert.rejects(runCpd(selected, [path]));
  }
  for (const failure of unlistedDetectorPaths(path)) {
    configure(failure);
    for (const selected of [normal, profile]) await assert.rejects(runCpd(selected, [path]), /unlisted detector input/u);
  }
});

test("failed ordinary completion preserves both prior reports", async (context) => {
  const { directory, path, profile, configure } = detectorFixture(context);
  const duplo = join(directory, "duplo.mjs");
  writeFileSync(duplo, "#!/usr/bin/env node\nprocess.stdout.write('[]');\n", { mode: 0o755 });
  const output = relative(process.cwd(), join(directory, "ordinary.json")), rejected = join(directory, "rejected.json");
  writeFileSync(output, "previous ordinary"); writeFileSync(rejected, '{"previous":"rejected"}');
  for (const failure of [{ xml: "<pmd-cpd/>" }, ...unlistedDetectorPaths(path)]) {
    configure(failure);
    await assert.rejects(generateReport({ ...CLEANUP_PROFILES.cpp, output, cpd: profile.cpd, duplo: { ...CLEANUP_PROFILES.cpp.duplo, binary: duplo } }, { inventoryInputs: inventory([path]) }));
    assert.equal(readFileSync(output, "utf8"), "previous ordinary");
    assert.equal(readFileSync(rejected, "utf8"), '{"previous":"rejected"}');
  }
});
