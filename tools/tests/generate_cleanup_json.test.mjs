import assert from "node:assert/strict";
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
  parseArgs,
  parseCpdXml,
  parseDuploJson,
  parseInlineSuppressions,
  serializeReport,
  structuralHits,
} from "../generate_cleanup_json.mjs";

const cpp = CLEANUP_PROFILES.cpp;
const frontend = CLEANUP_PROFILES.frontend;

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
    duplo: ["-j", "3", "-ml", "7", "-ip", "-json", "-", "-"],
    cpd: [
      "cpd",
      "--file-list",
      "/f",
      "--language",
      "cpp",
      "--minimum-tokens",
      "69",
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
    removable_duplicate_lines: 10,
    files: [{ path: "src/frontend/iced/src/z.rs", lines: [[1, 10], [20, 29]] }],
  };
  const cross = {
    kind: "cross-file",
    line_count: 9,
    file_count: 3,
    occurrence_count: 3,
    removable_duplicate_lines: 18,
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

test("injected-clock report metadata is byte stable", () => {
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
  const report = JSON.parse(first);
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
});
