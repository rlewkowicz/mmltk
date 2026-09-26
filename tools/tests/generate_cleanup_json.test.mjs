import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";

import {
  CLEANUP_PROFILES,
  INVENTORY_COMMANDS,
  applyInlineSuppressions,
  buildInventory,
  buildReport,
  compactDuploHits,
  compareHits,
  detectorArguments,
  filterDuploCandidates,
  generateRawCpd,
  parseArgs,
  parseCpdXml,
  parseDuploJson,
  parseInlineSuppressions,
  rejectionReport,
  runCpd,
  serializeReport,
  structuralHits,
} from "../generate_cleanup_json.mjs";
import { filterCpdCandidates } from "../cleanup/cpd_patterns.mjs";
import { sourceOccurrence } from "../cleanup/declaration_patterns.mjs";

const cpp = CLEANUP_PROFILES.cpp;
const frontend = CLEANUP_PROFILES.frontend;

test("raw CPD includes real PMD-suppressed code with original paths, columns, literals and CRLF", async (context) => {
  const directory = mkdtempSync(join(tmpdir(), "mmltk-cpd-source-"));
  context.after(() => rmSync(directory, { recursive: true, force: true }));
  const body = 'int repeated(int value) {\r\n  const char* marker = "CPD-OFF";\r\n  int result = value * value + 3;\r\n  return result + value + marker[0];\r\n}\r\n';
  const paths = [join(directory, "suppressed.cpp"), join(directory, "ordinary.cpp")];
  const sources = ["// CPD-OFF\r\n" + body + "// CPD-ON\r\n", "// ordinary source\r\n" + body];
  paths.forEach((path, index) => writeFileSync(path, sources[index]));
  const raw = parseArgs(["--raw-cpd"]).profile;
  const normal = { ...cpp, cpd: { ...cpp.cpd, minTokens: 12, flags: [] } };
  assert.ok((await runCpd(normal, paths)).duplications.length > 0, "ordinary evidence also exposes native suppression ranges");
  const result = await runCpd(raw, paths);
  const match = result.duplications.find((candidate) => candidate.occurrences.length === 2 && candidate.tokenCount >= 30);
  assert.ok(match, "unsuppressed complete function is present in raw evidence");
  assert.deepEqual(match.occurrences.map(({ path }) => path).sort(), [...paths].sort());
  for (const occurrence of match.occurrences) {
    const source = sources[paths.indexOf(occurrence.path)];
    const evidence = sourceOccurrence(occurrence.path, source, occurrence);
    assert.equal(occurrence.start, 2);
    assert.equal(occurrence.column, 1);
    assert.ok(evidence.text.startsWith("int repeated("));
    assert.ok(evidence.text.includes('"CPD-OFF"'));
    assert.ok(evidence.text.includes("\r\n"));
    assert.equal(source.slice(evidence.start_offset, evidence.end_offset), evidence.text);
    assert.equal(readFileSync(occurrence.path, "utf8"), source);
  }
  assert.equal(existsSync(dirname(result.arguments[result.arguments.indexOf("--file-list") + 1])), false);
  const invalid = join(directory, "invalid.cpp");
  writeFileSync(invalid, 'int invalid() { return "unterminated; }\n');
  await assert.rejects(runCpd(raw, [invalid]), /status 5|could not lex|incomplete raw source coverage/u);
});

test("raw CPD rejects partial detector runs without replacing a complete report and removes scan copies", async (context) => {
  const directory = mkdtempSync(join(tmpdir(), "mmltk-cpd-failure-"));
  context.after(() => rmSync(directory, { recursive: true, force: true }));
  const binary = join(directory, "pmd-fixture.mjs"), capture = join(directory, "arguments.json");
  const output = join(directory, "report.json"), original = '{"previous":"complete"}\n';
  writeFileSync(output, original);
  for (const result of [
    { status: 5, stdout: "<pmd-cpd/>", stderr: "Cannot lex src/broken.cpp: unterminated literal" },
    { status: 0, stdout: '<pmd-cpd><error filename="src/broken.cpp" msg="lexical failure"/></pmd-cpd>', stderr: "" },
    { status: 0, stdout: "<pmd-cpd/>", stderr: "source scanner: ERROR in src/broken.cpp" },
    { status: 0, stdout: "<pmd-cpd/>", stderr: "", complete: true },
  ]) {
    writeFileSync(binary, `#!/usr/bin/env node\nimport { readFileSync, writeFileSync } from 'node:fs';\nwriteFileSync(${JSON.stringify(capture)}, JSON.stringify(process.argv));\nconst paths = readFileSync(process.argv[process.argv.indexOf('--file-list') + 1], 'utf8').trim().split('\\n');\nconst coverage = paths.map(path => '<file path="' + path + '" totalNumberOfTokens="1"/>').join('');\nprocess.stdout.write(${result.complete ? "'<pmd-cpd>' + coverage + '</pmd-cpd>'" : JSON.stringify(result.stdout)});\nprocess.stderr.write(${JSON.stringify(result.stderr)});\nprocess.exitCode = ${result.status};\n`, { mode: 0o755 });
    const profile = parseArgs(["--raw-cpd"]).profile;
    profile.cpd.binary = binary;
    if (result.complete) await runCpd(profile, ["src/frameworks/reflection/declaration_annotations.h"]);
    else await assert.rejects(generateRawCpd(profile, output, { inventoryInputs: { tracked: ["src/frameworks/reflection/declaration_annotations.h"], untracked: [], deleted: [] } }), /src\/broken\.cpp/u);
    assert.equal(readFileSync(output, "utf8"), original);
    const args = JSON.parse(readFileSync(capture, "utf8"));
    assert.equal(existsSync(dirname(args[args.indexOf("--file-list") + 1])), false);
  }
});

test("empty raw CPD inventories complete without launching a detector", async () => {
  const profile = parseArgs(["--raw-cpd"]).profile;
  profile.cpd.binary = "/nonexistent/pmd";
  const result = await runCpd(profile, []);
  assert.equal(result.status, 0);
  assert.deepEqual(result.duplications, []);
});

test("profiles and arguments select exactly one frozen configuration", () => {
  assert.equal(parseArgs([]).profile, cpp);
  assert.equal(parseArgs(["--cpp"]).profile, cpp);
  assert.equal(parseArgs(["--frontend"]).profile, frontend);
  assert.equal(parseArgs(["--help"]).kind, "help");
  assert.ok(Object.isFrozen(CLEANUP_PROFILES));
  assert.ok(Object.isFrozen(frontend.cpd));
  for (const args of [
    ["--cpp", "--frontend"],
    ["--cpp", "--cpp"],
    ["frontend"],
    ["--unknown"],
  ]) {
    assert.throws(() => parseArgs(args));
  }
});

test("inventory commands are exact and frozen", () => {
  assert.deepEqual(INVENTORY_COMMANDS.tracked, ["git", "ls-files", "-z"]);
  assert.deepEqual(INVENTORY_COMMANDS.untracked, [
    "git",
    "ls-files",
    "--others",
    "--exclude-standard",
    "-z",
  ]);
  assert.deepEqual(INVENTORY_COMMANDS.deleted, [
    "git",
    "ls-files",
    "--deleted",
    "-z",
  ]);
  assert.ok(Object.isFrozen(INVENTORY_COMMANDS));
  assert.ok(Object.isFrozen(INVENTORY_COMMANDS.untracked));
});

test("detector commands exactly encode each profile", () => {
  assert.deepEqual(detectorArguments(cpp, { threads: 3, fileList: "/f" }), {
    duplo: ["-j", "3", "-ml", "6", "-ip", "-json", "-", "-"],
    cpd: [
      "cpd",
      "--file-list",
      "/f",
      "--language",
      "cpp",
      "--minimum-tokens",
      "39",
      "--ignore-identifiers",
      "--ignore-literal-sequences",
      "--format",
      "xml",
      "--no-fail-on-violation",
    ],
  });
  assert.deepEqual(
    detectorArguments(frontend, { threads: 4, fileList: "/rust" }),
    {
      duplo: ["-j", "4", "-ml", "9", "-json", "-", "-"],
      cpd: [
        "cpd",
        "--file-list",
        "/rust",
        "--language",
        "rust",
        "--minimum-tokens",
        "75",
        "--format",
        "xml",
        "--no-fail-on-violation",
      ],
    },
  );
});

test("frontend inventory unions tracked and untracked-unignored files", () => {
  const inventory = buildInventory(
    frontend,
    {
      tracked: [
        "src/frontend/iced/src/app.rs",
        "src/frontend/iced/src/deleted.rs",
        "src/frontend/iced/src/generated.rs",
        "src/frontend/iced/target/debug/generated.rs",
        "src/frontend/iced/third_party/dependency.rs",
        "src/controller/not_frontend.rs",
      ],
      untracked: [
        "src/frontend/iced/src/new.rs",
        "src/frontend/iced/generated/bindings.rs",
      ],
      // An ignored file is absent because `git ls-files --others
      // --exclude-standard` does not return it.
      deleted: ["src/frontend/iced/src/deleted.rs"],
    },
    () => true,
  );
  assert.deepEqual(inventory.scannedFiles, [
    "src/frontend/iced/src/app.rs",
    "src/frontend/iced/src/generated.rs",
    "src/frontend/iced/src/new.rs",
  ]);
  assert.deepEqual(inventory.deletedFiles, [
    "src/frontend/iced/src/deleted.rs",
  ]);
  assert.deepEqual(inventory.excludedByPrefix, [
    "src/frontend/iced/target/debug/generated.rs",
    "src/frontend/iced/third_party/dependency.rs",
  ]);
  assert.deepEqual(inventory.excludedGeneratedOutput, [
    "src/frontend/iced/generated/bindings.rs",
  ]);
  assert.equal(inventory.untrackedCandidateCount, 2);
});

test("inventory rejects unavailable and newline paths", () => {
  const input = {
    tracked: ["src/frontend/iced/src/missing.rs"],
    untracked: [],
    deleted: [],
  };
  assert.throws(() => buildInventory(frontend, input, () => false));
  assert.throws(() =>
    buildInventory(
      frontend,
      {
        tracked: ["src/frontend/iced/src/bad\nname.rs"],
        untracked: [],
        deleted: [],
      },
      () => true,
    ),
  );
});

test("Duplo and CPD fixture parsers preserve source ranges", () => {
  const duplo = parseDuploJson(
    '[{"LineCount":9,"SourceFile1":"src/frontend/iced/src/a.rs","StartLineNumber1":2,"EndLineNumber1":10,"SourceFile2":"src/frontend/iced/src/b.rs","StartLineNumber2":4,"EndLineNumber2":12,"Lines":["x"]}]',
  );
  const compacted = compactDuploHits(frontend, duplo, [
    "src/frontend/iced/src/a.rs",
    "src/frontend/iced/src/b.rs",
  ]);
  assert.equal(compacted[0].kind, "cross-file");
  assert.equal(compacted[0].line_count, 9);

  const cpd = parseCpdXml(
    '<?xml version="1.0"?><pmd-cpd><duplication lines="8" tokens="75"><file line="2" endline="9" path="/repo/src/frontend/iced/src/a.rs"/><file line="6" endline="13" path="/repo/src/frontend/iced/src/b.rs"/><codefragment><![CDATA[x]]></codefragment></duplication></pmd-cpd>',
  );
  const structural = structuralHits(
    frontend,
    cpd,
    [
      "src/frontend/iced/src/a.rs",
      "src/frontend/iced/src/b.rs",
    ],
    "/repo",
  );
  assert.equal(structural[0].token_count, 75);
  assert.deepEqual(structural[0].files[1].lines, [[6, 13]]);
});

test("shared sorting and inline suppression retain two occurrences", () => {
  const within = {
    kind: "within-file",
    line_count: 10,
    file_count: 1,
    occurrence_count: 2,
    files: [{ path: "src/frontend/iced/src/z.rs", lines: [[1, 10], [20, 29]] }],
  };
  const cross = {
    kind: "cross-file",
    line_count: 9,
    file_count: 3,
    occurrence_count: 3,
    files: [
      { path: "src/frontend/iced/src/a.rs", lines: [[2, 10]] },
      { path: "src/frontend/iced/src/b.rs", lines: [[2, 10]] },
      { path: "src/frontend/iced/src/c.rs", lines: [[2, 10]] },
    ],
  };
  assert.deepEqual([within, cross].sort(compareHits), [cross, within]);

  const suppressions = parseInlineSuppressions(
    "src/frontend/iced/src/a.rs",
    [
      "// CLEANUP-OFF: fixture false positive",
      "one",
      "two",
      "three",
      "four",
      "five",
      "six",
      "seven",
      "eight",
      "// CLEANUP-ON",
      "",
    ].join("\n"),
  );
  const applied = applyInlineSuppressions(
    [cross],
    suppressions,
    "duplo",
  );
  assert.equal(applied.suppressed.length, 1);
  assert.equal(applied.hits[0].occurrence_count, 2);
});

test("cleanup contains only targets while rejected output retains diagnostics by profile", () => {
  const files = buildInventory(
    frontend,
    {
      tracked: [
        "src/frontend/iced/src/generated.rs",
        "src/frontend/iced/third_party/dependency.rs",
      ],
      untracked: [],
      deleted: [],
    },
    () => true,
  );
  const input = {
    profile: frontend,
    files,
    duplo: {
      arguments: detectorArguments(frontend, {
        threads: 2,
        fileList: "/unused",
      }).duplo,
      status: 0,
      stderr: "",
    },
    cpd: {
      arguments: detectorArguments(frontend, {
        threads: 2,
        fileList: "/tmp/cpd-fixture/files.txt",
      }).cpd,
      status: 0,
      stderr: "",
      excludedLexerCount: 0,
    },
    codeOnlyDuplo: { filteredCount: 0 },
    hits: [],
    structuralHitsAfterSuppression: [],
    duploSuppression: { suppressed: [] },
    cpdSuppression: { suppressed: [] },
    clock: () => new Date("2026-08-31T12:34:56.000Z"),
    threads: 2,
  };
  const first = serializeReport(buildReport(input));
  const second = serializeReport(buildReport(input));
  assert.equal(first, second);
  assert.deepEqual(JSON.parse(first), { hits: [], structural_hits: [] });
  const report = buildReport(input);
  assert.equal(report.generated_at, "2026-08-31T12:34:56.000Z");
  assert.equal(report.profile, "frontend");
  assert.deepEqual(report.scope.retained_generated_paths, [
    "src/frontend/iced/src/generated.rs",
  ]);
  assert.deepEqual(report.scope.excluded_by_prefix_files, [
    "src/frontend/iced/third_party/dependency.rs",
  ]);
  assert.deepEqual(report.scope.inventory_commands, {
    tracked: [...INVENTORY_COMMANDS.tracked],
    untracked_unignored: [...INVENTORY_COMMANDS.untracked],
    worktree_deleted: [...INVENTORY_COMMANDS.deleted],
  });
  assert.deepEqual(report.duplo.arguments, [
    "-j",
    "2",
    "-ml",
    "9",
    "-json",
    "-",
    "-",
  ]);
  const priorCpp = { generated_at: "previous cpp pass" };
  const rejected = rejectionReport({ cpp: priorCpp }, report, { indexedFiles: 0, filtered: [] });
  assert.deepEqual(rejected.cpp, priorCpp);
  assert.equal(rejected.frontend.generated_at, report.generated_at);
  assert.ok(!Object.hasOwn(rejected.frontend, "hits"));
  assert.ok(!first.includes("suppressed_duplo_occurrences"));
});

function classifySources(sources, ranges = {}, tokenCount = 39) {
  const entries = Object.entries(sources);
  const reads = new Map();
  const result = filterCpdCandidates([{
    lineCount: 5, tokenCount,
    occurrences: entries.map(([path, source]) => ({
      path, start: 1, end: source.split("\n").length, ...ranges[path],
    })),
  }], {
    sourceReader(path) {
      reads.set(path, (reads.get(path) ?? 0) + 1);
      return sources[path];
    },
  });
  assert.deepEqual([...reads.values()], entries.map(() => 1));
  return result;
}

test("context filtering checks every match size and retains repeated implementations", () => {
  for (const tokenCount of [39, 99, 100, 101, 640]) {
    const unrelated = classifySources({
      "a.h": "struct A { int width; int height; };",
      "b.h": "struct B { bool active; bool pending; };",
    }, {}, tokenCount);
    assert.deepEqual(unrelated.duplications, []);
    assert.equal(unrelated.filtered.length, 1);
    assert.equal(unrelated.indexedFiles, 2);
    assert.equal(classifySources({
      "a.cpp": "void A() { auto item = acquire(); inspect(item); publish(item); }",
      "b.cpp": "void B() { auto value = acquire(); inspect(value); publish(value); }",
    }, {}, tokenCount).duplications.length, 1);
  }
});

test("CPD retains overloaded conversion setup despite distinct enum types and fallbacks", () => {
  const result = classifySources({
    "a.cpp": `void operator()(const char* key, Kind& value) const {
      int index = static_cast<int>(value);
      get_optional(json, key, index);
      value = kind_from_index(index, value);
    }`,
    "b.cpp": `void operator()(const char* key, Order& value) const {
      int index = static_cast<int>(value);
      get_optional(json, key, index);
      value = index == 1 ? Order::Shuffled : Order::Sequential;
    }`,
  });
  assert.equal(result.duplications.length, 1);
  assert.ok(result.duplications[0].patterns.includes("same function, first parameter and opening statement"));
});

test("overload identity includes the complete templated first parameter", () => {
  assert.equal(classifySources({
    "a.cpp": "void read(const std::map<int, First>& input, Kind& value) { int index = static_cast<int>(value); first(input, index); }",
    "b.cpp": "void read(const std::map<int, Second>& input, Order& value) { int index = static_cast<int>(value); second(input, index); }",
  }).duplications.length, 0);
});

test("equal signatures in separate implementations do not manufacture an overload family", () => {
  assert.equal(classifySources({
    "a.cpp": "void read(const Input& input, Value& value) { auto index = select(input); first(index, value); }",
    "b.cpp": "void read(const Input& input, Value& value) { auto index = select(input); second(index, value); }",
  }).duplications.length, 0);
});

test("aliases and bare control transfers do not turn one call into a shared algorithm", () => {
  for (const body of [
    "const int threads = launch::kThreads; const int blocks = blocks_for(count, threads);",
    "PixelThread pixel; if (!locate_pixel(width, height, pixel)) return;",
    "store(pixel, color); return;",
    "continue; } store(pixel, color);",
  ]) {
    const nested = body.startsWith("continue") ? "for (;;) { if (skip) { " : "";
    const closing = nested ? " }" : "";
    const sources = Object.fromEntries(["a", "b"].map((name) => [
      `${name}.cpp`, `void ${name}() { ${nested}${body}${closing} }`,
    ]));
    const ranges = Object.fromEntries(Object.entries(sources).map(([path, source]) =>
      [path, { column: source.indexOf(body) + 1 }]));
    assert.equal(classifySources(sources, ranges).duplications.length, 0);
  }
  assert.equal(classifySources({
    "a.cpp": "void A() { prepare(); store(pixel, color); return; }",
    "b.cpp": "void B() { prepare(); store(pixel, color); return; }",
  }).duplications.length, 1);
});

test("scalar calculations remain candidates despite different local roles", () => {
  const ranges = { "a.cpp": { start: 2, end: 3 }, "b.cpp": { start: 2, end: 3 } };
  assert.equal(classifySources({
    "a.cpp": "void A() {\nconst float transformed_x = matrix[0] * x + matrix[1] * y;\nconst float transformed_y = matrix[2] * x + matrix[3] * y;\n}",
    "b.cpp": "void B() {\nconst float sample_x = matrix[0] * x + matrix[1] * y;\nconst float sample_y = matrix[2] * x + matrix[3] * y;\n}",
  }, ranges).duplications.length, 1);
  for (const bodies of [
    ["const int area = width * height;\nconst int bytes = area * 4;", "const int count = width * height;\nconst int size = count * 4;"],
    ["auto first = acquire();\nauto second = acquire();", "auto left = acquire();\nauto right = acquire();"],
  ]) assert.equal(classifySources({
    "a.cpp": `void A() {\n${bodies[0]}\n}`,
    "b.cpp": `void B() {\n${bodies[1]}\n}`,
  }, ranges).duplications.length, 1, bodies.join("\nversus\n"));
});

test("repeated admission guards remain candidates despite different input roles", () => {
  for (const braced of [false, true]) {
    const failure = braced ? "{ return Invalid; }" : "return Invalid;";
    const make = (name, input) => `void ${name}(void* ${input}, int pitch) {\nif (${input} == nullptr) ${failure}\nif (pitch < minimum) ${failure}\n}`;
    assert.equal(classifySources({ "a.cpp": make("A", "scratch"), "b.cpp": make("B", "target") }).duplications.length, 1);
    assert.equal(classifySources({ "a.cpp": make("A", "scratch"), "b.cpp": make("B", "scratch") }).duplications.length, 1);
  }
});

test("CUDA setup remains visible alongside complete kernels and numerical work", () => {
  const make = (name, operation) => `__global__ void ${name}(int* output, int count) {
const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
if (index >= count) return;
output[index] = ${operation}(index);
}`;
  const prefix = { "a.cu": { end: 3 }, "b.cu": { end: 3 } };
  assert.equal(classifySources({ "a.cu": make("A", "first"), "b.cu": make("B", "second") }, prefix).duplications.length, 1);
  assert.equal(classifySources({ "a.cu": make("A", "first"), "b.cu": make("B", "first") }).duplications.length, 1);
  assert.equal(classifySources({
    "a.cu": "__global__ void A() {\nconst int area = width * height;\nconst int bytes = area * 4;\nwrite(bytes);\n}",
    "b.cu": "__global__ void B() {\nconst int area = width * height;\nconst int bytes = area * 4;\nwrite(bytes);\n}",
  }, { "a.cu": { start: 2, end: 3 }, "b.cu": { start: 2, end: 3 } }).duplications.length, 1);
  const projection = "__device__ int column() { return static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x); }\n";
  const projected = Object.fromEntries(["a", "b"].map((name) => [
    `${name}.cu`, projection + make(name, name).replace("static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x)", "column()"),
  ]));
  assert.equal(classifySources(projected, { "a.cu": { start: 2, end: 4 }, "b.cu": { start: 2, end: 4 } }).duplications.length, 1);
  const guarded = Object.fromEntries(["a", "b"].map((name) => [
    `${name}.cu`, make(name, name).replace("index >= count", "!admit(index) || !advance(count)"),
  ]));
  assert.equal(classifySources(guarded, prefix).duplications.length, 1);
  const numerical = Object.fromEntries(["a", "b"].map((name) => [
    `${name}.cu`, make(name, name).replace("output[index] =", "const int value = input[index] * gain;\nconst int result = value * value;\noutput[index] ="),
  ]));
  assert.equal(classifySources(numerical, { "a.cu": { end: 5 }, "b.cu": { end: 5 } }).duplications.length, 1);
  const admissions = Object.fromEntries(["a", "b"].map((name) => [
    `${name}.cu`, make(name, name).replace("if (index >= count) return;", "if (!admit(index)) return;\nif (!advance(count)) return;"),
  ]));
  assert.equal(classifySources(admissions, { "a.cu": { end: 4 }, "b.cu": { end: 4 } }).duplications.length, 1);
});

test("Duplo compares complete initializer context and preserves real repeated operations", () => {
  const hit = {
    LineCount: 6, SourceFile1: "a.cpp", StartLineNumber1: 2, EndLineNumber1: 7,
    SourceFile2: "b.cpp", StartLineNumber2: 2, EndLineNumber2: 7,
  };
  const initializer = (value) => `void publish_fact() {
const Fact fact{
.kind = Kind::Ready,
.width = width,
.height = height,
.epoch = epoch,
.generation = generation,
.capacity = ${value},
};
publish(fact);
}`;
  const rejected = filterDuploCandidates(cpp, [hit], (path) => initializer(path === "a.cpp" ? "width" : "capacity"));
  assert.deepEqual(rejected.hits, []);
  assert.equal(rejected.filteredCount, 0);
  assert.equal(rejected.contextRejected.length, 1);
  assert.deepEqual(rejected.contextRejected[0].occurrences.map(({ start, end }) => [start, end]), [[2, 7], [2, 7]]);
  const body = "void work() {\nauto item = acquire();\ninspect(item);\nprepare(item);\nwrite(item);\npublish(item);\nrelease(item);\n}";
  assert.deepEqual(filterDuploCandidates(cpp, [hit], () => body).hits, [hit]);
});

test("CPD does not present ordinary declarations or concatenated getters as extraction work", () => {
  for (const sources of [
    {
      "a.h": "struct Input { int width; int height; void Read(int); void Stop(); };",
      "b.h": "struct Output { int count; int capacity; void Write(int); void Cancel(); };",
    },
    {
      "a.cpp": "int A::first() { return first_.load(); }\nint A::second() { return second_.load(); }",
      "b.cpp": "int B::count() { return count_.load(); }\nint B::limit() { return limit_.load(); }",
    },
  ]) {
    const result = classifySources(sources);
    assert.equal(result.duplications.length, 0);
    assert.equal(result.filtered.length, 1);
  }
});

test("CPD keeps repeated executable sequences and separates different operation names", () => {
  const repeated = classifySources({
    "a.cpp": "void StartA() { auto x = acquire(); inspect(x); publish(x); }",
    "b.cpp": "void StartB() { auto y = acquire(); inspect(y); publish(y); }",
  });
  assert.equal(repeated.duplications.length, 1);
  const unrelated = classifySources({
    "a.cpp": "void Start() { auto x = open(); read(x); }",
    "b.cpp": "void Finish() { auto y = flush(); close(y); }",
  });
  assert.equal(unrelated.duplications.length, 0);
});

test("CPD retains repeated named fields that may duplicate a canonical declaration", () => {
  const result = classifySources({
    "a.h": "struct A { unsigned width = 0; unsigned height = 0; unsigned stride = 0; };",
    "b.h": "struct B { unsigned width = 0; unsigned height = 0; unsigned stride = 0; };",
  });
  assert.equal(result.duplications.length, 1);
  assert.ok(result.duplications[0].patterns.includes("same complete record declaration"));
});

test("CPD excludes namespace aliases and records that only share a field prefix", () => {
  for (const sources of [
    {
      "a.cpp": "namespace runtime = application::runtime;\nnamespace gpu = application::gpu;",
      "b.cpp": "namespace runtime = application::runtime;\nnamespace gpu = application::gpu;",
    },
    {
      "a.h": "struct A { int x = 0; int y = 0; bool valid() const { return x != 0 && y != 0; } };",
      "b.h": "struct B { int x = 0; int y = 0; bool valid() const { return x != 0 || y != 0; } };",
    },
  ]) assert.equal(classifySources(sources).duplications.length, 0);
});

test("a lock and one forwarded operation do not manufacture an overload algorithm", () => {
  for (const guard of ["lock(mutex_)", "lock{mutex_}"]) {
    assert.equal(classifySources({
      "a.cpp": `void A::SetPeer(int epoch) { std::scoped_lock ${guard}; input_.SetPeer(epoch); }`,
      "b.cpp": `void B::SetPeer(int epoch) { std::scoped_lock ${guard}; input_.SetPeer(epoch); }`,
    }).duplications.length, 0);
  }
});

test("CPD preserves member identities, constants and alias relationships after local renaming", () => {
  for (const changed of [
    "void B(State& value) { inspect(value.height); publish(value, 4); }",
    "void B(State& value) { inspect(value.width); publish(value, 8); }",
  ]) {
    assert.equal(classifySources({
      "a.cpp": "void A(State& state) { inspect(state.width); publish(state, 4); }",
      "b.cpp": changed,
    }).duplications.length, 0);
  }
  assert.equal(classifySources({
    "a.cpp": "void A(State& first, State& second) { inspect(first, first); publish(second); }",
    "b.cpp": "void B(State& left, State& right) { inspect(left, right); publish(right); }",
  }).duplications.length, 0);
});

test("CPD recognizes executable template and assignment-operator bodies", () => {
  assert.equal(classifySources({
    "a.cpp": "template<class T> void A(T& first) { inspect(first); publish(first); }",
    "b.cpp": "template<class T> void B(T& second) { inspect(second); publish(second); }",
  }).duplications.length, 1);
  assert.equal(classifySources({
    "a.cpp": "First& operator=(const First& value) { inspect(value); publish(value); return *this; }",
    "b.cpp": "Second& operator=(const Second& other) { inspect(other); publish(other); return *this; }",
  }).duplications.length, 1);
});

test("partial CPD spans retain the complete operation and initializer context", () => {
  const sources = {
    "a.cpp": "void A(int fd) { auto count = write(fd, &value, sizeof(value)); check(count); }",
    "b.cpp": "void B(int fd) { auto count = read(fd, &value, sizeof(value)); check(count); }",
  };
  const ranges = Object.fromEntries(Object.entries(sources).map(([path, source]) =>
    [path, { column: source.indexOf("(fd,") + 1, endColumn: source.indexOf("check(count);") + 13 }]));
  assert.equal(classifySources(sources, ranges).duplications.length, 0);
  const changed = {
    "a.cpp": "void A() { start(); Config config{.count = 2, .device = 0, .size = 8}; publish(config); }",
    "b.cpp": "void B() { start(); Config config{.count = 3, .device = 0, .size = 8}; publish(config); }",
  };
  for (const edge of ["column", "endColumn"]) {
    const partial = Object.fromEntries(Object.entries(changed).map(([path, source]) =>
      [path, { [edge]: edge === "column" ? source.indexOf(".device") + 1 : source.indexOf(".count") }]));
    assert.equal(classifySources(changed, partial).duplications.length, 0);
  }
  const same = { ...sources, "b.cpp": sources["b.cpp"].replace("read", "write") };
  assert.equal(classifySources(same, ranges).duplications.length, 1);
});

test("statement context handles for headers and compact nested blocks", () => {
  assert.equal(classifySources({
    "a.cpp": "void A() { for (int i = 0; i < count; ++i) { inspect(i); publish(i); } }",
    "b.cpp": "void B() { for (int n = 0; n < count; ++n) { inspect(n); publish(n); } }",
  }).duplications.length, 1);
});

test("loop headers, simple aliases, and contract guards are not shared algorithms", () => {
  for (const sources of [
    {
      "a.cpp": "void A() { for (int i = 0; i < count; ++i) { inspect(i); } }",
      "b.cpp": "void B() { for (int i = 0; i < count; ++i) { publish(i); } }",
    },
    {
      "a.cpp": "void forward(Tensor x) { const auto residual = x; first(x); }",
      "b.cpp": "void forward(Tensor x) { const auto residual = x; second(x); }",
    },
    {
      "a.cpp": "void apply(T& value) { static_assert(accepts<T>()); first(value); }",
      "b.cpp": "void apply(T& value) { static_assert(accepts<T>()); second(value); }",
    },
    {
      "a.cpp": "void Start(Request& request) { if (!request.current()) return; first(request); }",
      "b.cpp": "void Start(Request& request) { if (!request.current()) return; second(request); }",
    },
    {
      "a.cpp": "void A(T& value) { if constexpr (supported<T>()) { inspect(value); } first(value); }",
      "b.cpp": "void B(T& value) { if constexpr (supported<T>()) { inspect(value); } second(value); }",
    },
  ]) assert.equal(classifySources(sources).duplications.length, 0);
});

test("independent tensor dimension reads preserve their distinct local roles", () => {
  assert.equal(classifySources({
    "a.cpp": "void A(Tensor& value) { const int batch = value.size(0); const int heads = value.size(2); const int channels = value.size(3); }",
    "b.cpp": "void B(Tensor& value) { const int batch = value.size(0); const int height = value.size(2); const int width = value.size(3); }",
  }).duplications.length, 0);
});

test("shared arithmetic accepts local or member receiver inputs without losing alias relationships", () => {
  assert.equal(classifySources({
    "a.cpp": "void A(Tensor& masks) { const auto count = masks.size(-2) * masks.size(-1); auto points = random(count, masks.device()); }",
    "b.cpp": "void B(Sparse& sparse) { const auto count = sparse.features.size(-2) * sparse.features.size(-1); auto points = random(count, sparse.features.device()); }",
  }).duplications.length, 1);
  assert.equal(classifySources({
    "a.cpp": "void A(Tensor& first, Tensor& second) { inspect(first); const auto count = first.size(0) * second.size(0); publish(count); }",
    "b.cpp": "void B(Tensor& first, Tensor& second) { inspect(first); const auto count = second.size(0) * second.size(0); publish(count); }",
  }).duplications.length, 0);
});

test("multiple spans of one enclosing declaration are not independent duplicates", () => {
  const source = "struct State {\nint width;\nint height;\nint stride;\nint x;\nint y;\nint z;\n};";
  const result = filterCpdCandidates([{
    lineCount: 3, tokenCount: 39, occurrences: [
      { path: "state.h", start: 2, end: 4 },
      { path: "state.h", start: 5, end: 7 },
    ],
  }], { sourceReader: () => source });
  assert.equal(result.duplications.length, 0);
});

test("independent operation groups stay separate within one CPD candidate", () => {
  const result = classifySources({
    "a.cpp": "void A() { prepare(); publish(); }",
    "b.cpp": "void B() { prepare(); publish(); }",
    "c.cpp": "void C() { inspect(); release(); }",
    "d.cpp": "void D() { inspect(); release(); }",
  });
  assert.equal(result.duplications.length, 2);
  assert.deepEqual(result.duplications.map((hit) => hit.occurrences.map(({ path }) => path)),
    [["a.cpp", "b.cpp"], ["c.cpp", "d.cpp"]]);
});

test("overlapping CPD spans of the same complete statements produce one target", () => {
  const sources = {
    "a.cpp": "void A() { inspect(value); publish(value); }",
    "b.cpp": "void B() { inspect(value); publish(value); }",
  };
  const candidate = {
    lineCount: 1, tokenCount: 39,
    occurrences: Object.entries(sources).map(([path, source]) =>
      ({ path, start: 1, end: 1, column: source.indexOf("inspect") + 1, endColumn: source.indexOf("; }") + 1 })),
  };
  const result = filterCpdCandidates([candidate, {
    ...candidate, tokenCount: 42,
    occurrences: candidate.occurrences.map((occurrence) => ({ ...occurrence, column: occurrence.column + 7 })),
  }], { sourceReader: (path) => sources[path] });
  assert.equal(result.duplications.length, 1);
  assert.equal(result.duplications[0].tokenCount, 42);
  assert.match(result.filtered[0].reason, /already represented/u);
});

test("one overload family consolidates matches without losing distinct function targets", () => {
  const source = `void read(const char* key, A& value) {
  int index = static_cast<int>(value); decode(key, index); first(index, value);
}
void read(const char* key, B& value) {
  int index = static_cast<int>(value); decode(key, index); second(index, value);
}
void read(const char* key, C& value) {
  int index = static_cast<int>(value); decode(key, index); third(index, value);
}`;
  const result = filterCpdCandidates([
    { lineCount: 3, tokenCount: 39, occurrences: [
      { path: "a.cpp", start: 1, end: 3 }, { path: "a.cpp", start: 4, end: 6 },
    ] },
    { lineCount: 3, tokenCount: 41, occurrences: [
      { path: "a.cpp", start: 4, end: 6 }, { path: "a.cpp", start: 7, end: 9 },
    ] },
  ], { sourceReader: () => source });
  assert.equal(result.duplications.length, 1);
  assert.deepEqual(result.duplications[0].occurrences.map(({ start, end }) => [start, end]),
    [[1, 2], [4, 5], [7, 8]]);
});

test("CPD preserves assertion facts rather than merging unrelated test expectations", () => {
  const result = classifySources({
    "a.cpp": "void A() { CHECK(frame.ready); CHECK(frame.width == 3); }",
    "b.cpp": "void B() { CHECK(run.active); CHECK(run.epoch == 3); }",
  });
  assert.equal(result.duplications.length, 0);
  const repeated = classifySources({
    "a.cpp": "void A() { CHECK(frame.ready); CHECK(frame.width == 3); }",
    "b.cpp": "void B() { CHECK(frame.ready); CHECK(frame.width == 3); }",
  });
  assert.equal(repeated.duplications.length, 1);
});

test("CPD skips strings and comments and handles outer code after a nested callback", () => {
  const result = classifySources({
    "a.cpp": `void A() {
      const auto text = R"tag({ void fake() { })tag";
      /* } arbitrary comment { */
      callback([]() { inspect(); });
      auto x = acquire(); inspect(x); publish(x);
    }`,
    "b.cpp": `void B() {
      const auto text = R"tag({ void fake() { })tag";
      callback([]() { inspect(); });
      auto y = acquire(); inspect(y); publish(y);
    }`,
  });
  assert.equal(result.duplications.length, 1);
  assert.ok(!result.duplications[0].patterns.includes("unclassified syntax requires manual review"));
});

test("a call in a lambda capture is not a function declaration or overload family", () => {
  assert.equal(classifySources({
    "a.cpp": "void A(Runtime& runtime) { owner.Defer(runtime, [value = std::move(candidate)]() mutable { runtime.Commit(std::move(value)); first(); }); }",
    "b.cpp": "void B(Runtime& runtime) { owner.Defer(runtime, [value = std::move(candidate)]() mutable { runtime.Commit(std::move(value)); second(); }); }",
  }).duplications.length, 0);
});


test("raw CPD argument validation cannot alter ordinary profile thresholds", () => {
  const raw = parseArgs(["--raw-cpd", "--min-tokens", "16", "--output", "cleanup/raw.json"]);
  assert.equal(raw.profile.cpd.minTokens, 16);
  assert.equal(raw.output, "cleanup/raw.json");
  assert.equal(cpp.cpd.minTokens, 39);
  assert.deepEqual(raw.profile.cpd.flags, ["--skip-blocks-pattern", ""]);
  for (const args of [["--raw-cpd", "--min-tokens", "1"], ["--raw-cpd", "--min-tokens", "NaN"],
    ["--raw-cpd", "--min-tokens", "12.5"], ["--raw-cpd", "--output"], ["--raw-cpd", "--cpp"],
    ["--raw-cpd", "--output", "a", "--output", "b"]]) assert.throws(() => parseArgs(args));
});
